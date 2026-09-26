// K510 launcher: runs one camera/display app at a time and owns the system UI.
//
//  * starts the last chosen app from /app/launcher/apps or /root/data/apps
//  * long press (2 s) in the top-left screen corner, or any board key, stops
//    the app and shows the touch menu (apps, Wi-Fi with on-screen keyboard)
//  * Wi-Fi setup access point fallback, status hints in /tmp/k510_status.txt
//  * small web page on :8080 to switch apps from a browser
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mutex>

#include <rapidjson/document.h>

#include "../third_party/httplib.h"
#include "apps.h"
#include "drm_out.h"
#include "state.h"
#include "text.h"
#include "touch.h"
#include "ui.h"
#include "util.h"
#include "wifi.h"

#define CONF_DIR "/root/data/launcher"
#define CONF_PATH CONF_DIR "/config.json"
#define STATUS_PATH "/tmp/k510_status.txt"
#define CORNER_PX 220
#define LONG_PRESS_MS 2000

struct LauncherConfig {
    std::string default_app = "10-parking";
    std::string ap_ssid = "K510-Setup", ap_psk = "k510setup";
    int ap_timeout_s = 60;
    int menu_timeout_s = 300;
    int web_port = 8080;
    int key_menu = 48;    // board button that opens/closes the menu (KEY_B)
    int key_action = 30;  // board button for the app action, e.g. photo (KEY_A)
    TouchCalib touch;

    void load()
    {
        std::string s;
        if (!util::read_file(CONF_PATH, s))
            return;
        rapidjson::Document d;
        d.Parse(s.c_str());
        if (d.HasParseError() || !d.IsObject())
            return;
        auto str = [&](const char *k, std::string &v) { if (d.HasMember(k) && d[k].IsString()) v = d[k].GetString(); };
        auto num = [&](const char *k, int &v) { if (d.HasMember(k) && d[k].IsInt()) v = d[k].GetInt(); };
        str("default_app", default_app);
        str("ap_ssid", ap_ssid);
        str("ap_psk", ap_psk);
        num("ap_timeout_s", ap_timeout_s);
        num("menu_timeout_s", menu_timeout_s);
        num("web_port", web_port);
        num("key_menu", key_menu);
        num("key_action", key_action);
        if (d.HasMember("touch") && d["touch"].IsObject()) {
            auto &t = d["touch"];
            if (t.HasMember("raw_w") && t["raw_w"].IsInt()) touch.raw_w = t["raw_w"].GetInt();
            if (t.HasMember("raw_h") && t["raw_h"].IsInt()) touch.raw_h = t["raw_h"].GetInt();
            if (t.HasMember("invert_x") && t["invert_x"].IsBool()) touch.invert_x = t["invert_x"].GetBool();
            if (t.HasMember("invert_y") && t["invert_y"].IsBool()) touch.invert_y = t["invert_y"].GetBool();
            if (t.HasMember("swap_xy") && t["swap_xy"].IsBool()) touch.swap_xy = t["swap_xy"].GetBool();
        }
    }

    void save() const
    {
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "{\n  \"default_app\": \"%s\",\n  \"ap_ssid\": \"%s\",\n  \"ap_psk\": \"%s\",\n"
                 "  \"ap_timeout_s\": %d,\n  \"menu_timeout_s\": %d,\n  \"web_port\": %d,\n"
                 "  \"key_menu\": %d,\n  \"key_action\": %d,\n"
                 "  \"touch\": {\"raw_w\": %d, \"raw_h\": %d, \"invert_x\": %s, \"invert_y\": %s, \"swap_xy\": %s}\n}\n",
                 util::json_escape(default_app).c_str(), util::json_escape(ap_ssid).c_str(),
                 util::json_escape(ap_psk).c_str(), ap_timeout_s, menu_timeout_s, web_port, key_menu, key_action, touch.raw_w,
                 touch.raw_h, touch.invert_x ? "true" : "false", touch.invert_y ? "true" : "false",
                 touch.swap_xy ? "true" : "false");
        util::mkdirs(CONF_DIR);
        util::write_file_atomic(CONF_PATH, buf);
    }
};

