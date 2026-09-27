#include "state.h"
#include "util.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>

using namespace rapidjson;

AppState g_state;
std::string g_config_path = CONFIG_PATH;
std::atomic<bool> g_quit(false);

static std::string str_list_json(const std::vector<std::string> &v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i)
            o += ",";
        o += "\"" + util::json_escape(v[i]) + "\"";
    }
    return o + "]";
}

std::string Config::to_json() const
{
    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\n"
             "  \"model\": \"%s\",\n"
             "  \"net_len\": %d,\n"
             "  \"obj_thresh\": %.3f,\n"
             "  \"nms_thresh\": %.3f,\n"
             "  \"cam_enabled\": [%s, %s],\n"
             "  \"occupancy_threshold\": %.3f,\n"
             "  \"footprint\": %.3f,\n"
             "  \"debounce\": %d,\n"
             "  \"ai_fps\": %.1f,\n"
             "  \"photo_interval_min\": %d,\n"
             "  \"photo_width\": %d,\n"
             "  \"photo_height\": %d,\n"
             "  \"jpeg_quality\": %d,\n"
             "  \"keep_raw\": %s,\n"
             "  \"min_free_pct\": %.1f,\n"
             "  \"video_max_min\": %d,\n"
             "  \"trigger_ai_cam\": %d,\n"
             "  \"trigger_video_s\": %d,\n"
             "  \"trigger_confirm\": %d,\n"
             "  \"web_port\": %d,\n"
             "  \"ap_ssid\": \"%s\",\n"
             "  \"ap_psk\": \"%s\",\n"
             "  \"ap_timeout_s\": %d,\n",
             util::json_escape(model).c_str(), net_len, obj_thresh, nms_thresh,
             cam_enabled[0] ? "true" : "false", cam_enabled[1] ? "true" : "false",
             occupancy_threshold, footprint, debounce, ai_fps, photo_interval_min, photo_width,
             photo_height, jpeg_quality, keep_raw ? "true" : "false", min_free_pct, video_max_min, trigger_ai_cam, trigger_video_s, trigger_confirm, web_port, util::json_escape(ap_ssid).c_str(),
             util::json_escape(ap_psk).c_str(), ap_timeout_s);
    std::string o = buf;
    o += "  \"vehicle_classes\": " + str_list_json(vehicle_classes) + ",\n";
    o += "  \"draw_classes\": " + str_list_json(draw_classes) + ",\n";
    o += "  \"trigger_classes\": " + str_list_json(trigger_classes) + ",\n";
    o += "  \"webhook_enabled\": " + std::string(webhook_enabled ? "true" : "false") + ",\n";
    o += "  \"webhook_url\": \"" + util::json_escape(webhook_url) + "\",\n";
    o += "  \"webhook_token\": \"" + util::json_escape(webhook_token) + "\",\n";
    o += "  \"webhook_device_id\": \"" + util::json_escape(webhook_device_id) + "\",\n";
    o += "  \"webhook_video\": " + std::string(webhook_video ? "true" : "false") + ",\n";
    o += "  \"api_token\": \"" + util::json_escape(api_token) + "\"\n}\n";
    return o;
}

bool Config::from_json(const std::string &json, std::string *err)
{
    Document d;
    d.Parse(json.c_str());
    if (d.HasParseError() || !d.IsObject()) {
        if (err)
            *err = "invalid json";
        return false;
    }
    Config c = *this;
#define GET_STR(k) if (d.HasMember(#k) && d[#k].IsString()) c.k = d[#k].GetString()
#define GET_INT(k) if (d.HasMember(#k) && d[#k].IsInt()) c.k = d[#k].GetInt()
#define GET_NUM(k) if (d.HasMember(#k) && d[#k].IsNumber()) c.k = d[#k].GetDouble()
    GET_STR(model);
    GET_INT(net_len);
    GET_NUM(obj_thresh);
    GET_NUM(nms_thresh);
    GET_NUM(occupancy_threshold);
    GET_NUM(footprint);
    GET_INT(debounce);
    GET_NUM(ai_fps);
    GET_INT(photo_interval_min);
    GET_INT(photo_width);
    GET_INT(photo_height);
    GET_INT(jpeg_quality);
    if (d.HasMember("keep_raw") && d["keep_raw"].IsBool())
        c.keep_raw = d["keep_raw"].GetBool();
    GET_NUM(min_free_pct);
    GET_INT(video_max_min);
    GET_INT(trigger_ai_cam);
    GET_INT(trigger_video_s);
    GET_INT(trigger_confirm);
    GET_STR(webhook_url);
    GET_STR(webhook_token);
    GET_STR(webhook_device_id);
    GET_STR(api_token);
    if (d.HasMember("webhook_enabled") && d["webhook_enabled"].IsBool())
        c.webhook_enabled = d["webhook_enabled"].GetBool();
    if (d.HasMember("webhook_video") && d["webhook_video"].IsBool())
        c.webhook_video = d["webhook_video"].GetBool();
    GET_INT(web_port);
    GET_STR(ap_ssid);
    GET_STR(ap_psk);
    GET_INT(ap_timeout_s);
#undef GET_STR
#undef GET_INT
#undef GET_NUM
    if (d.HasMember("cam_enabled") && d["cam_enabled"].IsArray()) {
        auto &a = d["cam_enabled"];
        for (SizeType i = 0; i < a.Size() && i < NUM_CAMS; i++)
            if (a[i].IsBool())
                c.cam_enabled[i] = a[i].GetBool();
    }
    auto get_list = [&](const char *k, std::vector<std::string> &dst) {
        if (d.HasMember(k) && d[k].IsArray()) {
            dst.clear();
            for (auto &v : d[k].GetArray())
                if (v.IsString())
                    dst.push_back(v.GetString());
        }
    };
    get_list("vehicle_classes", c.vehicle_classes);
    get_list("draw_classes", c.draw_classes);
    get_list("trigger_classes", c.trigger_classes);

    if (c.photo_interval_min < 0 || c.photo_interval_min > 1440 || c.debounce < 1 || c.ai_fps < 0 || c.ai_fps > 30 ||
        c.occupancy_threshold <= 0 || c.occupancy_threshold > 1 || c.footprint <= 0 ||
        c.footprint > 1 || c.jpeg_quality < 10 || c.jpeg_quality > 100 ||
        (c.photo_width & 15) || (c.photo_height & 1) || c.photo_width < 320 ||
        c.photo_width > 1920 || c.photo_height < 240 || c.photo_height > 1080 ||
        c.ap_psk.size() < 8 || c.video_max_min < 1 || c.video_max_min > 120 ||
        c.trigger_ai_cam < 0 || c.trigger_ai_cam >= NUM_CAMS || c.trigger_video_s < 30 || c.trigger_video_s > 600 ||
        c.trigger_confirm < 1 || c.trigger_confirm > 30 || c.webhook_device_id.empty() ||
        c.webhook_device_id.size() > 64 ||
        c.webhook_device_id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") !=
            std::string::npos) {
        if (err)
            *err = "value out of range";
        return false;
    }
    *this = c;
    return true;
}

