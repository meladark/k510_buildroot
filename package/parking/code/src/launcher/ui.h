#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "apps.h"
#include "text.h"
#include "wifi.h"

// Full-screen touch menu drawn on a BGRA canvas: app list, Wi-Fi setup with an
// on-screen keyboard, info. Everything is laid out relative to the screen size.
class Menu {
public:
    struct Actions {
        std::function<void(const std::string &app_id)> launch;
        std::function<void()> resume;  // back to the running/last app
        std::function<void()> reboot;
    };

    ~Menu()
    {
        if (worker_.joinable())
            worker_.join();
    }
    void init(int w, int h, TextRenderer *text, Wifi *wifi, Actions act);
    void open(const std::vector<App> &apps, const std::string &current_app, const std::string &error);
    void tap(int x, int y);
    void swipe(int dir);  // -1 = left (next), +1 = right (previous)
    void render(cv::Mat &canvas);
    bool dirty() const { return dirty_ || busy_.load(); }
    void mark_dirty() { dirty_ = true; }

private:
    enum Screen { MAIN, WIFI, KEYBOARD, MESSAGE, SETTINGS, GAL_DAYS, GAL_GRID, GAL_VIEW, GAL_CONFIRM };
    struct Button {
        cv::Rect r;
        std::string label;
        std::function<void()> on_tap;
        int style;  // 0 normal, 1 primary, 2 selected, 3 key, 4 danger
    };

    void layout_main(cv::Mat &c);
    void layout_wifi(cv::Mat &c);
    void layout_keyboard(cv::Mat &c);
    void layout_message(cv::Mat &c);
    void layout_settings(cv::Mat &c);
    void layout_gal_days(cv::Mat &c);
    void layout_gal_grid(cv::Mat &c);
    void layout_gal_view(cv::Mat &c);
    void layout_gal_confirm(cv::Mat &c);
    void segmented(cv::Mat &c, int &y, const std::string &title, const std::vector<std::string> &labels,
                   int selected, std::function<void(int)> on_pick);
    void gal_open_day(const std::string &day);
    cv::Mat thumb(const std::string &path, int w, int h);
    cv::Mat gal_cam_image(int cam, bool raw);  // current shot, one camera, full size (cached)
    void gal_draw_fit(cv::Mat &c, const cv::Mat &img, const cv::Rect &area);
    void blit(cv::Mat &c, const cv::Mat &bgr, const cv::Rect &r);
    void button(cv::Mat &c, const cv::Rect &r, const std::string &label, int style, std::function<void()> fn);
    void header(cv::Mat &c, const std::string &title);
    void start_scan();
    void start_connect(const std::string &ssid, const std::string &psk);

    int w_ = 1080, h_ = 1920, u_ = 10;  // u_ = layout unit (~1% of width)
    TextRenderer *text_ = nullptr;
    Wifi *wifi_ = nullptr;
    Actions act_;
    Screen screen_ = MAIN;
    std::vector<Button> buttons_;
    bool dirty_ = true;

    std::vector<App> apps_;
    std::string current_app_, error_;

    // wifi state (worker thread writes, UI reads)
    std::mutex m_;
    std::atomic<bool> busy_{false};
    std::thread worker_;
    std::vector<WifiNet> nets_;
    WifiStatus status_;
    int net_page_ = 0;
    std::string msg_title_, msg_text_;

    // keyboard
    std::string kb_ssid_, kb_text_;
    int kb_layer_ = 0;  // 0 lower, 1 upper, 2 symbols
    bool kb_show_ = false;

    // camera settings (parking config file, edited while the app is stopped)
    struct CamSettings {
        float ai_fps = 2;
        bool cam[2] = {true, true};
        int photo_interval_min = 15;
        bool loaded = false;
    } cs_;
    void load_cam_settings();
    void save_cam_settings();

    // gallery: one Shot per photo event. New photos are one stacked file
    // (+ raw/ frames, + json with per-camera offsets); old ones are separate
    // camN_HHMMSS.jpg files that get paired by time.
    struct Shot {
        std::string key;        // HHMMSS[_n], used for sorting and captions
        std::string stacked;    // stacked image file name, or ""
        std::string cam[2];     // old per-camera files
        std::string raw[2];     // raw/ clean frames
        std::string json;       // metadata file name, or ""
        int offset_y[2] = {-1, -1}, height[2] = {0, 0};
        std::string summary;    // "CAM0: car ×2 · места 3/5 свободно"
        bool parsed = false;
    };
    void gal_parse(Shot &s);
    std::vector<std::string> gal_days_;
    std::string gal_day_;
    std::vector<Shot> gal_shots_;  // newest first
    int gal_page_ = 0, gal_index_ = 0, gal_days_page_ = 0;
    int gal_mode_ = 0;             // 0 both, 1 cam0, 2 cam1
    bool gal_raw_ = false;         // show clean frames instead of annotated
    std::map<std::string, cv::Mat> thumbs_;
    std::string view_key_;
    cv::Mat view_img_[2];          // current shot, per camera (annotated or raw)
    cv::Mat view_stacked_;
};
