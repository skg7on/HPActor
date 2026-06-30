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

#include "network_runtime.hpp"

#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>

namespace hpactor {

// Error codes are defined in network_runtime.hpp.
// Reserved for future Task 2–6: network_invalid_config (10501),
// network_discovery_start_failed (10502), network_listen_failed (10503),
// network_loop_thread_failed (10504), network_remote_spawn_failed (10505),
// network_runtime_stopping (10506).

// ── NetworkRuntime::Impl ─────────────────────────────────────────────────────

struct NetworkRuntime::Impl {
    NetworkRuntimeConfig config;
    Dependencies deps;

    // Lifecycle
    mutable std::mutex lifecycle_mutex;
    std::atomic<State> state_{State::Constructed};
    std::atomic<bool> ingress_open_{false};

    // Owned resources
    std::unique_ptr<net::EventLoop> event_loop;
    std::unique_ptr<net::TcpTransport> transport;
    std::shared_ptr<net::UdpRegistrar> registrar;
    std::shared_ptr<net::IServiceDiscovery> discovery;
    std::shared_ptr<net::ActorLocationCache> location_cache;
    std::unique_ptr<RpcChannel> rpc_channel;
    std::unique_ptr<net::HttpClient> http_client;
    std::thread network_thread;

    // Timer IDs (valid only while running)
    uint64_t cache_purge_timer{0};
    uint64_t retry_timer{0};

    // Snapshot counters
    std::atomic<uint64_t> callback_rejections{0};
    std::atomic<uint32_t> last_error{0};
    std::atomic<uint8_t> last_stage{0};

    Impl(NetworkRuntimeConfig c, Dependencies d)
        : config(std::move(c)), deps(std::move(d)) {}

    /// \brief Atomically close ingress so no new callback can begin.
    void close_ingress() {
        ingress_open_.store(false, std::memory_order_release);
    }

    /// \brief Check whether ingress is still open (acquire barrier).
    [[nodiscard]] bool ingress_open() const noexcept {
        return ingress_open_.load(std::memory_order_acquire);
    }

    /// \brief Increment rejected callback counter.
    void reject_callback() {
        callback_rejections.fetch_add(1, std::memory_order_relaxed);
    }

    /// \brief Build the snapshot from current state.
    /// \note Some counters are approximate — they read atomic fields
    ///       without locking individual subsystem mutexes.
    [[nodiscard]] net::NetworkSnapshot snapshot() const noexcept {
        net::NetworkSnapshot s;
        s.state = static_cast<uint8_t>(state_.load(std::memory_order_acquire));
        s.enabled = config.enabled;
        // Transport state — use existence as a proxy for "listening"
        // until finer-grained counters are exposed.
        s.listening = transport != nullptr;
        s.active_connections = 0; // TODO: wire when transport exposes counters
        s.idle_connections = 0;   // TODO: wire when transport exposes counters
        s.discovery_status = discovery ? 1U : 0U;
        s.member_count = 0;  // TODO: wire when discovery exposes counters
        s.pending_rpc = 0;   // TODO: wire when RPC exposes counters
        s.pending_http = 0;  // TODO: wire when HTTP exposes counters
        s.cache_entries = 0; // TODO: wire when cache exposes counters
        s.callback_rejections = callback_rejections.load(std::memory_order_relaxed);
        s.loop_thread_running = network_thread.joinable() ? 1 : 0;
        s.last_stage = last_stage.load(std::memory_order_relaxed);
        s.last_error = last_error.load(std::memory_order_relaxed);
        return s;
    }
};

// ── Construction / Destruction ───────────────────────────────────────────────

NetworkRuntime::NetworkRuntime(NetworkRuntimeConfig config, Dependencies deps) noexcept
    : impl_(std::make_unique<Impl>(std::move(config), std::move(deps))) {}

NetworkRuntime::~NetworkRuntime() {
    // Defensive stop: if owner never called stop(), abort now.
    // This is a safety net, not the primary teardown path.
    State s = state();
    if (s != State::Stopped) {
        (void)stop(StopMode::Abort);
    }
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

NetworkRuntime::State NetworkRuntime::state() const noexcept {
    return impl_->state_.load(std::memory_order_acquire);
}

result<void> NetworkRuntime::start() noexcept {
    if (!impl_->config.enabled) {
        return result<void>::make(error(errors::network_disabled, "NetworkDisabled"));
    }

    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);

    State current = impl_->state_.load(std::memory_order_relaxed);
    if (current == State::Running) {
        // Idempotent: already running.
        return result<void>::make();
    }
    if (current == State::Stopped) {
        return result<void>::make(
            error(errors::network_already_stopped, "NetworkAlreadyStopped"));
    }
    if (current == State::Starting) {
        // Concurrent start — another thread is starting. Wait?
        return result<void>::make(
            error(errors::network_already_running, "NetworkAlreadyRunning"));
    }

    impl_->state_.store(State::Starting, std::memory_order_release);

    // ── Stage 1: Construct transport and obtain its loop ─────────────────
    impl_->last_stage.store(1, std::memory_order_relaxed);
    // TODO: Move transport construction here (Task 2)

    // ── Stage 2: Construct location cache, RPC, HTTP, discovery ──────────
    impl_->last_stage.store(2, std::memory_order_relaxed);
    // TODO: Move secondary construction here (Task 3)

    // ── Stage 3: Install callbacks ───────────────────────────────────────
    impl_->last_stage.store(3, std::memory_order_relaxed);
    // TODO: Install frame, RPC, spawn, node event callbacks (Task 2-6)

    // ── Stage 4: Install remote-spawn receiver ───────────────────────────
    impl_->last_stage.store(4, std::memory_order_relaxed);
    // TODO: Install spawn receiver through RemoteSpawnPort (Task 6)

    // ── Stage 5: Register maintenance timers ─────────────────────────────
    impl_->last_stage.store(5, std::memory_order_relaxed);
    // TODO: Register cache purge and retry timers (Task 4)

    // ── Stage 6: Start discovery ─────────────────────────────────────────
    impl_->last_stage.store(6, std::memory_order_relaxed);
    // TODO: Start discovery (Task 3)

    // ── Stage 7: Start listening ─────────────────────────────────────────
    impl_->last_stage.store(7, std::memory_order_relaxed);
    // TODO: Start listening (Task 2)

    // ── Stage 8: Launch loop thread ──────────────────────────────────────
    impl_->last_stage.store(8, std::memory_order_relaxed);
    // TODO: Launch thread with progress barrier (Task 2)

    // ── Stage 9: Publish Running ─────────────────────────────────────────
    impl_->last_stage.store(9, std::memory_order_relaxed);
    impl_->ingress_open_.store(true, std::memory_order_release);
    impl_->state_.store(State::Running, std::memory_order_release);
    impl_->last_error.store(0, std::memory_order_relaxed);

    return result<void>::make();
}

result<void> NetworkRuntime::stop(StopMode mode) noexcept {
    (void)mode; // Future: differentiate Drain vs Abort behavior.
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);

