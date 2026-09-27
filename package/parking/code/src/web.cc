#include "web.h"

#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include "../third_party/httplib.h"

#include <rapidjson/document.h>

#include "model.h"
#include "photos.h"
#include "video.h"
#include "webhook.h"
#include "state.h"
#include "util.h"
#include "wifi.h"

extern std::atomic<bool> g_restart;

using namespace httplib;

static void json_reply(Response &res, const std::string &body, int status = 200)
{
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body, "application/json; charset=utf-8");
}

static void json_error(Response &res, const std::string &msg, int status = 400)
{
    json_reply(res, "{\"error\":\"" + util::json_escape(msg) + "\"}", status);
}

static std::string base64(const unsigned char *p, size_t n)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += t[v >> 18 & 63];
        o += t[v >> 12 & 63];
        o += i + 1 < n ? t[v >> 6 & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

// /api/v1/* for bots: "Authorization: Bearer <api_token>" or "X-Api-Token", when a token is set.
static bool api_authorized(const Request &req, Response &res)
{
    std::string want;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        want = g_state.cfg.api_token;
    }
    if (want.empty())
        return true;
    std::string auth = req.get_header_value("Authorization"), tok = req.get_header_value("X-Api-Token");
    if (auth.compare(0, 7, "Bearer ") == 0)
        tok = auth.substr(7);
    // constant time compare
    unsigned diff = tok.size() ^ want.size();
    for (size_t i = 0; i < want.size(); i++)
        diff |= (unsigned char)want[i] ^ (unsigned char)(i < tok.size() ? tok[i] : 0);
    if (diff == 0)
        return true;
    json_reply(res, "{\"ok\":false,\"error\":\"unauthorized\"}", 401);
    return false;
}

static std::string status_json()
{
    std::string ip_eth = util::iface_ip("eth0"), ip_wlan = util::iface_ip("wlan0");
    double disk = util::free_space_pct(DATA_DIR);
    std::string sys;
    util::read_file(SYS_STATUS_PATH, sys);
    while (!sys.empty() && (sys.back() == '\n' || sys.back() == '\r'))
        sys.pop_back();
    std::lock_guard<std::mutex> lk(g_state.mtx);
    std::string msg = g_state.status_msg;
    if (!sys.empty())
        msg += (msg.empty() ? "" : " · ") + sys;
    char head[1536];
    snprintf(head, sizeof(head),
             "{\"time\":%lld,\"time_str\":\"%s\",\"clock_ok\":%s,\"eth0\":\"%s\",\"wlan0\":\"%s\","
             "\"disk_free_pct\":%.1f,\"message\":\"%s\",\"model\":\"%s\",\"model_title\":\"%s\","
             "\"recording_s\":%d,\"mode\":\"%s\",\"trigger_last\":\"%s\",\"trigger_count\":%d,\"cams\":[",
             (long long)util::now_ms(), util::time_str(util::now_ms(), "%Y-%m-%d %H:%M:%S").c_str(),
             util::clock_is_sane() ? "true" : "false", ip_eth.c_str(), ip_wlan.c_str(), disk,
             util::json_escape(msg).c_str(), util::json_escape(g_state.model_id).c_str(),
             util::json_escape(g_state.model_title).c_str(),
             g_state.rec_started_ms ? (int)((util::mono_ms() - g_state.rec_started_ms) / 1000) : -1,
             g_state.trigger_mode ? "trigger" : "parking", util::json_escape(g_state.trigger_last).c_str(),
             g_state.trigger_count);
    std::string o = head;
    for (int c = 0; c < NUM_CAMS; c++) {
        const CamState &cs = g_state.cams[c];
        char b[256];
        snprintf(b, sizeof(b),
                 "%s{\"enabled\":%s,\"running\":%s,\"fps\":%.1f,\"frames\":%llu,\"infer_ms\":%.1f,"
                 "\"post_ms\":%.1f,\"dets\":[",
                 c ? "," : "", g_state.cfg.cam_enabled[c] ? "true" : "false", cs.running ? "true" : "false",
                 cs.fps, (unsigned long long)cs.frames, cs.infer_ms, cs.post_ms);
        o += b;
        for (size_t i = 0; i < cs.dets.size(); i++) {
            const Detection &d = cs.dets[i];
            snprintf(b, sizeof(b), "%s{\"name\":\"%s\",\"score\":%.2f,\"box\":[%.4f,%.4f,%.4f,%.4f],\"shape\":%d",
                     i ? "," : "", util::json_escape(d.name).c_str(), d.score, d.x1, d.y1, d.x2, d.y2, d.shape);
            o += b;
            if (!d.text.empty())
                o += ",\"text\":\"" + util::json_escape(d.text) + "\"";
            if (!d.pts.empty()) {
                o += ",\"pts\":[";
                for (size_t k = 0; k < d.pts.size(); k++) {
                    snprintf(b, sizeof(b), "%s%.3f", k ? "," : "", d.pts[k]);
                    o += b;
                }
                o += "]";
            }
            o += "}";
        }
        o += "]}";
    }
    o += "],\"spots\":";
    std::string spots = g_state.spots_to_json(true);
    // spots_to_json returns {"spots":[...]}; embed just the array
    size_t a = spots.find('['), z = spots.rfind(']');
    o += spots.substr(a, z - a + 1);
    return o + "}";
}

