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

#include <hpactor/runtime/network_runtime.hpp>

#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>
#include <chrono>

namespace hpactor {

// ── Adapter functions ────────────────────────────────────────────────────────
//
// These are bound to the ReliableAckEmitter and BackpressureSignalEmitter
// function pointers. The void* context points to the owning NetworkRuntime.
// Neither function captures ActorSystem or Impl.

namespace {

[[maybe_unused]] void
reliable_ack_adapter(void* context, const ActorAddress& target,
                     const ActorAddress& acker, uint64_t message_id,
                     uint8_t status, uint32_t retry_after_ms) noexcept {
    auto* runtime = static_cast<NetworkRuntime*>(context);
    if (!runtime)
        return;

    auto* transport = runtime->transport();
    if (!transport)
        return;

    net::WireFrame frame;
    bool is_nack = (status == 1); // 1 = AckStatus::Rejected
    frame.pb_envelope.mutable_data_frame()->set_flags(
        is_nack ? net::WireFrame::AckResponse : net::WireFrame::AckRequested);
    frame.pb_envelope.mutable_data_frame()->set_message_id(message_id);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(), acker);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                  target);

    if (is_nack) {
        frame.pb_envelope.mutable_data_frame()->set_type_tag(
            static_cast<uint32_t>(status));
        std::string payload_str(reinterpret_cast<const char*>(&retry_after_ms),
                                sizeof(uint32_t));
        frame.pb_envelope.mutable_data_frame()->set_payload(payload_str);
    }

    auto encoded = frame.encode();
    (void)transport->try_send(target, encoded);
}

[[maybe_unused]] bool
backpressure_wire_adapter(void* context, const ActorAddress& target,
                          const StreamBuffer& encoded) noexcept {
    auto* runtime = static_cast<NetworkRuntime*>(context);
    if (!runtime)
        return false;

    auto* transport = runtime->transport();
    if (transport) {
        return transport->try_send(target, encoded) == TransportSendResult::Sent;
    }
    return false;
}

} // namespace

// ── NetworkRuntime::Impl ─────────────────────────────────────────────────────

struct NetworkRuntime::Impl {
    NetworkRuntimeConfig config;
    Dependencies deps;

    // Lifecycle
    mutable std::mutex lifecycle_mutex;
    std::atomic<State> state_{State::Constructed};
    std::atomic<bool> ingress_open_{false};

    // Owned resources
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

