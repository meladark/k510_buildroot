// Parking / object detection daemon for the K510 CRB with two IMX219 cameras.
//
//   ISP ds0 -> DRM video planes (live view)
//   ISP ds1 -> JPEG photos (timer, web, labels)
//   ISP ds2 -> KPU YOLOv5 -> detections -> parking spot occupancy
//   ARGB overlay: boxes, spots, status; web UI on :80; Wi-Fi setup AP fallback
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "camera.h"
#include "detector.h"
#include "display.h"
#include "photos.h"
#include "state.h"
#include "util.h"
#include "web.h"
#include "wifi.h"

#define APP_DIR "/app/parking"

std::atomic<bool> g_restart(false);

static void on_signal(int)
{
    g_quit = true;
}

static void cfg_noc_priority()
{
    // Bus priorities from the stock AI demo (object_detect.sh). With the values
    // of the plain dual camera demo, KPU traffic starves the second video layer
    // and camera 1 shows ripple/torn frames whenever AI is running.
    util::run({"devmem", "0x970E00f4", "32", "0x00550000"});
    util::run({"devmem", "0x970E00f8", "32", "0x00000000"});
    util::run({"devmem", "0x970E00fc", "32", "0x0fffff00"});
    util::run({"devmem", "0x970E0100", "32", "0x000000ff"});
    util::run({"devmem", "0x970E0104", "32", "0x00000000"});
}

int main(int argc, char **argv)
{
    const char *app_dir = argc > 1 ? argv[1] : APP_DIR;
    setvbuf(stdout, nullptr, _IOLBF, 0);  // logs go to a file; keep them live
    printf("parking: build %s %s\n", __DATE__, __TIME__);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // mediactl resolves the sensor config files relative to the cwd
    if (chdir(app_dir) != 0) {
        fprintf(stderr, "cannot chdir to %s\n", app_dir);
        return 1;
    }
    util::mkdirs(PHOTOS_DIR);
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        if (!g_state.cfg.load(CONFIG_PATH)) {
            printf("parking: no valid %s, writing defaults\n", CONFIG_PATH);
            g_state.cfg.save(CONFIG_PATH);
        }
        if (!g_state.load_spots(SPOTS_PATH))
            printf("parking: no spots yet, draw them in the web UI\n");
    }
    Config cfg;
    {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        cfg = g_state.cfg;
    }

    // Wi-Fi AP fallback runs in the launcher; the web UI here only uses g_wifi
    // for scan/connect.

    uint32_t sw = 1920, sh = 1080;
    bool have_display = drm_query_resolution(&sw, &sh) == 0;
    if (!have_display)
        fprintf(stderr, "parking: no display connected, running headless\n");
    printf("parking: screen %ux%u\n", sw, sh);

    CamLayout layout[NUM_CAMS];
    compute_layout(sw, sh, cfg.cam_enabled, layout);
    AiGeom ai;
    ai.net_len = cfg.net_len;
    ai.valid_w = cfg.net_len;
    ai.valid_h = cfg.net_len * 3 / 4;

    cfg_noc_priority();
    // Right after a restart the ISP may still be held by the previous instance
    // for a few seconds; retry instead of dropping into headless mode.
    bool pipeline_ok = false;
    for (int attempt = 1; attempt <= 10 && !g_quit.load(); attempt++) {
        if (video_pipeline_init(std::string(app_dir) + "/video_base.conf", cfg, layout, ai) == 0) {
            pipeline_ok = true;
            break;
        }
        fprintf(stderr, "parking: camera pipeline init failed (attempt %d), retrying\n", attempt);
        sleep(2);
    }

    Display display;
    PhotoCam photo_cams[NUM_CAMS];
    Detector detectors[NUM_CAMS];
    Photos photos;
    WebServer web;
    PhotoCam *pc[NUM_CAMS] = {nullptr, nullptr};

    if (pipeline_ok) {
        if (have_display && display.start(layout) != 0)
            fprintf(stderr, "parking: display init failed, running headless\n");
        for (int c = 0; c < NUM_CAMS; c++) {
            if (!layout[c].enabled)
                continue;
            if (channel_disabled("photo", c))
                printf("cam%d: photo channel disabled (PARKING_DISABLE)\n", c);
            else if (photo_cams[c].start(c, cfg.photo_width, cfg.photo_height) == 0)
                pc[c] = &photo_cams[c];
            else
                fprintf(stderr, "cam%d: photo channel failed\n", c);
            if (channel_disabled("ai", c))
                printf("cam%d: AI channel disabled (PARKING_DISABLE)\n", c);
            else
                detectors[c].start(c, ai);
        }
    } else {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.status_msg = "camera pipeline failed, see log";
    }
    photos.start(pc);
    if (web.start(cfg.web_port, (std::string(app_dir) + "/www").c_str(), &photos) != 0)
        fprintf(stderr, "parking: web server failed\n");

    while (!g_quit.load())
        sleep(1);

    printf("parking: stopping\n");
    web.stop();
    photos.stop();
    for (int c = 0; c < NUM_CAMS; c++)
        detectors[c].stop();
    for (int c = 0; c < NUM_CAMS; c++)
        photo_cams[c].stop();
    display.stop();
    if (pipeline_ok)
        video_pipeline_deinit();
    // exit code 3 asks the respawn wrapper for an immediate restart
    return g_restart.load() ? 3 : 0;
}
