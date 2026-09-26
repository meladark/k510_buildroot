#include "spots.h"

#include <algorithm>

// Spots are sampled on a coarse grid; coverage is the share of samples inside
// the spot polygon that fall into any vehicle footprint.
#define GRID_W 96
#define GRID_H 54

struct SpotCells {
    std::vector<Point2> cells;
};

static bool point_in_poly(const std::vector<Point2> &poly, float x, float y)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Point2 &a = poly[i], &b = poly[j];
        if (((a.y > y) != (b.y > y)) && (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x))
            in = !in;
    }
    return in;
}

static std::vector<SpotCells> g_cells[NUM_CAMS];
static uint64_t g_cells_version[NUM_CAMS] = {~0ull, ~0ull};

static void rebuild_cells(int cam, const std::vector<Spot> &spots)
{
    g_cells[cam].assign(spots.size(), SpotCells());
    for (size_t s = 0; s < spots.size(); s++) {
        if (spots[s].cam != cam)
            continue;
        const auto &poly = spots[s].pts;
        float minx = 1, miny = 1, maxx = 0, maxy = 0;
        for (auto &p : poly) {
            minx = std::min(minx, p.x);
            maxx = std::max(maxx, p.x);
            miny = std::min(miny, p.y);
            maxy = std::max(maxy, p.y);
        }
        for (int gy = 0; gy < GRID_H; gy++) {
            float y = (gy + 0.5f) / GRID_H;
            if (y < miny || y > maxy)
                continue;
            for (int gx = 0; gx < GRID_W; gx++) {
                float x = (gx + 0.5f) / GRID_W;
                if (x >= minx && x <= maxx && point_in_poly(poly, x, y))
                    g_cells[cam][s].cells.push_back(Point2{x, y});
            }
        }
        // tiny spots: fall back to the centroid so they still get a sample
        if (g_cells[cam][s].cells.empty()) {
            Point2 c = {0, 0};
            for (auto &p : poly) {
                c.x += p.x / poly.size();
                c.y += p.y / poly.size();
            }
            g_cells[cam][s].cells.push_back(c);
        }
    }
}

bool is_vehicle(const Config &cfg, const std::string &name)
{
    return std::find(cfg.vehicle_classes.begin(), cfg.vehicle_classes.end(), name) !=
           cfg.vehicle_classes.end();
}

void evaluate_spots(int cam, const std::vector<Detection> &dets, const Config &cfg,
                    std::vector<Spot> &spots, uint64_t spots_version, int64_t now_ms)
{
    if (g_cells_version[cam] != spots_version || g_cells[cam].size() != spots.size()) {
        rebuild_cells(cam, spots);
        g_cells_version[cam] = spots_version;
    }

    struct Box {
        float x1, y1, x2, y2;
    };
    std::vector<Box> feet;
    for (auto &d : dets) {
        if (!is_vehicle(cfg, d.name))
            continue;
        float h = d.y2 - d.y1;
        feet.push_back(Box{d.x1, d.y2 - h * cfg.footprint, d.x2, d.y2});
    }

    for (size_t s = 0; s < spots.size(); s++) {
        Spot &sp = spots[s];
        if (sp.cam != cam)
            continue;
        const auto &cells = g_cells[cam][s].cells;
        int hit = 0;
        for (auto &c : cells) {
            for (auto &b : feet) {
                if (c.x >= b.x1 && c.x <= b.x2 && c.y >= b.y1 && c.y <= b.y2) {
                    hit++;
                    break;
                }
            }
        }
        sp.coverage = cells.empty() ? 0.f : (float)hit / cells.size();
        bool target = sp.coverage >= cfg.occupancy_threshold;
        if (sp.since_ms == 0) {
            // first evaluation after start: take the state right away
            sp.occupied = target;
            sp.since_ms = now_ms;
            sp.streak = 0;
        } else if (target != sp.occupied) {
            if (++sp.streak >= cfg.debounce) {
                sp.occupied = target;
                sp.since_ms = now_ms;
                sp.streak = 0;
            }
        } else {
            sp.streak = 0;
        }
    }
}
