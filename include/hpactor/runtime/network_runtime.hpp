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

#pragma once

#include <hpactor/runtime/network_runtime_ports.hpp>

#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/network_snapshot.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// ── Network error codes (10500–10599) ────────────────────────────────────────
// Defined here so both production and test code can reference them.

namespace hpactor {
namespace errors {
inline constexpr uint32_t network_disabled = 10'500;
inline constexpr uint32_t network_already_stopped = 10'508;
inline constexpr uint32_t network_already_running = 10'509;
inline constexpr uint32_t network_stop_deferred = 10'507;
} // namespace errors
} // namespace hpactor

namespace hpactor {

class ActorDirectory;
class MessagingRuntime;
namespace sched {
class IScheduler;
}

/// \brief Effective network configuration, consumed by NetworkRuntime.
///
/// Contains only network concerns; no TOML parser objects, no
/// ActorSystem::Config back-reference, and no mutable state.
struct NetworkRuntimeConfig {
    /// \brief Whether networking is enabled.
    bool enabled{false};

    /// \brief Local node endpoint.
    EndPoint endpoint{};

    /// \brief TCP port for listening (0 disables listening).
    uint16_t tcp_port{0};

    /// \brief TLS configuration.
    net::TlsConfig tls{};

    /// \brief Connection pool configuration.
    net::PoolConfig pool{};

    /// \brief Service discovery backend.
    /// \c nullptr auto-selects based on enabled and registrar config.
    std::shared_ptr<net::IServiceDiscovery> service_discovery{nullptr};

    /// \brief Registrar configuration for UDP-based discovery.
    net::RegistrarConfig registrar{};

    /// \brief Whether HTTP client is enabled.
    bool enable_http_client{false};

    /// \brief Whether HTTP gateway is enabled.
    bool enable_http_gateway{false};

    /// \brief HTTP bind host.
    std::string http_bind_host{};

    /// \brief HTTP port.
    uint16_t http_port{0};

    /// \brief Maximum RPC ask retries.
    uint32_t ask_max_retries{3};

    /// \brief Location cache purge interval in milliseconds.
    uint64_t cache_purge_ms{60000};

    /// \brief Reliable-retry poll interval in milliseconds.
    uint64_t retry_poll_ms{100};

    /// \brief Maximum inbound frame size (Phase 4 limit).
    size_t max_frame_size{16 * 1024 * 1024};
};

/// \brief Cohesive owner of all ActorSystem network resources.
///
/// Owns the TcpTransport (and its authoritative EventLoop), network
/// thread, service discovery, location cache, cache/retry timers,
/// RpcChannel, HttpClient, and remote-spawn protocol integration.
///
/// Construction has no network side effects. \c start() begins
/// listening and thread work; \c stop() is idempotent and
/// callback-quiescent.
///
/// \note Thread safety: Lifecycle state uses a mutex. Port callbacks
///       fire on the network loop thread and check the ingress gate.
class NetworkRuntime final {
  public:
    /// \brief Lifecycle state machine.
    enum class State : uint8_t {
        Constructed = 0,
        Starting = 1,
        Running = 2,
        Stopping = 3,
        Stopped = 4,
        Failed = 5,
    };

    /// \brief Stop mode.
    enum class StopMode : uint8_t {
        Drain, ///< Graceful drain before stop.
        Abort, ///< Immediate stop.
    };

    /// \brief Non-owning dependencies that must outlive this component.
    struct Dependencies {
        ActorDirectory* actors{nullptr};
        MessagingRuntime* messaging{nullptr};
        sched::IScheduler* scheduler{nullptr};
        InboundFrameSinkPort inbound_sink{};
        NodeEventSink node_events{};
        OutboundRetryPort retry_port{};
        RemoteSpawnPort spawn_port{};
        NetworkTelemetryPort telemetry{};
    };

    /// \brief Construct with validated config and fixed dependencies.
    ///
    /// No network side effects: no threads, sockets, timers, or
    /// discovery membership.
    NetworkRuntime(NetworkRuntimeConfig config, Dependencies deps) noexcept;

    ~NetworkRuntime();

    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
    NetworkRuntime(NetworkRuntime&&) = delete;
    NetworkRuntime& operator=(NetworkRuntime&&) = delete;

    /// \brief Start networking (listen, thread, discovery, timers).
    ///
    /// Returns an error if already started or a startup stage fails.
    /// \note Failure rolls back all completed stages in reverse.
    result<void> start() noexcept;

    /// \brief Stop networking (idempotent, callback-quiescent).
    ///
    /// Closes ingress, cancels timers/RPC/HTTP operations, stops
    /// listening and discovery, joins the network thread, then clears
    /// handlers.
    ///
    /// \param mode Drain (graceful) or Abort (immediate).
    /// \return Result with error if stop fails or was deferred.
    result<void> stop(StopMode mode = StopMode::Drain) noexcept;

    /// \brief Current lifecycle state.
    [[nodiscard]] State state() const noexcept;

    /// \brief Bounded snapshot for observability.
    [[nodiscard]] net::NetworkSnapshot snapshot() const noexcept;

    // ── Compatibility accessors ──────────────────────────────────────────

    /// \brief The authoritative event loop.
    /// \return nullptr if disabled or not running.
    [[nodiscard]] net::EventLoop* event_loop() noexcept;

    /// \brief Primary transport for remote messaging.
    /// \return nullptr if disabled.
    [[nodiscard]] net::TcpTransport* transport() noexcept;

    /// \brief Service discovery backend.
    /// \return nullptr if disabled.
    [[nodiscard]] net::IServiceDiscovery* discovery() noexcept;

    /// \brief UDP registrar (may be null even with discovery).
    [[nodiscard]] net::UdpRegistrar* registrar() noexcept;

    /// \brief RPC channel for ask/request-response.
    /// \return nullptr if disabled.
    [[nodiscard]] RpcChannel* rpc_channel() noexcept;

    /// \brief HTTP client.
    /// \return nullptr if disabled.
    [[nodiscard]] net::HttpClient* http_client() noexcept;

    /// \brief Actor location cache.
    /// \return nullptr if disabled.
    [[nodiscard]] net::ActorLocationCache* location_cache() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hpactor
