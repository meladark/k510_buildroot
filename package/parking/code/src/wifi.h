#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct WifiNet {
    std::string ssid;
    int signal;  // dBm
    int freq;
    bool secure;
};

struct WifiStatus {
    bool present = false;  // wlan0 exists
    bool ap_mode = false;
    std::string state;     // wpa_state
    std::string ssid;
    std::string ip;
};

// Station setup through wpa_supplicant (wpa_cli), plus a fallback access point
// for first-time setup when the board has no network at all.
// Stateless across processes: AP mode is marked by a flag file, so the launcher
// (which runs the AP fallback loop) and app web UIs can all use this class.
class Wifi {
public:
    void configure(const std::string &ap_ssid, const std::string &ap_psk, int ap_timeout_s);
    // Receives on-screen hints ("" clears), e.g. AP credentials.
    void on_status(std::function<void(const std::string &)> cb) { status_cb_ = cb; }
    void start();  // AP fallback loop; only one process should run it
    void stop();

    std::vector<WifiNet> scan();
    bool connect(const std::string &ssid, const std::string &psk, std::string *err);
    WifiStatus status();

private:
    void loop();
    std::string cli(const std::vector<std::string> &args, int timeout_ms = 5000);
    bool start_ap();
    void stop_ap();
    bool ap_has_clients();

    bool ap_active();
    void set_status(const std::string &msg);

    std::mutex m_;  // serializes wpa_cli sequences
    std::thread th_;
    std::string ap_ssid_ = "K510-Setup", ap_psk_ = "k510setup";
    int ap_timeout_s_ = 60;
    std::function<void(const std::string &)> status_cb_;
};

extern Wifi g_wifi;
