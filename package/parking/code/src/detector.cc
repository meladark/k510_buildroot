#include "detector.h"

#include <linux/videodev2.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <memory>

#include "canaan/cv2_utils.h"
#include "model.h"
#include "spots.h"
#include "util.h"

static std::mutex g_kpu_mtx;  // one KPU for both cameras
static std::unique_ptr<Engine> g_kpu;

bool detector_load_model(const std::string &id, ModelInfo *used)
{
    ModelInfo m;
    if (!find_model(id, m)) {
        fprintf(stderr, "ai: model %s not found\n", id.c_str());
        return false;
    }
    std::unique_ptr<Engine> k(new Engine());
    if (!k->load(m))
        return false;  // frees its buffers
    g_kpu = std::move(k);
    if (used)
        *used = m;
    return true;
}

void detector_unload_model()
{
    g_kpu.reset();
}

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

AiGeom detector_geometry(const ModelInfo &m)
{
    AiGeom g;
    g.net_w = m.input_w;
    g.net_h = m.input_h;
    g.valid_w = m.valid_w;
    g.valid_h = m.valid_h;
    g.center = m.center;
    return g;
}

// ISP ds2 frame (valid rows first, pitch net_w) into the KPU input: shifted so
// the image lands where the model expects it, the rest filled with the
// letterbox gray (same as the stock demos).
void place_frame(uint8_t *dst, const uint8_t *frame, size_t len, const AiGeom &g)
{
    const int W = g.net_w, H = g.net_h;
    const int pad_l = g.center ? (W - g.valid_w) / 2 : 0, pad_t = g.center ? (H - g.valid_h) / 2 : 0;
    const size_t plane = (size_t)W * H, offset = pad_l + (size_t)pad_t * W;
    memcpy(dst + offset, frame, std::min(len, plane * 3 - offset));
    for (int c = 0; c < 3; c++) {
        uint8_t *p = dst + c * plane;
        memset(p, PADDING_R, (size_t)pad_t * W);
        memset(p + (size_t)(pad_t + g.valid_h) * W, PADDING_R, (size_t)(H - pad_t - g.valid_h) * W);
        if (g.valid_w < W) {
            for (int row = pad_t; row < pad_t + g.valid_h; row++) {
                memset(p + (size_t)row * W, PADDING_R, pad_l);
                memset(p + (size_t)row * W + pad_l + g.valid_w, PADDING_R, W - g.valid_w - pad_l);
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
    Engine *kpu = g_kpu.get();
    if (!kpu)
        return;

    MmapCam cap;
    if (cap.open(cam_node(cam_, 3), V4L2_PIX_FMT_RGB24, g.net_w, g.net_h, 2) < 0) {
        fprintf(stderr, "cam%d ai: cannot open %s\n", cam_, cam_node(cam_, 3));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.cams[cam_].running = true;
    }

    const size_t frame_bytes = (size_t)g.net_w * g.net_h * 3;
    std::vector<uint8_t> frame(frame_bytes);
    size_t frame_len = 0;

    int64_t fps_t0 = util::mono_ms();
    int fps_frames = 0;
    int timeouts = 0;
    int64_t next_run = 0;
    float infer_avg = 0, post_avg = 0;
    std::vector<DetObj> boxes;
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
            frame_len = std::min<size_t>(len, frame_bytes);
            memcpy(frame.data(), mem, frame_len);
        });
        if (r <= 0) {
            if (r == 0 && ++timeouts % 5 == 0)
                fprintf(stderr, "cam%d ai: no frames\n", cam_);
            continue;
        }

        float infer_ms = 0, post_ms = 0;
        bool ok;
        {
            // one model, one KPU: the cameras take turns
            std::lock_guard<std::mutex> lk(g_kpu_mtx);
            place_frame(kpu->input(), frame.data(), frame_len, g);
            ok = kpu->process(cfg.obj_thresh, cfg.nms_thresh, boxes, &infer_ms, &post_ms);
        }
        if (!ok) {
            fprintf(stderr, "cam%d ai: inference failed\n", cam_);
            usleep(500000);
            continue;
        }
        infer_avg = infer_avg ? infer_avg * 0.8f + infer_ms * 0.2f : infer_ms;
        post_avg = post_avg ? post_avg * 0.8f + post_ms * 0.2f : post_ms;

        std::vector<Detection> dets;
        dets.reserve(boxes.size());
        for (auto &b : boxes) {
            Detection d;
            d.label = b.label;
            d.name = b.name;
            d.score = b.score;
            d.x1 = b.x1;
            d.y1 = b.y1;
            d.x2 = b.x2;
            d.y2 = b.y2;
            d.text = b.text;
            d.pts = b.pts;
            d.shape = b.shape;
            dets.push_back(d);
        }

        fps_frames++;
        int64_t now = util::mono_ms();
        std::lock_guard<std::mutex> lk(g_state.mtx);
        CamState &cs = g_state.cams[cam_];
        cs.frames++;
        cs.ts_ms = util::now_ms();
        cs.infer_ms = infer_avg;
        cs.post_ms = post_avg;
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
