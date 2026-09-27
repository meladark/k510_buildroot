#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <linux/videodev2.h>
#include <opencv2/imgproc.hpp>
#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "canaan/v4l2.h"
#include "util.h"

extern "C" {
#include <media_ctl.h>
}

using namespace rapidjson;

std::mutex g_v4l2_mtx;

#define PIPELINE_CONF "/tmp/parking_video.conf"

// /dev/videoN per camera: main, ds0, ds1, ds2
static const char *kNodes[NUM_CAMS][4] = {
    {"/dev/video2", "/dev/video3", "/dev/video4", "/dev/video5"},
    {"/dev/video6", "/dev/video7", "/dev/video8", "/dev/video9"},
};

const char *cam_node(int cam, int ch)
{
    return kNodes[cam][ch];
}

void compute_layout(uint32_t sw, uint32_t sh, const bool enabled[NUM_CAMS], CamLayout out[NUM_CAMS])
{
    int n = 0;
    for (int i = 0; i < NUM_CAMS; i++) {
        out[i] = CamLayout();
        out[i].enabled = enabled[i];
        n += enabled[i];
    }
    bool landscape = sw >= sh;
    int w, h;
    if (n == 2) {
        // same sizes as the stock v4l2_drm demo for the known panels
        if (sw == 1920 && sh == 1080) {
            w = 944; h = 532;
        } else if (sw == 1080 && sh == 1920) {
            w = 1072; h = 604;
        } else if (sw == 1280 && sh == 720) {
            w = 624; h = 352;
        } else if (landscape) {
            w = (sw / 2 - 16) & ~15; h = (w * 9 / 16) & ~1;
        } else {
            w = (sw - 8) & ~15; h = (w * 9 / 16) & ~1;
        }
    } else {
        if (landscape && sw <= 1920 && sh <= 1080) {
            w = sw & ~15; h = sh & ~1;
        } else {
            w = (sw - 8) & ~15; h = (w * 9 / 16) & ~1;
        }
    }
    int k = 0;
    for (int i = 0; i < NUM_CAMS; i++) {
        if (!enabled[i])
            continue;
        Rect r;
        r.w = w;
        r.h = h;
        if (n == 2 && landscape) {
            r.x = k * sw / 2 + (sw / 2 - w) / 2;
            r.y = (sh - h) / 2;
        } else if (n == 2) {
            r.x = (sw - w) / 2;
            r.y = k * sh / 2 + (sh / 2 - h) / 2;
        } else {
            r.x = (sw - w) / 2;
            r.y = (sh - h) / 2;
        }
        out[i].dst = r;
        k++;
    }
}

int video_pipeline_init(const std::string &base_conf, const Config &cfg, const CamLayout layout[NUM_CAMS],
                        const AiGeom &ai)
{
    std::string json;
    if (!util::read_file(base_conf, json)) {
        fprintf(stderr, "cannot read %s\n", base_conf.c_str());
        return -1;
    }
    Document root;
    root.Parse(json.c_str());
    if (root.HasParseError()) {
        fprintf(stderr, "cannot parse %s\n", base_conf.c_str());
        return -1;
    }
    for (int c = 0; c < NUM_CAMS; c++) {
        std::string s = "/sensor" + std::to_string(c);
        int v = c * 4 + 2;  // first video node of this sensor
        auto key = [&](int node, const char *field) {
            return s + "/~1dev~1video" + std::to_string(node) + "/video" + std::to_string(node) + "_" + field;
        };
        for (int i = 0; i < 4; i++)
            Pointer(key(v + i, "used").c_str()).Set(root, 0);
        if (!layout[c].enabled)
            continue;
        // IMX219 1920x1080 timing, as in the stock dual camera demo
        Pointer((s + "/sensor" + std::to_string(c) + "_cfg_file").c_str())
            .Set(root, c == 0 ? "imx219_0.conf" : "imx219_1.conf");
        Pointer((s + "/sensor" + std::to_string(c) + "_total_size/sensor" + std::to_string(c) + "_total_width").c_str()).Set(root, 3476);
        Pointer((s + "/sensor" + std::to_string(c) + "_total_size/sensor" + std::to_string(c) + "_total_height").c_str()).Set(root, 1166);
        Pointer((s + "/sensor" + std::to_string(c) + "_active_size/sensor" + std::to_string(c) + "_active_width").c_str()).Set(root, 1936);
        Pointer((s + "/sensor" + std::to_string(c) + "_active_size/sensor" + std::to_string(c) + "_active_height").c_str()).Set(root, 1088);
        // main output (unused, but sizes drive the pipeline)
        Pointer(key(v, "width").c_str()).Set(root, 1920);
        Pointer(key(v, "height").c_str()).Set(root, 1080);
        Pointer(key(v, "out_format").c_str()).Set(root, 1);
        // ds0: display, NV12
        Pointer(key(v + 1, "used").c_str()).Set(root, 1);
        Pointer(key(v + 1, "width").c_str()).Set(root, layout[c].dst.w);
        Pointer(key(v + 1, "height").c_str()).Set(root, layout[c].dst.h);
        Pointer(key(v + 1, "out_format").c_str()).Set(root, 1);
        // ds1: photos, NV12
        Pointer(key(v + 2, "used").c_str()).Set(root, channel_disabled("photo", c) ? 0 : 1);
        Pointer(key(v + 2, "width").c_str()).Set(root, cfg.photo_width);
        Pointer(key(v + 2, "height").c_str()).Set(root, cfg.photo_height);
        Pointer(key(v + 2, "out_format").c_str()).Set(root, 1);
        // ds2: AI input, planar RGB in a net_w x net_h buffer
        Pointer(key(v + 3, "used").c_str()).Set(root, channel_disabled("ai", c) ? 0 : 1);
        Pointer(key(v + 3, "width").c_str()).Set(root, ai.valid_w);
        Pointer(key(v + 3, "height").c_str()).Set(root, ai.net_h);
        Pointer(key(v + 3, "height_r").c_str()).Set(root, ai.valid_h);
        Pointer(key(v + 3, "pitch").c_str()).Set(root, ai.net_w);
        Pointer(key(v + 3, "out_format").c_str()).Set(root, 0);
    }
    StringBuffer sb;
    Writer<StringBuffer> w(sb);
    root.Accept(w);
    if (!util::write_file_atomic(PIPELINE_CONF, sb.GetString()))
        return -1;

    static struct video_info dev_info[2];
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    if (mediactl_init((char *)PIPELINE_CONF, &dev_info[0])) {
        fprintf(stderr, "mediactl_init failed\n");
        return -1;
    }
    return 0;
}

