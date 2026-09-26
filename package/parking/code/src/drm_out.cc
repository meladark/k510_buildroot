// Based on package/ai/code/common/k510_drm.c and
// package/mediactl_lib/src/v4l2_drm/drm/k510_drm.c (Canaan), reduced to what
// this app needs: two NV12 video planes + one ARGB overlay in a single commit.
#include "drm_out.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_CARD "/dev/dri/card0"

static void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void *data)
{
    *(bool *)data = false;
}

int DrmOut::find_connector()
{
    drmModeRes *res = drmModeGetResources(fd_);
    if (!res) {
        fprintf(stderr, "drm: drmModeGetResources failed\n");
        return -1;
    }
    drmModeConnector *conn = nullptr;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(fd_, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
            break;
        if (conn)
            drmModeFreeConnector(conn);
        conn = nullptr;
    }
    if (!conn) {
        fprintf(stderr, "drm: no connected display\n");
        drmModeFreeResources(res);
        return -1;
    }
    conn_id_ = conn->connector_id;
    drmModeModeInfo mode = conn->modes[0];
    width_ = mode.hdisplay;
    height_ = mode.vdisplay;
    if (drmModeCreatePropertyBlob(fd_, &mode, sizeof(mode), &blob_id_)) {
        fprintf(stderr, "drm: cannot create mode blob\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return -1;
    }

    crtc_id_ = 0;
    drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(fd_, conn->encoder_id) : nullptr;
    if (enc && enc->crtc_id) {
        crtc_id_ = enc->crtc_id;
    } else {
        for (int i = 0; i < conn->count_encoders && !crtc_id_; i++) {
            drmModeEncoder *e = drmModeGetEncoder(fd_, conn->encoders[i]);
            if (!e)
                continue;
            for (int c = 0; c < res->count_crtcs; c++) {
                if (e->possible_crtcs & (1u << c)) {
                    crtc_id_ = res->crtcs[c];
                    break;
                }
            }
            drmModeFreeEncoder(e);
        }
    }
    if (enc)
        drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);

    int idx = -1;
    for (int i = 0; i < res->count_crtcs; i++)
        if (res->crtcs[i] == crtc_id_)
            idx = i;
    drmModeFreeResources(res);
    if (!crtc_id_ || idx < 0) {
        fprintf(stderr, "drm: no crtc\n");
        return -1;
    }
    crtc_idx_ = idx;
    return 0;
}

