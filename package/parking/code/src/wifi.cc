#include "wifi.h"

#include <algorithm>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "state.h"
#include "util.h"

Wifi g_wifi;

#define IFACE "wlan0"
#define AP_FLAG "/var/run/wlan0.ap"  // tells /etc/wpa_action.sh to leave DHCP alone
#define AP_IP "192.168.4.1"
#define UDHCPD_CONF "/tmp/udhcpd-ap.conf"
#define UDHCPD_PID "/tmp/udhcpd-ap.pid"
#define AP_WINDOW_S 600  // then retry the saved networks if nobody joined the AP

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool iface_exists()
{
    return util::is_dir("/sys/class/net/" IFACE);
}

// wpa_cli prints SSIDs with printf-style escapes
static std::string unescape(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            o += s[i];
            continue;
        }
        char c = s[++i];
        if (c == 'x' && i + 2 < s.size()) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (c == 'n') {
            o += '\n';
        } else if (c == 't') {
            o += '\t';
        } else if (c == 'e') {
            o += '\033';
        } else {
            o += c;
        }
    }
    return o;
}

static std::string kv(const std::string &status, const std::string &key)
{
    size_t p = 0;
    while (p < status.size()) {
        size_t e = status.find('\n', p);
        std::string line = status.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (line.compare(0, key.size() + 1, key + "=") == 0)
            return trim(line.substr(key.size() + 1));
        if (e == std::string::npos)
            break;
        p = e + 1;
    }
    return "";
}

void Wifi::configure(const std::string &ap_ssid, const std::string &ap_psk, int ap_timeout_s)
{
    std::lock_guard<std::mutex> lk(m_);
    ap_ssid_ = ap_ssid;
    ap_psk_ = ap_psk;
    ap_timeout_s_ = ap_timeout_s;
}

void Wifi::set_status(const std::string &msg)
{
    if (status_cb_)
        status_cb_(msg);
}

bool Wifi::ap_active()
{
    return util::file_size(AP_FLAG) >= 0;
}

std::string Wifi::cli(const std::vector<std::string> &args, int timeout_ms)
{
    std::vector<std::string> argv = {"wpa_cli", "-i", IFACE};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string out;
    util::run(argv, &out, timeout_ms);
    return trim(out);
}

void Wifi::start()
{
    th_ = std::thread(&Wifi::loop, this);
}

void Wifi::stop()
{
    if (th_.joinable())
        th_.join();
}

std::vector<WifiNet> Wifi::scan()
{
    std::vector<WifiNet> nets;
    if (!iface_exists())
        return nets;
    std::lock_guard<std::mutex> lk(m_);
    cli({"scan"});
    for (int i = 0; i < 8 && !g_quit.load(); i++)
        usleep(500000);
    std::string res = cli({"scan_results"});
    size_t p = 0;
    bool header = true;
    while (p < res.size()) {
        size_t e = res.find('\n', p);
        std::string line = res.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? res.size() : e + 1;
        if (header) {  // "bssid / frequency / signal level / flags / ssid"
            header = false;
            continue;
        }
        std::vector<std::string> f;
        size_t q = 0;
        for (int k = 0; k < 4; k++) {
            size_t t = line.find('\t', q);
            if (t == std::string::npos)
                break;
            f.push_back(line.substr(q, t - q));
            q = t + 1;
        }
        if (f.size() != 4)
            continue;
        WifiNet n;
        n.ssid = unescape(trim(line.substr(q)));
        n.freq = atoi(f[1].c_str());
        n.signal = atoi(f[2].c_str());
        n.secure = f[3].find("WPA") != std::string::npos || f[3].find("WEP") != std::string::npos;
        if (n.ssid.empty())
            continue;
        auto it = std::find_if(nets.begin(), nets.end(), [&](const WifiNet &o) { return o.ssid == n.ssid; });
        if (it == nets.end())
            nets.push_back(n);
        else if (n.signal > it->signal)
            *it = n;
    }
    std::sort(nets.begin(), nets.end(), [](const WifiNet &a, const WifiNet &b) { return a.signal > b.signal; });
    return nets;
}

bool Wifi::connect(const std::string &ssid, const std::string &psk, std::string *err)
{
    if (!iface_exists()) {
        *err = "нет Wi-Fi адаптера";
        return false;
    }
    if (ssid.empty() || ssid.size() > 32) {
        *err = "некорректное имя сети";
        return false;
    }
    if (!psk.empty()) {
        bool hex64 = psk.size() == 64 && psk.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
        if (!hex64 && (psk.size() < 8 || psk.size() > 63)) {
            *err = "пароль должен быть от 8 до 63 символов";
            return false;
        }
        for (unsigned char c : psk)
            if (c < 0x20 || c > 0x7e) {
                *err = "пароль: только латиница, цифры и символы";
                return false;
            }
    }

    std::lock_guard<std::mutex> lk(m_);
    if (ap_active())
        stop_ap();

    std::string id = cli({"add_network"});
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
        *err = "wpa_supplicant не отвечает";
        return false;
    }
    bool hex64 = psk.size() == 64;
    bool ok = cli({"set_network", id, "ssid", util::to_hex(ssid)}) == "OK" &&
              cli({"set_network", id, "scan_ssid", "1"}) == "OK";
    if (psk.empty())
        ok = ok && cli({"set_network", id, "key_mgmt", "NONE"}) == "OK";
    else
        ok = ok && cli({"set_network", id, "psk", hex64 ? psk : "\"" + psk + "\""}) == "OK";
    ok = ok && cli({"select_network", id}) == "OK";
    if (!ok) {
        cli({"reconfigure"});
        *err = "не удалось применить настройки";
        return false;
    }

    std::string state;
    for (int i = 0; i < 40; i++) {
        usleep(500000);
        state = kv(cli({"status"}), "wpa_state");
        if (state == "COMPLETED")
            break;
    }
    if (state != "COMPLETED") {
        cli({"reconfigure"});  // back to the saved networks
        *err = "не удалось подключиться (проверьте пароль и сигнал)";
        return false;
    }

    // keep only the new network, persist it
    std::string list = cli({"list_networks"});
    size_t p = list.find('\n');
    while (p != std::string::npos && p + 1 < list.size()) {
        size_t e = list.find('\n', p + 1);
        std::string line = list.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
        std::string nid = line.substr(0, line.find('\t'));
        if (!nid.empty() && nid != id)
            cli({"remove_network", nid});
        p = e;
    }
    cli({"enable_network", id});
    cli({"save_config"});

    for (int i = 0; i < 30 && util::iface_ip(IFACE).empty(); i++)
        usleep(500000);
    set_status("");
    return true;
}

