// Parking / object detection daemon for the K510 CRB with two IMX219 cameras.
//
//   ISP ds0 -> DRM video planes (live view)
//   ISP ds1 -> JPEG photos (timer, web, labels)
//   ISP ds2 -> KPU (YOLOv5/v8/11, see model.h) -> detections -> parking spot occupancy
//   ARGB overlay: boxes, spots, status; web UI on :80; Wi-Fi setup AP fallback
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "camera.h"
#include "detector.h"
#include "display.h"
#include "model.h"
#include "photos.h"
#include "state.h"
#include "util.h"
#include "trigger.h"
#include "video.h"
#include "web.h"
#include "wifi.h"

#define APP_DIR "/app/parking"

std::atomic<bool> g_restart(false);

int run_bench(int argc, char **argv);  // bench.cc
int venc_selftest(int w, int h);        // video.cc
int shm_reclaim();                     // shm_guard.cc

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
    setvbuf(stdout, nullptr, _IOLBF, 0);  // logs go to a file; keep them live
    shm_reclaim();  // KPU/ISP memory a crashed or killed instance did not return
    if (argc > 1 && !strcmp(argv[1], "--bench"))
        return run_bench(argc - 2, argv + 2);
    if (argc > 1 && !strcmp(argv[1], "--models")) {
        for (auto &m : list_models())
            printf("%-22s %-34s %6.1f MB  %s\n", m.id.c_str(), m.title().c_str(), m.file_size / 1e6,
                   m.what.c_str());
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--venc-test"))
        return venc_selftest(argc > 3 ? atoi(argv[2]) : 640, argc > 3 ? atoi(argv[3]) : 480);
    if (argc > 2 && !strcmp(argv[1], "--inspect"))
        return inspect_kmodel(argv[2]);
    // parking [app_dir] [--trigger]
    const char *app_dir = APP_DIR;
    bool trigger_mode = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trigger"))
            trigger_mode = true;
        else if (argv[i][0] != '-')
            app_dir = argv[i];
    }
    printf("parking: build %s %s%s\n", __DATE__, __TIME__, trigger_mode ? " (trigger mode)" : "");

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
        g_state.trigger_mode = trigger_mode;
        if (trigger_mode) {
            // its own settings: model, cameras, classes, video length
            g_config_path = TRIGGER_CONFIG_PATH;
            util::mkdirs("/root/data/trigger");
        }
        if (!g_state.cfg.load(g_config_path)) {
            printf("parking: no valid %s, writing defaults\n", g_config_path.c_str());
            if (trigger_mode)
                g_state.cfg.photo_interval_min = 0;  // photos come from the trigger
            g_state.cfg.save(g_config_path);
        }
        if (!trigger_mode && !g_state.load_spots(SPOTS_PATH))
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

    // The model decides the ISP AI output size, so it is looked up first; it is
    // loaded only after the display got its buffers (both need contiguous
    // memory, and the DRM ones fail if the KPU took it first).
    ModelInfo model;
    if (!find_model(cfg.model, model) && !find_model(kDefaultModel, model))
        model.input_w = 0;
    if (model.input_w)
        cfg.net_len = model.input_w;

    if (trigger_mode) {
        // the recording camera needs no AI output from the ISP
        std::string dis = getenv("PARKING_DISABLE") ? getenv("PARKING_DISABLE") : "";
        for (int c = 0; c < NUM_CAMS; c++)
            if (c != cfg.trigger_ai_cam)
                dis += (dis.empty() ? "" : ",") + std::string("ai") + std::to_string(c);
        setenv("PARKING_DISABLE", dis.c_str(), 1);
    }

    CamLayout layout[NUM_CAMS];
    compute_layout(sw, sh, cfg.cam_enabled, layout);
    AiGeom ai = detector_geometry(model);

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

    bool model_ok = false;
    if (pipeline_ok) {
        util::defrag_memory();
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
        }

        // The KPU buffers come last: the display and photo buffers need
        // contiguous memory too and fail if the model took it first.
        model_ok = model.input_w && detector_load_model(model.id, &model);
        if (!model_ok && model.id != kDefaultModel) {
            // the ISP is already set up for this input size: only a same-size fallback works
            ModelInfo def;
            if (find_model(kDefaultModel, def) && def.input_w == model.input_w && def.input_h == model.input_h &&
                def.valid_w == model.valid_w && def.valid_h == model.valid_h && def.center == model.center)
                model_ok = detector_load_model(kDefaultModel, &model);
        }
        {
            std::lock_guard<std::mutex> lk(g_state.mtx);
            if (model_ok) {
                g_state.model_id = model.id;
                g_state.model_title = model.title();
                if (model.id != cfg.model && model.path != cfg.model)
                    g_state.status_msg = "model " + cfg.model + " failed, using " + model.id;
            } else {
                g_state.status_msg = "AI model " + cfg.model + " failed to load, see log";
            }
        }
        for (int c = 0; c < NUM_CAMS; c++) {
            if (!layout[c].enabled)
                continue;
            if (channel_disabled("ai", c))
                printf("cam%d: AI channel disabled (PARKING_DISABLE)\n", c);
            else if (trigger_mode && c != cfg.trigger_ai_cam)
                printf("cam%d: records on trigger, no AI\n", c);
            else if (model_ok)
                detectors[c].start(c, ai);
        }
    } else {
        std::lock_guard<std::mutex> lk(g_state.mtx);
        g_state.status_msg = "camera pipeline failed, see log";
    }
    photos.start(pc);
    VideoRecorder video;
    video.init(pc);
    if (web.start(cfg.web_port, (std::string(app_dir) + "/www").c_str(), &photos, &video) != 0)
        fprintf(stderr, "parking: web server failed\n");

    Trigger trigger;
    while (!g_quit.load()) {
        usleep(trigger_mode ? 100000 : 1000000);
        video.tick();
        if (trigger_mode && pipeline_ok && model_ok)
            trigger.tick(photos, video);
    }

    printf("parking: stopping\n");
    video.stop();  // finish the MP4s before the cameras go away
    web.stop();
    photos.stop();
    for (int c = 0; c < NUM_CAMS; c++)
        detectors[c].stop();
    detector_unload_model();
    for (int c = 0; c < NUM_CAMS; c++)
        photo_cams[c].stop();
    display.stop();
    if (pipeline_ok)
        video_pipeline_deinit();
    // exit code 3 asks the respawn wrapper for an immediate restart
    return g_restart.load() ? 3 : 0;
}
