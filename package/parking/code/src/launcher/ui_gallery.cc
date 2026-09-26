// Menu screens: camera settings (parking app config) and the photo gallery.
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <map>
#include <unistd.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "util.h"

#define PARKING_CONFIG "/root/data/parking/config.json"
#define PHOTOS "/root/data/parking/photos"

static const cv::Scalar kBg(30, 25, 20, 255), kText(245, 245, 245, 255), kMuted(170, 160, 150, 255);
static const cv::Scalar kPanel(58, 50, 42, 255), kWarn(0, 180, 255, 255);

// ---------------------------------------------------------------- settings

void Menu::load_cam_settings()
{
    cs_ = CamSettings();
    std::string s;
    if (util::read_file(PARKING_CONFIG, s)) {
        rapidjson::Document d;
        d.Parse(s.c_str());
        if (!d.HasParseError() && d.IsObject()) {
            if (d.HasMember("ai_fps") && d["ai_fps"].IsNumber())
                cs_.ai_fps = d["ai_fps"].GetDouble();
            if (d.HasMember("photo_interval_min") && d["photo_interval_min"].IsInt())
                cs_.photo_interval_min = d["photo_interval_min"].GetInt();
            if (d.HasMember("cam_enabled") && d["cam_enabled"].IsArray() && d["cam_enabled"].Size() == 2) {
                cs_.cam[0] = d["cam_enabled"][0].GetBool();
                cs_.cam[1] = d["cam_enabled"][1].GetBool();
            }
        }
    }
    cs_.loaded = true;
}

// Only touches our three keys; everything else in the parking config stays.
void Menu::save_cam_settings()
{
    std::string s;
    rapidjson::Document d;
    if (!util::read_file(PARKING_CONFIG, s) || d.Parse(s.c_str()).HasParseError() || !d.IsObject())
        d.SetObject();
    auto &al = d.GetAllocator();
    auto set_num = [&](const char *k, rapidjson::Value v) {
        if (d.HasMember(k))
            d[k] = v;
        else
            d.AddMember(rapidjson::StringRef(k), v, al);
    };
    set_num("ai_fps", rapidjson::Value((double)cs_.ai_fps));
    set_num("photo_interval_min", rapidjson::Value(cs_.photo_interval_min));
    rapidjson::Value cams(rapidjson::kArrayType);
    cams.PushBack(cs_.cam[0], al).PushBack(cs_.cam[1], al);
    set_num("cam_enabled", std::move(cams));
    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
    d.Accept(w);
    util::mkdirs("/root/data/parking");
    util::write_file_atomic(PARKING_CONFIG, std::string(sb.GetString()) + "\n");
}

void Menu::segmented(cv::Mat &c, int &y, const std::string &title, const std::vector<std::string> &labels,
                     int selected, std::function<void(int)> on_pick)
{
    text_->draw(c, title, cv::Point(4 * u_, y), 4 * u_, kMuted);
    y += 3 * u_;
    int n = labels.size();
    int bw = (w_ - 8 * u_ - (n - 1) * 2 * u_) / n;
    for (int i = 0; i < n; i++)
        button(c, cv::Rect(4 * u_ + i * (bw + 2 * u_), y, bw, 12 * u_), labels[i], i == selected ? 2 : 0,
               [on_pick, i] { on_pick(i); });
    y += 20 * u_;
}