    void close_ingress() {
        ingress_open_.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool ingress_open() const noexcept {
        return ingress_open_.load(std::memory_order_acquire);
    }

    void reject_callback() {
        callback_rejections.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] net::NetworkSnapshot snapshot() const noexcept {
        net::NetworkSnapshot s;
        s.state = static_cast<uint8_t>(state_.load(std::memory_order_acquire));
        s.enabled = config.enabled;
        s.listening = transport != nullptr;
        s.active_connections = 0;
        s.idle_connections = 0;
        s.discovery_status = discovery ? 1U : 0U;
        s.member_count = 0;
        s.pending_rpc = 0;
        s.pending_http = 0;
        s.cache_entries = 0;
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
        return result<void>::make();
    }
    if (current == State::Stopped) {
        return result<void>::make(
            error(errors::network_already_stopped, "NetworkAlreadyStopped"));
    }
    if (current == State::Starting) {
        return result<void>::make(
            error(errors::network_already_running, "NetworkAlreadyRunning"));
    }

    impl_->state_.store(State::Starting, std::memory_order_release);

    // ── Stage 1: Construct transport and obtain its authoritative loop ─────
    impl_->last_stage.store(1, std::memory_order_relaxed);
    impl_->transport = std::make_unique<net::TcpTransport>(
        impl_->config.endpoint, impl_->config.tls, impl_->config.pool, nullptr);

    // ── Stage 2: Construct location cache, discovery, RPC, HTTP ────────────
    impl_->last_stage.store(2, std::memory_order_relaxed);

    // Discovery: use injected backend, or auto-select from config.
    if (impl_->config.service_discovery) {
        impl_->discovery = impl_->config.service_discovery;
    } else if (impl_->config.registrar.udp_port > 0) {
        auto reg = std::make_shared<net::UdpRegistrar>(impl_->config.registrar,
                                                       impl_->config.endpoint,
                                                       &impl_->transport->loop());
        impl_->discovery = reg;
        impl_->registrar = reg;
    } else {
        impl_->discovery =
            std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    }

    impl_->location_cache = std::make_shared<net::ActorLocationCache>();

    impl_->rpc_channel =
        std::make_unique<RpcChannel>(impl_->transport.get(), impl_->deps.scheduler,
                                     impl_->config.ask_max_retries);

    if (impl_->config.enable_http_client) {
        impl_->http_client =
            std::make_unique<net::HttpClient>(&impl_->transport->loop());
    }

    // ── Stage 3: Install callbacks ────────────────────────────────────────
    impl_->last_stage.store(3, std::memory_order_relaxed);

    // Install the inbound frame sink (Phase 4 contract) before listening.
    if (impl_->deps.inbound_sink.sink) {
        impl_->transport->set_actor_message_handler(
            [sink = impl_->deps.inbound_sink](const net::WireFrame& frame) {
                sink.sink(sink.context, frame);
            });
    }

    // Install RPC handler.
    if (impl_->rpc_channel) {
        impl_->transport->set_rpc_handler(
            [rpc = impl_->rpc_channel.get()](const hpactor::RpcResponseFrame& response) {
                rpc->on_response(response);
            });
    }

    // Install node event callback through the port.
    if (impl_->deps.node_events.member_changed) {
        impl_->discovery->on_member_change(
            [port = impl_->deps.node_events](const net::Member& m, bool joined) {
                port.member_changed(port.context, m, joined);
            });
    }

    // ── Stage 4: Install remote-spawn receiver ────────────────────────────
    impl_->last_stage.store(4, std::memory_order_relaxed);
    if (impl_->deps.spawn_port.install_receiver && impl_->config.tcp_port > 0) {
        auto spawn_result = impl_->deps.spawn_port.install_receiver(
            impl_->deps.spawn_port.context, *impl_->transport);
        if (spawn_result.is_error()) {
            // Rollback: stop discovery, reset transport.
            if (impl_->discovery)
                impl_->discovery->stop();
            impl_->transport.reset();
            impl_->state_.store(State::Failed, std::memory_order_release);
            return result<void>::make(spawn_result.error());
        }
    }

    // ── Stage 5: Register maintenance timers ──────────────────────────────
    impl_->last_stage.store(5, std::memory_order_relaxed);

    // Cache purge timer.
    impl_->cache_purge_timer = impl_->transport->loop().run_every(
        [this]() {
            if (!impl_->ingress_open())
                return;
            if (impl_->location_cache)
                impl_->location_cache->purge_expired();
        },
        static_cast<int>(impl_->config.cache_purge_ms));

    // Reliable-retry poll timer.
    if (impl_->deps.retry_port.process_due) {
        impl_->retry_timer = impl_->transport->loop().run_every(
            [this]() {
                if (!impl_->ingress_open()) {
                    impl_->reject_callback();
                    return;
                }
                uint64_t now_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                impl_->deps.retry_port.process_due(impl_->deps.retry_port.context,
                                                   now_ns);
            },
            static_cast<int>(impl_->config.retry_poll_ms));
    }

    // ── Stage 6: Start discovery ──────────────────────────────────────────
    impl_->last_stage.store(6, std::memory_order_relaxed);
    impl_->discovery->start();

    // ── Stage 7: Start listening ──────────────────────────────────────────
    impl_->last_stage.store(7, std::memory_order_relaxed);
    if (impl_->config.tcp_port > 0) {
        impl_->transport->listen(impl_->config.tcp_port);
    }

    // ── Stage 8: Launch loop thread ───────────────────────────────────────
    impl_->last_stage.store(8, std::memory_order_relaxed);
    impl_->network_thread = std::thread([this]() {
        auto& loop = impl_->transport->loop();
        while (impl_->state_.load(std::memory_order_acquire) != State::Stopping &&
               loop.wait(100) >= 0) {
            loop.process_completions();
        }
    });

    // ── Stage 9: Publish Running ──────────────────────────────────────────
    impl_->last_stage.store(9, std::memory_order_relaxed);
    impl_->ingress_open_.store(true, std::memory_order_release);
    impl_->state_.store(State::Running, std::memory_order_release);
    impl_->last_error.store(0, std::memory_order_relaxed);

    return result<void>::make();
}

result<void> NetworkRuntime::stop(StopMode mode) noexcept {
    (void)mode;

    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);

    State current = impl_->state_.load(std::memory_order_relaxed);

    if (current == State::Stopped) {
        return result<void>::make();
    }

    // Detect self-stop from the network thread.
    if (current == State::Running && impl_->network_thread.joinable() &&
        impl_->network_thread.get_id() == std::this_thread::get_id()) {
        impl_->close_ingress();
        impl_->state_.store(State::Stopping, std::memory_order_release);
        return result<void>::make(
            error(errors::network_stop_deferred, "StopDeferred"));
    }

    impl_->state_.store(State::Stopping, std::memory_order_release);
    impl_->close_ingress();

    // ── Teardown in reverse startup order ────────────────────────────────

    // Stop the event loop to signal the thread to exit.
    if (impl_->transport) {
        impl_->transport->loop().stop();
    }

    // Join the network thread.
    if (impl_->network_thread.joinable()) {
        impl_->network_thread.join();
    }

    // Remove spawn receiver protocol registration.
    if (impl_->deps.spawn_port.remove_receiver) {
        impl_->deps.spawn_port.remove_receiver(impl_->deps.spawn_port.context);
    }

    // Stop listening.
    if (impl_->transport) {
        impl_->transport->stop_listening();
    }

    // Stop discovery.
    if (impl_->discovery) {
        impl_->discovery->stop();
    }

    // Clear transport handlers (safe after thread join).
    if (impl_->transport) {
        impl_->transport->set_actor_message_handler(nullptr);
        impl_->transport->set_rpc_handler(nullptr);
    }

    // Reset owned resources.
    impl_->rpc_channel.reset();
    impl_->http_client.reset();
    impl_->location_cache.reset();
    impl_->registrar.reset();
    impl_->discovery.reset();
    impl_->transport.reset();

    impl_->state_.store(State::Stopped, std::memory_order_release);

    return result<void>::make();
}

// ── Compatibility Accessors ──────────────────────────────────────────────────

net::EventLoop* NetworkRuntime::event_loop() noexcept {
    if (!impl_->config.enabled || !impl_->transport)
        return nullptr;
    return &impl_->transport->loop();
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
