#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "camera.h"

#define VIDEOS_DIR DATA_DIR "/videos"

// Records the photo channel (ISP ds1, NV12) of every running camera to its
// own MP4: hardware H.264 encoder (venc_lib), fragmented MP4 via libavformat,
// so a file cut short by a power loss still plays. Files:
//   VIDEOS_DIR/YYYY-MM-DD/HHMMSS_camN.mp4 + HHMMSS_camN.jpg (first frame)
class VideoRecorder {
public:
    void init(PhotoCam *cams[NUM_CAMS]);
    ~VideoRecorder();
    // Returns the recording name (YYYY-MM-DD/HHMMSS) or "" when nothing records.
    // cams: bit mask of the cameras to record (default all running ones)
    std::string start(int max_seconds, unsigned cams = ~0u);
    void stop();
    bool recording() const { return active_.load(); }
    int64_t started_ms() const { return started_ms_; }
    // Stops a recording that ran past its limit or out of disk; call periodically.
    void tick();

    std::vector<std::string> days();
    // file names of one day (mp4 and posters)
    std::vector<std::string> files(const std::string &day);
    bool remove(const std::string &day, const std::string &name);  // one recording, all cameras

private:
    struct Track;
    PhotoCam *cams_[NUM_CAMS] = {nullptr, nullptr};
    Track *tracks_[NUM_CAMS] = {nullptr, nullptr};
    std::mutex m_;
    std::atomic<bool> active_{false};
    int64_t started_ms_ = 0;
    int max_s_ = 600;
};
