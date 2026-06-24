// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "command_utils.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/metrics/metrics_actor.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hpactor::cli {

StreamBuffer encode_as_frame(const std::string& protobuf_data) {
    StreamBuffer result;
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    result.append(magic.data(), 4);
    uint32_t payload_len = static_cast<uint32_t>(protobuf_data.size());
    uint32_t net_len = htonl(payload_len);
    result.append(reinterpret_cast<const uint8_t*>(&net_len), 4);
    result.append(reinterpret_cast<const uint8_t*>(protobuf_data.data()),
                  protobuf_data.size());
    return result;
}

} // namespace hpactor::cli

namespace hpactor::cli {

std::string format_bytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f GB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

void render_scheduler_workers(ActorSystem& sys, OutputFormatter& output) {
    auto* sched = sys.scheduler();
    if (!sched) {
        output.raw("Scheduler is not running.");
        return;
    }
    auto snaps = sched->worker_snapshots();
    output.header("Scheduler Workers (" +
                  std::to_string(sched->worker_count()) + " threads, A2WS)");
    if (snaps.empty()) {
        output.raw("Per-worker statistics not available.");
        return;
    }
    std::vector<std::string> cols = {
        "Worker", "Thread ID", "Work",  "IdleIters", "CV→block",
        "CV¬ify", "CV⏰",      "Model", "Steals",    "Idle"};
    std::vector<std::vector<std::string>> rows;
    for (auto& ws : snaps) {
        char tid_buf[24];
        snprintf(tid_buf, sizeof(tid_buf), "%llu",
                 static_cast<unsigned long long>(ws.thread_id));
        rows.push_back({
            std::to_string(ws.worker_index),
            tid_buf,
            std::to_string(ws.work_found),
            std::to_string(ws.idle_iters),
            std::to_string(ws.cv_escalations),
            std::to_string(ws.cv_notify_wakes),
            std::to_string(ws.cv_timeout_wakes),
            ws.idle_model,
            std::to_string(ws.steals_attempted),
            ws.is_idle ? "yes" : "no",
        });
    }
    output.table(cols, rows);
}

void render_metrics_show(ActorSystem& sys, OutputFormatter& output) {
    output.header("Metrics");
    auto* ma = sys.metrics_actor();
    if (!ma) {
        output.raw("Metrics subsystem is not enabled.");
        return;
    }
    output.raw(ma->format_snapshot());
}

} // namespace hpactor::cli
