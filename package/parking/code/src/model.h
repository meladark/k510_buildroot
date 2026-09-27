#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

// Models the app can run on the KPU.
//
// Our COCO detectors are <id>.kmodel + <id>.json in a model directory (built by
// package/parking/models on the build server; see export.py for the layouts).
// The stock SDK models (/app/ai/kmodel) are described in model.cc: faces,
// people, plates, hands, poses. Some of them are two-stage: a detector finds
// the object, a second network looks at the crop (landmarks, text, emotion...).
struct ModelInfo {
    std::string id;
    std::string name;      // menu title for SDK models (ours derive it from the id)
    std::string path;      // kmodel (first stage)
    std::string family;    // decoder, see engine.cc
    std::string quant;     // bf16 / uint8
    std::string what;      // what it recognizes, shown in the menus (Russian)
    std::string note;      // caveat, e.g. trained on Chinese plates
    int input_w = 320, input_h = 320;  // network input
    int valid_w = 320, valid_h = 240;  // camera image inside the input
    bool center = true;                // image centered in the input (else top-left)
    float thresh = -1, nms = -1;       // model defaults; < 0 = from the app config
    bool coco = false;                 // 80 COCO classes (cars etc.)

    // yolov5 / yolov8
    std::vector<int> strides = {8, 16, 32};
    std::vector<std::vector<float>> anchors;  // yolov5: per stride w0,h0,w1,h1,w2,h2 in pixels
    int reg_max = 16;
    std::vector<std::string> labels;

    // second stage
    std::string path2;
    int input2_w = 0, input2_h = 0;

    long file_size = 0;       // all stages together

    std::string title() const;  // "YOLOv8n 320 · uint8", "Лица (RetinaFace 320)"
};

std::vector<ModelInfo> list_models();
// Accepts a model id or a kmodel path (old configs store the path).
bool find_model(const std::string &id_or_path, ModelInfo &out);
extern const char *kDefaultModel;

// How the points of a result are connected when drawn.
enum PointShape {
    SHAPE_NONE = 0,
    SHAPE_DOTS,       // face points
    SHAPE_POLYGON,    // plate corners
    SHAPE_HAND21,
    SHAPE_COCO17,
    SHAPE_OPENPOSE18,
    SHAPE_AXES,       // head pose: center + x, y, z axis ends
};
const std::vector<std::pair<int, int>> &shape_edges(int shape);

// One result, coordinates normalized to the camera image (0..1).
struct DetObj {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0, score = 0;
    int label = 0;
    std::string name;             // class / kind
    std::string text;             // plate text, emotion, angles
    std::vector<float> pts;       // x,y pairs; negative = not found
    int shape = SHAPE_NONE;
};

class Net;

// A loaded model (one or two networks) plus its decoder. Not thread safe: the
// caller serializes (both cameras share one instance).
class Engine {
public:
    struct Impl;
    Engine();
    ~Engine();
    bool load(const ModelInfo &m);
    const ModelInfo &model() const { return info_; }
    int input_w() const;
    int input_h() const;
    uint8_t *input();  // planar RGB, input_w x input_h per plane
    // Runs on input(): the camera image is valid_w x valid_h, placed as the
    // model wants (see ModelInfo::center).
    bool process(float thresh, float nms, std::vector<DetObj> &out, float *infer_ms, float *post_ms);

private:
    std::unique_ptr<Impl> p_;
    ModelInfo info_;
};

// Draws the points of a result (skeleton lines, dots) into area of img (BGR or
// BGRA); pts are normalized to the area.
void draw_points(cv::Mat &img, const cv::Rect &area, const std::vector<float> &pts, int shape, const cv::Scalar &color,
                 int thickness);

// Prints input/output shapes of a kmodel (parking --inspect).
int inspect_kmodel(const std::string &path);
