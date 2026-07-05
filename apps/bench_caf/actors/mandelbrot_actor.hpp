// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>

#include "../caf_bench_config.hpp"

#include <atomic>
#include <cstdint>

namespace hpactor::apps::bench_caf {

inline constexpr TypeTag MandelTaskTag{0x00010308};
inline constexpr TypeTag MandelDoneTag{0x00010309};

struct MandelDimensions {
    uint32_t width = 128;
    uint32_t height = 128;
    uint32_t max_iterations = 256;
    uint32_t workers = 4;
};

inline MandelDimensions mandel_dimensions_for_preset(PresetKind preset) {
    switch (preset) {
        case PresetKind::Smoke:
            return {128, 128, 256, 4};
        case PresetKind::Nightly:
            return {512, 512, 1024, 8};
        case PresetKind::PaperScale:
            return {1024, 1024, 4096, 16};
        case PresetKind::Stress:
            return {4096, 4096, 10000, 32};
    }
    return {128, 128, 256, 4};
}

inline uint32_t mandel_iter(double cx, double cy, uint32_t max_iters) {
    double zx = 0.0, zy = 0.0;
    uint32_t iter = 0;
    while (zx * zx + zy * zy <= 4.0 && iter < max_iters) {
        double tmp = zx * zx - zy * zy + cx;
        zy = 2.0 * zx * zy + cy;
        zx = tmp;
        ++iter;
    }
    return iter;
}

struct MandelCounters {
    std::atomic<uint64_t> workers_done{0};
    std::atomic<uint64_t> total_iters{0};
};

class MandelWorkerActor : public EventBasedActor {
  public:
    MandelWorkerActor(ActorContext* ctx, ActorSystem& sys,
                      MandelCounters* counters, double xmin, double xmax,
                      double ymin, double ymax, uint32_t width,
                      uint32_t row_start, uint32_t row_end, uint32_t max_iters)
        : EventBasedActor(ctx, sys), counters_(counters), xmin_(xmin),
          xmax_(xmax), ymin_(ymin), ymax_(ymax), width_(width),
          row_start_(row_start), row_end_(row_end), max_iters_(max_iters) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MandelTaskTag)
                return;
            uint64_t sum = 0;
            for (uint32_t row = row_start_; row < row_end_; ++row) {
                double cy =
                    ymin_ + (ymax_ - ymin_) * static_cast<double>(row) /
                                static_cast<double>(row_end_ - row_start_ + 1);
                for (uint32_t col = 0; col < width_; ++col) {
                    double cx = xmin_ + (xmax_ - xmin_) * static_cast<double>(col) /
                                            static_cast<double>(width_);
                    sum += mandel_iter(cx, cy, max_iters_);
                }
            }
            counters_->total_iters.fetch_add(sum, std::memory_order_relaxed);
            counters_->workers_done.fetch_add(1, std::memory_order_release);
        }};
    }

  private:
    MandelCounters* counters_ = nullptr;
    double xmin_ = -2.0, xmax_ = 1.0, ymin_ = -1.5, ymax_ = 1.5;
    uint32_t width_ = 0;
    uint32_t row_start_ = 0, row_end_ = 0;
    uint32_t max_iters_ = 256;
};

} // namespace hpactor::apps::bench_caf
