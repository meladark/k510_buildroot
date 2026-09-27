#pragma once

#include <stddef.h>
#include <stdint.h>

#include <thread>

#include <string>

#include "camera.h"

struct ModelInfo;

// ISP ds2 geometry the model needs, and one ds2 frame placed into its input.
AiGeom detector_geometry(const ModelInfo &m);
void place_frame(uint8_t *dst, const uint8_t *frame, size_t len, const AiGeom &g);

// Loads the detection model shared by both cameras (id or kmodel path).
bool detector_load_model(const std::string &id, ModelInfo *used);
void detector_unload_model();

// One thread per camera: ISP ds2 -> KPU -> detections + spot state.
class Detector {
public:
    int start(int cam, const AiGeom &geom);
    void stop();

private:
    void loop();
    int cam_ = 0;
    AiGeom geom_;
    std::thread th_;
};
