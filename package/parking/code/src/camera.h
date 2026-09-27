#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "drm_out.h"
#include "state.h"

struct v4l2_device;

// Guards V4L2/mediactl calls, same as the Canaan demos do.
extern std::mutex g_v4l2_mtx;

struct CamLayout {
    bool enabled = false;
    Rect dst = {0, 0, 0, 0};  // display rectangle on screen
};

// AI input geometry (ISP ds2): valid_w x valid_h image inside a net_w x net_h
// buffer, centered or in the top-left corner (as the model was trained).
struct AiGeom {
    int net_w = 320, net_h = 320;
    int valid_w = 320, valid_h = 240;
    bool center = true;
};

// Picks the on-screen layout for the enabled cameras.
void compute_layout(uint32_t sw, uint32_t sh, const bool enabled[NUM_CAMS], CamLayout out[NUM_CAMS]);

// Writes the ISP pipeline config from the base file and calls mediactl_init().
int video_pipeline_init(const std::string &base_conf, const Config &cfg, const CamLayout layout[NUM_CAMS],
                        const AiGeom &ai);
void video_pipeline_deinit();

const char *cam_node(int cam, int ch);  // ch: 1 = ds0 display, 2 = ds1 photo, 3 = ds2 AI

// ISP ds0 -> DRM NV12 buffers (zero copy, DMABUF like the stock v4l2_drm demo).
class DisplayCam {
public:
    int open(int cam, DrmOut &drm, const Rect &dst);
    void close();
    int fd() const { return fd_; }
    // Dequeue the newest frame; returns its DRM buffer index or -1.
    int dequeue();
    int queue(int idx);

private:
    int fd_ = -1;
    DrmOut *drm_ = nullptr;
    int cam_ = 0;
};

// Debug switch: PARKING_DISABLE="photo1,ai1" turns off ISP channels per camera.
bool channel_disabled(const char *what, int cam);

// Generic capture used for the AI (RGB planar) and photo (NV12) channels.
// With shm the frames land in KPU/ISP shared memory (V4L2 USERPTR) and have a
// physical address, so the hardware video encoder reads them without a copy.
class MmapCam {
public:
    int open(const char *node, uint32_t fourcc, int w, int h, int nbufs, bool shm = false);
    void close();
    int fd() const;
    // Waits for a frame and calls fn(data, bytes) while the buffer is dequeued.
    template <typename F>
    int grab(int timeout_ms, F fn);
    // Same, fn(data, bytes, physical address or 0).
    template <typename F>
    int grab_phys(int timeout_ms, F fn);

private:
    int dq(void **mem, unsigned *len, unsigned *idx);
    int q(unsigned idx);
    v4l2_device *dev_ = nullptr;
    std::vector<uint32_t> phys_;  // shm buffers
    std::vector<void *> maps_;
    std::vector<size_t> sizes_;
    int shm_fd_ = -1, mem_fd_ = -1;
};

// One NV12 frame of the photo channel, valid during the sink call.
struct Nv12Frame {
    const uint8_t *data;  // Y plane, then interleaved UV
    uint32_t phys;        // physical address of data (0 if unknown)
    int w, h, stride;
    int64_t ts_ms;        // monotonic capture time
};

// Keeps the ds1 stream flowing and hands out JPEG snapshots on request.
class PhotoCam {
public:
    int start(int cam, int w, int h);
    void stop();
    // Returns false on timeout; frame is BGR.
    bool snapshot(cv::Mat &bgr, int timeout_ms = 3000);
    bool running() const { return running_; }
    int width() const { return w_; }
    int height() const { return h_; }
    // Called for every frame from the capture thread (video recording); empty = off.
    void set_sink(std::function<void(const Nv12Frame &)> fn);

private:
    void loop();
    MmapCam cap_;
    int cam_ = 0, w_ = 0, h_ = 0;
    std::thread th_;
    bool running_ = false;
    std::mutex m_;
    std::condition_variable cv_;
    int want_ = 0;       // pending requests
    uint64_t seq_ = 0;   // incremented when a frame has been stored
    cv::Mat last_nv12_;
    std::mutex sink_m_;
    std::function<void(const Nv12Frame &)> sink_;
};

#include <sys/select.h>

template <typename F>
int MmapCam::grab(int timeout_ms, F fn)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd(), &fds);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = select(fd() + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0)
        return r == 0 ? 0 : -1;
    void *mem;
    unsigned len, idx;
    if (dq(&mem, &len, &idx) < 0)
        return 0;  // EAGAIN etc.
    fn(mem, len);
    q(idx);
    return 1;
}

template <typename F>
int MmapCam::grab_phys(int timeout_ms, F fn)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd(), &fds);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = select(fd() + 1, &fds, nullptr, nullptr, &tv);
    if (r <= 0)
        return r == 0 ? 0 : -1;
    void *mem;
    unsigned len, idx;
    if (dq(&mem, &len, &idx) < 0)
        return 0;
    fn(mem, len, idx < phys_.size() ? phys_[idx] : 0u);
    q(idx);
    return 1;
}
