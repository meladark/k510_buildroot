#pragma once

#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "camera.h"
#include "text.h"

// Periodic and on-demand photos: PHOTOS_DIR/YYYY-MM-DD/HHMMSS.jpg shows all
// cameras stacked (cam0 on top, AI boxes/spots drawn when AI runs), HHMMSS.json
// has detections + spot states, raw/HHMMSS_camN.jpg keeps clean frames.
class Photos {
public:
    int start(PhotoCam *cams[NUM_CAMS]);
    void stop();

    // Takes a photo of all running cameras now (stacked, cam0 on top);
    // returns the path relative to PHOTOS_DIR or "".
    // cam/dets: draw these detections for that camera instead of the current
    // ones (the trigger passes the frame that fired; the photo comes ~1 s later).
    std::string capture(const std::string &reason, int cam = -1, const std::vector<Detection> *dets = nullptr);
    // Current frame as JPEG for the web editor (not stored).
    bool live_jpeg(int cam, int quality, std::vector<unsigned char> &out);

    std::vector<std::string> days();
    std::vector<std::string> files(const std::string &day);
    bool delete_day(const std::string &day);

    // Streams a tar of all photos with from <= day <= to (inclusive, YYYY-MM-DD).
    // sink returns false to abort.
    void write_tar(const std::string &from, const std::string &to,
                   const std::function<bool(const char *, size_t)> &sink);

private:
    void loop();
    void enforce_free_space();
    void annotate(cv::Mat &bgra, int cam, const std::vector<Detection> &dets, const std::vector<Spot> &spots,
                  const Config &cfg);
    TextRenderer text_;

    PhotoCam *cams_[NUM_CAMS] = {nullptr, nullptr};
    std::thread th_;
    std::mutex capture_mtx_;
};

bool valid_day(const std::string &d);
