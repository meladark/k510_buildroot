#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// Notifications to a Gopac webhook (POST /api/v1/notify, see Gopac
// docs/webhook.md): text plus one photo or video, multipart, the file streamed
// from disk. Used by the trigger app; settings (webhook_*) come from the app
// config at send time, so they can be changed while it runs. Sending happens
// in a thread: the trigger never waits for the network.
class Webhook {
public:
    void start();
    void stop();
    // media: "photo" / "video" / "" and the file path
    void send(const std::string &text, const std::string &media = "", const std::string &path = "");
    // One POST right now (for the settings "test" button); returns the HTTP status or -1.
    int send_now(const std::string &text, std::string *err);

private:
    struct Job {
        std::string text, media, path;
    };
    int post(const Job &j, std::string *err);
    void loop();
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    bool run_ = false;
};

extern Webhook g_webhook;
