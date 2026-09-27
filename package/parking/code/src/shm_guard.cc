// Keeps /dev/k510-share-memory from leaking across app restarts.
//
// The driver hands out blocks of the 256 MB KPU/ISP pool but does not free
// them when a process exits (its release() is empty). Whatever the app did not
// free itself - after a crash, SIGKILL, or a slow shutdown - is lost until the
// next reboot, and after a few such restarts the ISP and KPU cannot allocate:
// blank screen, no detection.
//
// This file wraps ioctl() for the whole executable (our code, the static
// mediactl and nncase libraries) and records every block taken from the pool
// in /run/k510_shm/<pid>. shm_reclaim() frees the blocks of processes that are
// gone. /run is tmpfs (unlike /tmp on this board), cleared on reboot together
// with the pool, so the records never outlive what they describe.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <mutex>
#include <set>
#include <string>

#include "canaan/cv2_utils.h"  // SHARE_MEMORY_* ioctl numbers, device path

// must be tmpfs: after a reboot the records would point at other processes' blocks
#define SHM_DIR "/run/k510_shm"

namespace {

struct AllocArgs {
    uint32_t size, phys;
};
struct AlignAllocArgs {
    uint32_t size, alignment, phys;
};

std::mutex g_mtx;
std::set<uint32_t> g_blocks;

long real_ioctl(int fd, unsigned long req, void *arg)
{
    return syscall(SYS_ioctl, fd, req, arg);
}

bool is_shm_fd(int fd)
{
    char link[64], target[128];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0)
        return false;
    target[n] = 0;
    return strcmp(target, SHARE_MEMORY_DEV) == 0;
}

// g_mtx held
void persist()
{
    mkdir(SHM_DIR, 0755);
    char path[64], tmp[72];
    snprintf(path, sizeof(path), SHM_DIR "/%d", (int)getpid());
    if (g_blocks.empty()) {
        unlink(path);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (uint32_t p : g_blocks)
        fprintf(f, "%08x\n", p);
    fclose(f);
    rename(tmp, path);
}

}  // namespace

extern "C" int ioctl(int fd, unsigned long req, ...) __THROW
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    long r = real_ioctl(fd, req, arg);
    if (r != 0 || !arg)
        return (int)r;
    if (req != SHARE_MEMORY_ALLOC && req != SHARE_MEMORY_ALIGN_ALLOC && req != SHARE_MEMORY_FREE)
        return 0;
    if (!is_shm_fd(fd))
        return 0;

    std::lock_guard<std::mutex> lk(g_mtx);
    if (req == SHARE_MEMORY_ALLOC)
        g_blocks.insert(((AllocArgs *)arg)->phys);
    else if (req == SHARE_MEMORY_ALIGN_ALLOC)
        g_blocks.insert(((AlignAllocArgs *)arg)->phys);
    else
        g_blocks.erase(*(uint32_t *)arg);
    persist();
    return 0;
}

// Frees the pool blocks of dead processes; returns how many.
int shm_reclaim()
{
    DIR *d = opendir(SHM_DIR);
    if (!d)
        return 0;
    int fd = open(SHARE_MEMORY_DEV, O_RDWR);
    int freed = 0;
    while (struct dirent *e = readdir(d)) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (pid <= 0 || *end)
            continue;
        if (pid == getpid() || kill(pid, 0) == 0 || errno != ESRCH)
            continue;  // still running (or not ours to judge)
        std::string path = std::string(SHM_DIR "/") + e->d_name;
        FILE *f = fopen(path.c_str(), "r");
        if (f) {
            unsigned p;
            while (fd >= 0 && fscanf(f, "%x", &p) == 1) {
                uint32_t phys = p;
                if (real_ioctl(fd, SHARE_MEMORY_FREE, &phys) == 0)
                    freed++;
            }
            fclose(f);
        }
        unlink(path.c_str());
    }
    closedir(d);
    if (fd >= 0)
        close(fd);
    if (freed)
        printf("shm: freed %d blocks left by dead processes\n", freed);
    return freed;
}