    State current = impl_->state_.load(std::memory_order_relaxed);

    // Already stopped — idempotent.
    if (current == State::Stopped) {
        return result<void>::make();
    }

    // Detect self-stop from the network thread.
    if (current == State::Running && impl_->network_thread.joinable() &&
        impl_->network_thread.get_id() == std::this_thread::get_id()) {
        // Close ingress but defer thread join to owner.
        impl_->close_ingress();
        impl_->state_.store(State::Stopping, std::memory_order_release);
        return result<void>::make(
            error(errors::network_stop_deferred, "StopDeferred"));
    }

    // Transition to Stopping.
    impl_->state_.store(State::Stopping, std::memory_order_release);
    impl_->close_ingress();

    // ── Teardown in reverse startup order ────────────────────────────────

    // Stop event loop (signals the thread to exit).
    if (impl_->event_loop) {
        impl_->event_loop->stop();
    }

    // Join the network thread.
    if (impl_->network_thread.joinable()) {
        impl_->network_thread.join();
    }

    // Stop listening.
    if (impl_->transport) {
        impl_->transport->stop_listening();
    }

    // Stop discovery.
    if (impl_->discovery) {
        impl_->discovery->stop();
    }

    // Clear handlers (safe now — thread is joined).
    // TODO: Clear transport/RPC/HTTP handlers after resource moves (Task 2-5)

    // Reset owned resources.
    impl_->rpc_channel.reset();
    impl_->http_client.reset();
    impl_->location_cache.reset();
    impl_->registrar.reset();
    impl_->discovery.reset();
    impl_->transport.reset();
    impl_->event_loop.reset();

    impl_->state_.store(State::Stopped, std::memory_order_release);

    return result<void>::make();
}

// ── Compatibility Accessors ──────────────────────────────────────────────────

net::EventLoop* NetworkRuntime::event_loop() noexcept {
    if (!impl_->config.enabled)
        return nullptr;
    return impl_->event_loop.get();
}

net::TcpTransport* NetworkRuntime::transport() noexcept {
    if (!impl_->config.enabled)
        return nullptr;
    return impl_->transport.get();
}

net::IServiceDiscovery* NetworkRuntime::discovery() noexcept {
    if (!impl_->config.enabled)
        return nullptr;
    return impl_->discovery.get();
}

net::UdpRegistrar* NetworkRuntime::registrar() noexcept {
    if (!impl_->config.enabled)
        return nullptr;
    return impl_->registrar.get();
}

RpcChannel* NetworkRuntime::rpc_channel() noexcept {
    if (!impl_->config.enabled)
        return nullptr;
    return impl_->rpc_channel.get();
}

net::HttpClient* NetworkRuntime::http_client() noexcept {
    if (!impl_->config.enabled)
        return nullptr;
    return impl_->http_client.get();
}

net::ActorLocationCache* NetworkRuntime::location_cache() noexcept {
    if (!impl_->config.enabled)
        return nullptr;
    return impl_->location_cache.get();
}

net::NetworkSnapshot NetworkRuntime::snapshot() const noexcept {
    return impl_->snapshot();
}

} // namespace hpactor