void Menu::layout_settings(cv::Mat &c)
{
    header(c, "Настройки камер");
    if (!cs_.loaded)
        load_cam_settings();
    int y = 20 * u_;

    static const float fps[] = {1, 2, 5, 0};
    int fsel = -1;
    for (int i = 0; i < 4; i++)
        if (cs_.ai_fps == fps[i])
            fsel = i;
    segmented(c, y, "Распознавание, кадров в секунду", {"1", "2", "5", "Макс"}, fsel, [this](int i) {
        cs_.ai_fps = fps[i];
        save_cam_settings();
    });

    int csel = cs_.cam[0] && cs_.cam[1] ? 0 : cs_.cam[0] ? 1 : cs_.cam[1] ? 2 : -1;
    segmented(c, y, "Камеры", {"Обе", "Камера 0", "Камера 1"}, csel, [this](int i) {
        cs_.cam[0] = i != 2;
        cs_.cam[1] = i != 1;
        save_cam_settings();
    });

    static const int iv[] = {0, 5, 15, 30, 60};
    int isel = -1;
    for (int i = 0; i < 5; i++)
        if (cs_.photo_interval_min == iv[i])
            isel = i;
    segmented(c, y, "Автофото", {"Выкл", "5 мин", "15 мин", "30 мин", "60 мин"}, isel, [this](int i) {
        cs_.photo_interval_min = iv[i];
        save_cam_settings();
    });

    text_->draw(c, "Применится, когда вернётесь к приложению.", cv::Point(4 * u_, y), 3 * u_, kMuted);
    y += 5 * u_;
    text_->draw(c, "Меньше кадров ИИ — тише, холоднее и экономнее.", cv::Point(4 * u_, y), 3 * u_, kMuted);

    int half = (w_ - 10 * u_) / 2;
    button(c, cv::Rect(4 * u_, h_ - 20 * u_, half, 13 * u_), "Назад", 0, [this] { screen_ = MAIN; });
    button(c, cv::Rect(6 * u_ + half, h_ - 20 * u_, half, 13 * u_), "Вернуться", 1, [this] { act_.resume(); });
}

// ---------------------------------------------------------------- gallery

void Menu::blit(cv::Mat &c, const cv::Mat &bgr, const cv::Rect &r)
{
    if (bgr.empty())
        return;
    cv::Rect roi = r & cv::Rect(0, 0, c.cols, c.rows);
    if (roi.area() <= 0)
        return;
    cv::Mat bgra;
    cv::cvtColor(bgr(cv::Rect(roi.x - r.x, roi.y - r.y, roi.width, roi.height)), bgra, cv::COLOR_BGR2BGRA);
    bgra.copyTo(c(roi));
}

// Decodes at 1/4 or 1/8 scale inside libjpeg (fast), then fits into w x h.
cv::Mat Menu::thumb(const std::string &path, int w, int h)
{
    std::string key = path + "@" + std::to_string(w) + "x" + std::to_string(h);
    auto it = thumbs_.find(key);
    if (it != thumbs_.end())
        return it->second;
    cv::Mat img = cv::imread(path, w <= 200 ? cv::IMREAD_REDUCED_COLOR_8 : cv::IMREAD_REDUCED_COLOR_4);
    cv::Mat out;
    if (!img.empty()) {
        double s = std::min((double)w / img.cols, (double)h / img.rows);
        cv::resize(img, out, cv::Size(std::max(1, (int)(img.cols * s)), std::max(1, (int)(img.rows * s))), 0, 0,
                   cv::INTER_AREA);
    }
    if (thumbs_.size() > 64)
        thumbs_.clear();
    thumbs_[key] = out;
    return out;
}

static bool is_jpg(const std::string &f)
{
    return f.size() > 4 && f.compare(f.size() - 4, 4, ".jpg") == 0;
}

static int hms_seconds(const std::string &hhmmss)
{
    if (hhmmss.size() < 6)
        return -1;
    return atoi(hhmmss.substr(0, 2).c_str()) * 3600 + atoi(hhmmss.substr(2, 2).c_str()) * 60 +
           atoi(hhmmss.substr(4, 2).c_str());
}

static std::string caption_of(const std::string &key)
{
    if (key.size() < 6)
        return key;
    return key.substr(0, 2) + ":" + key.substr(2, 2) + ":" + key.substr(4, 2) +
           (key.size() > 7 ? " (" + key.substr(7) + ")" : "");
}

