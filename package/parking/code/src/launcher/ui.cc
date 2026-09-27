#include "ui.h"

#include <algorithm>
#include <cmath>

#include <opencv2/imgproc.hpp>

#include "util.h"

// BGRA colors
static const cv::Scalar kBg(30, 25, 20, 255), kPanel(58, 50, 42, 255), kText(245, 245, 245, 255);
static const cv::Scalar kMuted(170, 160, 150, 255), kAccent(235, 99, 37, 255), kGreen(70, 150, 50, 255);
static const cv::Scalar kRed(50, 50, 200, 255), kKey(80, 70, 60, 255), kWarn(0, 180, 255, 255);

#define SYS_STATUS_PATH "/tmp/k510_status.txt"

void Menu::init(int w, int h, TextRenderer *text, Wifi *wifi, Actions act)
{
    w_ = w;
    h_ = h;
    u_ = std::max(6, std::min(w, h) / 100);
    text_ = text;
    wifi_ = wifi;
    act_ = act;
}

void Menu::open(const std::vector<App> &apps, const std::string &current_app, const std::string &error)
{
    apps_ = apps;
    current_app_ = current_app;
    error_ = error;
    screen_ = MAIN;
    press_ = Press();
    restore_img_.release();
    refresh_status();
    status_t_ = util::mono_ms();
    dirty_ = true;
}

void Menu::button(cv::Mat &c, const cv::Rect &r, const std::string &label, int style, std::function<void()> fn)
{
    cv::Scalar bg = style == 1 ? kAccent : style == 2 ? kGreen : style == 3 ? kKey : style == 4 ? kRed : kPanel;
    cv::rectangle(c, r, bg, cv::FILLED, cv::LINE_AA);
    cv::rectangle(c, r, cv::Scalar(bg[0] + 25, bg[1] + 25, bg[2] + 25, 255), 2, cv::LINE_AA);
    int px = std::min(r.height * 45 / 100, 5 * u_);
    text_->draw_centered(c, label, r, px, kText);
    buttons_.push_back(Button{r, label, fn, style});
}

void Menu::header(cv::Mat &c, const std::string &title)
{
    text_->draw(c, title, cv::Point(4 * u_, 9 * u_), 6 * u_, kText);
    cv::Size sz = text_->measure(clock_, 5 * u_);
    text_->draw(c, clock_, cv::Point(w_ - 4 * u_ - sz.width, 9 * u_), 5 * u_, kMuted);
    cv::line(c, cv::Point(4 * u_, 12 * u_), cv::Point(w_ - 4 * u_, 12 * u_), kPanel, 2);
}

bool Menu::refresh_status()
{
    std::string eth = util::iface_ip("eth0"), wl = util::iface_ip("wlan0");
    std::string net = (eth.empty() ? "" : "Кабель: " + eth + "   ") + (wl.empty() ? "" : "Wi-Fi: " + wl);
    std::string sys;
    util::read_file(SYS_STATUS_PATH, sys);
    while (!sys.empty() && (sys.back() == '\n' || sys.back() == '\r'))
        sys.pop_back();
    std::string clk = util::time_str(util::now_ms(), "%H:%M");
    bool changed = net != net_line_ || sys != sys_line_ || clk != clock_;
    net_line_ = net;
    sys_line_ = sys;
    clock_ = clk;
    return changed;
}

static cv::Rect unite(const cv::Rect &a, const cv::Rect &b)
{
    return a.empty() ? b : b.empty() ? a : (a | b);
}

