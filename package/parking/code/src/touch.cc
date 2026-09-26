#include "touch.h"

#include <algorithm>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "util.h"

std::string find_input_device(const char *needle)
{
    for (int i = 0; i < 16; i++) {
        std::string path = "/dev/input/event" + std::to_string(i);
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        char name[128] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        ::close(fd);
        if (strcasestr(name, needle))
            return path;
    }
    return "";
}

bool Touch::open(const char *dev, int screen_w, int screen_h, const TouchCalib &cal)
{
    fd_ = ::open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    sw_ = screen_w;
    sh_ = screen_h;
    cal_ = cal;
    return fd_ >= 0;
}

void Touch::close()
{
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

void Touch::map(int rx, int ry)
{
    if (cal_.swap_xy)
        std::swap(rx, ry);
    int w = cal_.raw_w, h = cal_.raw_h;
    if (cal_.invert_x)
        rx = w - 1 - rx;
    if (cal_.invert_y)
        ry = h - 1 - ry;
    sx_ = std::max(0, std::min(sw_ - 1, rx * sw_ / w));
    sy_ = std::max(0, std::min(sh_ - 1, ry * sh_ / h));
}

bool Touch::read(TouchEvent &ev)
{
    struct input_event ie;
    while (::read(fd_, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
        if (ie.type == EV_ABS) {
            if (ie.code == ABS_MT_POSITION_X || ie.code == ABS_X)
                rx_ = ie.value;
            else if (ie.code == ABS_MT_POSITION_Y || ie.code == ABS_Y)
                ry_ = ie.value;
        } else if (ie.type == EV_KEY && ie.code == BTN_TOUCH) {
            if (ie.value) {
                pending_down_ = true;
            } else if (down_) {
                down_ = false;
                ev.type = TouchEvent::UP;
                ev.x = sx_;
                ev.y = sy_;
                ev.down_ms = down_ms_;
                return true;
            }
        } else if (ie.type == EV_SYN && ie.code == SYN_REPORT) {
            map(rx_, ry_);
            if (pending_down_) {
                pending_down_ = false;
                down_ = true;
                down_ms_ = util::mono_ms();
                ev.type = TouchEvent::DOWN;
                ev.x = sx_;
                ev.y = sy_;
                ev.down_ms = down_ms_;
                return true;
            }
            if (down_) {
                ev.type = TouchEvent::MOVE;
                ev.x = sx_;
                ev.y = sy_;
                ev.down_ms = down_ms_;
                return true;
            }
        }
    }
    return false;
}
