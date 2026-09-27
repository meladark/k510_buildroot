#include "trigger.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "photos.h"
#include "state.h"
#include "util.h"
#include "video.h"
#include "webhook.h"

void Trigger::notify_video()
{
    const int64_t kMax = 45 * 1000 * 1000;  // Telegram takes 50 MB per request
    int64_t size = util::file_size(pending_video_);
    if (size > 0 && size <= kMax) {
        g_webhook.send(pending_text_, "video", pending_video_);
    } else if (size > kMax) {
        std::string ip = util::iface_ip("wlan0").empty() ? util::iface_ip("eth0") : util::iface_ip("wlan0");
        std::string rel = pending_video_.substr(strlen(VIDEOS_DIR));
        g_webhook.send(pending_text_ + "\nВидео " + std::to_string(size / 1000000) +
                       " МБ, слишком большое для Telegram: http://" + ip + "/videos" + rel);
    }
    pending_video_.clear();
}

void Trigger::tick(Photos &photos, VideoRecorder &video)
{
    if (video.recording()) {
        streak_ = 0;  // one event at a time: wait for the running video to end
        return;
    }
    if (!pending_video_.empty())
        notify_video();
    Config cfg;
    uint64_t frames;
    std::string hit_name;
    float hit_score = 0;
    std::vector<Detection> dets;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        cfg = g_state.cfg;
        const CamState &cs = g_state.cams[cfg.trigger_ai_cam];
        frames = cs.frames;
        if (frames == last_frame_)
            return;  // no new AI result yet
        dets = cs.dets;
        for (auto &d : cs.dets) {
            if (d.score >= cfg.obj_thresh && d.score > hit_score &&
                std::find(cfg.trigger_classes.begin(), cfg.trigger_classes.end(), d.name) != cfg.trigger_classes.end()) {
                hit_name = d.name;
                hit_score = d.score;
            }
        }
    }
    last_frame_ = frames;
    streak_ = hit_name.empty() ? 0 : streak_ + 1;
    if (streak_ < cfg.trigger_confirm)
        return;
    streak_ = 0;

    // video of the other camera first (the photo takes a second or two), then the
    // photo with the detections of the frame that fired
    int rec_cam = cfg.trigger_ai_cam == 0 ? 1 : 0;
    std::string name = video.start(cfg.trigger_video_s, 1u << rec_cam);
    if (name.empty())  // the other camera is off: record the AI camera instead
        name = video.start(cfg.trigger_video_s, 1u << cfg.trigger_ai_cam);
    std::string photo = photos.capture("trigger:" + hit_name, cfg.trigger_ai_cam, &dets);
    char line[160];
    snprintf(line, sizeof(line), "%s %.2f  %s", hit_name.c_str(), hit_score,
             util::time_str(util::now_ms(), "%H:%M:%S").c_str());
    printf("trigger: %s, photo %s, video %s\n", line, photo.c_str(), name.c_str());

    // Telegram via the webhook: the photo now, the video when it is finished
    if (cfg.webhook_enabled) {
        char text[256];
        snprintf(text, sizeof(text), "Сработал триггер: %s (%.0f%%)\nКамера %d · видео %d с с камеры %d",
                 hit_name.c_str(), hit_score * 100, cfg.trigger_ai_cam, cfg.trigger_video_s, rec_cam);
        if (!photo.empty())
            g_webhook.send(text, "photo", std::string(PHOTOS_DIR) + "/" + photo);
        else
            g_webhook.send(text);
        if (cfg.webhook_video && !name.empty()) {
            // name = day/HHMMSS; the camera that actually recorded
            std::string base = std::string(VIDEOS_DIR) + "/" + name + "_cam";
            pending_video_ = util::file_size(base + std::to_string(rec_cam) + ".mp4") >= 0
                                 ? base + std::to_string(rec_cam) + ".mp4"
                                 : base + std::to_string(cfg.trigger_ai_cam) + ".mp4";
            pending_text_ = "Видео: " + hit_name + ", " + util::time_str(util::now_ms(), "%H:%M:%S");
        }
    }
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.trigger_last = line;
    g_state.trigger_count++;
    g_state.toast = "Сработал триггер: " + hit_name;
    g_state.toast_until_ms = util::mono_ms() + 3000;
}