// Requests from the web thread, executed by the main loop.
static std::mutex g_req_mtx;
static std::string g_req_switch;
static bool g_req_menu = false;
static std::string g_current;  // app id shown on the web page

// POSTs to the app's action URL without blocking the UI loop.
static void post_action(const std::string &url)
{
    std::thread([url] {
        // http://host[:port]/path
        std::string rest = url.compare(0, 7, "http://") == 0 ? url.substr(7) : url;
        size_t slash = rest.find('/');
        std::string hostport = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
        std::string host = hostport.substr(0, hostport.find(':'));
        int port = hostport.find(':') == std::string::npos ? 80 : atoi(hostport.substr(hostport.find(':') + 1).c_str());
        httplib::Client cli(host, port);
        cli.set_read_timeout(15, 0);
        auto r = cli.Post(path.c_str(), "", "application/json");
        printf("launcher: action %s -> %d\n", url.c_str(), r ? r->status : -1);
    }).detach();
}

static void on_signal(int)
{
    g_quit = true;
}

static const App *find_app(const std::vector<App> &apps, const std::string &id)
{
    for (auto &a : apps)
        if (a.id == id)
            return &a;
    return nullptr;
}

static void start_web(httplib::Server &svr, int port, std::thread &th)
{
    svr.Get("/", [](const httplib::Request &req, httplib::Response &res) {
        auto apps = load_apps();
        std::string cur;
        {
            std::lock_guard<std::mutex> lk(g_req_mtx);
            cur = g_current;
        }
        std::string host = req.get_header_value("Host");
        host = host.substr(0, host.find(':'));
        std::string html =
            "<!doctype html><html lang=ru><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>K510 Приложения</title><style>body{font:16px system-ui;margin:0;padding:16px;max-width:640px;"
            "background:#f5f6f8;color:#1d2330}@media(prefers-color-scheme:dark){body{background:#11151c;color:#e6e9ef}"
            ".app{background:#1a202b!important;border-color:#2c3444!important}}h1{font-size:20px}"
            ".app{display:block;width:100%;text-align:left;padding:12px 14px;margin:8px 0;border:1px solid #d9dde5;"
            "border-radius:10px;background:#fff;color:inherit;font:inherit;cursor:pointer}.app small{display:block;opacity:.7}"
            ".cur{border-color:#16a34a!important;box-shadow:0 0 0 2px #16a34a55}a{color:#2563eb}</style>"
            "<h1>K510 · приложения</h1><p>Сейчас: <b>" + util::json_escape(cur) + "</b> · "
            "<a href='http://" + host + "/'>веб-интерфейс приложения (:80)</a></p>";
        for (auto &a : apps) {
            html += "<form method=post action=/switch><input type=hidden name=id value='" + a.id + "'>"
                    "<button class='app" + std::string(a.id == cur ? " cur" : "") + "'>" + a.name +
                    "<small>" + a.desc + "</small></button></form>";
        }
        html += "<form method=post action=/menu><button class=app>Открыть меню на экране платы</button></form>"
                "<p><small>Меню на плате: удерживать палец 2 секунды в левом верхнем углу экрана.</small></p></html>";
        res.set_content(html, "text/html; charset=utf-8");
    });
    svr.Post("/switch", [](const httplib::Request &req, httplib::Response &res) {
        {
            std::lock_guard<std::mutex> lk(g_req_mtx);
            g_req_switch = req.get_param_value("id");
        }
        res.set_redirect("/");
    });
    svr.Post("/menu", [](const httplib::Request &, httplib::Response &res) {
        {
            std::lock_guard<std::mutex> lk(g_req_mtx);
            g_req_menu = true;
        }
        res.set_redirect("/");
    });
    svr.Get("/api/apps", [](const httplib::Request &, httplib::Response &res) {
        auto apps = load_apps();
        std::string cur;
        {
            std::lock_guard<std::mutex> lk(g_req_mtx);
            cur = g_current;
        }
        std::string o = "{\"current\":\"" + util::json_escape(cur) + "\",\"apps\":[";
        for (size_t i = 0; i < apps.size(); i++)
            o += std::string(i ? "," : "") + "{\"id\":\"" + util::json_escape(apps[i].id) + "\",\"name\":\"" +
                 util::json_escape(apps[i].name) + "\"}";
        res.set_content(o + "]}", "application/json");
    });
    if (!svr.bind_to_port("0.0.0.0", port)) {
        fprintf(stderr, "launcher: cannot bind web port %d\n", port);
        return;
    }
    th = std::thread([&svr] { svr.listen_after_bind(); });
}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);  // logs go to a file; keep them live
    printf("launcher: build %s %s\n", __DATE__, __TIME__);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    LauncherConfig cfg;
    cfg.load();
    cfg.save();  // writes defaults on first run

    Wifi wifi;
    wifi.configure(cfg.ap_ssid, cfg.ap_psk, cfg.ap_timeout_s);
    wifi.on_status([](const std::string &msg) {
        if (msg.empty())
            unlink(STATUS_PATH);
        else
            util::write_file_atomic(STATUS_PATH, msg + "\n");
    });
    unlink(STATUS_PATH);
    wifi.start();

    uint32_t sw = 1080, sh = 1920;
    bool have_display = drm_query_resolution(&sw, &sh) == 0;

    TextRenderer text;
    if (!text.init())
        fprintf(stderr, "launcher: no font, menu text will be missing\n");

    Touch touch;
    std::string tdev = find_input_device("touch");
    if (tdev.empty() || !touch.open(tdev.c_str(), sw, sh, cfg.touch))
        fprintf(stderr, "launcher: no touchscreen\n");
    int key_fd = -1;
    std::string kdev = find_input_device("gpio-keys");
    if (!kdev.empty())
        key_fd = open(kdev.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    httplib::Server web;
    std::thread web_th;
    start_web(web, cfg.web_port, web_th);

    Supervisor sup;
    std::vector<App> apps = load_apps();
    std::string last_app = cfg.default_app;

    DrmOut drm;
    Menu menu;
    bool in_menu = false;
    int64_t menu_activity = 0, last_render = 0;
    bool ignore_until_up = false;
    int down_x = 0, down_y = 0;
    int osd_idx = 0;
    cv::Mat canvas;

    auto set_current = [&](const std::string &id) {
        std::lock_guard<std::mutex> lk(g_req_mtx);
        g_current = id;
    };

    auto launch = [&](const std::string &id) {
        apps = load_apps();
        const App *a = find_app(apps, id);
        if (!a)
            return false;
        if (in_menu) {
            drm.disable_planes();
            drm.deinit();
            in_menu = false;
        }
        sup.start(*a);
        last_app = id;
        set_current(a->name);
        if (cfg.default_app != id) {
            cfg.default_app = id;
            cfg.save();
        }
        return true;
    };

    auto enter_menu = [&](const std::string &err) {
        if (in_menu || !have_display)
            return;
        sup.stop();
        set_current("меню");
        if (drm.init() != 0) {
            fprintf(stderr, "launcher: cannot open display for the menu\n");
            drm.deinit();
            return;
        }
        canvas.create(drm.height(), drm.width(), CV_8UC4);
        apps = load_apps();
        menu.open(apps, last_app, err);
        in_menu = true;
        menu_activity = util::mono_ms();
        last_render = 0;
    };

    Menu::Actions act;
    act.launch = [&](const std::string &id) { launch(id); };
    act.resume = [&] {
        if (!launch(last_app) && !apps.empty())
            launch(apps.front().id);
    };
    act.reboot = [&] {
        sup.stop();
        sync();
        util::run({"reboot"});
    };
    menu.init(sw, sh, &text, &wifi, act);

    if (!launch(cfg.default_app)) {
        if (!apps.empty() && !have_display)
            launch(apps.front().id);
        else
            enter_menu(apps.empty() ? "" : "Приложение по умолчанию не найдено, выберите другое");
    }

    while (!g_quit.load()) {
        struct pollfd fds[3];
        int n = 0, drm_i = -1;
        if (touch.fd() >= 0)
            fds[n++] = {touch.fd(), POLLIN, 0};
        if (key_fd >= 0)
            fds[n++] = {key_fd, POLLIN, 0};
        if (in_menu && drm.fd() >= 0) {
            drm_i = n;
            fds[n++] = {drm.fd(), POLLIN, 0};
        }
        poll(fds, n, 100);
        bool drm_ready = drm_i >= 0 && (fds[drm_i].revents & POLLIN);

        // web requests
        std::string req_switch;
        bool req_menu;
        {
            std::lock_guard<std::mutex> lk(g_req_mtx);
            req_switch.swap(g_req_switch);
            req_menu = g_req_menu;
            g_req_menu = false;
        }
        if (!req_switch.empty())
            launch(req_switch);
        if (req_menu)
            enter_menu("");

        // board keys: one toggles the menu, the other triggers the app action
        struct input_event ie;
        while (key_fd >= 0 && read(key_fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
            if (ie.type != EV_KEY || ie.value != 1)
                continue;
            if (ie.code == cfg.key_menu) {
                if (in_menu)
                    act.resume();
                else
                    enter_menu("");
            } else if (ie.code == cfg.key_action && !in_menu && sup.running() &&
                       !sup.app().action_url.empty()) {
                post_action(sup.app().action_url);
            }
        }

        TouchEvent ev;
        while (touch.fd() >= 0 && touch.read(ev)) {
            if (ev.type == TouchEvent::DOWN) {
                down_x = ev.x;
                down_y = ev.y;
            }
            if (in_menu) {
                menu_activity = util::mono_ms();
                if (ev.type == TouchEvent::UP) {
                    int dx = ev.x - down_x, dy = ev.y - down_y;
                    if (ignore_until_up)
                        ignore_until_up = false;  // release of the long press that opened the menu
                    else if (std::abs(dx) > (int)sw / 7 && std::abs(dx) > 2 * std::abs(dy))
                        menu.swipe(dx < 0 ? -1 : 1);
                    else
                        menu.tap(ev.x, ev.y);
                }
            }
        }
        // long press in the corner while an app is running
        if (!in_menu && touch.is_down() && touch.x() < CORNER_PX && touch.y() < CORNER_PX &&
            util::mono_ms() - touch.down_since() >= LONG_PRESS_MS) {
            ignore_until_up = true;
            enter_menu("");
        }

        if (!in_menu) {
            sup.poll();
            if (sup.gave_up())
                enter_menu(sup.last_error());
            continue;
        }

        // menu mode
        if (drm_ready)
            drm.handle_event();  // page flip done
        if (util::mono_ms() - menu_activity > cfg.menu_timeout_s * 1000LL && !sup.gave_up()) {
            act.resume();  // opened by accident or forgotten: go back to the app
            continue;
        }
        if (!in_menu)
            continue;
        if ((menu.dirty() || util::mono_ms() - last_render > 1000) && !drm.pending()) {
            menu.render(canvas);
            DrmBuf &b = drm.osd_buf(osd_idx);
            for (int y = 0; y < canvas.rows; y++)
                memcpy((uint8_t *)b.map + (size_t)y * b.pitch, canvas.ptr(y), (size_t)canvas.cols * 4);
            int vid[2] = {-1, -1};
            drm.commit(vid, osd_idx);
            osd_idx = (osd_idx + 1) % OSD_BUFS;
            last_render = util::mono_ms();
        }
    }

    printf("launcher: stopping\n");
    sup.stop();
    if (in_menu) {
        drm.disable_planes();
        drm.deinit();
    }
    web.stop();
    if (web_th.joinable())
        web_th.join();
    wifi.stop();
    unlink(STATUS_PATH);
    return 0;
}
