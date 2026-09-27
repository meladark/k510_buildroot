#include "model.h"

// Model catalog (no nncase dependency: the launcher menu uses it too).
#include <stdio.h>
#include <sys/stat.h>

#include <algorithm>

#include <rapidjson/document.h>

#include "util.h"

#define SDK_KMODELS_ROOT "/app/ai/kmodel/kmodel_release"

const char *kDefaultModel = "sdk_yolov5s_320";

static const std::vector<std::string> &coco_labels()
{
    static const std::vector<std::string> l = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard",
        "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
        "scissors", "teddy bear", "hair drier", "toothbrush"};
    return l;
}

static long file_size(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

#define K SDK_KMODELS_ROOT

static const char *kCocoWhat = "80 классов COCO: люди, машины, автобусы, животные, предметы";

static ModelInfo sdk(const char *id, const char *name, const char *family, const std::string &path, int w, int h,
                     const char *what)
{
    ModelInfo m;
    m.id = id;
    m.name = name;
    m.family = family;
    m.path = path;
    m.quant = "bf16";
    m.input_w = w;
    m.input_h = h;
    m.valid_w = w;
    m.valid_h = w * 3 / 4 < h ? w * 3 / 4 : h;
    m.what = what;
    return m;
}

// Stock SDK kmodels (package ai). Same geometry and thresholds as the demos.
static std::vector<ModelInfo> sdk_models()
{
    std::vector<ModelInfo> v;
    for (int n : {320, 640}) {
        std::string ns = std::to_string(n);
        ModelInfo m = sdk(("sdk_yolov5s_" + ns).c_str(), "", "yolov5",
                          K "/object_detect/yolov5s_" + ns + "/yolov5s_" + ns +
                              "_sigmoid_bf16_with_preprocess_output_nhwc.kmodel",
                          n, n, kCocoWhat);
        m.anchors = {{10, 13, 16, 30, 33, 23}, {30, 61, 62, 45, 59, 119}, {116, 90, 156, 198, 373, 326}};
        m.labels = coco_labels();
        m.coco = true;
        v.push_back(m);
    }
    const std::string face320 = K "/face_detect/mb_rf320/retinaface_mobile0.25_320_bf16_swapRB_with_preprocess.kmodel";
    for (int n : {320, 640}) {
        std::string ns = std::to_string(n);
        ModelInfo m = sdk(("sdk_face_" + ns).c_str(), ("Лица · RetinaFace " + ns).c_str(), "retinaface",
                          K "/face_detect/mb_rf" + ns + "/retinaface_mobile0.25_" + ns +
                              "_bf16_swapRB_with_preprocess.kmodel",
                          n, n, "Лица: рамка и 5 точек (глаза, нос, уголки рта)");
        m.center = false;
        m.thresh = 0.5f;
        m.nms = 0.2f;
        v.push_back(m);
    }
    {
        ModelInfo m = sdk("sdk_face_landmarks", "Лица · 106 точек (PFLD)", "face_pfld", face320, 320, 320,
                          "Лица и 106 точек: контур, брови, глаза, нос, губы");
        m.center = false;
        m.thresh = 0.7f;
        m.nms = 0.2f;
        m.path2 = K "/face_landmarks/pfld_106/v2/v2_process_bf16_swapRB_with_preprocess.kmodel";
        m.input2_w = m.input2_h = 112;
        v.push_back(m);
    }
    {
        ModelInfo m = sdk("sdk_face_expression", "Лица · эмоции", "face_fer", face320, 320, 320,
                          "Лица и эмоция: нейтрально, радость, грусть, удивление, страх, отвращение, злость, презрение");
        m.center = false;
        m.thresh = 0.7f;
        m.nms = 0.2f;
        m.path2 = K "/face_expression/facex/face_expression_bf16_with_preprocess.kmodel";
        m.input2_w = m.input2_h = 224;
        v.push_back(m);
    }
    {
        ModelInfo m = sdk("sdk_head_pose", "Лица · поворот головы", "face_hpe", face320, 320, 320,
                          "Лица и поворот головы: наклон, поворот, крен в градусах");
        m.center = false;
        m.thresh = 0.7f;
        m.nms = 0.2f;
        m.path2 = K "/head_pose_estimation/head_pose/model_fixed_input_size_bf16_with_preprocess.kmodel";
        m.input2_w = m.input2_h = 120;
        v.push_back(m);
    }
    {
        ModelInfo m = sdk("sdk_person_scrfd", "Люди · SCRFD 640×480", "scrfd",
                          K "/person_detect/scrfd/scrfd_person_2.5g_fixed_input_size_simplify_bf16_with_preprocess.kmodel",
                          640, 480, "Только люди (быстрый детектор фигур)");
        m.thresh = 0.3f;
        m.nms = 0.2f;
        v.push_back(m);
    }
    for (int n : {320, 640}) {
        std::string ns = std::to_string(n);
        ModelInfo m = sdk(("sdk_plates_" + ns).c_str(), ("Автономера · LPD " + ns + " + LPRNet").c_str(), "plate",
                          K "/license_detect/mb_rf" + ns + "/LPD_" + ns + "_bf16_swapRB_with_preprocess.kmodel", n, n,
                          "Номерные знаки: рамка, 4 угла и текст номера");
        m.note = "текст обучен на китайских номерах";
        m.center = false;
        m.thresh = 0.3f;
        m.nms = 0.2f;
        m.path2 = K "/license_recorg/lprnet/LPR_bf16_swapRB_out_nhwc_with_preprocess.kmodel";
        m.input2_w = 94;
        m.input2_h = 24;
        v.push_back(m);
    }
    {
        ModelInfo m = sdk("sdk_hands", "Руки · 21 точка", "hand",
                          K "/hand_pose_x/detect/tinyYOLOV3_bf16_with_preprocess.kmodel", 512, 512,
                          "Кисти рук и 21 точка суставов пальцев");
        m.thresh = 0.35f;
        m.nms = 0.4f;
        m.path2 = K "/hand_pose_x/hand_landmark/squeezenet1_1_bf16_swapRB_with_preprocess.kmodel";
        m.input2_w = m.input2_h = 256;
        v.push_back(m);
    }
    {
        ModelInfo m = sdk("sdk_openpose", "Позы · OpenPose", "openpose",
                          K "/person_pose/openpose/hp_concat_new_bf16_swapRB_with_preprocess_input_nchw.kmodel", 432,
                          368, "Скелеты людей: 18 точек, сразу несколько человек");
        m.valid_h = 368;  // full input, as in the demo
        v.push_back(m);
    }
    {
        ModelInfo m = sdk("sdk_simple_pose", "Позы · SimplePose (17 точек)", "simplepose", v[0].path, 320, 320,
                          "Люди (YOLOv5s) и 17 точек тела у каждого");
        m.anchors = v[0].anchors;
        m.labels = v[0].labels;
        m.thresh = 0.5f;
        m.nms = 0.45f;
        m.path2 = K "/person_pose/simplepose/deploy_modify_bf16_with_preprocess.kmodel";
        m.input2_w = 192;
        m.input2_h = 256;
        v.push_back(m);
    }

    std::vector<ModelInfo> out;
    for (auto &m : v) {
        long a = file_size(m.path), b = m.path2.empty() ? 0 : file_size(m.path2);
        if (a <= 0 || b < 0)
            continue;  // SDK package without this model
        m.file_size = a + b;
        out.push_back(m);
    }
    return out;
}

static std::vector<std::string> model_dirs()
{
    std::vector<std::string> d = {"/app/parking/models", "/root/data/models"};
    for (auto &sd : util::list_dir("/root/sd"))  // SD card partitions (automounted)
        d.push_back("/root/sd/" + sd + "/models");
    return d;
}

static bool parse_meta(const std::string &json_path, ModelInfo &m)
{
    std::string s;
    if (!util::read_file(json_path, s))
        return false;
    rapidjson::Document d;
    if (d.Parse(s.c_str()).HasParseError() || !d.IsObject())
        return false;
    auto str = [&](const char *k) { return d.HasMember(k) && d[k].IsString() ? std::string(d[k].GetString()) : ""; };
    m.id = str("id");
    m.family = str("family");
    m.quant = str("quant");
    std::string file = str("file");
    if (m.id.empty() || file.empty() || (m.family != "yolov5" && m.family != "yolov8"))
        return false;
    m.path = json_path.substr(0, json_path.rfind('/') + 1) + file;
    if (d.HasMember("input") && d["input"].IsInt()) {
        m.input_w = m.input_h = m.valid_w = d["input"].GetInt();
        m.valid_h = m.input_w * 3 / 4;
    }
    m.what = kCocoWhat;
    if (d.HasMember("reg_max") && d["reg_max"].IsInt())
        m.reg_max = d["reg_max"].GetInt();
    if (d.HasMember("strides") && d["strides"].IsArray()) {
        m.strides.clear();
        for (auto &v : d["strides"].GetArray())
            m.strides.push_back(v.GetInt());
    }
    if (d.HasMember("anchors") && d["anchors"].IsArray()) {
        for (auto &lvl : d["anchors"].GetArray()) {  // [[w,h],[w,h],[w,h]] per stride
            std::vector<float> a;
            for (auto &wh : lvl.GetArray())
                for (auto &x : wh.GetArray())
                    a.push_back(x.GetFloat());
            m.anchors.push_back(a);
        }
    }
    if (d.HasMember("labels") && d["labels"].IsArray())
        for (auto &v : d["labels"].GetArray())
            m.labels.push_back(v.GetString());
    if (m.labels.empty())
        m.labels = coco_labels();
    m.coco = m.labels.size() == 80;
    if (!m.coco)
        m.what = std::to_string(m.labels.size()) + " классов";
    if (m.family == "yolov5" && m.anchors.size() != m.strides.size())
        return false;
    m.file_size = file_size(m.path);
    return m.file_size > 0;
}

std::vector<ModelInfo> list_models()
{
    std::vector<ModelInfo> v = sdk_models();
    for (auto &dir : model_dirs()) {
        for (auto &f : util::list_dir(dir)) {
            if (f.size() < 6 || f.compare(f.size() - 5, 5, ".json") != 0)
                continue;
            ModelInfo m;
            if (!parse_meta(dir + "/" + f, m))
                continue;
            if (std::none_of(v.begin(), v.end(), [&](const ModelInfo &o) { return o.id == m.id; }))
                v.push_back(m);
        }
    }
    // COCO detectors first (by input size), then the special ones in catalog order
    std::stable_sort(v.begin(), v.end(), [](const ModelInfo &a, const ModelInfo &b) {
        if (a.coco != b.coco)
            return a.coco;
        return a.coco && a.input_w != b.input_w ? a.input_w < b.input_w : a.coco && a.id < b.id;
    });
    return v;
}

bool find_model(const std::string &id_or_path, ModelInfo &out)
{
    for (auto &m : list_models()) {
        if (m.id == id_or_path || m.path == id_or_path) {
            out = m;
            return true;
        }
    }
    return false;
}

std::string ModelInfo::title() const
{
    if (!name.empty())
        return name;
    // yolov8n_320_uint8 -> YOLOv8n 320 · uint8
    std::string base = id, sdk;
    if (base.compare(0, 4, "sdk_") == 0) {
        base = base.substr(4);
        sdk = " (SDK)";
    }
    std::string n = base.substr(0, base.find('_'));
    if (n.compare(0, 4, "yolo") == 0)
        n = "YOLO" + n.substr(4);
    return n + " " + std::to_string(input_w) + " · " + quant + sdk;
}