bool Config::load(const std::string &path)
{
    std::string s;
    if (!util::read_file(path, s))
        return false;
    return from_json(s, nullptr);
}

bool Config::save(const std::string &path) const
{
    return util::write_file_atomic(path, to_json());
}

bool AppState::load_spots(const std::string &path)
{
    std::string s;
    if (!util::read_file(path, s))
        return false;
    return spots_from_json(s, nullptr);
}

bool AppState::save_spots(const std::string &path)
{
    return util::write_file_atomic(path, spots_to_json(false));
}

std::string AppState::spots_to_json(bool with_state)
{
    std::string o = "{\"spots\":[";
    for (size_t i = 0; i < spots.size(); i++) {
        const Spot &s = spots[i];
        if (i)
            o += ",";
        o += "\n{\"id\":\"" + util::json_escape(s.id) + "\",\"cam\":" + std::to_string(s.cam) +
             ",\"points\":[";
        for (size_t j = 0; j < s.pts.size(); j++) {
            char b[64];
            snprintf(b, sizeof(b), "%s[%.4f,%.4f]", j ? "," : "", s.pts[j].x, s.pts[j].y);
            o += b;
        }
        o += "]";
        if (with_state) {
            char b[128];
            snprintf(b, sizeof(b), ",\"occupied\":%s,\"coverage\":%.3f,\"since\":%lld",
                     s.occupied ? "true" : "false", s.coverage, (long long)s.since_ms);
            o += b;
        }
        o += "}";
    }
    return o + "\n]}\n";
}

bool AppState::spots_from_json(const std::string &json, std::string *err)
{
    Document d;
    d.Parse(json.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("spots") || !d["spots"].IsArray()) {
        if (err)
            *err = "expected {\"spots\": [...]}";
        return false;
    }
    std::vector<Spot> out;
    for (auto &v : d["spots"].GetArray()) {
        if (!v.IsObject() || !v.HasMember("id") || !v["id"].IsString() || !v.HasMember("points") ||
            !v["points"].IsArray()) {
            if (err)
                *err = "spot needs id and points";
            return false;
        }
        Spot s;
        s.id = v["id"].GetString();
        s.cam = (v.HasMember("cam") && v["cam"].IsInt()) ? v["cam"].GetInt() : 0;
        if (s.id.empty() || s.id.size() > 32 || s.cam < 0 || s.cam >= NUM_CAMS) {
            if (err)
                *err = "bad id or cam";
            return false;
        }
        for (auto &p : v["points"].GetArray()) {
            if (!p.IsArray() || p.Size() != 2 || !p[0].IsNumber() || !p[1].IsNumber()) {
                if (err)
                    *err = "point must be [x,y]";
                return false;
            }
            Point2 pt = {(float)p[0].GetDouble(), (float)p[1].GetDouble()};
            if (pt.x < 0 || pt.x > 1 || pt.y < 0 || pt.y > 1) {
                if (err)
                    *err = "points must be normalized 0..1";
                return false;
            }
            s.pts.push_back(pt);
        }
        if (s.pts.size() < 3 || s.pts.size() > 16) {
            if (err)
                *err = "spot needs 3..16 points";
            return false;
        }
        // keep runtime state of spots that survived the edit
        for (auto &old : spots) {
            if (old.id == s.id && old.cam == s.cam) {
                s.occupied = old.occupied;
                s.since_ms = old.since_ms;
                s.coverage = old.coverage;
            }
        }
        out.push_back(s);
    }
    for (size_t i = 0; i < out.size(); i++)
        for (size_t j = i + 1; j < out.size(); j++)
            if (out[i].id == out[j].id) {
                if (err)
                    *err = "duplicate spot id " + out[i].id;
                return false;
            }
    spots.swap(out);
    spots_version++;
    return true;
}
