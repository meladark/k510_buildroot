#include "util.h"

#include <algorithm>
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace util {

int64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::string time_str(int64_t ms, const char *fmt)
{
    time_t t = ms / 1000;
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

bool clock_is_sane()
{
    // rc.sysinit sets 2010-12-01 until ntpdate succeeds
    return time(nullptr) > 1700000000;
}

bool read_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

bool write_file_atomic(const std::string &path, const std::string &data)
{
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fflush(f) == 0) && ok;
    fsync(fileno(f));
    fclose(f);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool mkdirs(const std::string &path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) {
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    return true;
}

bool is_dir(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> list_dir(const std::string &path)
{
    std::vector<std::string> names;
    DIR *d = opendir(path.c_str());
    if (!d)
        return names;
    while (struct dirent *e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

int64_t file_size(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return -1;
    return st.st_size;
}

bool remove_tree(const std::string &path)
{
    if (is_dir(path)) {
        for (auto &n : list_dir(path))
            remove_tree(path + "/" + n);
        return rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

double free_space_pct(const std::string &path)
{
    struct statvfs vfs;
    if (statvfs(path.c_str(), &vfs) != 0 || vfs.f_blocks == 0)
        return 100.0;
    return 100.0 * (double)vfs.f_bavail / (double)vfs.f_blocks;
}

int run(const std::vector<std::string> &argv, std::string *out, int timeout_ms)
{
    if (argv.empty())
        return -1;
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char *> args;
        for (auto &a : argv)
            args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(pipefd[1]);
    std::string buf;
    int64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - mono_ms());
        if (left <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        struct pollfd pfd = {pipefd[0], POLLIN, 0};
        int r = poll(&pfd, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            kill(pid, SIGKILL);
            break;
        }
        char tmp[1024];
        ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
        if (n <= 0)
            break;
        buf.append(tmp, n);
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (out)
        *out = buf;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string iface_ip(const char *ifname)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return "";
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_addr.sa_family = AF_INET;
    std::string ip;
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        char buf[INET_ADDRSTRLEN];
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            ip = buf;
    }
    close(fd);
    if (ip == "0.0.0.0")
        ip.clear();
    return ip;
}

std::string json_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += (char)c;
            }
        }
    }
    return o;
}

std::string to_hex(const std::string &s)
{
    static const char *hx = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) {
        o += hx[c >> 4];
        o += hx[c & 15];
    }
    return o;
}

bool safe_rel_path(const std::string &p)
{
    if (p.empty() || p[0] == '/' || p.find("..") != std::string::npos)
        return false;
    for (unsigned char c : p) {
        if (!(isalnum(c) || c == '/' || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

}  // namespace util
