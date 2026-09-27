#include "video.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

extern "C" {
#include <enc_interface.h>
#include <libavformat/avformat.h>
}

#include "photos.h"  // valid_day
#include "util.h"

// One camera: encoder channel + MP4 writer. Fed from the capture thread.
struct VideoRecorder::Track {
    int cam = 0;
    std::string path, poster;
    EncoderHandle *enc = nullptr;
    AVFormatContext *oc = nullptr;
    AVStream *st = nullptr;
    bool header = false;
    int64_t t0 = -1, last_pts = -1;
    long frames = 0;
    std::mutex m;

    bool open(int w, int h)
    {
        if (w % 32) {  // the encoder reads rows 32-byte aligned; the photo sizes are
            fprintf(stderr, "video cam%d: width %d is not a multiple of 32\n", cam, w);
            return false;
        }
        EncSettings s;
        memset(&s, 0, sizeof(s));
        s.channel = cam;
        s.width = w;
        s.height = h;
        s.FrameRate = 30;
        s.rcMode = CBR;
        s.BitRate = std::max(1000000, w * h * 4);  // ~4 Mbit/s at 720p
        s.MaxBitRate = s.BitRate;
        s.SliceQP = 25;
        s.MinQP = 0;
        s.MaxQP = 51;
        s.profile = AVC_HIGH;
        s.level = 42;
        s.AspectRatio = ASPECT_RATIO_AUTO;
        s.FreqIDR = 30;  // a key frame per second: MP4 fragments, seeking
        s.gopLen = 30;
        s.entropyMode = ENTROPY_MODE_CABAC;
        s.roiCtrlMode = ROI_QP_TABLE_NONE;
        enc = VideoEncoder_Create(&s);
        if (!enc) {
            fprintf(stderr, "video cam%d: encoder create failed\n", cam);
            return false;
        }
        if (avformat_alloc_output_context2(&oc, nullptr, "mp4", path.c_str()) < 0 || !oc)
            return false;
        st = avformat_new_stream(oc, nullptr);
        if (!st)
            return false;
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_H264;
        st->codecpar->width = w;
        st->codecpar->height = h;
        st->time_base = AVRational{1, 90000};
        if (avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "video cam%d: cannot write %s\n", cam, path.c_str());
            return false;
        }
        return true;
    }

    // SPS + PPS of the first key frame become the MP4 decoder config.
    static std::vector<uint8_t> config_nals(const uint8_t *p, size_t n)
    {
        std::vector<uint8_t> out;
        size_t i = 0;
        auto next_start = [&](size_t from) {
            for (size_t k = from; k + 3 < n; k++)
                if (p[k] == 0 && p[k + 1] == 0 && (p[k + 2] == 1 || (p[k + 2] == 0 && p[k + 3] == 1)))
                    return k;
            return n;
        };
        i = next_start(0);
        while (i < n) {
            size_t sc = p[i + 2] == 1 ? 3 : 4, j = next_start(i + sc);
            int type = p[i + sc] & 0x1f;
            if (type == 7 || type == 8)
                out.insert(out.end(), p + i, p + j);
            i = j;
        }
        return out;
    }

    void write(const uint8_t *data, size_t size, bool key, int64_t ts_ms)
    {
        if (!header) {
            if (!key)
                return;  // start with a key frame
            std::vector<uint8_t> cfg = config_nals(data, size);
            if (cfg.empty())
                return;
            st->codecpar->extradata = (uint8_t *)av_mallocz(cfg.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(st->codecpar->extradata, cfg.data(), cfg.size());
            st->codecpar->extradata_size = cfg.size();
            AVDictionary *opts = nullptr;
            // fragmented: every key frame closes a fragment, the file stays playable
            av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
            int r = avformat_write_header(oc, &opts);
            av_dict_free(&opts);
            if (r < 0) {
                fprintf(stderr, "video cam%d: mp4 header failed\n", cam);
                return;
            }
            header = true;
            t0 = ts_ms;
        }
        AVPacket *pkt = av_packet_alloc();
        pkt->data = (uint8_t *)data;
        pkt->size = size;
        pkt->stream_index = st->index;
        int64_t pts = av_rescale_q(ts_ms - t0, AVRational{1, 1000}, st->time_base);
        if (pts <= last_pts)
            pts = last_pts + 1;
        pkt->pts = pkt->dts = last_pts = pts;
        pkt->duration = av_rescale_q(33, AVRational{1, 1000}, st->time_base);
        if (key)
            pkt->flags |= AV_PKT_FLAG_KEY;
        av_write_frame(oc, pkt);  // not refcounted: written right away
        av_packet_free(&pkt);
        frames++;
    }

    void frame(const Nv12Frame &f)
    {
        std::lock_guard<std::mutex> lk(m);
        if (!enc || !f.phys)
            return;
        if (frames == 0 && poster.size()) {  // first frame as a small JPEG for the lists
            cv::Mat nv12(f.h * 3 / 2, f.w, CV_8UC1, (void *)f.data), bgr, small;
            cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
            cv::resize(bgr, small, cv::Size(640, 640 * f.h / f.w), 0, 0, cv::INTER_AREA);
            cv::imwrite(poster, small, {cv::IMWRITE_JPEG_QUALITY, 80});
            poster.clear();
        }
        EncInputFrame in;
        memset(&in, 0, sizeof(in));
        in.width = f.w;
        in.height = f.h;
        in.stride = f.stride;
        in.data = (unsigned char *)(uintptr_t)f.phys;  // the encoder wants the physical address
        if (VideoEncoder_EncodeOneFrame(enc, &in) != Enc_SUCCESS)
            return;
        EncOutputStream out;
        memset(&out, 0, sizeof(out));
        // returns 1 even with a valid stream (as encode_app shows, it ignores
        // it); the buffer must always go back or the encoder stalls after 4 frames
        VideoEncoder_GetStream(enc, &out);
        if (out.bufSize && out.bufAddr)
            write(out.bufAddr, out.bufSize, out.packetType == EM_VIDEO_PACKET_IDR, f.ts_ms);
        VideoEncoder_ReleaseStream(enc, &out);
    }

    void close()
    {
        std::lock_guard<std::mutex> lk(m);
        if (oc) {
            if (header)
                av_write_trailer(oc);
            if (oc->pb)
                avio_closep(&oc->pb);
            avformat_free_context(oc);
            oc = nullptr;
        }
        if (enc) {
            VideoEncoder_Destroy(enc);
            enc = nullptr;
        }
        if (!header)
            unlink(path.c_str());  // nothing was written
        printf("video cam%d: %ld frames -> %s\n", cam, frames, path.c_str());
    }
};

