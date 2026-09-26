#include "photos.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "spots.h"
#include "util.h"

bool valid_day(const std::string &d)
{
    if (d.size() != 10 || d[4] != '-' || d[7] != '-')
        return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!isdigit((unsigned char)d[i]))
            return false;
    return true;
}

int Photos::start(PhotoCam *cams[NUM_CAMS])
{
    for (int c = 0; c < NUM_CAMS; c++)
        cams_[c] = cams[c];
    util::mkdirs(PHOTOS_DIR);
    text_.init();
    th_ = std::thread(&Photos::loop, this);
    return 0;
}

void Photos::stop()
{
    if (th_.joinable())
        th_.join();
}

bool Photos::live_jpeg(int cam, int quality, std::vector<unsigned char> &out)
{
    if (cam < 0 || cam >= NUM_CAMS || !cams_[cam] || !cams_[cam]->running())
        return false;
    cv::Mat bgr;
    if (!cams_[cam]->snapshot(bgr))
        return false;
    return cv::imencode(".jpg", bgr, out, {cv::IMWRITE_JPEG_QUALITY, quality});
}

// Draws what the AI saw (boxes + spot polygons) onto one camera's frame.
void Photos::annotate(cv::Mat &bgra, int cam, const std::vector<Detection> &dets, const std::vector<Spot> &spots,
                      const Config &cfg)
{
    const int W = bgra.cols, H = bgra.rows;
    const int lw = std::max(2, W / 400), px = std::max(14, W / 45);
    auto P = [&](float x, float y) { return cv::Point((int)(x * W), (int)(y * H)); };
    for (auto &s : spots) {
        if (s.cam != cam)
            continue;
        std::vector<cv::Point> poly;
        cv::Point2f ctr(0, 0);
        for (auto &p : s.pts) {
            poly.push_back(P(p.x, p.y));
            ctr += cv::Point2f(poly.back().x, poly.back().y) * (1.f / s.pts.size());
        }
        cv::Scalar col = s.occupied ? cv::Scalar(40, 40, 230, 255) : cv::Scalar(60, 200, 60, 255);
        cv::Mat layer = bgra.clone();
        std::vector<std::vector<cv::Point>> polys = {poly};
        cv::fillPoly(layer, polys, col, cv::LINE_AA);
        cv::addWeighted(layer, 0.3, bgra, 0.7, 0, bgra);
        cv::polylines(bgra, polys, true, col, lw + 1, cv::LINE_AA);
        if (text_.ok())
            text_.draw_centered(bgra, s.id, cv::Rect((int)ctr.x - px * 2, (int)ctr.y - px, px * 4, px * 2), px,
                                cv::Scalar(255, 255, 255, 255));
    }
    for (auto &d : dets) {
        if (!cfg.draw_classes.empty() &&
            std::find(cfg.draw_classes.begin(), cfg.draw_classes.end(), d.name) == cfg.draw_classes.end())
            continue;
        bool veh = is_vehicle(cfg, d.name);
        cv::Scalar col = veh ? cv::Scalar(0, 220, 255, 255) : cv::Scalar(230, 230, 230, 255);
        cv::rectangle(bgra, P(d.x1, d.y1), P(d.x2, d.y2), col, veh ? lw + 1 : lw);
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%s %.2f", d.name.c_str(), d.score);
        cv::putText(bgra, lbl, P(d.x1, d.y1) + cv::Point(4, px), cv::FONT_HERSHEY_SIMPLEX, px / 30.0, col, 2,
                    cv::LINE_AA);
    }
}