void video_pipeline_deinit()
{
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    mediactl_exit();
}

// ---------------------------------------------------------------------------

bool channel_disabled(const char *what, int cam)
{
    const char *env = getenv("PARKING_DISABLE");
    if (!env)
        return false;
    std::string key = std::string(what) + std::to_string(cam);
    std::string list = std::string(",") + env + ",";
    return list.find("," + key + ",") != std::string::npos;
}

int DisplayCam::open(int cam, DrmOut &drm, const Rect &dst)
{
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    cam_ = cam;
    drm_ = &drm;
    fd_ = ::open(kNodes[cam][1], O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        fprintf(stderr, "cam%d display: cannot open %s\n", cam, kNodes[cam][1]);
        return -1;
    }
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.width = dst.w;
    fmt.fmt.pix.height = dst.h;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "cam%d display: S_FMT failed: %s\n", cam, strerror(errno));
        return -1;
    }
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = VIDEO_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < VIDEO_BUFS) {
        fprintf(stderr, "cam%d display: REQBUFS failed: %s\n", cam, strerror(errno));
        return -1;
    }
    for (int i = 0; i < VIDEO_BUFS; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_DMABUF;
        b.index = i;
        b.m.fd = drm.video_buf(cam, i).dmabuf_fd;
        b.length = drm.video_buf(cam, i).size;
        if (ioctl(fd_, VIDIOC_QBUF, &b) < 0) {
            fprintf(stderr, "cam%d display: QBUF failed: %s\n", cam, strerror(errno));
            return -1;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "cam%d display: stream on failed: %s\n", cam, strerror(errno));
        return -1;
    }
    return 0;
}

void DisplayCam::close()
{
    if (fd_ < 0)
        return;
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_DMABUF;
    ioctl(fd_, VIDIOC_REQBUFS, &req);
    ::close(fd_);
    fd_ = -1;
}

int DisplayCam::dequeue()
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_DMABUF;
    {
        std::lock_guard<std::mutex> lk(g_v4l2_mtx);
        if (ioctl(fd_, VIDIOC_DQBUF, &b) < 0)
            return -1;
    }
    if (b.flags & V4L2_BUF_FLAG_ERROR) {
        queue(b.index);
        return -1;
    }
    return b.index;
}

int DisplayCam::queue(int idx)
{
    if (idx < 0)
        return 0;
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = idx;
    b.m.fd = drm_->video_buf(cam_, idx).dmabuf_fd;
    b.length = drm_->video_buf(cam_, idx).size;
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    return ioctl(fd_, VIDIOC_QBUF, &b);
}

// ---------------------------------------------------------------------------

