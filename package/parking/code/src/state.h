#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#define NUM_CAMS 2

#define DATA_DIR "/root/data/parking"
#define CONFIG_PATH DATA_DIR "/config.json"
#define TRIGGER_CONFIG_PATH "/root/data/trigger/config.json"
#define SPOTS_PATH DATA_DIR "/spots.json"
#define LABELS_PATH DATA_DIR "/labels.jsonl"
#define PHOTOS_DIR DATA_DIR "/photos"
#define SYS_STATUS_PATH "/tmp/k510_status.txt"  // written by the launcher
// Exists while the screen is "off" (menu key held). The panel backlight is not
// controllable on this board, so the app covers everything with black.
#define SCREEN_OFF_PATH "/run/k510_screen_off"

struct Config {
    std::string model = "sdk_yolov5s_320";  // model id (see model.h) or a kmodel path
    int net_len = 320;                      // taken from the model at startup
    float obj_thresh = 0.4f;
    float nms_thresh = 0.45f;
    bool cam_enabled[NUM_CAMS] = {true, true};
    std::vector<std::string> vehicle_classes = {"car", "truck", "bus", "motorcycle"};
    std::vector<std::string> draw_classes;  // empty = draw everything detected
    float occupancy_threshold = 0.3f;       // share of the spot covered by a vehicle footprint
    float footprint = 0.5f;                 // lower part of the bbox treated as the vehicle footprint
    int debounce = 5;                       // consecutive evaluations before a spot flips
    float ai_fps = 2.f;                     // inferences per second per camera, 0 = as fast as possible
    int photo_interval_min = 15;             // 0 = timer photos off
    int photo_width = 1280;
    int photo_height = 720;
    int jpeg_quality = 90;
    bool keep_raw = true;                    // also store clean per-camera frames in raw/
    double min_free_pct = 10.0;
    int video_max_min = 10;                  // recording stops by itself after this

    // trigger mode (parking --trigger): one camera watches with AI, the other records
    int trigger_ai_cam = 0;
    std::vector<std::string> trigger_classes = {"cat"};
    int trigger_video_s = 60;                // 30 s .. 10 min
    int trigger_confirm = 2;                 // consecutive AI frames with the class before it fires
    // trigger notifications to a Gopac webhook (Telegram)
    bool webhook_enabled = false;
    std::string webhook_url;                 // http://host:8080/api/v1/notify
    std::string webhook_token;
    std::string webhook_device_id = "k510";
    bool webhook_video = true;               // also send the video when it is finished
    // parking: token for /api/v1/* (bots); empty = no token needed
    std::string api_token;
    int web_port = 80;
    std::string ap_ssid = "K510-Setup";
    std::string ap_psk = "k510setup";
    int ap_timeout_s = 60;

    bool load(const std::string &path);
    bool save(const std::string &path) const;
    std::string to_json() const;
    bool from_json(const std::string &json, std::string *err);
};

struct Point2 {
    float x, y;  // normalized 0..1 in camera frame
};

struct Spot {
    std::string id;
    int cam = 0;
    std::vector<Point2> pts;
    // runtime
    bool occupied = false;
    float coverage = 0.f;
    int streak = 0;
    int64_t since_ms = 0;
};

struct Detection {
    int label;
    std::string name;
    float score;
    float x1, y1, x2, y2;     // normalized 0..1
    std::string text;         // plate text, emotion, head angles
    std::vector<float> pts;   // x,y pairs normalized, negative = not found
    int shape = 0;            // PointShape (model.h): how the points are drawn
};

struct CamState {
    bool running = false;
    std::vector<Detection> dets;
    uint64_t frames = 0;
    float fps = 0.f;
    float infer_ms = 0.f, post_ms = 0.f;  // KPU run and CPU decode, smoothed
    int64_t ts_ms = 0;
};

class AppState {
public:
    std::mutex mtx;  // guards everything below
    Config cfg;
    std::vector<Spot> spots;
    CamState cams[NUM_CAMS];
    uint64_t spots_version = 0;  // bumped on every edit, lets readers notice changes
    std::string model_id, model_title;  // model actually running
    int64_t rec_started_ms = 0;         // video recording (mono ms), 0 = not recording
    bool trigger_mode = false;
    std::string trigger_last;           // "cat 0.87 at 12:34:56", shown on screen and web
    int trigger_count = 0;

    std::string status_msg;  // shown on screen (wifi setup hints etc.)
    std::string toast;       // short centered banner, e.g. "Снимок сохранён"
    int64_t toast_until_ms = 0;

    bool load_spots(const std::string &path);
    bool save_spots(const std::string &path);
    std::string spots_to_json(bool with_state);
    bool spots_from_json(const std::string &json, std::string *err);
};

extern AppState g_state;
extern std::string g_config_path;  // CONFIG_PATH, or TRIGGER_CONFIG_PATH in trigger mode
extern std::atomic<bool> g_quit;