cv::Rect Menu::render(cv::Mat &c)
{
    const int64_t now = util::mono_ms();
    const cv::Rect full(0, 0, c.cols, c.rows);
    cv::Rect changed;
    if (dirty_.exchange(false)) {
        buttons_.clear();
        c.setTo(kBg);
        switch (screen_) {
        case MAIN: layout_main(c); break;
        case WIFI: layout_wifi(c); break;
        case KEYBOARD: layout_keyboard(c); break;
        case MESSAGE: layout_message(c); break;
        case SETTINGS: layout_settings(c); break;
    case MODELS: layout_models(c); break;
        case GAL_DAYS: layout_gal_days(c); break;
        case GAL_GRID: layout_gal_grid(c); break;
        case GAL_VIEW: layout_gal_view(c); break;
        case GAL_CONFIRM: layout_gal_confirm(c); break;
        }
        changed = full;
        restore_img_.release();
        // left the video, or swiped to another shot: stop playing
        if (playing_ && (screen_ != GAL_VIEW || gal_index_ >= (int)gal_shots_.size() ||
                         gal_shots_[gal_index_].key != play_key_))
            play_stop();
        play_seq_[0] = play_seq_[1] = ~0ull;  // the layout wiped the frames: redraw them
        if (press_.active && !press_.released) {
            // redrawn under the finger (clock tick etc.): keep the press if the button is still there
            const Button *b = hit(press_.at.x, press_.at.y);
            if (!b || b->r != press_.r)
                press_ = Press();
        }
        if (press_.active)
            press_.under = c(press_.r).clone();
    } else {
        if (!restore_img_.empty()) {  // press cancelled: put the plain button back
            restore_img_.copyTo(c(restore_r_));
            changed = restore_r_;
            restore_img_.release();
        }
        if (press_.active) {
            if (press_.under.empty())
                press_.under = c(press_.r).clone();
            else
                press_.under.copyTo(c(press_.r));
        }
    }
    changed = unite(changed, draw_play(c));
    changed = unite(changed, draw_press(c, now));
    changed = unite(changed, draw_busy(c, now));
    return changed;
}

// ---------------------------------------------------------------- touch feedback

static const int kGrowMs = 280;     // ripple spreads over the button while held
static const int kReleaseMs = 120;  // flash after release, then the action runs

const Menu::Button *Menu::hit(int x, int y) const
{
    for (auto &b : buttons_)
        if (b.on_tap && b.r.contains(cv::Point(x, y)))
            return &b;
    return nullptr;
}

void Menu::press(int x, int y)
{
    if (busy_.load() || press_.released || dirty_)
        return;  // dirty: buttons_ belong to the previous screen until the next render
    const Button *b = hit(x, y);
    if (!b)
        return;
    press_ = Press();
    press_.active = true;
    press_.r = b->r & cv::Rect(0, 0, w_, h_);
    press_.at = cv::Point(x, y);
    press_.t0 = util::mono_ms();
    press_.fn = b->on_tap;
}

// Finger slid off the button (with some slack): the tap will not happen.
void Menu::drag(int x, int y)
{
    if (!press_.active || press_.released)
        return;
    cv::Rect slack(press_.r.x - 3 * u_, press_.r.y - 3 * u_, press_.r.width + 6 * u_, press_.r.height + 6 * u_);
    if (!slack.contains(cv::Point(x, y)))
        cancel_press();
}

void Menu::release(int x, int y)
{
    drag(x, y);
    if (!press_.active || press_.released)
        return;
    press_.released = true;
    press_.t_up = util::mono_ms();
}

void Menu::cancel_press()
{
    if (!press_.active || press_.released)
        return;
    if (!press_.under.empty()) {
        restore_r_ = press_.r;
        restore_img_ = press_.under;
    }
    press_ = Press();
}

bool Menu::tick(int64_t now)
{
    if (press_.released && now - press_.t_up >= kReleaseMs) {
        auto fn = press_.fn;
        press_ = Press();
        if (fn)
            fn();
        dirty_ = true;
    }
    if (now - status_t_ >= 1000) {
        status_t_ = now;
        if (refresh_status())
            dirty_ = true;
    }
    return dirty_ || !restore_img_.empty() || animating();
}

bool Menu::animating() const
{
    if (playing_)
        return true;  // new video frames; draw_play copies only when one arrived
    int64_t now = util::mono_ms();
    if (press_.active && (press_.released || press_.under.empty() || now - press_.t0 < kGrowMs + 40))
        return true;
    return busy_.load() && now - busy_frame_ >= 66;  // spinner at ~15 fps
}

