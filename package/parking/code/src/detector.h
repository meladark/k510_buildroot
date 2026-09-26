#pragma once

#include <thread>

#include "camera.h"

// One thread per camera: ISP ds2 -> KPU (YOLOv5) -> detections + spot state.
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