static bool parse_body(const Request &req, rapidjson::Document &d)
{
    d.Parse(req.body.c_str());
    return !d.HasParseError() && d.IsObject();
}

int WebServer::start(int port, const char *www_dir, Photos *photos, VideoRecorder *video)
{
    Server *svr = new Server();
    svr_ = svr;

    svr->set_mount_point("/", www_dir);
    svr->set_mount_point("/photos", PHOTOS_DIR);
    svr->set_file_extension_and_mimetype_mapping("json", "application/json");

    svr->Get("/api/status", [](const Request &, Response &res) { json_reply(res, status_json()); });

    svr->Get(R"(/api/cam/(\d)/snapshot\.jpg)", [photos](const Request &req, Response &res) {
        int cam = std::stoi(req.matches[1].str());
        std::vector<unsigned char> jpg;
        if (!photos->live_jpeg(cam, 85, jpg)) {
            res.status = 503;
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::string((const char *)jpg.data(), jpg.size()), "image/jpeg");
    });

    svr->Get("/api/spots", [](const Request &, Response &res) {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        json_reply(res, g_state.spots_to_json(false));
    });

    svr->Put("/api/spots", [](const Request &req, Response &res) {
        std::string err;
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (!g_state.spots_from_json(req.body, &err))
            return json_error(res, err);
        if (!g_state.save_spots(SPOTS_PATH))
            return json_error(res, "не удалось сохранить на SD", 500);
        json_reply(res, "{\"ok\":true,\"count\":" + std::to_string(g_state.spots.size()) + "}");
    });

    // installed models, the configured one and the last `parking --bench` table
    svr->Get("/api/models", [](const Request &, Response &res) {
        std::string cur, running;
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            cur = g_state.cfg.model;
            running = g_state.model_id;
        }
        ModelInfo cm;
        if (find_model(cur, cm))
            cur = cm.id;  // old configs store the kmodel path
        std::string o = "{\"configured\":\"" + util::json_escape(cur) + "\",\"running\":\"" +
                        util::json_escape(running) + "\",\"models\":[";
        bool first = true;
        for (auto &m : list_models()) {
            char b[512];
            snprintf(b, sizeof(b),
                     "%s{\"id\":\"%s\",\"title\":\"%s\",\"family\":\"%s\",\"quant\":\"%s\",\"input\":%d,"
                     "\"size\":%ld,\"coco\":%s,",
                     first ? "" : ",", util::json_escape(m.id).c_str(), util::json_escape(m.title()).c_str(),
                     m.family.c_str(), m.quant.c_str(), m.input_w, m.file_size, m.coco ? "true" : "false");
            o += b;
            o += "\"what\":\"" + util::json_escape(m.what) + "\",\"note\":\"" + util::json_escape(m.note) + "\"}";
            first = false;
        }
        std::string bench;
        if (!util::read_file(DATA_DIR "/bench.json", bench) || bench.empty())
            bench = "null";
        json_reply(res, o + "],\"bench\":" + bench + "}");
    });
    util::mkdirs(DATA_DIR "/bench");
    svr->set_mount_point("/bench", DATA_DIR "/bench");

    svr->Get("/api/config", [](const Request &, Response &res) {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        json_reply(res, g_state.cfg.to_json());
    });

    svr->Put("/api/config", [](const Request &req, Response &res) {
        std::string err;
        std::lock_guard<std::mutex> lk(g_state.mtx);
        Config c = g_state.cfg;
        if (!c.from_json(req.body, &err))
            return json_error(res, err);
        // these are read once at start-up
        bool restart = c.model != g_state.cfg.model || c.net_len != g_state.cfg.net_len ||
                       c.trigger_ai_cam != g_state.cfg.trigger_ai_cam ||
                       c.obj_thresh != g_state.cfg.obj_thresh || c.nms_thresh != g_state.cfg.nms_thresh ||
                       c.cam_enabled[0] != g_state.cfg.cam_enabled[0] ||
                       c.cam_enabled[1] != g_state.cfg.cam_enabled[1] ||
                       c.photo_width != g_state.cfg.photo_width || c.photo_height != g_state.cfg.photo_height ||
                       c.web_port != g_state.cfg.web_port;
        if (!c.save(g_config_path))
            return json_error(res, "не удалось сохранить на SD", 500);
        g_state.cfg = c;
        json_reply(res, std::string("{\"ok\":true,\"restart_required\":") + (restart ? "true" : "false") + "}");
    });

    svr->Post("/api/restart", [](const Request &, Response &res) {
        json_reply(res, "{\"ok\":true}");
        g_restart = true;
        g_quit = true;
    });

    svr->Get("/api/photos", [photos](const Request &req, Response &res) {
        std::string o;
        if (req.has_param("date")) {
            std::string day = req.get_param_value("date");
            if (!valid_day(day))
                return json_error(res, "bad date");
            o = "{\"date\":\"" + day + "\",\"files\":[";
            auto files = photos->files(day);
            for (size_t i = 0; i < files.size(); i++)
                o += (i ? ",\"" : "\"") + files[i] + "\"";
            o += "]}";
        } else {
            o = "{\"days\":[";
            auto days = photos->days();
            for (size_t i = 0; i < days.size(); i++) {
                o += std::string(i ? "," : "") + "{\"date\":\"" + days[i] +
                     "\",\"count\":" + std::to_string(photos->files(days[i]).size()) + "}";
            }
            char b[64];
            snprintf(b, sizeof(b), "],\"disk_free_pct\":%.1f}", util::free_space_pct(DATA_DIR));
            o += b;
        }
        json_reply(res, o);
    });

    // Board keys (via the launcher). Short press: photo, or stop a running
    // recording. Long press: start / stop video.
    auto toast = [](const std::string &t) {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.toast = t;
        g_state.toast_until_ms = util::mono_ms() + 2500;
    };
    auto video_start = [video, toast]() {
        int max_s;
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            max_s = g_state.cfg.video_max_min * 60;
        }
        std::string name = video->start(max_s);
        toast(name.empty() ? "Видео не записывается" : "Запись видео");
        return name;
    };
    auto video_stop = [video, toast]() {
        video->stop();
        toast("Видео сохранено");
    };
    svr->Post("/api/action", [photos, video, video_stop, toast](const Request &, Response &res) {
        if (video->recording()) {
            video_stop();
            return json_reply(res, "{\"ok\":true,\"video\":\"stopped\"}");
        }
        std::string p = photos->capture("button");
        toast(p.empty() ? "Снимок не удался" : "Снимок сохранён");
        json_reply(res, "{\"ok\":true,\"photo\":\"" + p + "\"}");
    });
    svr->Post("/api/video/toggle", [video, video_start, video_stop](const Request &, Response &res) {
        if (video->recording()) {
            video_stop();
            return json_reply(res, "{\"ok\":true,\"recording\":false}");
        }
        std::string n = video_start();
        json_reply(res, std::string("{\"ok\":") + (n.empty() ? "false" : "true") + ",\"recording\":" +
                            (n.empty() ? "false" : "true") + ",\"name\":\"" + n + "\"}");
    });
    svr->Post("/api/video/start", [video, video_start](const Request &, Response &res) {
        std::string n = video->recording() ? "" : video_start();
        if (n.empty() && !video->recording())
            return json_error(res, "камеры не пишут (нет фото-канала?)");
        json_reply(res, "{\"ok\":true,\"name\":\"" + n + "\"}");
    });
    svr->Post("/api/video/stop", [video, video_stop](const Request &, Response &res) {
        if (video->recording())
            video_stop();
        json_reply(res, "{\"ok\":true}");
    });
    // recordings by day: [{day, items:[{name, cams:[0,1], poster:bool}]}]
    svr->Get("/api/videos", [video](const Request &, Response &res) {
        std::string o = "{\"recording\":" + std::string(video->recording() ? "true" : "false") + ",\"days\":[";
        auto days = video->days();
        std::reverse(days.begin(), days.end());
        for (size_t d = 0; d < days.size(); d++) {
            o += std::string(d ? "," : "") + "{\"day\":\"" + days[d] + "\",\"files\":[";
            auto files = video->files(days[d]);
            for (size_t i = 0; i < files.size(); i++) {
                std::string path = std::string(VIDEOS_DIR) + "/" + days[d] + "/" + files[i];
                o += std::string(i ? "," : "") + "{\"name\":\"" + util::json_escape(files[i]) +
                     "\",\"size\":" + std::to_string(util::file_size(path)) + "}";
            }
            o += "]}";
        }
        json_reply(res, o + "]}");
    });
    svr->Post("/api/videos/delete", [video](const Request &req, Response &res) {
        if (!video->remove(req.get_param_value("date"), req.get_param_value("name")))
            return json_error(res, "нет такого видео");
        json_reply(res, "{\"ok\":true}");
    });
    util::mkdirs(VIDEOS_DIR);
    svr->set_mount_point("/videos", VIDEOS_DIR);

    // ---- for bots (parking mode): places and a fresh annotated photo
    //   GET /api/v1/parking            {"ok","time","free","busy","total","spots":[...],"photo_url"}
    //   GET /api/v1/parking?photo=1    same + "photo_base64" (JPEG)
    //   GET /api/v1/parking/photo.jpg  the photo itself
    svr->Get("/api/v1/parking", [photos](const Request &req, Response &res) {
        if (!api_authorized(req, res))
            return;
        if (g_state.trigger_mode)
            return json_reply(res, "{\"ok\":false,\"error\":\"not in parking mode\"}", 404);
        std::string o;
        int free_n = 0, total = 0;
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            o = "\"spots\":[";
            for (size_t i = 0; i < g_state.spots.size(); i++) {
                const Spot &s = g_state.spots[i];
                char b[256];
                snprintf(b, sizeof(b),
                         "%s{\"id\":\"%s\",\"cam\":%d,\"occupied\":%s,\"coverage\":%.2f,\"since\":%lld,\"since_str\":\"%s\"}",
                         i ? "," : "", util::json_escape(s.id).c_str(), s.cam, s.occupied ? "true" : "false",
                         s.coverage, (long long)s.since_ms,
                         s.since_ms ? util::time_str(s.since_ms, "%Y-%m-%d %H:%M:%S").c_str() : "");
                o += b;
                total++;
                free_n += !s.occupied;
            }
            o += "]";
        }
        char head[256];
        snprintf(head, sizeof(head),
                 "{\"ok\":true,\"time\":%lld,\"time_str\":\"%s\",\"free\":%d,\"busy\":%d,\"total\":%d,",
                 (long long)util::now_ms(), util::time_str(util::now_ms(), "%Y-%m-%d %H:%M:%S").c_str(), free_n,
                 total - free_n, total);
        std::string body = head + o + ",\"photo_url\":\"/api/v1/parking/photo.jpg\"";
        if (req.get_param_value("photo") == "1") {
            std::vector<unsigned char> jpg;
            if (photos->live_annotated(jpg))
                body += ",\"photo_base64\":\"" + base64(jpg.data(), jpg.size()) + "\"";
        }
        json_reply(res, body + "}");
    });
    svr->Get("/api/v1/parking/photo.jpg", [photos](const Request &req, Response &res) {
        if (!api_authorized(req, res))
            return;
        std::vector<unsigned char> jpg;
        if (!photos->live_annotated(jpg))
            return json_reply(res, "{\"ok\":false,\"error\":\"no camera frame\"}", 503);
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::string((const char *)jpg.data(), jpg.size()), "image/jpeg");
    });
    // trigger mode: settings page "check" button
    svr->Post("/api/webhook/test", [](const Request &, Response &res) {
        std::string err;
        int st = g_webhook.send_now("Проверка связи с K510 (" + util::time_str(util::now_ms(), "%H:%M:%S") + ")", &err);
        if (st == 200)
            return json_reply(res, "{\"ok\":true}");
        json_error(res, st == 0 ? "уведомления выключены — включите и сохраните" :
                        "ответ " + std::to_string(st) + ": " + err, 502);
    });

    svr->Post("/api/photos/capture", [photos](const Request &req, Response &res) {
        std::string o = "{\"files\":[";
        std::string p = photos->capture("manual");
        bool first = p.empty();
        if (!p.empty())
            o += "\"" + p + "\"";
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            g_state.toast = first ? "Снимок не удался" : "Снимок сохранён";
            g_state.toast_until_ms = util::mono_ms() + 2500;
        }
        json_reply(res, o + "]}");
    });

    svr->Post("/api/photos/delete", [photos](const Request &req, Response &res) {
        std::string day = req.get_param_value("date");
        if (!valid_day(day) || !photos->delete_day(day))
            return json_error(res, "cannot delete");
        json_reply(res, "{\"ok\":true}");
    });

    svr->Get("/api/photos/archive", [photos](const Request &req, Response &res) {
        std::string from = req.get_param_value("from"), to = req.get_param_value("to");
        if ((!from.empty() && !valid_day(from)) || (!to.empty() && !valid_day(to))) {
            res.status = 400;
            return;
        }
        std::string fname = "parking_photos" + (from.empty() ? "" : "_" + from) + (to.empty() ? "" : "_" + to) + ".tar";
        res.set_header("Content-Disposition", "attachment; filename=\"" + fname + "\"");
        res.set_chunked_content_provider("application/x-tar", [photos, from, to](size_t, DataSink &sink) {
            photos->write_tar(from, to, [&](const char *d, size_t n) { return sink.write(d, n); });
            sink.done();
            return true;
        });
    });

    // Manual correction from the status page: stored as a label for training,
    // together with a fresh photo of that camera.
    svr->Post("/api/label", [photos](const Request &req, Response &res) {
        rapidjson::Document d;
        if (!parse_body(req, d) || !d.HasMember("spot") || !d["spot"].IsString() || !d.HasMember("occupied") ||
            !d["occupied"].IsBool())
            return json_error(res, "expected {spot, occupied}");
        std::string id = d["spot"].GetString();
        bool occ = d["occupied"].GetBool();
        int cam = -1;
        bool auto_occ = false;
        float cov = 0;
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            for (auto &s : g_state.spots)
                if (s.id == id) {
                    cam = s.cam;
                    auto_occ = s.occupied;
                    cov = s.coverage;
                }
        }
        if (cam < 0)
            return json_error(res, "unknown spot");
        std::string photo = photos->capture("label");
        char line[512];
        snprintf(line, sizeof(line),
                 "{\"time\":%lld,\"spot\":\"%s\",\"cam\":%d,\"occupied\":%s,\"auto_occupied\":%s,"
                 "\"coverage\":%.3f,\"photo\":\"%s\"}\n",
                 (long long)util::now_ms(), util::json_escape(id).c_str(), cam, occ ? "true" : "false",
                 auto_occ ? "true" : "false", cov, photo.c_str());
        FILE *f = fopen(LABELS_PATH, "a");
        if (!f)
            return json_error(res, "cannot write labels", 500);
        fputs(line, f);
        fclose(f);
        json_reply(res, "{\"ok\":true,\"photo\":\"" + photo + "\"}");
    });

    svr->Get("/api/labels", [](const Request &, Response &res) {
        std::string s;
        util::read_file(LABELS_PATH, s);
        res.set_header("Content-Disposition", "attachment; filename=\"labels.jsonl\"");
        res.set_content(s, "application/x-ndjson");
    });

    svr->Get("/api/wifi/status", [](const Request &, Response &res) {
        WifiStatus s = g_wifi.status();
        json_reply(res, std::string("{\"present\":") + (s.present ? "true" : "false") +
                            ",\"ap_mode\":" + (s.ap_mode ? "true" : "false") + ",\"state\":\"" +
                            util::json_escape(s.state) + "\",\"ssid\":\"" + util::json_escape(s.ssid) +
                            "\",\"ip\":\"" + s.ip + "\"}");
    });

    svr->Get("/api/wifi/scan", [](const Request &, Response &res) {
        auto nets = g_wifi.scan();
        std::string o = "{\"networks\":[";
        for (size_t i = 0; i < nets.size(); i++) {
            char b[64];
            snprintf(b, sizeof(b), "\",\"signal\":%d,\"freq\":%d,\"secure\":%s}", nets[i].signal, nets[i].freq,
                     nets[i].secure ? "true" : "false");
            o += std::string(i ? "," : "") + "{\"ssid\":\"" + util::json_escape(nets[i].ssid) + b;
        }
        json_reply(res, o + "]}");
    });

    svr->Post("/api/wifi/connect", [](const Request &req, Response &res) {
        rapidjson::Document d;
        if (!parse_body(req, d) || !d.HasMember("ssid") || !d["ssid"].IsString())
            return json_error(res, "expected {ssid, psk}");
        std::string psk = (d.HasMember("psk") && d["psk"].IsString()) ? d["psk"].GetString() : "";
        std::string err;
        if (!g_wifi.connect(d["ssid"].GetString(), psk, &err))
            return json_error(res, err);
        json_reply(res, "{\"ok\":true,\"ip\":\"" + util::iface_ip("wlan0") + "\"}");
    });

    if (!svr->bind_to_port("0.0.0.0", port)) {
        fprintf(stderr, "web: cannot bind port %d\n", port);
        return -1;
    }
    th_ = std::thread([svr] { svr->listen_after_bind(); });
    printf("web: listening on port %d\n", port);
    return 0;
}

void WebServer::stop()
{
    if (svr_)
        ((Server *)svr_)->stop();
    if (th_.joinable())
        th_.join();
    delete (Server *)svr_;
    svr_ = nullptr;
}
