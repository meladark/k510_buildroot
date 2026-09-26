#include "detector.h"

#include <linux/videodev2.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include "canaan/object_detect.h"
#include "spots.h"
#include "util.h"

static std::mutex g_kpu_mtx;  // one KPU for both cameras

int Detector::start(int cam, const AiGeom &geom)
{
    cam_ = cam;
    geom_ = geom;
    th_ = std::thread(&Detector::loop, this);
    return 0;
}

void Detector::stop()
{
    if (th_.joinable())
        th_.join();
}

// Pads the area around the valid image with the YOLO letterbox gray, per plane
// (same as the stock object_detect demo).
static void pad_planes(uint8_t *base, const AiGeom &g)
{
    const int n = g.net_len;
    const int plane = n * n;
    for (int c = 0; c < 3; c++) {
        uint8_t *p = base + c * plane;
        int pad_t = (n - g.valid_h) / 2;
        int pad_b = n - g.valid_h - pad_t;
        memset(p, PADDING_R, pad_t * n);
        memset(p + (pad_t + g.valid_h) * n, PADDING_R, pad_b * n);
        if (g.valid_w < n) {
            int pad_l = (n - g.valid_w) / 2;
            int pad_r = n - g.valid_w - pad_l;
            for (int row = pad_t; row < pad_t + g.valid_h; row++) {
                memset(p + row * n, PADDING_R, pad_l);
                memset(p + row * n + pad_l + g.valid_w, PADDING_R, pad_r);
            }
        }
    }
}

void Detector::loop()
{
    Config cfg;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        cfg = g_state.cfg;
    }
    const AiGeom g = geom_;
    objectDetect od(cfg.obj_thresh, cfg.nms_thresh, g.net_len, {g.valid_w, g.valid_h});
    od.load_model((char *)cfg.model.c_str());
    od.prepare_memory();

    MmapCam cap;
    if (cap.open(cam_node(cam_, 3), V4L2_PIX_FMT_RGB24, g.net_len, g.net_len, 2) < 0) {
        fprintf(stderr, "cam%d ai: cannot open %s\n", cam_, cam_node(cam_, 3));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.cams[cam_].running = true;
    }

    uint8_t *in = (uint8_t *)od.virtual_addr_input[0];
    const size_t in_cap = od.allocAlignMemOdInput[0].size;
    // ISP writes each plane as net_len x net_len with the valid rows first; shift so
    // the valid rows land centered, like the demo does.
    const size_t offset = (g.net_len - g.valid_w) / 2 + (g.net_len - g.valid_h) / 2 * g.net_len;
    const size_t frame_bytes = (size_t)g.net_len * g.net_len * 3;

    int64_t fps_t0 = util::mono_ms();
    int fps_frames = 0;
    int timeouts = 0;
    int64_t next_run = 0;
    while (!g_quit.load()) {
        // Pace the KPU: less load means less heat, power and coil whine, and a
        // parking lot does not change faster than a couple of times a second.
        float ai_fps;
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            ai_fps = g_state.cfg.ai_fps;
        }
        if (ai_fps > 0) {
            int64_t wait = next_run - util::mono_ms();
            if (wait > 0) {
                usleep(std::min<int64_t>(wait, 200) * 1000);
                // keep the ISP queue drained so the next frame is fresh
                cap.grab(0, [](void *, unsigned) {});
                continue;
            }
            next_run = util::mono_ms() + (int64_t)(1000.f / ai_fps);
        }
        int r = cap.grab(2000, [&](void *mem, unsigned len) {
            size_t n = std::min<size_t>(std::min<size_t>(len, frame_bytes), in_cap - offset);
            memcpy(in + offset, mem, n);
        });
        if (r <= 0) {
            if (r == 0 && ++timeouts % 5 == 0)
                fprintf(stderr, "cam%d ai: no frames\n", cam_);
            continue;
        }
        pad_planes(in, g);

        {
            std::lock_guard<std::mutex> lk(g_kpu_mtx);
            od.set_input(0);
            od.set_output();
            od.run();
            od.get_output();
        }
        std::vector<BoxInfo> boxes;
        od.post_process(boxes);

        std::vector<Detection> dets;
        dets.reserve(boxes.size());
        for (auto &b : boxes) {
            Detection d;
            d.label = b.label;
            d.name = (b.label >= 0 && b.label < (int)od.labels.size()) ? od.labels[b.label] : "?";
            d.score = b.score;
            d.x1 = std::max(0.f, std::min(1.f, b.x1 / g.valid_w));
            d.x2 = std::max(0.f, std::min(1.f, b.x2 / g.valid_w));
            d.y1 = std::max(0.f, std::min(1.f, b.y1 / g.valid_h));
            d.y2 = std::max(0.f, std::min(1.f, b.y2 / g.valid_h));
            dets.push_back(d);
        }

        fps_frames++;
        int64_t now = util::mono_ms();
        std::lock_guard<std::mutex> lk(g_state.mtx);
        CamState &cs = g_state.cams[cam_];
        cs.frames++;
        cs.ts_ms = util::now_ms();
        if (now - fps_t0 >= 2000) {
            cs.fps = fps_frames * 1000.f / (now - fps_t0);
            fps_t0 = now;
            fps_frames = 0;
        }
        evaluate_spots(cam_, dets, g_state.cfg, g_state.spots, g_state.spots_version, cs.ts_ms);
        cs.dets.swap(dets);
    }
    cap.close();
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.cams[cam_].running = false;
}