static float ease_out(float t)
{
    t = std::max(0.f, std::min(1.f, t));
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

// Lighter button, a ripple from the finger and an accent outline; on release
// the whole button flashes before the action runs.
cv::Rect Menu::draw_press(cv::Mat &c, int64_t now)
{
    if (!press_.active)
        return cv::Rect();
    cv::Mat roi = c(press_.r);
    const cv::Point o = press_.at - press_.r.tl();
    const int w = roi.cols, h = roi.rows;
    float reach = std::sqrt((float)std::max(o.x, w - o.x) * std::max(o.x, w - o.x) +
                            (float)std::max(o.y, h - o.y) * std::max(o.y, h - o.y));
    float grow = ease_out((now - press_.t0) / (float)kGrowMs);
    float light = 0.12f, ripple = 0.16f;
    if (press_.released) {
        float u = (now - press_.t_up) / (float)kReleaseMs;
        grow = 1;
        light = 0.30f - 0.12f * std::min(1.f, u);
    }
    roi.convertTo(roi, -1, 1 - light, 255 * light);
    int rad = (int)(reach * (0.25f + 0.75f * grow));
    if (rad > 0 && !press_.released) {
        cv::Mat tmp = roi.clone();
        cv::circle(tmp, o, rad, cv::Scalar(255, 255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::addWeighted(roi, 1 - ripple, tmp, ripple, 0, roi);
    }
    int t = std::max(2, u_ / 2);
    cv::rectangle(roi, cv::Rect(t / 2, t / 2, w - t, h - t), kAccent, t);
    return press_.r;
}

// "Searching…" panel with a spinning arc.
cv::Rect Menu::draw_busy(cv::Mat &c, int64_t now)
{
    if (!busy_.load())
        return cv::Rect();
    busy_frame_ = now;
    cv::Rect r(w_ / 2 - 32 * u_, h_ / 2 - 8 * u_, 64 * u_, 16 * u_);
    cv::rectangle(c, r, kPanel, cv::FILLED);
    cv::rectangle(c, r, kKey, 2);
    cv::Point ctr(r.x + 9 * u_, r.y + r.height / 2);
    int rad = 4 * u_, th = std::max(3, u_ * 6 / 10);
    cv::circle(c, ctr, rad, kKey, th, cv::LINE_AA);
    double a = (now % 1000) * 360.0 / 1000;
    cv::ellipse(c, ctr, cv::Size(rad, rad), a, 0, 100, kAccent, th, cv::LINE_AA);
    int px = 5 * u_;
    text_->draw(c, screen_ == MESSAGE ? "Подключение…" : "Поиск сетей…",
                cv::Point(r.x + 17 * u_, r.y + (r.height + px * 72 / 100) / 2), px, kText);
    return r;
}

// ---------------------------------------------------------------- main

void Menu::layout_main(cv::Mat &c)
{
    header(c, "K510");
    int y = 18 * u_;
    text_->draw(c, net_line_.empty() ? "Сети нет" : net_line_, cv::Point(4 * u_, y), 4 * u_, kMuted);
    y += 6 * u_;
    const std::string &sys = sys_line_;
    if (!error_.empty()) {
        text_->draw(c, error_, cv::Point(4 * u_, y), 3 * u_, kWarn);
        y += 5 * u_;
    }
    if (!sys.empty()) {
        text_->draw(c, sys, cv::Point(4 * u_, y), 3 * u_, kWarn);
        y += 5 * u_;
    }
    y += 3 * u_;
    text_->draw(c, "Приложения", cv::Point(4 * u_, y), 4 * u_, kMuted);
    y += 3 * u_;

    const int bh = std::min(14 * u_, (h_ - y - 52 * u_) / std::max<int>(1, apps_.size()) - 2 * u_);
    for (auto &a : apps_) {
        bool cur = a.id == current_app_;
        std::string id = a.id;
        button(c, cv::Rect(4 * u_, y, w_ - 8 * u_, bh), (cur ? "● " : "") + a.name, cur ? 2 : 0,
               [this, id] { act_.launch(id); });
        y += bh + 2 * u_;
    }
    if (apps_.empty()) {
        text_->draw(c, "Нет приложений в /app/launcher/apps", cv::Point(4 * u_, y + 5 * u_), 4 * u_, kWarn);
        y += 10 * u_;
    }

    int by = h_ - 48 * u_;
    int half = (w_ - 10 * u_) / 2;
    button(c, cv::Rect(4 * u_, by, half, 12 * u_), "Настройки камер", 0, [this] {
        load_cam_settings();
        screen_ = SETTINGS;
    });
    button(c, cv::Rect(6 * u_ + half, by, half, 12 * u_), "Галерея", 0, [this] {
        gal_days_.clear();
        // days with photos or videos, newest first
        for (const char *root : {"/root/data/parking/photos", "/root/data/parking/videos"})
            for (auto &d : util::list_dir(root))
                if (d.size() == 10 && d[4] == '-' && util::is_dir(std::string(root) + "/" + d) &&
                    std::find(gal_days_.begin(), gal_days_.end(), d) == gal_days_.end())
                    gal_days_.push_back(d);
        std::sort(gal_days_.rbegin(), gal_days_.rend());
        gal_days_page_ = 0;
        gal_day_counts_.clear();
        screen_ = GAL_DAYS;
    });
    by += 14 * u_;
    button(c, cv::Rect(4 * u_, by, half, 13 * u_), "Wi-Fi", 0, [this] {
        screen_ = WIFI;
        {
            std::lock_guard<std::mutex> lk(m_);
            status_ = wifi_->status();
        }
        if (nets_.empty())
            start_scan();
    });
    button(c, cv::Rect(6 * u_ + half, by, half, 13 * u_), "Вернуться", 1, [this] { act_.resume(); });
    button(c, cv::Rect(4 * u_, by + 16 * u_, w_ - 8 * u_, 11 * u_), "Перезагрузить плату", 4, [this] {
        msg_title_ = "Перезагрузка";
        msg_text_ = "Плата перезагружается…";
        screen_ = MESSAGE;
        act_.reboot();
    });
}

// ---------------------------------------------------------------- wifi

void Menu::start_scan()
{
    if (worker_.joinable())
        worker_.join();
    busy_ = true;
    worker_ = std::thread([this] {
        auto nets = wifi_->scan();
        auto st = wifi_->status();
        std::lock_guard<std::mutex> lk(m_);
        nets_ = nets;
        status_ = st;
        net_page_ = 0;
        busy_ = false;
        dirty_ = true;
    });
}

void Menu::start_connect(const std::string &ssid, const std::string &psk)
{
    if (worker_.joinable())
        worker_.join();
    msg_title_ = "Wi-Fi";
    msg_text_ = "";
    screen_ = MESSAGE;
    busy_ = true;
    worker_ = std::thread([this, ssid, psk] {
        std::string err;
        bool ok = wifi_->connect(ssid, psk, &err);
        auto st = wifi_->status();
        std::lock_guard<std::mutex> lk(m_);
        status_ = st;
        msg_title_ = ok ? "Подключено" : "Ошибка";
        msg_text_ = ok ? ssid + (st.ip.empty() ? "" : ", IP " + st.ip) : err;
        busy_ = false;
        dirty_ = true;
    });
}

void Menu::layout_wifi(cv::Mat &c)
{
    header(c, "Wi-Fi");
    std::lock_guard<std::mutex> lk(m_);
    int y = 18 * u_;
    std::string st;
    if (!status_.present)
        st = "Wi-Fi адаптер не найден";
    else if (status_.ap_mode)
        st = "Режим точки доступа для настройки";
    else if (status_.state == "COMPLETED")
        st = "Подключено: " + status_.ssid + (status_.ip.empty() ? "" : "  " + status_.ip);
    else
        st = "Не подключено";
    text_->draw(c, st, cv::Point(4 * u_, y), 4 * u_, kMuted);
    y += 5 * u_;

    const int per_page = std::max(3, (h_ - y - 40 * u_) / (13 * u_));
    int pages = std::max<int>(1, (nets_.size() + per_page - 1) / per_page);
    net_page_ = std::min(net_page_, pages - 1);
    for (int i = net_page_ * per_page; i < (int)nets_.size() && i < (net_page_ + 1) * per_page; i++) {
        const WifiNet n = nets_[i];
        std::string label = n.ssid + (n.secure ? "  · WPA" : "") + "   " + std::to_string(n.signal) + " dBm";
        button(c, cv::Rect(4 * u_, y, w_ - 8 * u_, 11 * u_), label, n.ssid == status_.ssid ? 2 : 0, [this, n] {
            if (n.secure) {
                kb_ssid_ = n.ssid;
                kb_text_.clear();
                kb_layer_ = 0;
                kb_show_ = false;
                screen_ = KEYBOARD;
            } else {
                start_connect(n.ssid, "");
            }
        });
        y += 13 * u_;
    }
    if (nets_.empty() && !busy_.load())
        text_->draw(c, "Сети не найдены", cv::Point(4 * u_, y + 6 * u_), 4 * u_, kMuted);

    int by = h_ - 34 * u_;
    int third = (w_ - 12 * u_) / 3;
    button(c, cv::Rect(4 * u_, by, third, 13 * u_), "‹", 0, [this] { net_page_ = std::max(0, net_page_ - 1); });
    button(c, cv::Rect(6 * u_ + third, by, third, 13 * u_), std::to_string(net_page_ + 1) + " / " + std::to_string(pages),
           0, nullptr);
    button(c, cv::Rect(8 * u_ + 2 * third, by, third, 13 * u_), "›", 0, [this, pages] {
        net_page_ = std::min(pages - 1, net_page_ + 1);
    });
    int half = (w_ - 10 * u_) / 2;
    button(c, cv::Rect(4 * u_, by + 16 * u_, half, 12 * u_), "Назад", 0, [this] { screen_ = MAIN; });
    button(c, cv::Rect(6 * u_ + half, by + 16 * u_, half, 12 * u_), "Обновить", 1, [this] { start_scan(); });
}

// ---------------------------------------------------------------- keyboard

void Menu::layout_keyboard(cv::Mat &c)
{
    header(c, "Пароль");
    text_->draw(c, "Сеть: " + kb_ssid_, cv::Point(4 * u_, 18 * u_), 4 * u_, kMuted);

    cv::Rect field(4 * u_, 22 * u_, w_ - 8 * u_, 12 * u_);
    cv::rectangle(c, field, cv::Scalar(20, 16, 12, 255), cv::FILLED);
    cv::rectangle(c, field, kAccent, 2);
    std::string shown;
    if (kb_show_) {
        shown = kb_text_;
    } else {
        for (size_t i = 0; i < kb_text_.size(); i++)
            shown += "•";
    }
    text_->draw(c, shown + "|", cv::Point(field.x + 2 * u_, field.y + field.height * 68 / 100), 5 * u_, kText);
    text_->draw(c, std::to_string(kb_text_.size()) + " симв.", cv::Point(4 * u_, field.br().y + 5 * u_), 3 * u_, kMuted);

    static const char *lower[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const char *upper[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const char *symbols[] = {"!@#$%^&*()", "-_=+[]{};:", "'\"/\\|<>?,.", "`~"};
    const char **rows = kb_layer_ == 0 ? lower : kb_layer_ == 1 ? upper : symbols;

    // keys fill the lower part of the screen
    const int kw = (w_ - 4 * u_) / 10;
    const int kh = std::min(kw * 12 / 10, (h_ - field.br().y - 30 * u_) / 6);
    int y = h_ - 6 * kh - 8 * u_;
    for (int r = 0; r < 4; r++) {
        std::u32string row = utf8_to_u32(rows[r]);
        int n = row.size();
        int extra = (r == 3) ? 2 : 0;  // shift + backspace on the last row
        int x = 2 * u_ + (10 - n - extra * 3 / 2) * kw / 2;
        if (r == 3) {
            button(c, cv::Rect(2 * u_, y, kw * 3 / 2 - u_ / 2, kh - u_), kb_layer_ == 2 ? "abc" : "⇧",
                   kb_layer_ == 1 ? 1 : 3, [this] { kb_layer_ = kb_layer_ == 0 ? 1 : 0; });
            x = 2 * u_ + kw * 3 / 2;
        }
        for (int i = 0; i < n; i++) {
            // back to utf-8 for the label and the typed text
            std::string s;
            char32_t cp = row[i];
            if (cp < 0x80) s += (char)cp;
            else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
            else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
            button(c, cv::Rect(x, y, kw - u_, kh - u_), s, 3, [this, s] {
                if (kb_text_.size() < 63)
                    kb_text_ += s;
                if (kb_layer_ == 1)
                    kb_layer_ = 0;  // one-shot shift
            });
            x += kw;
        }
        if (r == 3)
            button(c, cv::Rect(w_ - 2 * u_ - kw * 3 / 2, y, kw * 3 / 2 - u_, kh - u_), "⌫", 3, [this] {
                if (!kb_text_.empty())
                    kb_text_.pop_back();  // password is ASCII-only (wpa_supplicant)
            });
        y += kh;
    }
    // space row
    button(c, cv::Rect(2 * u_, y, kw * 2 - u_, kh - u_), kb_layer_ == 2 ? "abc" : "#+=", 3,
           [this] { kb_layer_ = kb_layer_ == 2 ? 0 : 2; });
    button(c, cv::Rect(2 * u_ + kw * 2, y, kw * 6 - u_, kh - u_), "пробел", 3, [this] {
        if (kb_text_.size() < 63)
            kb_text_ += " ";
    });
    button(c, cv::Rect(2 * u_ + kw * 8, y, kw * 2 - u_, kh - u_), kb_show_ ? "скрыть" : "показать", 3,
           [this] { kb_show_ = !kb_show_; });
    y += kh + u_;
    int half = (w_ - 6 * u_) / 2;
    button(c, cv::Rect(2 * u_, y, half, kh), "Отмена", 0, [this] { screen_ = WIFI; });
    button(c, cv::Rect(4 * u_ + half, y, half, kh), "Подключить", 1, [this] {
        if (kb_text_.size() < 8) {
            msg_title_ = "Пароль";
            msg_text_ = "Нужно минимум 8 символов";
            screen_ = MESSAGE;
            return;
        }
        start_connect(kb_ssid_, kb_text_);
    });
}

// ---------------------------------------------------------------- message

void Menu::layout_message(cv::Mat &c)
{
    header(c, msg_title_);
    std::string text;
    {
        std::lock_guard<std::mutex> lk(m_);
        text = msg_text_;
    }
    // naive word wrap
    int y = 26 * u_, px = 5 * u_;
    std::string line;
    auto flush = [&] {
        text_->draw(c, line, cv::Point(4 * u_, y), px, kText);
        y += px * 3 / 2;
        line.clear();
    };
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find(' ', p);
        std::string word = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() && text_->measure(cand, px).width > w_ - 8 * u_)
            flush(), line = word;
        else
            line = cand;
        if (e == std::string::npos)
            break;
        p = e + 1;
    }
    if (!line.empty())
        flush();
    if (!busy_.load())
        button(c, cv::Rect(4 * u_, h_ - 20 * u_, w_ - 8 * u_, 13 * u_), "OK", 1, [this] {
            screen_ = msg_title_ == "Пароль" ? KEYBOARD : WIFI;
        });
}
