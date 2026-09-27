// Engine: kmodels on the K510 KPU (nncase runtime) and their decoders.
//
// The SDK decoders are re-implemented from the package ai demos (same math,
// thresholds and crops) on top of one generic Net: RetinaFace (faces, plates),
// SCRFD (people), tiny YOLOv3 (hands), OpenPose, and the second-stage networks
// that look at a crop (face landmarks / emotion / head pose, plate text, hand
// landmarks, simple pose).
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#include <nncase/runtime/host_runtime_tensor.h>
#include <nncase/runtime/interpreter.h>
#include <nncase/runtime/runtime_tensor.h>
#include <opencv2/imgproc.hpp>

#include "canaan/cv2_utils.h"  // share memory ioctls
#include "canaan/pafprocess.h"
#include "model.h"

using namespace nncase;
using namespace nncase::runtime;

typedef std::chrono::steady_clock Clock;

static float ms_since(Clock::time_point t)
{
    return std::chrono::duration<float, std::milli>(Clock::now() - t).count();
}

// ---------------------------------------------------------------------------
// Net: one kmodel, uint8 NCHW input and float outputs in KPU-visible memory.

class Net {
public:
    ~Net();
    bool load(const std::string &path, bool verbose = true);
    int in_w = 0, in_h = 0;
    uint8_t *input() { return (uint8_t *)in_.virt; }
    size_t outputs() const { return outs_.size(); }
    const float *out(size_t i) const { return (const float *)outs_[i].virt; }
    const std::vector<int> &shape(size_t i) const { return shapes_[i]; }
    size_t count(size_t i) const { return counts_[i]; }
    bool run(float *ms);

private:
    struct Buf {
        uint32_t phys = 0, size = 0;
        void *virt = nullptr;
    };
    bool alloc(Buf &b, uint32_t size);
    void release(Buf &b);

    std::vector<unsigned char> blob_;
    interpreter *ip_ = nullptr;
    int shm_fd_ = -1, mem_fd_ = -1;
    Buf in_;
    std::vector<Buf> outs_;
    std::vector<std::vector<int>> shapes_;
    std::vector<size_t> counts_;
};

bool Net::alloc(Buf &b, uint32_t size)
{
    share_memory_alloc_align_args a;
    a.size = (size + 4095) & ~4095u;
    a.alignment = MEMORY_TEST_BLOCK_ALIGN;
    a.phyAddr = 0;
    if (ioctl(shm_fd_, SHARE_MEMORY_ALIGN_ALLOC, &a) < 0)
        return false;
    b.phys = a.phyAddr;
    b.size = a.size;
    b.virt = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd_, b.phys);
    if (b.virt == MAP_FAILED) {
        b.virt = nullptr;
        release(b);
        return false;
    }
    return true;
}

void Net::release(Buf &b)
{
    if (b.virt)
        munmap(b.virt, b.size);
    if (b.phys)
        ioctl(shm_fd_, SHARE_MEMORY_FREE, &b.phys);
    b = Buf();
}

Net::~Net()
{
    delete ip_;
    release(in_);
    for (auto &b : outs_)
        release(b);
    if (shm_fd_ >= 0)
        close(shm_fd_);
    if (mem_fd_ >= 0)
        close(mem_fd_);
}

bool Net::load(const std::string &path, bool verbose)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "kpu: cannot open %s\n", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    blob_.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(blob_.data(), 1, blob_.size(), f);
    fclose(f);
    if (got != blob_.size())
        return false;

    ip_ = new interpreter();
    if (!ip_->load_model({(const gsl::byte *)blob_.data(), blob_.size()}).is_ok()) {
        fprintf(stderr, "kpu: %s: not a kmodel for this runtime\n", path.c_str());
        return false;
    }
    shm_fd_ = open(SHARE_MEMORY_DEV, O_RDWR);
    mem_fd_ = open(MAP_MEMORY_DEV, O_RDWR | O_SYNC);
    if (shm_fd_ < 0 || mem_fd_ < 0)
        return false;

    auto ishape = ip_->input_shape(0);
    std::vector<int> is(ishape.begin(), ishape.end());
    if (is.size() != 4 || is[1] != 3) {
        fprintf(stderr, "kpu: %s: expected a [1,3,H,W] input\n", path.c_str());
        return false;
    }
    in_h = is[2];
    in_w = is[3];
    const uint32_t in_bytes = (uint32_t)in_w * in_h * 3;
    if (!alloc(in_, in_bytes))
        return false;
    auto it = host_runtime_tensor::create(dt_uint8, ishape, {(gsl::byte *)in_.virt, in_bytes}, false,
                                          hrt::pool_shared, in_.phys);
    if (!it.is_ok() || !ip_->input_tensor(0, it.unwrap()).is_ok())
        return false;

    for (size_t i = 0; i < ip_->outputs_size(); i++) {
        auto shape = ip_->output_shape(i);
        std::vector<int> s(shape.begin(), shape.end());
        size_t count = 1;
        for (int d : s)
            count *= d;
        Buf b;
        if (!alloc(b, count * sizeof(float)))
            return false;
        auto ot = host_runtime_tensor::create(dt_float32, shape, {(gsl::byte *)b.virt, count * sizeof(float)}, false,
                                              hrt::pool_shared, b.phys);
        if (!ot.is_ok() || !ip_->output_tensor(i, ot.unwrap()).is_ok())
            return false;
        outs_.push_back(b);
        shapes_.push_back(s);
        counts_.push_back(count);
    }
    if (verbose)
        printf("kpu: loaded %s (%zu KB)\n", path.c_str(), blob_.size() / 1024);
    return true;
}