static std::string dets_json(const std::vector<Detection> &dets)
{
    std::string o = "[";
    for (size_t i = 0; i < dets.size(); i++) {
        char b[256];
        snprintf(b, sizeof(b), "%s\n    {\"name\": \"%s\", \"score\": %.3f, \"box\": [%.4f, %.4f, %.4f, %.4f]}",
                 i ? "," : "", dets[i].name.c_str(), dets[i].score, dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
        o += b;
    }
    return o + "]";
}

static std::string spots_json(const std::vector<Spot> &spots, int cam)
{
    std::string o = "[";
    bool first = true;
    for (auto &s : spots) {
        if (s.cam != cam)
            continue;
        char b[160];
        snprintf(b, sizeof(b), "%s\n    {\"id\": \"%s\", \"occupied\": %s, \"coverage\": %.3f, \"points\": [",
                 first ? "" : ",", util::json_escape(s.id).c_str(), s.occupied ? "true" : "false", s.coverage);
        o += b;
        for (size_t j = 0; j < s.pts.size(); j++) {
            snprintf(b, sizeof(b), "%s[%.4f, %.4f]", j ? ", " : "", s.pts[j].x, s.pts[j].y);
            o += b;
        }
        o += "]}";
        first = false;
    }
    return o + "]";
}

// One photo = all running cameras stacked vertically (cam0 on top). With AI on,
// the stacked image shows boxes and spots; clean frames go to raw/ for training.
std::string Photos::capture(const std::string &reason)
{
    std::lock_guard<std::mutex> cap_lk(capture_mtx_);
    cv::Mat frames[NUM_CAMS];
    int n = 0;
    for (int c = 0; c < NUM_CAMS; c++) {
        if (!cams_[c] || !cams_[c]->running())
            continue;
        if (!cams_[c]->snapshot(frames[c])) {
            fprintf(stderr, "cam%d: snapshot timeout\n", c);
            continue;
        }
        n++;
    }
    if (n == 0)
        return "";

    int64_t now = util::now_ms();
    // Before NTP sync the date is meaningless; keep those apart.
    std::string day = util::clock_is_sane() ? util::time_str(now, "%Y-%m-%d") : "0000-00-00";
    std::string name = util::time_str(now, "%H%M%S");
    std::string dir = std::string(PHOTOS_DIR) + "/" + day;
    util::mkdirs(dir);
    std::string stem = name;
    for (int i = 1; util::file_size(dir + "/" + stem + ".jpg") >= 0; i++)
        stem = name + "_" + std::to_string(i);

    Config cfg;
    std::vector<Spot> spots;
    std::vector<Detection> dets[NUM_CAMS];
    bool ai[NUM_CAMS] = {false, false};
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        cfg = g_state.cfg;
        spots = g_state.spots;
        for (int c = 0; c < NUM_CAMS; c++) {
            dets[c] = g_state.cams[c].dets;
            ai[c] = g_state.cams[c].running;
        }
    }
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, cfg.jpeg_quality};

    // clean frames for labelling / training
    if (cfg.keep_raw) {
        util::mkdirs(dir + "/raw");
        for (int c = 0; c < NUM_CAMS; c++) {
            if (frames[c].empty())
                continue;
            std::vector<unsigned char> jpg;
            if (cv::imencode(".jpg", frames[c], jpg, params))
                util::write_file_atomic(dir + "/raw/" + stem + "_cam" + std::to_string(c) + ".jpg",
                                        std::string((const char *)jpg.data(), jpg.size()));
        }
    }

    // stacked, annotated image
    std::vector<cv::Mat> parts;
    std::string cams_meta;
    int offset_y = 0;
    for (int c = 0; c < NUM_CAMS; c++) {
        if (frames[c].empty())
            continue;
        cv::Mat bgra;
        cv::cvtColor(frames[c], bgra, cv::COLOR_BGR2BGRA);
        if (ai[c])
            annotate(bgra, c, dets[c], spots, cfg);
        int px = std::max(14, bgra.cols / 40);
        std::string stamp = util::time_str(now, "%Y-%m-%d %H:%M:%S") + "   CAM" + std::to_string(c);
        cv::Rect band(0, bgra.rows - px * 2, bgra.cols, px * 2);
        cv::Mat roi = bgra(band);
        roi = roi * 0.4;  // darken for readability
        if (text_.ok())
            text_.draw(bgra, stamp, cv::Point(px / 2, bgra.rows - px / 2), px, cv::Scalar(255, 255, 255, 255));
        cv::Mat bgr;
        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
        parts.push_back(bgr);

        char head[160];
        snprintf(head, sizeof(head), "%s\n  {\"cam\": %d, \"offset_y\": %d, \"width\": %d, \"height\": %d, \"ai\": %s,",
                 cams_meta.empty() ? "" : ",", c, offset_y, bgr.cols, bgr.rows, ai[c] ? "true" : "false");
        cams_meta += head;
        if (cfg.keep_raw)
            cams_meta += "\n   \"raw\": \"raw/" + stem + "_cam" + std::to_string(c) + ".jpg\",";
        cams_meta += "\n   \"detections\": " + dets_json(dets[c]) + ",\n   \"spots\": " + spots_json(spots, c) + "}";
        offset_y += bgr.rows;
    }
    cv::Mat stacked;
    cv::vconcat(parts, stacked);

    char head[256];
    snprintf(head, sizeof(head), "{\n\"time\": %lld,\n\"time_str\": \"%s\",\n\"reason\": \"%s\",\n\"cams\": [",
             (long long)now, util::time_str(now, "%Y-%m-%d %H:%M:%S").c_str(), util::json_escape(reason).c_str());
    std::string meta = std::string(head) + cams_meta + "\n]\n}\n";

    std::vector<unsigned char> jpg;
    std::string base = dir + "/" + stem;
    if (!cv::imencode(".jpg", stacked, jpg, params) ||
        !util::write_file_atomic(base + ".jpg", std::string((const char *)jpg.data(), jpg.size())) ||
        !util::write_file_atomic(base + ".json", meta)) {
        fprintf(stderr, "photos: cannot write %s\n", base.c_str());
        return "";
    }
    enforce_free_space();
    return day + "/" + stem + ".jpg";
}

