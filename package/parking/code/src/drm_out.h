#pragma once

#include <stdint.h>
#include <stddef.h>

#define VIDEO_BUFS 4  // NV12 buffers per camera (V4L2 USERPTR targets)
#define OSD_BUFS 3    // full-screen ARGB overlay buffers

struct DrmBuf {
    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint32_t fb = 0;
    uint32_t width = 0, height = 0;
    size_t size = 0;
    void *map = nullptr;
    int dmabuf_fd = -1;  // NV12 buffers only, handed to V4L2 as DMABUF
};

struct Rect {
    int x, y, w, h;
};

// Video layers take the camera frames as-is (no scaling), the OSD layer is
// full screen ARGB8888 on top of them.
class DrmOut {
public:
    int init();
    void deinit();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    int fd() const { return fd_; }

    // Allocate NV12 buffers for camera `cam` placed at `dst` on screen.
    int setup_video(int cam, const Rect &dst);
    DrmBuf &video_buf(int cam, int i) { return video_[cam][i]; }
    DrmBuf &osd_buf(int i) { return osd_[i]; }

    // Commit video buffers (index or -1 = keep plane disabled) and OSD buffer.
    // Non-blocking; wait for completion with handle_event() when fd() is readable.
    int commit(const int video_idx[2], int osd_idx);
    void handle_event();
    bool pending() const { return pending_; }
    // Detaches all our planes (blocking commit); call before handing the
    // display to another process.
    void disable_planes();

private:
    struct PlaneProps {
        uint32_t fb, crtc, sx, sy, sw, sh, cx, cy, cw, ch;
    };

    int find_connector();
    int find_plane(uint32_t format, uint32_t *plane_id);
    uint32_t prop_id(uint32_t obj_id, uint32_t obj_type, const char *name);
    int alloc(DrmBuf &b, uint32_t w, uint32_t h, uint32_t format);
    void cache_props(uint32_t plane, PlaneProps &p);
    void add_plane(void *req, uint32_t plane, const PlaneProps &p, uint32_t fb, const Rect &src,
                   const Rect &dst);

    PlaneProps video_props_[2];
    PlaneProps osd_props_;
    uint32_t conn_crtc_prop_ = 0, mode_prop_ = 0, active_prop_ = 0;

    int fd_ = -1;
    uint32_t conn_id_ = 0, crtc_id_ = 0, crtc_idx_ = 0, blob_id_ = 0;
    uint32_t width_ = 0, height_ = 0;
    uint32_t video_plane_[2] = {0, 0};
    uint32_t osd_plane_ = 0;
    uint32_t used_planes_[16] = {0};
    int n_used_ = 0;
    bool modeset_done_ = false;
    bool pending_ = false;
    bool video_on_[2] = {false, false};
    Rect video_dst_[2];
    DrmBuf video_[2][VIDEO_BUFS];
    DrmBuf osd_[OSD_BUFS];
};

// Screen resolution without keeping the device open (for layout decisions).
int drm_query_resolution(uint32_t *w, uint32_t *h);
