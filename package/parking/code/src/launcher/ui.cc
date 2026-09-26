#include "ui.h"

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
    std::string t = util::time_str(util::now_ms(), "%H:%M");
    cv::Size sz = text_->measure(t, 5 * u_);
    text_->draw(c, t, cv::Point(w_ - 4 * u_ - sz.width, 9 * u_), 5 * u_, kMuted);
    cv::line(c, cv::Point(4 * u_, 12 * u_), cv::Point(w_ - 4 * u_, 12 * u_), kPanel, 2);
}

void Menu::render(cv::Mat &c)
{
    buttons_.clear();
    c.setTo(kBg);
    switch (screen_) {
    case MAIN: layout_main(c); break;
    case WIFI: layout_wifi(c); break;
    case KEYBOARD: layout_keyboard(c); break;
    case MESSAGE: layout_message(c); break;
    case SETTINGS: layout_settings(c); break;
    case GAL_DAYS: layout_gal_days(c); break;
    case GAL_GRID: layout_gal_grid(c); break;
    case GAL_VIEW: layout_gal_view(c); break;
    case GAL_CONFIRM: layout_gal_confirm(c); break;
    }
    if (busy_.load()) {
        cv::Rect r(w_ / 2 - 25 * u_, h_ / 2 - 8 * u_, 50 * u_, 16 * u_);
        cv::rectangle(c, r, kPanel, cv::FILLED);
        text_->draw_centered(c, screen_ == MESSAGE ? "Подключение…" : "Поиск сетей…", r, 5 * u_, kText);
    }
    dirty_ = false;
}

void Menu::tap(int x, int y)
{
    if (busy_.load())
        return;
    for (auto &b : buttons_) {
        if (b.r.contains(cv::Point(x, y)) && b.on_tap) {
            auto fn = b.on_tap;  // buttons_ is rebuilt by the handler's re-render
            fn();
            dirty_ = true;
            return;
        }
    }
}

// ---------------------------------------------------------------- main

void Menu::layout_main(cv::Mat &c)
{
    header(c, "K510");
    int y = 18 * u_;
    std::string eth = util::iface_ip("eth0"), wl = util::iface_ip("wlan0");
    std::string net = (eth.empty() ? "" : "Кабель: " + eth + "   ") + (wl.empty() ? "" : "Wi-Fi: " + wl);
    text_->draw(c, net.empty() ? "Сети нет" : net, cv::Point(4 * u_, y), 4 * u_, kMuted);
    y += 6 * u_;
    std::string sys;
    util::read_file(SYS_STATUS_PATH, sys);
    while (!sys.empty() && (sys.back() == '\n' || sys.back() == '\r'))
        sys.pop_back();
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
        for (auto &d : util::list_dir("/root/data/parking/photos"))
            if (d.size() == 10 && d[4] == '-' && util::is_dir("/root/data/parking/photos/" + d))
                gal_days_.insert(gal_days_.begin(), d);  // newest first
        gal_days_page_ = 0;
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
