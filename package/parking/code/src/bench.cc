// parking --bench [--image photo.jpg] [--runs N] [model ids...]
//
// Runs every installed model (or the listed ones) on the same photo, prepared
// the way the ISP feeds the KPU, and reports KPU time, decode time and what was
// found. Annotated copies go to /root/data/parking/bench/<id>.jpg, the table to
// /root/data/parking/bench.json (shown in the web UI). Stop the app first: the
// KPU and memory are needed for one model at a time.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "detector.h"
#include "model.h"
#include "state.h"
#include "util.h"

#define BENCH_DIR DATA_DIR "/bench"
#define BENCH_JSON DATA_DIR "/bench.json"

static std::string newest_photo()
{
    std::string best;
    for (auto &day : util::list_dir(PHOTOS_DIR)) {
        std::string raw = std::string(PHOTOS_DIR) + "/" + day + "/raw";
        for (auto &f : util::list_dir(raw))
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".jpg") == 0 && (day + "/" + f) > best)
                best = day + "/raw/" + f;
    }
    return best.empty() ? "" : std::string(PHOTOS_DIR) + "/" + best;
}

// Same as ISP ds2: the photo scaled to valid_w x valid_h, planar RGB with the
// row pitch of the input, then placed like a camera frame.
static void fill_input(const cv::Mat &bgr, const AiGeom &g, uint8_t *dst)
{
    cv::Mat small, rgb;
    cv::resize(bgr, small, cv::Size(g.valid_w, g.valid_h), 0, 0, cv::INTER_AREA);
    cv::cvtColor(small, rgb, cv::COLOR_BGR2RGB);
    const size_t plane = (size_t)g.net_w * g.net_h;
    std::vector<uint8_t> frame(plane * 3, 114);
    for (int y = 0; y < g.valid_h; y++) {
        const uint8_t *s = rgb.ptr(y);
        for (int x = 0; x < g.valid_w; x++)
            for (int c = 0; c < 3; c++)
                frame[c * plane + (size_t)y * g.net_w + x] = s[x * 3 + c];
    }
    place_frame(dst, frame.data(), frame.size(), g);
}

int run_bench(int argc, char **argv)
{
    std::string image;
    int runs = 10;
    std::vector<std::string> ids;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--image") && i + 1 < argc)
            image = argv[++i];
        else if (!strcmp(argv[i], "--runs") && i + 1 < argc)
            runs = std::max(1, atoi(argv[++i]));
        else
            ids.push_back(argv[i]);
    }
    if (image.empty())
        image = newest_photo();
    cv::Mat img = cv::imread(image);
    if (img.empty()) {
        fprintf(stderr, "bench: cannot read image '%s' (use --image)\n", image.c_str());
        return 1;
    }
    util::mkdirs(BENCH_DIR);

    std::vector<ModelInfo> models;
    for (auto &m : list_models())
        if (ids.empty() || std::find(ids.begin(), ids.end(), m.id) != ids.end())
            models.push_back(m);
    printf("bench: %zu models on %s (%dx%d), %d runs each\n\n", models.size(), image.c_str(), img.cols, img.rows,
           runs);
    printf("%-22s %7s %8s %8s %6s  %s\n", "model", "size MB", "KPU ms", "CPU ms", "max fps", "found (score > 0.4)");

    std::string json = "{\"image\":\"" + util::json_escape(image) + "\",\"ts\":" + std::to_string(util::now_ms()) +
                       ",\"models\":[";
    bool first = true;
    for (auto &m : models) {
        float infer = 0, post = 0;
        std::vector<DetObj> boxes;
        bool ok = false;
        {
            Engine k;
            if (k.load(m)) {
                AiGeom g = detector_geometry(m);
                fill_input(img, g, k.input());
                // the input stays as is between runs: the KPU only reads it
                ok = k.process(0.4f, 0.45f, boxes, nullptr, nullptr);  // warm-up
                for (int r = 0; ok && r < runs; r++) {
                    float a, b;
                    ok = k.process(0.4f, 0.45f, boxes, &a, &b);
                    infer += a / runs;
                    post += b / runs;
                }
            }
        }
        if (!ok) {
            printf("%-22s  FAILED\n", m.id.c_str());
            continue;
        }
        std::map<std::string, int> counts;
        cv::Mat vis = img.clone();
        std::string dets;
        for (auto &b : boxes) {
            const std::string &name = b.name;
            counts[name]++;
            cv::Rect r(cv::Point(b.x1 * img.cols, b.y1 * img.rows), cv::Point(b.x2 * img.cols, b.y2 * img.rows));
            cv::rectangle(vis, r, cv::Scalar(0, 220, 255), 3);
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "%s %.2f", name.c_str(), b.score);
            cv::putText(vis, lbl, r.tl() + cv::Point(4, 24), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 220, 255), 2);
            if (!b.text.empty())
                cv::putText(vis, b.text, cv::Point(r.x + 4, r.br().y + 26), cv::FONT_HERSHEY_SIMPLEX, 0.8,
                            cv::Scalar(255, 255, 255), 2);
            if (!b.pts.empty())
                draw_points(vis, cv::Rect(0, 0, img.cols, img.rows), b.pts, b.shape, cv::Scalar(0, 220, 255), 2);
            char d[256];
            snprintf(d, sizeof(d), "%s{\"name\":\"%s\",\"text\":\"%s\",\"score\":%.3f,\"box\":[%.4f,%.4f,%.4f,%.4f]}",
                     dets.empty() ? "" : ",", util::json_escape(name).c_str(), util::json_escape(b.text).c_str(),
                     b.score, b.x1, b.y1, b.x2, b.y2);
            dets += d;
        }
        std::string title = m.title();
        // the id, not the title: Hershey fonts have no Cyrillic
        cv::putText(vis, m.id + "  KPU " + std::to_string((int)infer) + " ms", cv::Point(16, 40),
                    cv::FONT_HERSHEY_SIMPLEX, 1.1, cv::Scalar(255, 255, 255), 3);
        cv::imwrite(std::string(BENCH_DIR) + "/" + m.id + ".jpg", vis, {cv::IMWRITE_JPEG_QUALITY, 85});

        std::string found;
        for (auto &kv : counts)
            found += (found.empty() ? "" : ", ") + kv.first + (kv.second > 1 ? " x" + std::to_string(kv.second) : "");
        for (auto &b : boxes)
            if (!b.text.empty())
                found += " [" + b.text + "]";
        printf("%-22s %7.1f %8.1f %8.1f %6.1f  %s\n", m.id.c_str(), m.file_size / 1e6, infer, post,
               1000.f / (infer + post), found.empty() ? "-" : found.c_str());
        char row[512];
        snprintf(row, sizeof(row),
                 "%s{\"id\":\"%s\",\"title\":\"%s\",\"size\":%ld,\"infer_ms\":%.1f,\"post_ms\":%.1f,\"dets\":[",
                 first ? "" : ",", util::json_escape(m.id).c_str(), util::json_escape(title).c_str(), m.file_size,
                 infer, post);
        json += row + dets + "]}";
        first = false;
    }
    json += "]}\n";
    util::write_file_atomic(BENCH_JSON, json);
    printf("\nbench: pictures in %s, table in %s\n", BENCH_DIR, BENCH_JSON);
    return 0;
}