void VideoRecorder::init(PhotoCam *cams[NUM_CAMS])
{
    for (int c = 0; c < NUM_CAMS; c++)
        cams_[c] = cams[c];
}

VideoRecorder::~VideoRecorder()
{
    stop();
}

std::string VideoRecorder::start(int max_seconds, unsigned cam_mask)
{
    std::lock_guard<std::mutex> lk(m_);
    if (active_.load())
        return "";
    int64_t now = util::now_ms();
    std::string day = util::clock_is_sane() ? util::time_str(now, "%Y-%m-%d") : "0000-00-00";
    std::string hms = util::time_str(now, "%H%M%S");
    std::string dir = std::string(VIDEOS_DIR) + "/" + day;
    util::mkdirs(dir);
    int n = 0;
    for (int c = 0; c < NUM_CAMS; c++) {
        if (!cams_[c] || !cams_[c]->running() || !(cam_mask & (1u << c)))
            continue;
        Track *t = new Track();
        t->cam = c;
        std::string base = dir + "/" + hms + "_cam" + std::to_string(c);
        t->path = base + ".mp4";
        t->poster = base + ".jpg";
        if (!t->open(cams_[c]->width(), cams_[c]->height())) {
            t->close();
            delete t;
            continue;
        }
        tracks_[c] = t;
        cams_[c]->set_sink([t](const Nv12Frame &f) { t->frame(f); });
        n++;
    }
    if (!n)
        return "";
    max_s_ = max_seconds;
    started_ms_ = util::mono_ms();
    active_ = true;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.rec_started_ms = started_ms_;
    }
    printf("video: recording %s/%s (%d cameras)\n", day.c_str(), hms.c_str(), n);
    return day + "/" + hms;
}

void VideoRecorder::stop()
{
    std::lock_guard<std::mutex> lk(m_);
    if (!active_.load())
        return;
    for (int c = 0; c < NUM_CAMS; c++) {
        if (!tracks_[c])
            continue;
        cams_[c]->set_sink(nullptr);  // waits for a frame in progress
        tracks_[c]->close();
        delete tracks_[c];
        tracks_[c] = nullptr;
    }
    active_ = false;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.rec_started_ms = 0;
    }
    sync();
}

