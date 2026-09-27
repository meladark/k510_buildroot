#pragma once

#include <stdint.h>
#include <string>
#include <sys/types.h>
#include <vector>

// One launchable application, described by a *.conf file:
//   name=Парковка и объекты
//   exec=/app/parking/parking /app/parking
//   cwd=/app/parking
//   desc=YOLOv5 on both cameras, parking spots, web UI on :80
//   env=NAME=value            (optional, may repeat)
//   action_url=http://127.0.0.1/api/action         (optional, POSTed on a short press of the action key)
//   action_long_url=http://127.0.0.1/api/video/toggle  (optional, POSTed when it is held)
// Files are read from /app/launcher/apps (shipped) and /root/data/apps (user).
struct App {
    std::string id;    // file name without .conf
    std::string name;
    std::string desc;
    std::string cwd;
    std::vector<std::string> argv;
    std::vector<std::string> env;  // NAME=value
    std::string action_url;
    std::string action_long_url;
    std::string config;  // settings file the touch menu edits (empty: none)
};

std::vector<App> load_apps();

// Runs one app at a time in its own process group and restarts it when it dies.
class Supervisor {
public:
    bool start(const App &app);
    void stop();                 // graceful: SIGINT, SIGTERM, then SIGKILL
    // Call periodically; reaps the child and restarts it when needed.
    void poll();
    bool running() const { return pid_ > 0; }
    const std::string &app_id() const { return app_.id; }
    const App &app() const { return app_; }
    const std::string &last_error() const { return error_; }
    bool gave_up() const { return gave_up_; }

private:
    bool spawn();
    App app_;
    pid_t pid_ = -1;
    bool want_ = false;
    bool gave_up_ = false;
    int64_t restart_at_ = 0;
    std::vector<int64_t> crashes_;
    std::string error_;
};

#define LOG_DIR "/root/data/logs"