void Menu::gal_open_day(const std::string &day)
{
    gal_day_ = day;
    gal_shots_.clear();
    std::string dir = std::string(PHOTOS) + "/" + day;
    auto files = util::list_dir(dir);
    auto raws = util::list_dir(dir + "/raw");
    auto has = [](const std::vector<std::string> &v, const std::string &n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };

    // old separate files: cam0_HHMMSS.jpg / cam1_HHMMSS.jpg, paired within 3 s
    struct Old {
        int cam, sec;
        std::string file, stem;
        bool used;
    };
    std::vector<Old> old;
    for (auto &f : files) {
        if (!is_jpg(f))
            continue;
        std::string stem = f.substr(0, f.size() - 4);
        if (stem.compare(0, 3, "cam") == 0 && stem.size() > 5 && (stem[3] == '0' || stem[3] == '1') && stem[4] == '_') {
            old.push_back(Old{stem[3] - '0', hms_seconds(stem.substr(5)), f, stem.substr(5), false});
            continue;
        }
        Shot s;
        s.key = stem;
        s.stacked = f;
        if (has(files, stem + ".json"))
            s.json = stem + ".json";
        for (int c = 0; c < 2; c++)
            if (has(raws, stem + "_cam" + std::to_string(c) + ".jpg"))
                s.raw[c] = "raw/" + stem + "_cam" + std::to_string(c) + ".jpg";
        gal_shots_.push_back(s);
    }
    for (size_t i = 0; i < old.size(); i++) {
        if (old[i].used)
            continue;
        Shot s;
        s.key = old[i].stem;
        s.cam[old[i].cam] = old[i].file;
        old[i].used = true;
        for (size_t j = 0; j < old.size(); j++) {
            if (!old[j].used && old[j].cam != old[i].cam && std::abs(old[j].sec - old[i].sec) <= 3) {
                s.cam[old[j].cam] = old[j].file;
                old[j].used = true;
                break;
            }
        }
        // json of the old format sits next to each camera file
        std::string j0 = s.cam[0].empty() ? "" : s.cam[0].substr(0, s.cam[0].size() - 4) + ".json";
        if (!j0.empty() && has(files, j0))
            s.json = j0;
        gal_shots_.push_back(s);
    }
    std::sort(gal_shots_.begin(), gal_shots_.end(), [](const Shot &a, const Shot &b) { return a.key > b.key; });
    gal_page_ = 0;
    screen_ = GAL_GRID;
}

