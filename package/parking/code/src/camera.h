#pragma once

#include <condition_variable>
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

// AI input geometry (ISP ds2): valid_w x valid_h image inside a net_len x net_len buffer.
struct AiGeom {
    int net_len = 320;
    int valid_w = 320;
    int valid_h = 240;
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

// Generic MMAP capture used for the AI (RGB planar) and photo (NV12) channels.
class MmapCam {
public:
    int open(const char *node, uint32_t fourcc, int w, int h, int nbufs);
    void close();
    int fd() const;
    // Waits for a frame and calls fn(data, bytes) while the buffer is dequeued.
    template <typename F>
    int grab(int timeout_ms, F fn);

private:
    int dq(void **mem, unsigned *len, unsigned *idx);
    int q(unsigned idx);
    v4l2_device *dev_ = nullptr;
    std::vector<void *> maps_;
    std::vector<size_t> sizes_;
};

// Keeps the ds1 stream flowing and hands out JPEG snapshots on request.
class PhotoCam {
public:
    int start(int cam, int w, int h);
    void stop();
    // Returns false on timeout; frame is BGR.
    bool snapshot(cv::Mat &bgr, int timeout_ms = 3000);
    bool running() const { return running_; }

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
