#include "display.h"

#include <algorithm>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <opencv2/imgproc.hpp>

#include "spots.h"
#include "util.h"

// ---------------------------------------------------------------------------
// OsdQueue

int OsdQueue::acquire()
{
    std::lock_guard<std::mutex> lk(m_);
    int pick = -1;
    for (int i = 0; i < OSD_BUFS; i++) {
        if (i == pending_ || i == onscreen_)
            continue;
        if (pick < 0 || pick == ready_)
            pick = i;
    }
    if (pick == ready_)
        ready_ = -1;  // painter takes an unconsumed buffer back
    return pick;
}

void OsdQueue::publish(int idx)
{
    std::lock_guard<std::mutex> lk(m_);
    ready_ = idx;
}

int OsdQueue::take_for_commit()
{
    std::lock_guard<std::mutex> lk(m_);
    if (ready_ < 0)
        return -1;
    pending_ = ready_;
    ready_ = -1;
    return pending_;
}

void OsdQueue::commit_failed()
{
    std::lock_guard<std::mutex> lk(m_);
    if (ready_ < 0)
        ready_ = pending_;
    pending_ = -1;
}

void OsdQueue::on_flip()
{
    std::lock_guard<std::mutex> lk(m_);
    if (pending_ >= 0) {
        onscreen_ = pending_;
        pending_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Display

int Display::start(const CamLayout layout[NUM_CAMS])
{
    if (drm_.init())
        return -1;
    for (int c = 0; c < NUM_CAMS; c++) {
        layout_[c] = layout[c];
        if (!layout[c].enabled)
            continue;
        if (drm_.setup_video(c, layout[c].dst))
            return -1;
        if (cams_[c].open(c, drm_, layout[c].dst)) {
            fprintf(stderr, "cam%d: display channel failed, camera disabled\n", c);
            layout_[c].enabled = false;
        }
    }

    const int W = drm_.width(), H = drm_.height();
    const int bar = H >= 1080 ? 48 : 36;
    regions_.push_back(Rect{0, 0, W, bar});
    regions_.push_back(Rect{0, H - bar, W, bar});
    for (int c = 0; c < NUM_CAMS; c++)
        if (layout_[c].enabled)
            regions_.push_back(layout_[c].dst);

    text_.init();
    // centered band for toasts ("Снимок сохранён")
    regions_.push_back(Rect{0, H / 2 - bar * 2, W, bar * 4});

    video_th_ = std::thread(&Display::video_loop, this);
    osd_th_ = std::thread(&Display::osd_loop, this);
    return 0;
}

void Display::stop()
{
    if (video_th_.joinable())
        video_th_.join();
    if (osd_th_.joinable())
        osd_th_.join();
    for (int c = 0; c < NUM_CAMS; c++)
        cams_[c].close();
    drm_.deinit();
}

void Display::video_loop()
{
    int shown[NUM_CAMS] = {-1, -1};  // on screen
    int next[NUM_CAMS] = {-1, -1};   // newest frame, not committed yet
    int prev[NUM_CAMS] = {-1, -1};   // replaced by the pending commit, still scanned out
    int silent_ms = 0;

    while (!g_quit.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = drm_.fd();
        FD_SET(drm_.fd(), &fds);
        for (int c = 0; c < NUM_CAMS; c++) {
            if (!layout_[c].enabled)
                continue;
            FD_SET(cams_[c].fd(), &fds);
            maxfd = std::max(maxfd, cams_[c].fd());
        }
        struct timeval tv = {0, 200000};
        int r = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            perror("display select");
            break;
        }
        if (r == 0) {
            silent_ms += 200;
            if (silent_ms >= 5000) {
                fprintf(stderr, "display: no camera frames for 5 s\n");
                silent_ms = 0;
            }
        } else {
            silent_ms = 0;
        }

        if (FD_ISSET(drm_.fd(), &fds)) {
            drm_.handle_event();
            if (!drm_.pending()) {
                for (int c = 0; c < NUM_CAMS; c++) {
                    cams_[c].queue(prev[c]);
                    prev[c] = -1;
                }
                osdq_.on_flip();
            }
        }
        for (int c = 0; c < NUM_CAMS; c++) {
            if (!layout_[c].enabled || !FD_ISSET(cams_[c].fd(), &fds))
                continue;
            int idx = cams_[c].dequeue();
            if (idx < 0)
                continue;
            if (next[c] >= 0)
                cams_[c].queue(next[c]);  // drop the frame we did not get to show
            next[c] = idx;
        }

        if (drm_.pending())
            continue;
        bool video_new = false;
        int vid[2] = {-1, -1};
        for (int c = 0; c < NUM_CAMS; c++) {
            vid[c] = next[c] >= 0 ? next[c] : shown[c];
            video_new |= next[c] >= 0;
        }
        int osd = osdq_.take_for_commit();
        if (!video_new && osd < 0)
            continue;
        if (drm_.commit(vid, osd) == 0) {
            for (int c = 0; c < NUM_CAMS; c++) {
                if (next[c] >= 0) {
                    prev[c] = shown[c];
                    shown[c] = next[c];
                    next[c] = -1;
                }
            }
        } else {
            if (osd >= 0)
                osdq_.commit_failed();
            usleep(10000);
        }
    }
}

void Display::osd_loop()
{
    const int W = drm_.width(), H = drm_.height();
    cv::Mat canvas(H, W, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    while (!g_quit.load()) {
        int64_t t0 = util::mono_ms();
        for (auto &r : regions_)
            canvas(cv::Rect(r.x, r.y, r.w, r.h)).setTo(cv::Scalar(0, 0, 0, 0));
        paint(canvas);

        int idx = osdq_.acquire();
        if (idx >= 0) {
            DrmBuf &b = drm_.osd_buf(idx);
            for (auto &r : regions_) {
                for (int y = r.y; y < r.y + r.h; y++)
                    memcpy((uint8_t *)b.map + (size_t)y * b.pitch + r.x * 4, canvas.ptr(y) + r.x * 4,
                           (size_t)r.w * 4);
            }
            osdq_.publish(idx);
        }
        int64_t spent = util::mono_ms() - t0;
        if (spent < 250)
            usleep((250 - spent) * 1000);
    }
}

static void text_with_bg(cv::Mat &img, const std::string &s, cv::Point org, double scale,
                         const cv::Scalar &color, int thickness = 2)
{
    int base = 0;
    cv::Size sz = cv::getTextSize(s, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &base);
    cv::Rect bg(org.x - 4, org.y - sz.height - 6, sz.width + 8, sz.height + base + 10);
    bg &= cv::Rect(0, 0, img.cols, img.rows);
    img(bg).setTo(cv::Scalar(0, 0, 0, 160));
    cv::putText(img, s, org, cv::FONT_HERSHEY_SIMPLEX, scale, color, thickness, cv::LINE_AA);
}

void Display::paint(cv::Mat &canvas)
{
    const cv::Scalar kFree(60, 200, 60, 255), kBusy(40, 40, 230, 255);
    const cv::Scalar kVehicle(0, 220, 255, 255), kOther(230, 230, 230, 255);
    const cv::Scalar kWhite(255, 255, 255, 255), kWarn(0, 200, 255, 255);
    const int W = canvas.cols, H = canvas.rows;
    const double fs = H >= 1080 ? 0.9 : 0.7;
    const int bar = H >= 1080 ? 48 : 36;

    static int64_t ip_t = 0;
    static std::string ip_line, sys_status;
    if (util::mono_ms() - ip_t > 2000) {
        ip_t = util::mono_ms();
        // system hints from the launcher (Wi-Fi setup AP etc.)
        sys_status.clear();
        util::read_file(SYS_STATUS_PATH, sys_status);
        while (!sys_status.empty() && (sys_status.back() == '\n' || sys_status.back() == '\r'))
            sys_status.pop_back();
        std::string e = util::iface_ip("eth0"), w = util::iface_ip("wlan0");
        ip_line.clear();
        if (!e.empty())
            ip_line += "eth " + e;
        if (!w.empty())
            ip_line += (ip_line.empty() ? "" : "  ") + std::string("wifi ") + w;
        if (ip_line.empty())
            ip_line = "no network";
    }

    std::lock_guard<std::mutex> lk(g_state.mtx);
    const Config &cfg = g_state.cfg;
    int total = 0, free_n = 0;
    for (int c = 0; c < NUM_CAMS; c++) {
        if (!layout_[c].enabled)
            continue;
        const Rect &r = layout_[c].dst;
        auto P = [&](float x, float y) { return cv::Point(r.x + (int)(x * r.w), r.y + (int)(y * r.h)); };

        for (auto &s : g_state.spots) {
            if (s.cam != c)
                continue;
            total++;
            free_n += !s.occupied;
            std::vector<cv::Point> poly;
            cv::Point2f ctr(0, 0);
            for (auto &p : s.pts) {
                poly.push_back(P(p.x, p.y));
                ctr += cv::Point2f(poly.back().x, poly.back().y) * (1.f / s.pts.size());
            }
            cv::Scalar col = s.occupied ? kBusy : kFree;
            cv::Scalar fill = col;
            fill[3] = 70;
            std::vector<std::vector<cv::Point>> polys = {poly};
            cv::fillPoly(canvas, polys, fill, cv::LINE_AA);
            cv::polylines(canvas, polys, true, col, 3, cv::LINE_AA);
            cv::putText(canvas, s.id, cv::Point((int)ctr.x - 12, (int)ctr.y + 8), cv::FONT_HERSHEY_SIMPLEX,
                        fs, kWhite, 2, cv::LINE_AA);
        }

        for (auto &d : g_state.cams[c].dets) {
            bool veh = is_vehicle(cfg, d.name);
            if (!cfg.draw_classes.empty() &&
                std::find(cfg.draw_classes.begin(), cfg.draw_classes.end(), d.name) == cfg.draw_classes.end())
                continue;
            cv::Scalar col = veh ? kVehicle : kOther;
            cv::rectangle(canvas, P(d.x1, d.y1), P(d.x2, d.y2), col, veh ? 3 : 2);
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "%s %.2f", d.name.c_str(), d.score);
            cv::Point o = P(d.x1, d.y1) + cv::Point(4, 22);
            cv::putText(canvas, lbl, o, cv::FONT_HERSHEY_SIMPLEX, fs * 0.7, col, 2, cv::LINE_AA);
        }

        char info[64];
        snprintf(info, sizeof(info), "CAM%d  %.1f fps", c, g_state.cams[c].fps);
        cv::putText(canvas, info, cv::Point(r.x + 8, r.y + r.h - 12), cv::FONT_HERSHEY_SIMPLEX, fs * 0.7,
                    kWhite, 2, cv::LINE_AA);
    }

    std::string top = util::time_str(util::now_ms(), "%Y-%m-%d %H:%M");
    if (total > 0)
        top += "   FREE " + std::to_string(free_n) + " / " + std::to_string(total);
    top += "   " + ip_line;
    text_with_bg(canvas, top, cv::Point(12, bar - 14), fs, kWhite);

    if (!g_state.toast.empty() && util::mono_ms() < g_state.toast_until_ms && text_.ok()) {
        int px = bar;
        cv::Size sz = text_.measure(g_state.toast, px);
        cv::Rect box((W - sz.width) / 2 - px, H / 2 - px, sz.width + 2 * px, px * 2);
        box &= cv::Rect(0, 0, W, H);
        canvas(box).setTo(cv::Scalar(40, 40, 40, 210));
        text_.draw_centered(canvas, g_state.toast, box, px, kWhite);
    }

    std::string bottom = g_state.status_msg;
    if (!sys_status.empty())
        bottom += (bottom.empty() ? "" : "   ") + sys_status;
    if (!util::clock_is_sane())
        bottom += (bottom.empty() ? "" : "   ") + std::string("clock not synced");
    if (!bottom.empty())
        text_with_bg(canvas, bottom, cv::Point(12, H - 14), fs * 0.9, kWarn);
    (void)W;
}
