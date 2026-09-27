#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

// Plays one MP4 for the touch gallery: software H.264 decode (ffmpeg) in a
// thread, frames scaled to fit a box and paced by their timestamps. The menu
// picks up the newest frame when seq() changes.
class VideoPlayer {
public:
    ~VideoPlayer() { stop(); }
    bool start(const std::string &path, cv::Size box);
    void stop();
    bool finished() const { return done_.load(); }
    uint64_t seq() const { return seq_.load(); }
    bool frame(cv::Mat &bgra);  // copy of the newest frame
    double duration_s() const { return duration_; }

private:
    void loop();
    std::string path_;
    cv::Size box_;
    std::thread th_;
    std::atomic<bool> run_{false}, done_{false};
    std::atomic<uint64_t> seq_{0};
    std::mutex m_;
    cv::Mat last_;
    double duration_ = 0;
};

// Duration of an MP4 in seconds (0 if unknown), without decoding.
double video_duration(const std::string &path);