int DrmOut::find_plane(uint32_t format, uint32_t *plane_id)
{
    drmModePlaneResPtr planes = drmModeGetPlaneResources(fd_);
    if (!planes)
        return -1;
    int ret = -1;
    for (uint32_t i = 0; i < planes->count_planes && ret; i++) {
        uint32_t id = planes->planes[i];
        bool used = false;
        for (int u = 0; u < n_used_; u++)
            used |= used_planes_[u] == id;
        if (used)
            continue;
        drmModePlanePtr p = drmModeGetPlane(fd_, id);
        if (!p)
            continue;
        if (p->possible_crtcs & (1u << crtc_idx_)) {
            for (uint32_t j = 0; j < p->count_formats; j++) {
                if (p->formats[j] == format) {
                    *plane_id = id;
                    used_planes_[n_used_++] = id;
                    ret = 0;
                    break;
                }
            }
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(planes);
    return ret;
}

uint32_t DrmOut::prop_id(uint32_t obj_id, uint32_t obj_type, const char *name)
{
    uint32_t id = 0;
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd_, obj_id, obj_type);
    if (!props)
        return 0;
    for (uint32_t i = 0; i < props->count_props && !id; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd_, props->props[i]);
        if (p && strcmp(p->name, name) == 0)
            id = p->prop_id;
        if (p)
            drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    if (!id)
        fprintf(stderr, "drm: property %s not found on %u\n", name, obj_id);
    return id;
}

int DrmOut::init()
{
    // may be called again after deinit() (launcher menu)
    n_used_ = 0;
    modeset_done_ = false;
    pending_ = false;
    video_on_[0] = video_on_[1] = false;
    fd_ = open(DRM_CARD, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        fprintf(stderr, "drm: cannot open %s\n", DRM_CARD);
        return -1;
    }
    if (drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "drm: no atomic modesetting\n");
        return -1;
    }
    if (find_connector())
        return -1;
    if (find_plane(DRM_FORMAT_NV12, &video_plane_[0]) ||
        find_plane(DRM_FORMAT_NV12, &video_plane_[1]) ||
        find_plane(DRM_FORMAT_ARGB8888, &osd_plane_)) {
        fprintf(stderr, "drm: not enough planes (need 2x NV12 + ARGB)\n");
        return -1;
    }
    for (int c = 0; c < 2; c++)
        cache_props(video_plane_[c], video_props_[c]);
    cache_props(osd_plane_, osd_props_);
    conn_crtc_prop_ = prop_id(conn_id_, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    mode_prop_ = prop_id(crtc_id_, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    active_prop_ = prop_id(crtc_id_, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    for (int i = 0; i < OSD_BUFS; i++)
        if (alloc(osd_[i], width_, height_, DRM_FORMAT_ARGB8888))
            return -1;
    printf("drm: %ux%u, video planes %u/%u, osd plane %u\n", width_, height_, video_plane_[0],
           video_plane_[1], osd_plane_);
    return 0;
}

int DrmOut::alloc(DrmBuf &b, uint32_t w, uint32_t h, uint32_t format)
{
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    if (format == DRM_FORMAT_NV12) {
        creq.width = (w + 15) / 16 * 16;
        creq.height = h * 3 / 2;
        creq.bpp = 8;
    } else {
        creq.width = (w + 7) / 8 * 8;
        creq.height = h;
        creq.bpp = 32;
    }
    if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        fprintf(stderr, "drm: CREATE_DUMB %ux%u failed\n", w, h);
        return -1;
    }
    b.handle = creq.handle;
    b.pitch = creq.pitch;
    b.size = creq.size;
    b.width = w;
    b.height = h;

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
        fprintf(stderr, "drm: MAP_DUMB failed\n");
        return -1;
    }
    b.map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, mreq.offset);
    if (b.map == MAP_FAILED) {
        b.map = nullptr;
        fprintf(stderr, "drm: mmap failed\n");
        return -1;
    }
    memset(b.map, format == DRM_FORMAT_NV12 ? 0x10 : 0, creq.size);

    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    handles[0] = creq.handle;
    pitches[0] = creq.pitch;
    if (format == DRM_FORMAT_NV12) {
        handles[1] = creq.handle;
        pitches[1] = creq.pitch;
        offsets[1] = creq.pitch * h;
    }
    if (drmModeAddFB2(fd_, w, h, format, handles, pitches, offsets, &b.fb, 0)) {
        fprintf(stderr, "drm: AddFB2 failed: %s\n", strerror(errno));
        return -1;
    }
    if (format == DRM_FORMAT_NV12 && drmPrimeHandleToFD(fd_, b.handle, DRM_CLOEXEC | DRM_RDWR, &b.dmabuf_fd)) {
        // older kernels reject DRM_RDWR
        if (drmPrimeHandleToFD(fd_, b.handle, DRM_CLOEXEC, &b.dmabuf_fd)) {
            fprintf(stderr, "drm: PrimeHandleToFD failed: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}

int DrmOut::setup_video(int cam, const Rect &dst)
{
    video_dst_[cam] = dst;
    for (int i = 0; i < VIDEO_BUFS; i++)
        if (alloc(video_[cam][i], dst.w, dst.h, DRM_FORMAT_NV12))
            return -1;
    video_on_[cam] = true;
    return 0;
}

void DrmOut::cache_props(uint32_t plane, PlaneProps &p)
{
    const uint32_t T = DRM_MODE_OBJECT_PLANE;
    p.fb = prop_id(plane, T, "FB_ID");
    p.crtc = prop_id(plane, T, "CRTC_ID");
    p.sx = prop_id(plane, T, "SRC_X");
    p.sy = prop_id(plane, T, "SRC_Y");
    p.sw = prop_id(plane, T, "SRC_W");
    p.sh = prop_id(plane, T, "SRC_H");
    p.cx = prop_id(plane, T, "CRTC_X");
    p.cy = prop_id(plane, T, "CRTC_Y");
    p.cw = prop_id(plane, T, "CRTC_W");
    p.ch = prop_id(plane, T, "CRTC_H");
}

void DrmOut::add_plane(void *vreq, uint32_t plane, const PlaneProps &p, uint32_t fb,
                       const Rect &src, const Rect &dst)
{
    drmModeAtomicReq *req = (drmModeAtomicReq *)vreq;
    drmModeAtomicAddProperty(req, plane, p.fb, fb);
    drmModeAtomicAddProperty(req, plane, p.crtc, crtc_id_);
    drmModeAtomicAddProperty(req, plane, p.sx, (uint64_t)src.x << 16);
    drmModeAtomicAddProperty(req, plane, p.sy, (uint64_t)src.y << 16);
    drmModeAtomicAddProperty(req, plane, p.sw, (uint64_t)src.w << 16);
    drmModeAtomicAddProperty(req, plane, p.sh, (uint64_t)src.h << 16);
    drmModeAtomicAddProperty(req, plane, p.cx, dst.x);
    drmModeAtomicAddProperty(req, plane, p.cy, dst.y);
    drmModeAtomicAddProperty(req, plane, p.cw, dst.w);
    drmModeAtomicAddProperty(req, plane, p.ch, dst.h);
}

int DrmOut::commit(const int video_idx[2], int osd_idx)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    if (!modeset_done_) {
        drmModeAtomicAddProperty(req, conn_id_, conn_crtc_prop_, crtc_id_);
        drmModeAtomicAddProperty(req, crtc_id_, mode_prop_, blob_id_);
        drmModeAtomicAddProperty(req, crtc_id_, active_prop_, 1);
        flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
    }
    for (int c = 0; c < 2; c++) {
        if (!video_on_[c] || video_idx[c] < 0)
            continue;
        const Rect &d = video_dst_[c];
        add_plane(req, video_plane_[c], video_props_[c], video_[c][video_idx[c]].fb,
                  Rect{0, 0, d.w, d.h}, d);
    }
    if (osd_idx >= 0) {
        Rect full = {0, 0, (int)width_, (int)height_};
        add_plane(req, osd_plane_, osd_props_, osd_[osd_idx].fb, full, full);
    }
    pending_ = true;
    int ret = drmModeAtomicCommit(fd_, req, flags, &pending_);
    drmModeAtomicFree(req);
    if (ret) {
        pending_ = false;
        fprintf(stderr, "drm: commit failed: %s\n", strerror(errno));
        return -1;
    }
    modeset_done_ = true;
    return 0;
}

void DrmOut::disable_planes()
{
    if (fd_ < 0 || !modeset_done_)
        return;
    // wait for an outstanding flip first, the commit below is blocking
    for (int i = 0; i < 20 && pending_; i++) {
        struct timeval tv = {0, 50000};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        if (select(fd_ + 1, &fds, nullptr, nullptr, &tv) > 0)
            handle_event();
    }
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    const uint32_t planes[3] = {video_plane_[0], video_plane_[1], osd_plane_};
    const PlaneProps *props[3] = {&video_props_[0], &video_props_[1], &osd_props_};
    for (int i = 0; i < 3; i++) {
        drmModeAtomicAddProperty(req, planes[i], props[i]->fb, 0);
        drmModeAtomicAddProperty(req, planes[i], props[i]->crtc, 0);
    }
    if (drmModeAtomicCommit(fd_, req, 0, nullptr))
        fprintf(stderr, "drm: disabling planes failed: %s\n", strerror(errno));
    drmModeAtomicFree(req);
}

void DrmOut::handle_event()
{
    drmEventContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.version = 2;
    ctx.page_flip_handler = page_flip_handler;
    drmHandleEvent(fd_, &ctx);
}

void DrmOut::deinit()
{
    if (fd_ < 0)
        return;
    auto destroy = [&](DrmBuf &b) {
        if (b.dmabuf_fd >= 0)
            close(b.dmabuf_fd);
        if (b.fb)
            drmModeRmFB(fd_, b.fb);
        if (b.map)
            munmap(b.map, b.size);
        if (b.handle) {
            struct drm_mode_destroy_dumb d;
            memset(&d, 0, sizeof(d));
            d.handle = b.handle;
            drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        b = DrmBuf();
    };
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < VIDEO_BUFS; i++)
            destroy(video_[c][i]);
    for (int i = 0; i < OSD_BUFS; i++)
        destroy(osd_[i]);
    close(fd_);
    fd_ = -1;
}

int drm_query_resolution(uint32_t *w, uint32_t *h)
{
    int fd = open(DRM_CARD, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int ret = -1;
    drmModeRes *res = drmModeGetResources(fd);
    for (int i = 0; res && i < res->count_connectors && ret; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            *w = c->modes[0].hdisplay;
            *h = c->modes[0].vdisplay;
            ret = 0;
        }
        if (c)
            drmModeFreeConnector(c);
    }
    if (res)
        drmModeFreeResources(res);
    close(fd);
    return ret;
}
