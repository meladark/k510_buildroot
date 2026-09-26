#pragma once

#include <mutex>
#include <thread>

#include <opencv2/core.hpp>

#include "camera.h"
#include "drm_out.h"
#include "text.h"

// Triple buffering of the ARGB overlay between the OSD painter and the display loop.
class OsdQueue {
public:
    int acquire();           // painter: buffer to draw into
    void publish(int idx);   // painter: buffer is complete
    int take_for_commit();   // display: newest complete buffer or -1
    void commit_failed();
    void on_flip();

private:
    std::mutex m_;
    int ready_ = -1, pending_ = -1, onscreen_ = -1;
};

class Display {
public:
    int start(const CamLayout layout[NUM_CAMS]);
    void stop();
    DrmOut &drm() { return drm_; }

private:
    void video_loop();
    void osd_loop();
    void paint(cv::Mat &canvas);

    DrmOut drm_;
    OsdQueue osdq_;
    CamLayout layout_[NUM_CAMS];
    DisplayCam cams_[NUM_CAMS];
    std::thread video_th_, osd_th_;
    std::vector<Rect> regions_;  // parts of the overlay that get redrawn
    TextRenderer text_;
};