void VideoRecorder::tick()
{
    if (!active_.load())
        return;
    double min_free;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        min_free = g_state.cfg.min_free_pct;
    }
    if (util::mono_ms() - started_ms_ > max_s_ * 1000LL || util::free_space_pct(DATA_DIR) < min_free / 2) {
        printf("video: limit reached, stopping\n");
        stop();
    }
}

std::vector<std::string> VideoRecorder::days()
{
    std::vector<std::string> v;
    for (auto &d : util::list_dir(VIDEOS_DIR))
        if (valid_day(d))
            v.push_back(d);
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<std::string> VideoRecorder::files(const std::string &day)
{
    std::vector<std::string> v;
    if (!valid_day(day))
        return v;
    for (auto &f : util::list_dir(std::string(VIDEOS_DIR) + "/" + day))
        if (f.size() > 4 && (f.compare(f.size() - 4, 4, ".mp4") == 0 || f.compare(f.size() - 4, 4, ".jpg") == 0))
            v.push_back(f);
    std::sort(v.begin(), v.end());
    return v;
}

bool VideoRecorder::remove(const std::string &day, const std::string &name)
{
    if (!valid_day(day) || name.size() != 6 || name.find_first_not_of("0123456789") != std::string::npos)
        return false;
    std::string dir = std::string(VIDEOS_DIR) + "/" + day + "/";
    bool any = false;
    for (auto &f : files(day))
        if (f.compare(0, 6, name) == 0)
            any |= unlink((dir + f).c_str()) == 0;
    if (files(day).empty())
        rmdir(dir.c_str());
    sync();
    return any;
}

// parking --venc-test [w h]: encodes a few gray frames on one channel and
// prints every step (hardware encoder bring-up check).
int venc_selftest(int w, int h)
{
    int shm = open("/dev/k510-share-memory", O_RDWR), mem = open("/dev/mem", O_RDWR | O_SYNC);
    struct {
        uint32_t size, alignment, phys;
    } a = {(uint32_t)(w * h * 3 / 2 + 4095) & ~4095u, 4096, 0};
    if (shm < 0 || mem < 0 || ioctl(shm, _IOWR('m', 2, unsigned long), &a) < 0) {
        printf("venc test: no shared memory\n");
        return 1;
    }
    uint8_t *v = (uint8_t *)mmap(nullptr, a.size, PROT_READ | PROT_WRITE, MAP_SHARED, mem, a.phys);
    memset(v, 128, a.size);
    printf("venc test: frame %dx%d at 0x%08x\n", w, h, a.phys);
    EncSettings s;
    memset(&s, 0, sizeof(s));
    s.channel = 0;
    s.width = w;
    s.height = h;
    s.FrameRate = 30;
    s.rcMode = CBR;
    s.BitRate = s.MaxBitRate = 2000000;
    s.SliceQP = 25;
    s.MaxQP = 51;
    s.profile = AVC_HIGH;
    s.level = 42;
    s.FreqIDR = s.gopLen = 30;
    s.entropyMode = ENTROPY_MODE_CABAC;
    EncoderHandle *enc = VideoEncoder_Create(&s);
    printf("venc test: create -> %p\n", (void *)enc);
    for (int i = 0; enc && i < 5; i++) {
        EncInputFrame in;
        memset(&in, 0, sizeof(in));
        in.width = w;
        in.height = h;
        in.stride = w;
        in.data = (unsigned char *)(uintptr_t)a.phys;
        printf("venc test: encode %d...\n", i);
        int r = VideoEncoder_EncodeOneFrame(enc, &in);
        printf("venc test: encode -> %d, get stream...\n", r);
        EncOutputStream out;
        memset(&out, 0, sizeof(out));
        r = VideoEncoder_GetStream(enc, &out);
        printf("venc test: stream -> %d, %u bytes, type %d\n", r, out.bufSize, (int)out.packetType);
        VideoEncoder_ReleaseStream(enc, &out);
    }
    if (enc)
        VideoEncoder_Destroy(enc);
    munmap(v, a.size);
    ioctl(shm, _IOWR('m', 3, unsigned long), &a.phys);
    return 0;
}