// Reads per-camera offsets and a short summary from the json sidecar.
void Menu::gal_parse(Shot &s)
{
    if (s.parsed)
        return;
    s.parsed = true;
    std::string dir = std::string(PHOTOS) + "/" + gal_day_ + "/";
    auto summarize = [](const rapidjson::Value &cam) {
        std::map<std::string, int> counts;
        if (cam.HasMember("detections") && cam["detections"].IsArray())
            for (auto &d : cam["detections"].GetArray())
                if (d.HasMember("name") && d["name"].IsString())
                    counts[d["name"].GetString()]++;
        int total = 0, free_n = 0;
        if (cam.HasMember("spots") && cam["spots"].IsArray())
            for (auto &sp : cam["spots"].GetArray()) {
                total++;
                free_n += sp.HasMember("occupied") && sp["occupied"].IsBool() && !sp["occupied"].GetBool();
            }
        std::string o;
        for (auto &kv : counts)
            o += (o.empty() ? "" : ", ") + kv.first + (kv.second > 1 ? " ×" + std::to_string(kv.second) : "");
        if (o.empty())
            o = "ничего";
        if (total)
            o += " · свободно " + std::to_string(free_n) + "/" + std::to_string(total);
        return o;
    };

    std::vector<std::string> jsons;
    if (!s.json.empty())
        jsons.push_back(s.json);
    if (s.stacked.empty() && !s.cam[1].empty()) {  // old format: second json for cam1
        std::string j1 = s.cam[1].substr(0, s.cam[1].size() - 4) + ".json";
        if (j1 != s.json)
            jsons.push_back(j1);
    }
    std::string parts[2];
    for (auto &jf : jsons) {
        std::string text;
        if (!util::read_file(dir + jf, text))
            continue;
        rapidjson::Document d;
        if (d.Parse(text.c_str()).HasParseError() || !d.IsObject())
            continue;
        if (d.HasMember("cams") && d["cams"].IsArray()) {  // new stacked format
            for (auto &cam : d["cams"].GetArray()) {
                if (!cam.HasMember("cam") || !cam["cam"].IsInt())
                    continue;
                int c = cam["cam"].GetInt();
                if (c < 0 || c > 1)
                    continue;
                if (cam.HasMember("offset_y") && cam["offset_y"].IsInt())
                    s.offset_y[c] = cam["offset_y"].GetInt();
                if (cam.HasMember("height") && cam["height"].IsInt())
                    s.height[c] = cam["height"].GetInt();
                parts[c] = summarize(cam);
            }
        } else if (d.HasMember("cam") && d["cam"].IsInt()) {  // old per-camera format
            int c = d["cam"].GetInt();
            if (c >= 0 && c <= 1)
                parts[c] = summarize(d);
        }
    }
    for (int c = 0; c < 2; c++)
        if (!parts[c].empty())
            s.summary += (s.summary.empty() ? "" : "\n") + std::string("CAM") + std::to_string(c) + ": " + parts[c];
}

// Full-size image of one camera of the current shot (annotated or raw).
cv::Mat Menu::gal_cam_image(int cam, bool raw)
{
    Shot &s = gal_shots_[gal_index_];
    std::string dir = std::string(PHOTOS) + "/" + gal_day_ + "/";
    std::string key = s.key + (raw ? "/raw" : "/ann");
    if (view_key_ != key) {
        view_key_ = key;
        view_img_[0].release();
        view_img_[1].release();
        view_stacked_.release();
        gal_parse(s);
        if (raw) {
            for (int c = 0; c < 2; c++)
                if (!s.raw[c].empty())
                    view_img_[c] = cv::imread(dir + s.raw[c]);
        } else if (!s.stacked.empty()) {
            view_stacked_ = cv::imread(dir + s.stacked);
            if (!view_stacked_.empty()) {
                // split by json offsets; without json assume two equal halves
                bool have = s.offset_y[0] >= 0 || s.offset_y[1] >= 0;
                for (int c = 0; c < 2; c++) {
                    int y = have ? s.offset_y[c] : c * view_stacked_.rows / 2;
                    int h = have ? s.height[c] : view_stacked_.rows / 2;
                    if (y >= 0 && h > 0 && y + h <= view_stacked_.rows)
                        view_img_[c] = view_stacked_(cv::Rect(0, y, view_stacked_.cols, h));
                }
            }
        } else {
            for (int c = 0; c < 2; c++)
                if (!s.cam[c].empty())
                    view_img_[c] = cv::imread(dir + s.cam[c]);
        }
    }
    return view_img_[cam];
}