int MmapCam::open(const char *node, uint32_t fourcc, int w, int h, int nbufs, bool shm)
{
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    dev_ = v4l2_open(node);
    if (!dev_)
        return -1;
    struct v4l2_pix_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.pixelformat = fourcc;
    fmt.width = w;
    fmt.height = h;
    if (v4l2_set_format(dev_, &fmt) < 0 ||
        v4l2_alloc_buffers(dev_, shm ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP, nbufs) < 0) {
        fprintf(stderr, "%s: format/buffers failed\n", node);
        return -1;
    }
    if (shm) {
        // our own buffers in the shared pool, like the stock encode_app
        shm_fd_ = ::open("/dev/k510-share-memory", O_RDWR);
        mem_fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (shm_fd_ < 0 || mem_fd_ < 0)
            return -1;
        for (unsigned i = 0; i < dev_->nbufs; i++) {
            struct {
                uint32_t size, alignment, phys;
            } a = {(dev_->buffers[i].size + 4095u) & ~4095u, 4096, 0};
            if (ioctl(shm_fd_, _IOWR('m', 2, unsigned long), &a) < 0) {
                fprintf(stderr, "%s: no shared memory for %u bytes\n", node, a.size);
                return -1;
            }
            void *v = mmap(nullptr, a.size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd_, a.phys);
            phys_.push_back(a.phys);
            maps_.push_back(v == MAP_FAILED ? nullptr : v);
            sizes_.push_back(a.size);
            if (v == MAP_FAILED)
                return -1;
            dev_->buffers[i].mem = v;
            dev_->buffers[i].size = a.size;
        }
    }
    for (unsigned i = 0; i < dev_->nbufs; i++) {
        struct v4l2_video_buffer b = dev_->buffers[i];
        b.index = i;
        if (v4l2_queue_buffer(dev_, &b) < 0)
            return -1;
    }
    if (v4l2_stream_on(dev_) < 0) {
        fprintf(stderr, "%s: stream on failed\n", node);
        return -1;
    }
    return 0;
}

void MmapCam::close()
{
    if (!dev_)
        return;
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    v4l2_stream_off(dev_);
    v4l2_free_buffers(dev_);
    v4l2_close(dev_);
    dev_ = nullptr;
    for (size_t i = 0; i < phys_.size(); i++) {
        if (maps_[i])
            munmap(maps_[i], sizes_[i]);
        ioctl(shm_fd_, _IOWR('m', 3, unsigned long), &phys_[i]);
    }
    phys_.clear();
    maps_.clear();
    sizes_.clear();
    if (shm_fd_ >= 0)
        ::close(shm_fd_);
    if (mem_fd_ >= 0)
        ::close(mem_fd_);
    shm_fd_ = mem_fd_ = -1;
}

int MmapCam::fd() const
{
    return dev_ ? dev_->fd : -1;
}

int MmapCam::dq(void **mem, unsigned *len, unsigned *idx)
{
    struct v4l2_video_buffer b;
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    if (v4l2_dequeue_buffer(dev_, &b) < 0)
        return -1;
    *mem = b.mem;
    *len = b.bytesused ? b.bytesused : b.size;
    *idx = b.index;
    if (b.error) {
        struct v4l2_video_buffer q = dev_->buffers[b.index];
        q.index = b.index;
        v4l2_queue_buffer(dev_, &q);
        return -1;
    }
    return 0;
}

int MmapCam::q(unsigned idx)
{
    struct v4l2_video_buffer b = dev_->buffers[idx];
    b.index = idx;
    std::lock_guard<std::mutex> lk(g_v4l2_mtx);
    return v4l2_queue_buffer(dev_, &b);
}

// ---------------------------------------------------------------------------

int PhotoCam::start(int cam, int w, int h)
{
    cam_ = cam;
    w_ = w;
    h_ = h;
    // 4 buffers in shared memory: one may sit in the video encoder
    if (cap_.open(kNodes[cam][2], V4L2_PIX_FMT_NV12, w, h, 4, true) < 0)
        return -1;
    running_ = true;
    th_ = std::thread(&PhotoCam::loop, this);
    return 0;
}

void PhotoCam::stop()
{
    if (!running_)
        return;
    running_ = false;
    if (th_.joinable())
        th_.join();
    cap_.close();
}

void PhotoCam::set_sink(std::function<void(const Nv12Frame &)> fn)
{
    std::lock_guard<std::mutex> lk(sink_m_);
    sink_ = std::move(fn);
}

void PhotoCam::loop()
{
    const size_t need = (size_t)w_ * h_ * 3 / 2;
    while (running_ && !g_quit.load()) {
        // Keep dequeuing so the next snapshot is always fresh; copy only on request.
        int r = cap_.grab_phys(1000, [&](void *mem, unsigned len, uint32_t phys) {
            {
                std::lock_guard<std::mutex> lk(sink_m_);
                if (sink_ && len >= need)
                    sink_(Nv12Frame{(const uint8_t *)mem, phys, w_, h_, w_, util::mono_ms()});
            }
            std::lock_guard<std::mutex> lk(m_);
            if (want_ > 0 && len >= need) {
                last_nv12_.create(h_ * 3 / 2, w_, CV_8UC1);
                memcpy(last_nv12_.data, mem, need);
                want_ = 0;
                seq_++;
                cv_.notify_all();
            }
        });
        if (r < 0)
            fprintf(stderr, "cam%d photo: select error\n", cam_);
    }
}

bool PhotoCam::snapshot(cv::Mat &bgr, int timeout_ms)
{
    if (!running_)
        return false;
    cv::Mat nv12;
    {
        std::unique_lock<std::mutex> lk(m_);
        uint64_t seq0 = seq_;
        want_++;
        // Skip one frame: the first dequeued buffer may be a few frames old.
        for (int pass = 0; pass < 2; pass++) {
            uint64_t target = seq0 + pass + 1;
            if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return seq_ >= target; }))
                return false;
            if (pass == 0)
                want_++;
        }
        nv12 = last_nv12_.clone();
    }
    cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
    return true;
}
