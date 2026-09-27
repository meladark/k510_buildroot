#include "apps.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>

#include "util.h"

int shm_reclaim();  // shm_guard.cc

static const char *kAppDirs[] = {"/app/launcher/apps", "/root/data/apps"};

std::vector<App> load_apps()
{
    std::vector<App> apps;
    for (const char *dir : kAppDirs) {
        for (auto &f : util::list_dir(dir)) {
            if (f.size() < 6 || f.compare(f.size() - 5, 5, ".conf") != 0)
                continue;
            std::string text;
            if (!util::read_file(std::string(dir) + "/" + f, text))
                continue;
            App a;
            a.id = f.substr(0, f.size() - 5);
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                std::string k = line.substr(0, eq), v = line.substr(eq + 1);
                while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
                    v.pop_back();
                if (k == "name")
                    a.name = v;
                else if (k == "desc")
                    a.desc = v;
                else if (k == "cwd")
                    a.cwd = v;
                else if (k == "action_url")
                    a.action_url = v;
                else if (k == "action_long_url")
                    a.action_long_url = v;
                else if (k == "config")
                    a.config = v;
                else if (k == "env" && v.find('=') != std::string::npos)
                    a.env.push_back(v);
                else if (k == "exec") {
                    std::istringstream ws(v);
                    std::string w;
                    while (ws >> w)
                        a.argv.push_back(w);
                }
            }
            if (a.argv.empty())
                continue;
            if (a.name.empty())
                a.name = a.id;
            // user apps override shipped ones with the same id
            apps.erase(std::remove_if(apps.begin(), apps.end(), [&](const App &o) { return o.id == a.id; }),
                       apps.end());
            apps.push_back(a);
        }
    }
    std::sort(apps.begin(), apps.end(), [](const App &a, const App &b) { return a.id < b.id; });
    return apps;
}

bool Supervisor::start(const App &app)
{
    stop();
    app_ = app;
    want_ = true;
    gave_up_ = false;
    crashes_.clear();
    error_.clear();
    return spawn();
}

bool Supervisor::spawn()
{
    util::defrag_memory();  // the app allocates its display buffers right away
    shm_reclaim();          // KPU/ISP memory the previous run did not return
    util::mkdirs(LOG_DIR);
    std::string log = std::string(LOG_DIR) + "/" + app_.id + ".log";
    if (util::file_size(log) > 2000000)
        rename(log.c_str(), (log + ".1").c_str());

    pid_t pid = fork();
    if (pid < 0) {
        error_ = "fork failed";
        return false;
    }
    if (pid == 0) {
        setsid();  // own process group, so stop() reaches shell children too
        int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, STDIN_FILENO);
        if (!app_.cwd.empty() && chdir(app_.cwd.c_str()) != 0)
            fprintf(stderr, "launcher: cannot chdir %s\n", app_.cwd.c_str());
        setenv("LD_LIBRARY_PATH", "/lib:/usr/lib:/usr/local/lib", 1);
        for (auto &e : app_.env) {
            size_t eq = e.find('=');
            setenv(e.substr(0, eq).c_str(), e.substr(eq + 1).c_str(), 1);
        }
        signal(SIGPIPE, SIG_DFL);
        fprintf(stderr, "=== launcher start %s ===\n", util::time_str(util::now_ms(), "%Y-%m-%d %H:%M:%S").c_str());
        std::vector<char *> args;
        for (auto &a : app_.argv)
            args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);
        execv(args[0], args.data());
        fprintf(stderr, "launcher: exec %s failed: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    pid_ = pid;
    printf("launcher: started %s (pid %d)\n", app_.id.c_str(), pid);
    return true;
}

void Supervisor::stop()
{
    want_ = false;
    restart_at_ = 0;
    if (pid_ <= 0)
        return;
    const int sigs[] = {SIGINT, SIGTERM, SIGKILL};
    const int waits_ms[] = {4000, 4000, 2000};
    for (int s = 0; s < 3 && pid_ > 0; s++) {
        kill(-pid_, sigs[s]);
        for (int t = 0; t < waits_ms[s]; t += 100) {
            if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
                pid_ = -1;
                break;
            }
            usleep(100000);
        }
    }
    if (pid_ > 0) {
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }
    // Hardware (ISP, DRM) is released asynchronously after the process exits.
    usleep(1500000);
    printf("launcher: stopped %s\n", app_.id.c_str());
}

void Supervisor::poll()
{
    if (pid_ > 0) {
        int status = 0;
        if (waitpid(pid_, &status, WNOHANG) != pid_)
            return;
        pid_ = -1;
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        printf("launcher: %s exited with %d\n", app_.id.c_str(), code);
        if (!want_)
            return;
        if (code == 3) {  // app asked for a restart (e.g. settings changed)
            restart_at_ = util::mono_ms() + 1500;
            return;
        }
        int64_t now = util::mono_ms();
        crashes_.push_back(now);
        crashes_.erase(std::remove_if(crashes_.begin(), crashes_.end(),
                                      [&](int64_t t) { return now - t > 120000; }),
                       crashes_.end());
        if (crashes_.size() >= 5) {
            want_ = false;
            gave_up_ = true;
            error_ = app_.name + ": падает при запуске, см. " LOG_DIR "/" + app_.id + ".log";
            return;
        }
        restart_at_ = now + 3000;
        return;
    }
    if (want_ && restart_at_ && util::mono_ms() >= restart_at_) {
        restart_at_ = 0;
        spawn();
    }
}
