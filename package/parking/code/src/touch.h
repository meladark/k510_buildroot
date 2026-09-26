#pragma once

#include <stdint.h>
#include <string>

struct TouchEvent {
    enum Type { NONE, DOWN, MOVE, UP } type = NONE;
    int x = 0, y = 0;         // screen pixels
    int64_t down_ms = 0;      // monotonic time of the touch-down
};

// Calibration measured on the K510 CRB 1080x1920 panel (GT911): the chip reports
// a 720x1280 grid rotated by 180 degrees although the driver advertises 1080x1920.
struct TouchCalib {
    int raw_w = 720, raw_h = 1280;
    bool invert_x = true, invert_y = true, swap_xy = false;
};

class Touch {
public:
    bool open(const char *dev, int screen_w, int screen_h, const TouchCalib &cal = TouchCalib());
    void close();
    int fd() const { return fd_; }
    // Reads pending input; returns true and fills ev when a touch event completed.
    bool read(TouchEvent &ev);
    bool is_down() const { return down_; }
    int64_t down_since() const { return down_ms_; }
    int x() const { return sx_; }
    int y() const { return sy_; }

private:
    void map(int rx, int ry);
    int fd_ = -1;
    int sw_ = 1080, sh_ = 1920;
    TouchCalib cal_;
    int rx_ = 0, ry_ = 0, sx_ = 0, sy_ = 0;
    bool down_ = false, moved_ = false, pending_down_ = false;
    int64_t down_ms_ = 0;
};

// Finds the first /dev/input/eventN whose name contains `needle`.
std::string find_input_device(const char *needle);
