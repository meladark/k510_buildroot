#include "trigger.h"

#include <stdio.h>

#include <algorithm>

#include "photos.h"
#include "state.h"
#include "util.h"
#include "video.h"

void Trigger::tick(Photos &photos, VideoRecorder &video)
{
    if (video.recording()) {
        streak_ = 0;  // one event at a time: wait for the running video to end
        return;
    }
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
    std::lock_guard<std::mutex> lk(g_state.mtx);
    g_state.trigger_last = line;
    g_state.trigger_count++;
    g_state.toast = "Сработал триггер: " + hit_name;
    g_state.toast_until_ms = util::mono_ms() + 3000;
}
