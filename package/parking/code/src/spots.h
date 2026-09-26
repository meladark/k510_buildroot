#pragma once

#include <vector>

#include "state.h"

// Re-evaluates the spots of camera `cam` against fresh detections.
// Call with g_state.mtx held.
void evaluate_spots(int cam, const std::vector<Detection> &dets, const Config &cfg,
                    std::vector<Spot> &spots, uint64_t spots_version, int64_t now_ms);

bool is_vehicle(const Config &cfg, const std::string &name);
