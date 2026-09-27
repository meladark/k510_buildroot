#pragma once

#include <thread>

class Photos;
class VideoRecorder;

class WebServer {
public:
    int start(int port, const char *www_dir, Photos *photos, VideoRecorder *video);
    void stop();

private:
    std::thread th_;
    void *svr_ = nullptr;
};