WifiStatus Wifi::status()
{
    WifiStatus s;
    s.present = iface_exists();
    if (!s.present)
        return s;
    std::string st = cli({"status"}, 2000);
    s.state = kv(st, "wpa_state");
    s.ssid = unescape(kv(st, "ssid"));
    s.ip = util::iface_ip(IFACE);
    s.ap_mode = ap_active();
    return s;
}

bool Wifi::start_ap()
{
    const std::string ssid = ap_ssid_, psk = ap_psk_;
    util::write_file_atomic(AP_FLAG, "1\n");
    std::string id = cli({"add_network"});
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
        unlink(AP_FLAG);
        return false;
    }
    bool ok = cli({"set_network", id, "ssid", util::to_hex(ssid)}) == "OK" &&
              cli({"set_network", id, "mode", "2"}) == "OK" &&
              cli({"set_network", id, "frequency", "2437"}) == "OK" &&
              cli({"set_network", id, "key_mgmt", "WPA-PSK"}) == "OK" &&
              cli({"set_network", id, "proto", "RSN"}) == "OK" &&
              cli({"set_network", id, "pairwise", "CCMP"}) == "OK" &&
              cli({"set_network", id, "group", "CCMP"}) == "OK" &&
              cli({"set_network", id, "psk", "\"" + psk + "\""}) == "OK" &&
              cli({"select_network", id}) == "OK";
    if (!ok) {
        fprintf(stderr, "wifi: AP mode not accepted by wpa_supplicant\n");
        cli({"reconfigure"});
        unlink(AP_FLAG);
        return false;
    }
    util::run({"ifconfig", IFACE, AP_IP, "netmask", "255.255.255.0", "up"});
    util::write_file_atomic(UDHCPD_CONF,
                            "start 192.168.4.10\nend 192.168.4.60\ninterface " IFACE "\n"
                            "option subnet 255.255.255.0\noption router " AP_IP "\n"
                            "option lease 3600\nlease_file /tmp/udhcpd-ap.leases\npidfile " UDHCPD_PID "\n");
    util::write_file_atomic("/tmp/udhcpd-ap.leases", "");
    util::run({"udhcpd", UDHCPD_CONF});
    printf("wifi: access point %s started\n", ssid.c_str());
    set_status("WiFi setup: join \"" + ssid + "\" (password " + psk + "), open http://" AP_IP);
    return true;
}

void Wifi::stop_ap()
{
    std::string pid;
    if (util::read_file(UDHCPD_PID, pid) && atoi(pid.c_str()) > 0)
        kill(atoi(pid.c_str()), SIGTERM);
    unlink(UDHCPD_PID);
    unlink(AP_FLAG);
    util::run({"ifconfig", IFACE, "0.0.0.0"});
    cli({"reconfigure"});  // drops the unsaved AP network, reconnects saved ones
    set_status("");
    printf("wifi: access point stopped\n");
}

bool Wifi::ap_has_clients()
{
    return !cli({"all_sta"}).empty();
}

void Wifi::loop()
{
    if (!iface_exists()) {
        printf("wifi: no %s, setup AP disabled\n", IFACE);
        return;
    }
    int64_t no_net_since = util::mono_ms();
    int64_t ap_since = 0;
    while (!g_quit.load()) {
        sleep(2);
        bool have_net = !util::iface_ip("eth0").empty() || !util::iface_ip(IFACE).empty();
        std::lock_guard<std::mutex> lk(m_);
        if (ap_active()) {
            if (ap_since == 0) {  // AP started by a previous run of this loop
                ap_since = util::mono_ms();
                set_status("WiFi setup: join \"" + ap_ssid_ + "\" (password " + ap_psk_ + "), open http://" AP_IP);
            }
            if (util::mono_ms() - ap_since > AP_WINDOW_S * 1000 && !ap_has_clients()) {
                stop_ap();
                no_net_since = util::mono_ms();
            }
            continue;
        }
        if (ap_since)
            set_status("");  // AP was stopped by someone else (e.g. web connect)
        ap_since = 0;
        if (have_net) {
            no_net_since = util::mono_ms();
            continue;
        }
        if (util::mono_ms() - no_net_since > ap_timeout_s_ * 1000) {
            if (start_ap())
                ap_since = util::mono_ms();
            else
                no_net_since = util::mono_ms();  // retry later
        }
    }
    std::lock_guard<std::mutex> lk(m_);
    if (ap_active())
        stop_ap();
}