bool Net::run(float *ms)
{
    auto t0 = Clock::now();
    bool ok = ip_->run().is_ok();
    if (ms)
        *ms += ms_since(t0);
    return ok;
}

int inspect_kmodel(const std::string &path)
{
    Net n;
    if (!n.load(path))
        return 1;
    printf("input  [1,3,%d,%d]\n", n.in_h, n.in_w);
    for (size_t i = 0; i < n.outputs(); i++) {
        printf("output %zu [", i);
        for (size_t k = 0; k < n.shape(i).size(); k++)
            printf("%s%d", k ? "," : "", n.shape(i)[k]);
        printf("]\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// helpers

static float iou(const DetObj &a, const DetObj &b)
{
    float ix = std::max(0.f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
    float iy = std::max(0.f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    float inter = ix * iy;
    float u = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return u > 0 ? inter / u : 0;
}

// Class agnostic NMS, best first. A car also scored as truck is one vehicle.
static void nms_inplace(std::vector<DetObj> &v, float thr, size_t limit = 100)
{
    std::sort(v.begin(), v.end(), [](const DetObj &a, const DetObj &b) { return a.score > b.score; });
    std::vector<DetObj> keep;
    for (auto &b : v) {
        if (keep.size() >= limit)
            break;
        if (std::none_of(keep.begin(), keep.end(), [&](const DetObj &k) { return iou(k, b) > thr; }))
            keep.push_back(b);
    }
    v.swap(keep);
}

static float sigmoid(float x)
{
    return 1.f / (1.f + expf(-x));
}

// Element of an [1,C,H,W] (or [1,H,W,C]) tensor: channel c at cell (row-major y*W+x).
struct Grid {
    const float *p = nullptr;
    int c = 0, cells = 0;
    bool nchw = true;
    float at(int ch, int cell) const { return nchw ? p[(size_t)ch * cells + cell] : p[(size_t)cell * c + ch]; }
};

// ---------------------------------------------------------------------------
// Engine

struct Engine::Impl {
    Net net, net2;
    std::vector<uint8_t> img;  // copy of the stage 1 input for crops (cached memory)
    cv::Mat plane[3];          // views into img
    float infer = 0;

    // copy of the input once per frame, only for two-stage models
    void snapshot()
    {
        size_t n = (size_t)net.in_w * net.in_h;
        img.resize(n * 3);
        memcpy(img.data(), net.input(), n * 3);
        for (int c = 0; c < 3; c++)
            plane[c] = cv::Mat(net.in_h, net.in_w, CV_8UC1, img.data() + c * n);
    }
    cv::Mat plane2(int c)
    {
        return cv::Mat(net2.in_h, net2.in_w, CV_8UC1, net2.input() + (size_t)c * net2.in_w * net2.in_h);
    }
    // crop [x0,y0,x1,y1] of the stage 1 image resized into the stage 2 input
    bool crop_to_net2(float x0, float y0, float x1, float y1)
    {
        cv::Rect r(cv::Point((int)std::max(0.f, x0), (int)std::max(0.f, y0)),
                   cv::Point((int)std::min<float>(net.in_w, x1), (int)std::min<float>(net.in_h, y1)));
        if (r.width < 4 || r.height < 4)
            return false;
        for (int c = 0; c < 3; c++) {
            cv::Mat dst = plane2(c);
            cv::resize(plane[c](r), dst, dst.size(), 0, 0, cv::INTER_AREA);
        }
        return true;
    }
};

Engine::Engine() : p_(new Impl()) {}
Engine::~Engine() {}

int Engine::input_w() const { return p_->net.in_w; }
int Engine::input_h() const { return p_->net.in_h; }
uint8_t *Engine::input() { return p_->net.input(); }

bool Engine::load(const ModelInfo &m)
{
    info_ = m;
    if (!p_->net.load(m.path))
        return false;
    if (p_->net.in_w != m.input_w || p_->net.in_h != m.input_h) {
        fprintf(stderr, "kpu: %s: input is %dx%d, catalog says %dx%d\n", m.id.c_str(), p_->net.in_w, p_->net.in_h,
                m.input_w, m.input_h);
        return false;
    }
    if (!m.path2.empty() && !p_->net2.load(m.path2))
        return false;
    const Net &n = p_->net;
    if (m.family == "yolov5" || m.family == "yolov8" || m.family == "simplepose") {
        if (n.outputs() != m.strides.size())
            return false;
        for (size_t i = 0; i < n.outputs(); i++) {
            const auto &s = n.shape(i);
            if (s.size() != 4 || s[1] != m.input_h / m.strides[i] || s[2] != m.input_w / m.strides[i]) {
                fprintf(stderr, "kpu: %s output %zu has an unexpected shape\n", m.id.c_str(), i);
                return false;
            }
        }
    }
    return true;
}

// ---- YOLO (NHWC per stride, see models/export.py)

static void decode_v5(const ModelInfo &m, const Net &n, float thresh, std::vector<DetObj> &out)
{
    for (size_t i = 0; i < n.outputs(); i++) {
        const auto &sh = n.shape(i);
        const int gh = sh[1], gw = sh[2], ch = sh[3];
        const std::vector<float> &anc = m.anchors[i];
        const int na = anc.size() / 2, rec = ch / na, nc = rec - 5;
        const float s = m.strides[i];
        const float *o = n.out(i);
        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                const float *cell = o + ((size_t)y * gw + x) * ch;
                for (int a = 0; a < na; a++) {
                    const float *r = cell + a * rec;
                    float obj = r[4];
                    if (obj < thresh)  // score = obj * cls <= obj: skip the class scan
                        continue;
                    int best = 0;
                    for (int c = 1; c < nc; c++)
                        if (r[5 + c] > r[5 + best])
                            best = c;
                    float score = obj * r[5 + best];
                    if (score < thresh)
                        continue;
                    float cx = (r[0] * 2 - 0.5f + x) * s, cy = (r[1] * 2 - 0.5f + y) * s;
                    float w = (r[2] * 2) * (r[2] * 2) * anc[2 * a], h = (r[3] * 2) * (r[3] * 2) * anc[2 * a + 1];
                    DetObj d;
                    d.x1 = cx - w / 2, d.y1 = cy - h / 2, d.x2 = cx + w / 2, d.y2 = cy + h / 2;
                    d.score = score;
                    d.label = best;
                    out.push_back(d);
                }
            }
        }
    }
}

static void decode_v8(const ModelInfo &m, const Net &n, float thresh, std::vector<DetObj> &out)
{
    const int rm = m.reg_max;
    for (size_t i = 0; i < n.outputs(); i++) {
        const auto &sh = n.shape(i);
        const int gh = sh[1], gw = sh[2], ch = sh[3], nc = ch - 4 * rm;
        const float s = m.strides[i];
        const float *o = n.out(i);
        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                const float *p = o + ((size_t)y * gw + x) * ch;
                const float *cls = p + 4 * rm;
                int best = 0;
                for (int c = 1; c < nc; c++)
                    if (cls[c] > cls[best])
                        best = c;
                if (cls[best] < thresh)
                    continue;
                float d[4];
                for (int k = 0; k < 4; k++) {  // DFL: expected bin of a softmax
                    const float *b = p + k * rm;
                    float mx = b[0];
                    for (int j = 1; j < rm; j++)
                        mx = std::max(mx, b[j]);
                    float sum = 0, acc = 0;
                    for (int j = 0; j < rm; j++) {
                        float e = expf(b[j] - mx);
                        sum += e;
                        acc += e * j;
                    }
                    d[k] = acc / sum * s;
                }
                float cx = (x + 0.5f) * s, cy = (y + 0.5f) * s;
                DetObj o2;
                o2.x1 = cx - d[0], o2.y1 = cy - d[1], o2.x2 = cx + d[2], o2.y2 = cy + d[3];
                o2.score = cls[best];
                o2.label = best;
                out.push_back(o2);
            }
        }
    }
}

// ---- RetinaFace family (faces: 5 points, base 16; plates LPD: 4 points, base 24)

static bool decode_retina(const Net &n, int npts, float base, float thresh, float nms, std::vector<DetObj> &out)
{
    const int strides[3] = {8, 16, 32};
    for (int l = 0; l < 3; l++) {
        const int s = strides[l], gw = n.in_w / s, gh = n.in_h / s, cells = gw * gh;
        Grid loc, conf, lm;
        for (size_t i = 0; i < n.outputs(); i++) {
            const auto &sh = n.shape(i);
            if (sh.size() != 4)
                continue;
            bool nchw = sh[2] == gh && sh[3] == gw, nhwc = sh[1] == gh && sh[2] == gw;
            if (!nchw && !nhwc)
                continue;
            int c = nchw ? sh[1] : sh[3];
            Grid g{n.out(i), c, cells, nchw};
            if (c == 8)
                loc = g;
            else if (c == 4)
                conf = g;
            else if (c == 4 * npts)
                lm = g;
        }
        if (!loc.p || !conf.p || !lm.p)
            return false;
        for (int cell = 0; cell < cells; cell++) {
            const float pcx = (cell % gw + 0.5f) * s, pcy = (cell / gw + 0.5f) * s;
            for (int a = 0; a < 2; a++) {
                float prob = sigmoid(conf.at(a * 2 + 1, cell) - conf.at(a * 2, cell));  // softmax of 2
                if (prob < thresh)
                    continue;
                const float ps = base * (a ? 2 : 1) * (1 << (2 * l));
                float cx = pcx + loc.at(a * 4 + 0, cell) * 0.1f * ps;
                float cy = pcy + loc.at(a * 4 + 1, cell) * 0.1f * ps;
                float w = ps * expf(loc.at(a * 4 + 2, cell) * 0.2f);
                float h = ps * expf(loc.at(a * 4 + 3, cell) * 0.2f);
                DetObj d;
                d.x1 = cx - w / 2, d.y1 = cy - h / 2, d.x2 = cx + w / 2, d.y2 = cy + h / 2;
                d.score = prob;
                for (int k = 0; k < npts; k++) {
                    d.pts.push_back(pcx + lm.at(a * 2 * npts + 2 * k, cell) * 0.1f * ps);
                    d.pts.push_back(pcy + lm.at(a * 2 * npts + 2 * k + 1, cell) * 0.1f * ps);
                }
                out.push_back(d);
            }
        }
    }
    nms_inplace(out, nms, 32);
    return true;
}

// ---- SCRFD person (5 strides, one anchor per cell, distances to the sides)

static bool decode_scrfd(const Net &n, float thresh, float nms, std::vector<DetObj> &out)
{
    for (int s = 8; s <= 128; s *= 2) {
        // 480 / 128 = 3.75: the network rounds the grid up
        const int gw = (n.in_w + s - 1) / s, gh = (n.in_h + s - 1) / s, cells = gw * gh;
        const float *score = nullptr, *box = nullptr;
        for (size_t i = 0; i < n.outputs(); i++) {  // [cells, 1] scores, [cells, 4] boxes
            const auto &sh = n.shape(i);
            if (sh.size() != 2 || sh[0] != cells)
                continue;
            if (sh[1] == 1 && !score)
                score = n.out(i);
            else if (sh[1] == 4 && !box)
                box = n.out(i);
        }
        if (!score || !box)
            return false;
        for (int c = 0; c < cells; c++) {
            if (score[c] < thresh)
                continue;
            float ax = (c % gw) * s, ay = (c / gw) * s;
            const float *b = box + (size_t)c * 4;
            DetObj d;
            d.x1 = ax - b[0] * s, d.y1 = ay - b[1] * s, d.x2 = ax + b[2] * s, d.y2 = ay + b[3] * s;
            d.score = score[c];
            out.push_back(d);
        }
    }
    nms_inplace(out, nms);
    return true;
}

// ---- hands: tiny YOLOv3 with decoding inside the kmodel, rows of cx,cy,w,h,score,class

static void decode_hands(const Net &n, float thresh, float nms, std::vector<DetObj> &out)
{
    const float *o = n.out(0);
    size_t rows = n.count(0) / 6;
    for (size_t i = 0; i < rows; i++) {
        const float *r = o + i * 6;
        if (r[4] < thresh)
            continue;
        DetObj d;
        d.x1 = r[0] - r[2] / 2, d.y1 = r[1] - r[3] / 2, d.x2 = r[0] + r[2] / 2, d.y2 = r[1] + r[3] / 2;
        d.score = r[4];
        out.push_back(d);
    }
    nms_inplace(out, nms, 8);
}

// ---- OpenPose (heatmaps + part affinity fields, grouping by pafprocess)

static bool decode_openpose(const Net &n, std::vector<DetObj> &out)
{
    if (n.outputs() < 4)
        return false;
    const auto &sh = n.shape(0);  // [1, H, W, 19]
    if (sh.size() != 4)
        return false;
    const int h = sh[1], w = sh[2], c0 = sh[3], c1 = n.shape(1)[3];
    static std::vector<float> peaks;
    const size_t cnt = (size_t)h * w * c0;
    peaks.assign(n.out(2), n.out(2) + cnt);
    const float *mx = n.out(3);  // max pooled: keep local maxima only
    for (size_t i = 0; i < cnt; i++)
        if (peaks[i] < mx[i])
            peaks[i] = 0;
    process_paf(h, w, c0, peaks.data(), h, w, c0, (float *)n.out(0), h, w, c1, (float *)n.out(1));
    for (int hid = 0; hid < get_num_humans(); hid++) {
        DetObj d;
        d.shape = SHAPE_OPENPOSE18;
        d.name = "person";
        float x1 = 1e9, y1 = 1e9, x2 = -1, y2 = -1;
        for (int part = 0; part < NUM_PART; part++) {
            int cid = get_part_cid(hid, part);
            if (cid < 0) {
                d.pts.push_back(-1);
                d.pts.push_back(-1);
                continue;
            }
            float px = (float)get_part_x(cid) / w * n.in_w, py = (float)get_part_y(cid) / h * n.in_h;
            d.pts.push_back(px);
            d.pts.push_back(py);
            x1 = std::min(x1, px), y1 = std::min(y1, py), x2 = std::max(x2, px), y2 = std::max(y2, py);
            d.score = std::max(d.score, get_part_score(cid));
        }
        if (x2 < 0)
            continue;
        d.x1 = x1, d.y1 = y1, d.x2 = x2, d.y2 = y2;
        out.push_back(d);
    }
    return true;
}

// ---- second stages

static void face_landmarks(Engine::Impl &p, DetObj &d)
{
    // square 1.1 x the long side around the center (demo: get_enlarged_box)
    float cx = (d.x1 + d.x2) / 2, cy = (d.y1 + d.y2) / 2;
    float side = 1.1f * std::max(d.x2 - d.x1, d.y2 - d.y1);
    float x0 = std::max(0.f, cx - side / 2), y0 = std::max(0.f, cy - side / 2);
    float x1 = std::min<float>(p.net.in_w, cx + side / 2), y1 = std::min<float>(p.net.in_h, cy + side / 2);
    if (!p.crop_to_net2(x0, y0, x1, y1) || !p.net2.run(&p.infer))
        return;
    const float *o = p.net2.out(0);
    int npts = p.net2.count(0) / 2;
    d.pts.clear();
    for (int k = 0; k < npts; k++) {
        d.pts.push_back(o[2 * k] * (x1 - x0) + x0);
        d.pts.push_back(o[2 * k + 1] * (y1 - y0) + y0);
    }
    d.shape = SHAPE_DOTS;
}

static void face_expression(Engine::Impl &p, DetObj &d)
{
    static const char *labels[] = {"Neutral", "Happy", "Sad", "Surprise", "Fear", "Disgust", "Anger", "Contempt"};
    // eyes and nose onto a 224x224 template (demo: dstTri, first 3 points)
    cv::Point2f src[3], dst[3] = {{76.5892f, 103.3926f}, {147.0636f, 103.0028f}, {112.0504f, 143.4732f}};
    for (int k = 0; k < 3; k++)
        src[k] = cv::Point2f(d.pts[2 * k], d.pts[2 * k + 1]);
    cv::Mat warp = cv::getAffineTransform(src, dst);
    for (int c = 0; c < 3; c++) {
        cv::Mat out = p.plane2(c);
        cv::warpAffine(p.plane[c], out, warp, out.size());
    }
    if (!p.net2.run(&p.infer))
        return;
    const float *o = p.net2.out(0);
    int n = std::min<int>(p.net2.count(0), 8), best = 0;
    float mx = o[0], sum = 0;
    for (int k = 1; k < n; k++)
        if (o[k] > o[best])
            best = k;
    mx = o[best];
    for (int k = 0; k < n; k++)
        sum += expf(o[k] - mx);
    char t[48];
    snprintf(t, sizeof(t), "%s %.2f", labels[best], 1.f / sum);
    d.text = t;
}

static void head_pose(Engine::Impl &p, DetObj &d)
{
    const float edge = p.net2.in_w;
    float w = d.x2 - d.x1, h = d.y2 - d.y1, cx = (d.x1 + d.x2) / 2, cy = (d.y1 + d.y2) / 2;
    float scale = edge * 2.f / (2.7f * std::max(w, h));
    float m[2][3] = {{scale, 0, edge / 2 - scale * cx}, {0, scale, edge / 2 - scale * cy}};
    cv::Mat warp(2, 3, CV_32FC1, m);
    for (int c = 0; c < 3; c++) {
        cv::Mat out = p.plane2(c);
        cv::warpAffine(p.plane[c], out, warp, out.size());
    }
    if (!p.net2.run(&p.infer) || p.net2.count(0) < 12)
        return;
    const float *o = p.net2.out(0);
    float R[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            R[r][c] = o[r * 4 + c];
    float sy = sqrtf(R[0][0] * R[0][0] + R[1][0] * R[1][0]);
    float pitch, yaw, roll;
    if (sy < 1e-6f) {
        pitch = atan2f(-R[1][2], R[1][1]), yaw = atan2f(-R[2][0], sy), roll = 0;
    } else {
        pitch = atan2f(R[2][1], R[2][2]), yaw = atan2f(-R[2][0], sy), roll = atan2f(R[1][0], R[0][0]);
    }
    char t[64];
    snprintf(t, sizeof(t), "yaw %.0f pitch %.0f roll %.0f", yaw * 57.3f, pitch * 57.3f, roll * 57.3f);
    d.text = t;
    // axes from the face center, as the demo projects its cube
    float rad = 0.5f * std::max(w, h);
    d.pts = {cx, cy};
    for (int k = 0; k < 3; k++) {
        d.pts.push_back(cx + rad * R[k][0]);
        d.pts.push_back(cy - rad * R[k][1]);
    }
    d.shape = SHAPE_AXES;
}

static void plate_text(Engine::Impl &p, DetObj &d)
{
    static const char *labels[] = {
        "BJ", "SH", "TJ", "CQ", "HE", "SX", "NM", "LN", "JL", "HL", "JS", "ZJ", "AH", "FJ", "JX", "SD", "HA", "HB",
        "HN", "GD", "GX", "HI", "SC", "GZ", "YN", "XZ", "SN", "GS", "QH", "NX", "XJ", "HK", "MO", "TW", "xue", "shi",
        "ling", "jing", "gua", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F", "G",
        "H", "J", "K", "L", "M", "N", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "I", "O", "-"};
    const int nchar = 76;
    if (d.pts.size() < 8)
        return;
    // the 4 corners onto the 94x24 input (demo: dstTri)
    const float W = p.net2.in_w, H = p.net2.in_h;
    cv::Point2f src[4], dst[4] = {{W, H}, {0, H}, {0, 0}, {W, 0}};
    for (int k = 0; k < 4; k++)
        src[k] = cv::Point2f(d.pts[2 * k], d.pts[2 * k + 1]);
    cv::Mat warp = cv::getPerspectiveTransform(src, dst);
    for (int c = 0; c < 3; c++) {
        cv::Mat out = p.plane2(c);
        cv::warpPerspective(p.plane[c], out, warp, out.size());
    }
    if (!p.net2.run(&p.infer))
        return;
    const float *o = p.net2.out(0);
    const int len = p.net2.count(0) / nchar;  // [chars][positions]
    std::string s;
    int prev = nchar - 1;
    for (int pos = 0; pos < len; pos++) {  // greedy CTC: argmax, drop repeats and blanks
        int best = 0;
        for (int c = 1; c < nchar; c++)
            if (o[c * len + pos] > o[best * len + pos])
                best = c;
        if (best != nchar - 1 && best != prev)
            s += labels[best];
        prev = best;
    }
    d.text = s;
}

static void hand_landmarks(Engine::Impl &p, DetObj &d)
{
    float cx = (d.x1 + d.x2) / 2, cy = (d.y1 + d.y2) / 2;
    float side = 1.26f * std::max(d.x2 - d.x1, d.y2 - d.y1);
    float x0 = std::max(0.f, cx - side / 2), y0 = std::max(0.f, cy - side / 2);
    float x1 = std::min<float>(p.net.in_w, cx + side / 2), y1 = std::min<float>(p.net.in_h, cy + side / 2);
    if (!p.crop_to_net2(x0, y0, x1, y1) || !p.net2.run(&p.infer))
        return;
    const float *o = p.net2.out(0);
    int npts = std::min<int>(p.net2.count(0) / 2, 21);
    for (int k = 0; k < npts; k++) {
        d.pts.push_back(o[2 * k] * (x1 - x0) + x0);
        d.pts.push_back(o[2 * k + 1] * (y1 - y0) + y0);
    }
    d.shape = SHAPE_HAND21;
}

static void simple_pose(Engine::Impl &p, DetObj &d)
{
    // demo: get_enlarged_box, 1.4 x width and 1.2 x height around the center
    float w = d.x2 - d.x1, h = d.y2 - d.y1, cx = (d.x1 + d.x2) / 2, cy = (d.y1 + d.y2) / 2;
    float x0 = std::max(0.f, cx - 0.7f * w), y0 = std::max(0.f, cy - 0.6f * h);
    float x1 = std::min<float>(p.net.in_w, cx + 0.7f * w), y1 = std::min<float>(p.net.in_h, cy + 0.6f * h);
    if (!p.crop_to_net2(x0, y0, x1, y1) || !p.net2.run(&p.infer))
        return;
    const float *o = p.net2.out(0);
    const int oh = p.net2.in_h / 4, ow = p.net2.in_w / 4;
    const int npts = std::min<int>(p.net2.count(0) / (oh * ow), 17);
    for (int k = 0; k < npts; k++) {  // argmax of each heatmap
        const float *hm = o + (size_t)k * oh * ow;
        int best = 0;
        for (int i = 1; i < oh * ow; i++)
            if (hm[i] > hm[best])
                best = i;
        bool ok = hm[best] > 0.2f;
        d.pts.push_back(ok ? (best % ow) * (x1 - x0) / ow + x0 : -1);
        d.pts.push_back(ok ? (best / ow) * (y1 - y0) / oh + y0 : -1);
    }
    d.shape = SHAPE_COCO17;
}

bool Engine::process(float thresh, float nms, std::vector<DetObj> &out, float *infer_ms, float *post_ms)
{
    Impl &p = *p_;
    const ModelInfo &m = info_;
    if (m.thresh >= 0)
        thresh = m.thresh;
    if (m.nms >= 0)
        nms = m.nms;
    p.infer = 0;
    auto t0 = Clock::now();
    if (!m.path2.empty())
        p.snapshot();  // before the KPU run: its input buffer is ours to read either way
    if (!p.net.run(&p.infer))
        return false;

    out.clear();
    const std::string &f = m.family;
    const size_t kMaxStage2 = 6;  // bounds the frame time with many faces/hands
    if (f == "yolov5" || f == "simplepose") {
        decode_v5(m, p.net, thresh, out);
        nms_inplace(out, nms);
        if (f == "simplepose") {
            out.erase(std::remove_if(out.begin(), out.end(), [](const DetObj &d) { return d.label != 0; }),
                      out.end());
            if (out.size() > kMaxStage2)
                out.resize(kMaxStage2);
            for (auto &d : out)
                simple_pose(p, d);
        }
    } else if (f == "yolov8") {
        decode_v8(m, p.net, thresh, out);
        nms_inplace(out, nms);
    } else if (f == "retinaface" || f == "face_pfld" || f == "face_fer" || f == "face_hpe") {
        if (!decode_retina(p.net, 5, 16, thresh, nms, out))
            return false;
        for (size_t i = 0; i < out.size(); i++) {
            DetObj &d = out[i];
            d.name = "face";
            d.shape = SHAPE_DOTS;
            if (i >= kMaxStage2)
                continue;
            if (f == "face_pfld")
                face_landmarks(p, d);
            else if (f == "face_fer")
                face_expression(p, d);
            else if (f == "face_hpe")
                head_pose(p, d);
        }
    } else if (f == "plate") {
        if (!decode_retina(p.net, 4, 24, thresh, nms, out))
            return false;
        for (size_t i = 0; i < out.size(); i++) {
            out[i].name = "plate";
            out[i].shape = SHAPE_POLYGON;
            if (i < kMaxStage2)
                plate_text(p, out[i]);
        }
    } else if (f == "scrfd") {
        if (!decode_scrfd(p.net, thresh, nms, out))
            return false;
        for (auto &d : out)
            d.name = "person";
    } else if (f == "hand") {
        decode_hands(p.net, thresh, nms, out);
        for (size_t i = 0; i < out.size(); i++) {
            out[i].name = "hand";
            if (i < kMaxStage2)
                hand_landmarks(p, out[i]);
        }
    } else if (f == "openpose") {
        if (!decode_openpose(p.net, out))
            return false;
    } else {
        return false;
    }

    // input pixels -> camera image 0..1
    const float px = m.center ? (m.input_w - m.valid_w) / 2.f : 0, py = m.center ? (m.input_h - m.valid_h) / 2.f : 0;
    auto nx = [&](float v) { return std::max(0.f, std::min(1.f, (v - px) / m.valid_w)); };
    auto ny = [&](float v) { return std::max(0.f, std::min(1.f, (v - py) / m.valid_h)); };
    for (auto &d : out) {
        d.x1 = nx(d.x1), d.y1 = ny(d.y1), d.x2 = nx(d.x2), d.y2 = ny(d.y2);
        for (size_t k = 0; k + 1 < d.pts.size(); k += 2) {
            if (d.pts[k] < 0)
                continue;
            d.pts[k] = nx(d.pts[k]);
            d.pts[k + 1] = ny(d.pts[k + 1]);
        }
        if (d.name.empty())
            d.name = d.label >= 0 && d.label < (int)m.labels.size() ? m.labels[d.label] : "?";
    }
    if (infer_ms)
        *infer_ms = p.infer;
    if (post_ms)
        *post_ms = std::max(0.f, ms_since(t0) - p.infer);
    return true;
}

// ---------------------------------------------------------------------------

const std::vector<std::pair<int, int>> &shape_edges(int shape)
{
    static const std::vector<std::pair<int, int>> none, hand = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {0, 5}, {5, 6}, {6, 7}, {7, 8}, {0, 9}, {9, 10}, {10, 11}, {11, 12}, {0, 13}, {13, 14}, {14, 15}, {15, 16}, {0, 17}, {17, 18}, {18, 19}, {19, 20}},
        coco = {{15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12}, {5, 6}, {5, 7}, {6, 8}, {7, 9}, {8, 10}, {1, 2}, {0, 1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4, 6}},
        openpose = {{1, 2}, {1, 5}, {2, 3}, {3, 4}, {5, 6}, {6, 7}, {1, 8}, {8, 9}, {9, 10}, {1, 11}, {11, 12}, {12, 13}, {1, 0}, {0, 14}, {14, 16}, {0, 15}, {15, 17}},
        polygon = {{0, 1}, {1, 2}, {2, 3}, {3, 0}}, axes = {{0, 1}, {0, 2}, {0, 3}};
    switch (shape) {
    case SHAPE_HAND21: return hand;
    case SHAPE_COCO17: return coco;
    case SHAPE_OPENPOSE18: return openpose;
    case SHAPE_POLYGON: return polygon;
    case SHAPE_AXES: return axes;
    default: return none;
    }
}

void draw_points(cv::Mat &img, const cv::Rect &area, const std::vector<float> &pts, int shape, const cv::Scalar &color,
                 int thickness)
{
    auto P = [&](size_t k) {
        return cv::Point(area.x + (int)(pts[2 * k] * area.width), area.y + (int)(pts[2 * k + 1] * area.height));
    };
    const size_t n = pts.size() / 2;
    auto ok = [&](size_t k) { return k < n && pts[2 * k] >= 0; };
    static const cv::Scalar axis[3] = {cv::Scalar(60, 60, 255, 255), cv::Scalar(60, 220, 60, 255),
                                       cv::Scalar(255, 120, 40, 255)};
    const auto &edges = shape_edges(shape);
    for (size_t e = 0; e < edges.size(); e++) {
        int a = edges[e].first, b = edges[e].second;
        if (ok(a) && ok(b))
            cv::line(img, P(a), P(b), shape == SHAPE_AXES ? axis[e % 3] : color, thickness, cv::LINE_AA);
    }
    if (shape == SHAPE_AXES)
        return;
    int r = std::max(2, thickness + (n > 30 ? 0 : 1));
    for (size_t k = 0; k < n; k++)
        if (ok(k))
            cv::circle(img, P(k), r, color, cv::FILLED, cv::LINE_AA);
}
