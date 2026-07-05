// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/runtime/runtime_blueprint_builder.hpp>

#include <hpactor/actor/system/actor_system.hpp> // for Config

#include <cstring>

namespace hpactor {
namespace {

/// \brief Simple FNV-1a 64-bit hash for fingerprint computation.
/// Deterministic across platforms. Not cryptographic.
uint64_t hash_bytes(const uint8_t* data, size_t len,
                    uint64_t seed = 0xcbf29ce484222325ULL) noexcept {
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t hash_u64(uint64_t v, uint64_t seed) noexcept {
    return hash_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v), seed);
}

uint64_t hash_string(const std::string& s, uint64_t seed) noexcept {
    return hash_bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size(), seed);
}

} // namespace

result<RuntimeBlueprint>
RuntimeBlueprintBuilder::from_config(const Config& config) noexcept {
    // ── Validation ──────────────────────────────────────────────────────────
    if (config.enable_network && config.tcp_port == 0) {
        return result<RuntimeBlueprint>::make(
            error(errors::invalid_argument, "network enabled but tcp_port is 0"));
    }

    // ── Build component configs ─────────────────────────────────────────────
    RuntimeBlueprint bp;

    bp.actor_.scheduler_threads = config.scheduler_threads;
    bp.actor_.max_queue_depth = config.max_queue_depth;
    bp.actor_.endpoint = config.endpoint;
    bp.actor_.use_coroutines = config.use_coroutines;
    bp.actor_.scheduler_start_paused = config.scheduler_start_paused;

    bp.messaging_.default_message_ttl_ms = config.default_message_ttl_ms;

    if (config.enable_network) {
        BlueprintNetworkConfig net;
        net.enabled = true;
        net.tcp_port = config.tcp_port;
        net.http_client_enabled = config.enable_http_client;
        net.http_gateway_enabled = config.enable_http_gateway;
        net.http_bind_host = config.http_bind_host;
        net.http_port = config.http_port;
        bp.network_ = std::move(net);
    }

    // ── Observability config ─────────────────────────────────────────────────
    bp.observability_.tracing_enabled = config.tracing.enabled;
    bp.observability_.tracing_ring_buffer_capacity =
        config.tracing.ring_buffer_capacity;

    // Cluster is disabled by default in Config; the concrete Config does not
    // carry an explicit cluster-enable flag. enable_cluster() is called
    // post-construction. Blueprint captures the default (disabled) until the
    // cluster factory is injected.
    bp.cluster_.enabled = false;

    // ── Compute fingerprint ─────────────────────────────────────────────────
    uint64_t fp = 0xcbf29ce484222325ULL; // FNV offset basis

    fp = hash_u64(static_cast<uint64_t>(bp.actor_.scheduler_threads), fp);
    fp = hash_u64(bp.actor_.max_queue_depth, fp);
    fp = hash_string(endpoint_ops::to_string(bp.actor_.endpoint), fp);
    fp = hash_u64(static_cast<uint64_t>(bp.actor_.use_coroutines), fp);
    fp = hash_u64(static_cast<uint64_t>(bp.actor_.scheduler_start_paused), fp);

    fp = hash_u64(
        static_cast<uint64_t>(bp.messaging_.default_message_ttl_ms.count()), fp);

    if (bp.network_.has_value()) {
        fp = hash_u64(1, fp); // network enabled marker
        fp = hash_u64(bp.network_->tcp_port, fp);
        fp = hash_u64(static_cast<uint64_t>(bp.network_->http_client_enabled), fp);
        fp = hash_u64(static_cast<uint64_t>(bp.network_->http_gateway_enabled), fp);
        fp = hash_string(bp.network_->http_bind_host, fp);
        fp = hash_u64(bp.network_->http_port, fp);
    } else {
        fp = hash_u64(0, fp); // network disabled marker
    }

    // Observability fingerprint
    fp = hash_u64(static_cast<uint64_t>(bp.observability_.metrics_enabled), fp);
    fp = hash_u64(static_cast<uint64_t>(bp.observability_.logging_enabled), fp);
    fp = hash_u64(static_cast<uint64_t>(bp.observability_.tracing_enabled), fp);
    fp = hash_u64(bp.observability_.metrics_ring_buffer_capacity, fp);
    fp = hash_u64(bp.observability_.logging_ring_buffer_capacity, fp);
    fp = hash_u64(bp.observability_.tracing_ring_buffer_capacity, fp);
    fp = hash_u64(
        static_cast<uint64_t>(bp.observability_.fault_injection_enabled), fp);

    // Cluster fingerprint
    fp = hash_u64(static_cast<uint64_t>(bp.cluster_.enabled), fp);
    fp = hash_string(bp.cluster_.node_id, fp);

    bp.fingerprint_ = fp;

    return result<RuntimeBlueprint>::make(std::move(bp));
}

ReloadReport
RuntimeBlueprintBuilder::diff(const RuntimeBlueprint& current,
                              const RuntimeBlueprint& candidate) noexcept {
    ReloadReport report;

    if (current.fingerprint() == candidate.fingerprint()) {
        report.fully_applied = true;
        report.summary = "no changes detected";
        return report;
    }

    // Fingerprints differ — classify the change.
    // In the full implementation, individual field descriptors from
    // ConfigFieldRegistry would be checked. For now, we classify
    // at the blueprint level: if only Live fields changed, we accept.
    //
    // Default: all changes require restart (conservative).
    report.restart_required_fields = 1;
    report.summary = "fingerprint mismatch — restart required";
    return report;
}

} // namespace hpactor