void Menu::gal_draw_fit(cv::Mat &c, const cv::Mat &img, const cv::Rect &area)
{
    if (img.empty()) {
        cv::rectangle(c, area, kPanel, cv::FILLED);
        text_->draw_centered(c, "нет кадра", area, 4 * u_, kMuted);
        return;
    }
    double s = std::min((double)area.width / img.cols, (double)area.height / img.rows);
    cv::Mat fit;
    cv::resize(img, fit, cv::Size(std::max(1, (int)(img.cols * s)), std::max(1, (int)(img.rows * s))), 0, 0,
               s < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
    blit(c, fit, cv::Rect(area.x + (area.width - fit.cols) / 2, area.y + (area.height - fit.rows) / 2, fit.cols,
                          fit.rows));
}

void Menu::layout_gal_days(cv::Mat &c)
{
    header(c, "Галерея");
    int y = 18 * u_;
    if (gal_days_.empty())
        text_->draw(c, "Фото пока нет", cv::Point(4 * u_, y + 6 * u_), 5 * u_, kMuted);
    const int per_page = std::max(3, (h_ - y - 40 * u_) / (13 * u_));
    int pages = std::max<int>(1, (gal_days_.size() + per_page - 1) / per_page);
    gal_days_page_ = std::min(gal_days_page_, pages - 1);
    for (int i = gal_days_page_ * per_page; i < (int)gal_days_.size() && i < (gal_days_page_ + 1) * per_page; i++) {
        std::string day = gal_days_[i];
        int n = 0;
        for (auto &f : util::list_dir(std::string(PHOTOS) + "/" + day))
            n += is_jpg(f) && f.compare(0, 4, "cam1") != 0;  // old pairs count once
        std::string label = (day == "0000-00-00" ? std::string("без даты") : day) + "   ·   " + std::to_string(n) + " снимков";
        button(c, cv::Rect(4 * u_, y, w_ - 8 * u_, 11 * u_), label, 0, [this, day] { gal_open_day(day); });
        y += 13 * u_;
    }
    int by = h_ - 34 * u_;
    int third = (w_ - 12 * u_) / 3;
    button(c, cv::Rect(4 * u_, by, third, 13 * u_), "‹", 0, [this] { gal_days_page_ = std::max(0, gal_days_page_ - 1); });
    button(c, cv::Rect(6 * u_ + third, by, third, 13 * u_),
           std::to_string(gal_days_page_ + 1) + " / " + std::to_string(pages), 0, nullptr);
    button(c, cv::Rect(8 * u_ + 2 * third, by, third, 13 * u_), "›", 0,
           [this, pages] { gal_days_page_ = std::min(pages - 1, gal_days_page_ + 1); });
    button(c, cv::Rect(4 * u_, by + 16 * u_, w_ - 8 * u_, 12 * u_), "Назад", 0, [this] { screen_ = MAIN; });
}

void Menu::layout_gal_grid(cv::Mat &c)
{
    header(c, gal_day_ == "0000-00-00" ? "Без даты" : gal_day_);
    const int cols = w_ > h_ ? 4 : 3;
    const int gap = 2 * u_;
    const int tw = (w_ - 8 * u_ - (cols - 1) * gap) / cols;
    const int th = tw * 3 / 2;  // a tile shows both cameras stacked
    const int cell_h = th + 6 * u_;
    const int top = 15 * u_;
    const int rows = std::max(1, (h_ - top - 36 * u_) / cell_h);
    const int per_page = rows * cols;
    int pages = std::max<int>(1, (gal_shots_.size() + per_page - 1) / per_page);
    gal_page_ = std::min(gal_page_, pages - 1);
    if (gal_shots_.empty())
        text_->draw(c, "Нет фото", cv::Point(4 * u_, top + 6 * u_), 5 * u_, kMuted);

    std::string dir = std::string(PHOTOS) + "/" + gal_day_ + "/";
    for (int k = 0; k < per_page; k++) {
        int i = gal_page_ * per_page + k;
        if (i >= (int)gal_shots_.size())
            break;
        const Shot &s = gal_shots_[i];
        cv::Rect r(4 * u_ + (k % cols) * (tw + gap), top + (k / cols) * cell_h, tw, th);
        cv::rectangle(c, r, kPanel, cv::FILLED);
        if (!s.stacked.empty()) {
            cv::Mat t = thumb(dir + s.stacked, tw, th);
            if (!t.empty())
                blit(c, t, cv::Rect(r.x + (tw - t.cols) / 2, r.y + (th - t.rows) / 2, t.cols, t.rows));
        } else {
            for (int cam = 0; cam < 2; cam++) {  // old pair: cam0 top, cam1 bottom
                if (s.cam[cam].empty())
                    continue;
                cv::Mat t = thumb(dir + s.cam[cam], tw, th / 2);
                if (!t.empty())
                    blit(c, t, cv::Rect(r.x + (tw - t.cols) / 2, r.y + cam * th / 2 + (th / 2 - t.rows) / 2, t.cols,
                                        t.rows));
            }
        }
        text_->draw(c, caption_of(s.key), cv::Point(r.x, r.br().y + 4 * u_), 3 * u_, kMuted);
        buttons_.push_back(Button{r, "", [this, i] {
                                      gal_index_ = i;
                                      gal_mode_ = 0;
                                      screen_ = GAL_VIEW;
                                  },
                                  0});
    }

    int by = h_ - 32 * u_;
    int third = (w_ - 12 * u_) / 3;
    button(c, cv::Rect(4 * u_, by, third, 13 * u_), "‹", 0, [this] { swipe(+1); });
    button(c, cv::Rect(6 * u_ + third, by, third, 13 * u_), std::to_string(gal_page_ + 1) + " / " + std::to_string(pages),
           0, nullptr);
    button(c, cv::Rect(8 * u_ + 2 * third, by, third, 13 * u_), "›", 0, [this] { swipe(-1); });
    button(c, cv::Rect(4 * u_, by + 16 * u_, w_ - 8 * u_, 12 * u_), "Назад", 0, [this] { screen_ = GAL_DAYS; });
}

void Menu::layout_gal_view(cv::Mat &c)
{
    if (gal_shots_.empty()) {
        screen_ = GAL_GRID;
        layout_gal_grid(c);
        return;
    }
    gal_index_ = std::max(0, std::min<int>(gal_index_, gal_shots_.size() - 1));
    Shot &s = gal_shots_[gal_index_];
    gal_parse(s);
    bool raw_avail = !s.raw[0].empty() || !s.raw[1].empty();
    if (!raw_avail)
        gal_raw_ = false;
    header(c, caption_of(s.key) + "   " + std::to_string(gal_index_ + 1) + "/" + std::to_string(gal_shots_.size()));

    // view mode buttons
    int y = 14 * u_;
    int bw = (w_ - 8 * u_ - 6 * u_) / 4;
    const char *modes[] = {"Обе", "CAM0", "CAM1"};
    for (int m = 0; m < 3; m++)
        button(c, cv::Rect(4 * u_ + m * (bw + 2 * u_), y, bw, 10 * u_), modes[m], gal_mode_ == m ? 2 : 0,
               [this, m] { gal_mode_ = m; });
    button(c, cv::Rect(4 * u_ + 3 * (bw + 2 * u_), y, bw, 10 * u_), gal_raw_ ? "Без рамок" : "С рамками",
           raw_avail ? (gal_raw_ ? 1 : 0) : 3, [this, raw_avail] {
               if (raw_avail)
                   gal_raw_ = !gal_raw_;
           });
    y += 12 * u_;

    // summary from json (one line per camera)
    int lines = 0;
    size_t p = 0;
    while (p < s.summary.size() && lines < 2) {
        size_t e = s.summary.find('\n', p);
        std::string line = s.summary.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (gal_mode_ == 0 || line.compare(0, 4, gal_mode_ == 1 ? "CAM0" : "CAM1") == 0) {
            text_->draw(c, line, cv::Point(4 * u_, y + 4 * u_), 3 * u_, kMuted);
            y += 5 * u_;
            lines++;
        }
        if (e == std::string::npos)
            break;
        p = e + 1;
    }
    y += 2 * u_;

    cv::Rect area(2 * u_, y, w_ - 4 * u_, h_ - y - 34 * u_);
    if (gal_mode_ == 0 && !gal_raw_ && !s.stacked.empty()) {
        gal_cam_image(0, false);  // loads the stacked file
        gal_draw_fit(c, view_stacked_, area);
    } else if (gal_mode_ == 0) {
        // compose both cameras: stacked on portrait, side by side on landscape
        cv::Mat a = gal_cam_image(0, gal_raw_), b = gal_cam_image(1, gal_raw_);
        if (w_ >= h_) {
            gal_draw_fit(c, a, cv::Rect(area.x, area.y, area.width / 2 - u_, area.height));
            gal_draw_fit(c, b, cv::Rect(area.x + area.width / 2 + u_, area.y, area.width / 2 - u_, area.height));
        } else {
            gal_draw_fit(c, a, cv::Rect(area.x, area.y, area.width, area.height / 2 - u_));
            gal_draw_fit(c, b, cv::Rect(area.x, area.y + area.height / 2 + u_, area.width, area.height / 2 - u_));
        }
    } else {
        gal_draw_fit(c, gal_cam_image(gal_mode_ - 1, gal_raw_), area);
    }

    int by = h_ - 32 * u_;
    int third = (w_ - 12 * u_) / 3;
    button(c, cv::Rect(4 * u_, by, third, 13 * u_), "‹", 0, [this] { swipe(+1); });
    button(c, cv::Rect(6 * u_ + third, by, third, 13 * u_), "Удалить", 4, [this] { screen_ = GAL_CONFIRM; });
    button(c, cv::Rect(8 * u_ + 2 * third, by, third, 13 * u_), "›", 0, [this] { swipe(-1); });
    button(c, cv::Rect(4 * u_, by + 16 * u_, w_ - 8 * u_, 12 * u_), "К миниатюрам", 0, [this] { screen_ = GAL_GRID; });
}

void Menu::layout_gal_confirm(cv::Mat &c)
{
    header(c, "Удалить снимок?");
    if (gal_index_ < (int)gal_shots_.size())
        text_->draw(c, gal_day_ + "  " + caption_of(gal_shots_[gal_index_].key), cv::Point(4 * u_, 26 * u_), 5 * u_,
                    kText);
    text_->draw(c, "Удалятся обе камеры, чистые кадры (raw) и разметка.", cv::Point(4 * u_, 34 * u_), 3 * u_, kMuted);
    int half = (w_ - 10 * u_) / 2;
    button(c, cv::Rect(4 * u_, h_ - 20 * u_, half, 13 * u_), "Отмена", 0, [this] { screen_ = GAL_VIEW; });
    button(c, cv::Rect(6 * u_ + half, h_ - 20 * u_, half, 13 * u_), "Удалить", 4, [this] {
        if (gal_index_ < (int)gal_shots_.size()) {
            const Shot &s = gal_shots_[gal_index_];
            std::string dir = std::string(PHOTOS) + "/" + gal_day_ + "/";
            auto rm = [&](const std::string &f) {
                if (!f.empty())
                    unlink((dir + f).c_str());
            };
            auto rm_json = [&](const std::string &jpg) {
                if (!jpg.empty())
                    rm(jpg.substr(0, jpg.size() - 4) + ".json");
            };
            rm(s.stacked);
            rm_json(s.stacked);
            for (int cam = 0; cam < 2; cam++) {
                rm(s.cam[cam]);
                rm_json(s.cam[cam]);
                rm(s.raw[cam]);
            }
            sync();
            gal_shots_.erase(gal_shots_.begin() + gal_index_);
            thumbs_.clear();
            view_key_.clear();
        }
        screen_ = gal_shots_.empty() ? GAL_GRID : GAL_VIEW;
    });
}

void Menu::swipe(int dir)
{
    if (screen_ == GAL_VIEW) {
        gal_index_ = std::max(0, std::min<int>(gal_shots_.size() - 1, gal_index_ - dir));
    } else if (screen_ == GAL_GRID) {
        gal_page_ = std::max(0, gal_page_ - dir);  // clamped in layout
    } else {
        return;
    }
    dirty_ = true;
}