void Photos::enforce_free_space()
{
    double min_free;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        min_free = g_state.cfg.min_free_pct;
    }
    auto list = days();
    // keep at least today's folder
    for (size_t i = 0; i + 1 < list.size() && util::free_space_pct(PHOTOS_DIR) < min_free; i++) {
        fprintf(stderr, "photos: low space, removing %s\n", list[i].c_str());
        util::remove_tree(std::string(PHOTOS_DIR) + "/" + list[i]);
    }
}

void Photos::loop()
{
    // Aligned to wall clock (hh:00, hh:15, ...) once the clock is synced,
    // otherwise plain intervals from start.
    int64_t next_mono = util::mono_ms() + 60 * 1000;
    int64_t last_slot = -1;
    while (!g_quit.load()) {
        sleep(1);
        int interval;
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            interval = g_state.cfg.photo_interval_min;
        }
        bool due = false;
        if (interval <= 0) {  // timer photos disabled; manual captures still work
            last_slot = -1;
            next_mono = util::mono_ms() + 60 * 1000;
            continue;
        }
        if (util::clock_is_sane()) {
            int64_t slot = util::now_ms() / 1000 / 60 / interval;
            if (last_slot < 0)
                last_slot = slot;  // don't shoot right at start-up
            if (slot != last_slot) {
                last_slot = slot;
                due = true;
            }
        } else if (util::mono_ms() >= next_mono) {
            next_mono = util::mono_ms() + (int64_t)interval * 60 * 1000;
            due = true;
        }
        if (!due)
            continue;
        std::string p = capture("timer");
        if (!p.empty())
            printf("photo: %s\n", p.c_str());
    }
}

std::vector<std::string> Photos::days()
{
    std::vector<std::string> out;
    for (auto &n : util::list_dir(PHOTOS_DIR))
        if (valid_day(n) && util::is_dir(std::string(PHOTOS_DIR) + "/" + n))
            out.push_back(n);
    return out;
}

std::vector<std::string> Photos::files(const std::string &day)
{
    std::vector<std::string> out;
    if (!valid_day(day))
        return out;
    for (auto &n : util::list_dir(std::string(PHOTOS_DIR) + "/" + day))
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".jpg") == 0)
            out.push_back(n);
    return out;
}

bool Photos::delete_day(const std::string &day)
{
    if (!valid_day(day))
        return false;
    return util::remove_tree(std::string(PHOTOS_DIR) + "/" + day);
}

// Minimal ustar writer.
static void tar_header(char *h, const std::string &name, int64_t size, int64_t mtime, bool dir)
{
    memset(h, 0, 512);
    snprintf(h, 100, "%s", name.c_str());
    snprintf(h + 100, 8, "%07o", dir ? 0755 : 0644);
    snprintf(h + 108, 8, "%07o", 0);
    snprintf(h + 116, 8, "%07o", 0);
    snprintf(h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf(h + 136, 12, "%011llo", (unsigned long long)mtime);
    h[156] = dir ? '5' : '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++)
        sum += (unsigned char)h[i];
    snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
}

void Photos::write_tar(const std::string &from, const std::string &to,
                       const std::function<bool(const char *, size_t)> &sink)
{
    char hdr[512];
    static const char zeros[1024] = {0};
    int64_t mtime = util::now_ms() / 1000;
    for (auto &day : days()) {
        if ((!from.empty() && day < from) || (!to.empty() && day > to))
            continue;
        std::string dir = std::string(PHOTOS_DIR) + "/" + day;
        tar_header(hdr, "parking_photos/" + day + "/", 0, mtime, true);
        if (!sink(hdr, 512))
            return;
        std::vector<std::string> names;
        for (auto &n : util::list_dir(dir)) {
            if (util::is_dir(dir + "/" + n)) {  // raw/
                for (auto &m : util::list_dir(dir + "/" + n))
                    names.push_back(n + "/" + m);
            } else {
                names.push_back(n);
            }
        }
        for (auto &n : names) {
            std::string path = dir + "/" + n;
            int64_t size = util::file_size(path);
            FILE *f = fopen(path.c_str(), "rb");
            if (size < 0 || !f) {
                if (f)
                    fclose(f);
                continue;
            }
            tar_header(hdr, "parking_photos/" + day + "/" + n, size, mtime, false);
            bool ok = sink(hdr, 512);
            char buf[16384];
            int64_t left = size;
            while (ok && left > 0) {
                size_t n_rd = fread(buf, 1, (size_t)std::min<int64_t>(left, sizeof(buf)), f);
                if (n_rd == 0)
                    break;
                ok = sink(buf, n_rd);
                left -= n_rd;
            }
            fclose(f);
            if (!ok)
                return;
            // file shrank under us: keep the archive consistent
            while (left > 0) {
                size_t n_z = (size_t)std::min<int64_t>(left, sizeof(zeros));
                if (!sink(zeros, n_z))
                    return;
                left -= n_z;
            }
            size_t pad = (512 - size % 512) % 512;
            if (pad && !sink(zeros, pad))
                return;
        }
    }
    sink(zeros, 1024);
}
