#include "webhook.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../third_party/httplib.h"
#include "state.h"
#include "util.h"

Webhook g_webhook;

void Webhook::start()
{
    std::lock_guard<std::mutex> lk(m_);
    if (run_)
        return;
    run_ = true;
    th_ = std::thread(&Webhook::loop, this);
}

void Webhook::stop()
{
    {
        std::lock_guard<std::mutex> lk(m_);
        run_ = false;
    }
    cv_.notify_all();
    if (th_.joinable())
        th_.join();
}

void Webhook::send(const std::string &text, const std::string &media, const std::string &path)
{
    {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() >= 20)
            q_.pop_front();  // the service is down for long: keep the newest
        q_.push_back(Job{text, media, path});
    }
    cv_.notify_all();
}

int Webhook::send_now(const std::string &text, std::string *err)
{
    return post(Job{text, "", ""}, err);
}

void Webhook::loop()
{
    std::unique_lock<std::mutex> lk(m_);
    while (true) {
        cv_.wait(lk, [&] { return !run_ || !q_.empty(); });
        if (!run_)
            break;
        Job j = q_.front();
        q_.pop_front();
        lk.unlock();
        std::string err;
        int st = post(j, &err);
        if (st < 0) {  // network: one more try a bit later
            sleep(10);
            st = post(j, &err);
        }
        if (st != 200)
            fprintf(stderr, "webhook: %s -> %d %s\n", j.media.empty() ? "text" : j.media.c_str(), st, err.c_str());
        else
            printf("webhook: sent %s\n", j.media.empty() ? "text" : j.media.c_str());
        lk.lock();
    }
}

int Webhook::post(const Job &j, std::string *err)
{
    std::string url, token, device;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (!g_state.cfg.webhook_enabled) {
            *err = "disabled";
            return 0;
        }
        url = g_state.cfg.webhook_url;
        token = g_state.cfg.webhook_token;
        device = g_state.cfg.webhook_device_id;
    }
    // http://host[:port][/path]; https needs TLS, which this build has not
    if (url.compare(0, 7, "http://") != 0) {
        *err = "url must start with http://";
        return -2;
    }
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/api/v1/notify" : rest.substr(slash);
    if (path == "/")
        path = "/api/v1/notify";
    size_t colon = hostport.find(':');
    std::string host = hostport.substr(0, colon);
    int port = colon == std::string::npos ? 80 : atoi(hostport.c_str() + colon + 1);

    httplib::Client cli(host, port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(60, 0);
    cli.set_write_timeout(60, 0);
    httplib::Headers headers = {{"Authorization", "Bearer " + token}};
    httplib::MultipartFormDataItems items = {{"device_id", device, "", ""}};
    if (!j.text.empty())
        items.push_back({"text", j.text, "", ""});
    httplib::MultipartFormDataProviderItems files;
    FILE *f = nullptr;
    if (!j.media.empty()) {
        f = fopen(j.path.c_str(), "rb");
        if (!f) {
            *err = "cannot open " + j.path;
            return -2;
        }
        // streamed from disk: videos are tens of MB, the board has little RAM
        files.push_back({j.media,
                         [f](size_t, httplib::DataSink &sink) {
                             char buf[65536];
                             size_t n = fread(buf, 1, sizeof(buf), f);
                             if (n > 0)
                                 return sink.write(buf, n);
                             sink.done();
                             return true;
                         },
                         j.path.substr(j.path.rfind('/') + 1), j.media == "video" ? "video/mp4" : "image/jpeg"});
    }
    auto res = files.empty() ? cli.Post(path, headers, items) : cli.Post(path, headers, items, files);
    if (f)
        fclose(f);
    if (!res) {
        *err = httplib::to_string(res.error());
        return -1;
    }
    if (res->status != 200)
        *err = res->body.substr(0, 200);
    return res->status;
}
