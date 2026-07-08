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

#include <hpactor/python/python_runtime.hpp>

namespace hpactor::python {

// =============================================================================
// PythonActorLease
// =============================================================================

PythonActorLease::PythonActorLease(PythonRuntime* runtime, uint64_t generation) noexcept
    : runtime_(runtime), generation_(generation) {}

PythonActorLease::PythonActorLease(PythonActorLease&& other) noexcept
    : runtime_(other.runtime_), actor_id_(other.actor_id_),
      generation_(other.generation_), bound_(other.bound_) {
    other.runtime_ = nullptr;
    other.actor_id_ = ActorId{};
    other.generation_ = 0;
    other.bound_ = false;
}

PythonActorLease& PythonActorLease::operator=(PythonActorLease&& other) noexcept {
    if (this != &other) {
        reset();
        runtime_ = other.runtime_;
        actor_id_ = other.actor_id_;
        generation_ = other.generation_;
        bound_ = other.bound_;
        other.runtime_ = nullptr;
        other.actor_id_ = ActorId{};
        other.generation_ = 0;
        other.bound_ = false;
    }
    return *this;
}

PythonActorLease::~PythonActorLease() {
    reset();
}

bool PythonActorLease::bind(ActorId actor_id) noexcept {
    if (!runtime_ || bound_ || generation_ == 0) {
        return false;
    }
    if (runtime_->bind_actor(actor_id, generation_)) {
        actor_id_ = actor_id;
        bound_ = true;
        return true;
    }
    return false;
}

void PythonActorLease::reset() noexcept {
    if (runtime_) {
        runtime_->release_actor(actor_id_, generation_, bound_);
        runtime_ = nullptr;
    }
    actor_id_ = ActorId{};
    bound_ = false;
}

uint64_t PythonActorLease::generation() const noexcept {
    return generation_;
}

ActorId PythonActorLease::actor_id() const noexcept {
    return actor_id_;
}

// =============================================================================
// PythonRuntime
// =============================================================================

PythonRuntime::PythonRuntime(PythonRuntimeConfig config) noexcept
    : config_(config) {}

PythonRuntime::~PythonRuntime() {
    (void)stop();
}

result<std::unique_ptr<PythonRuntime>>
PythonRuntime::create(PythonRuntimeConfig config) noexcept {
    if (config.validate() != PythonConfigError::None) {
        return result<std::unique_ptr<PythonRuntime>>::make(
            error(errors::invalid_argument, "invalid PythonRuntimeConfig"));
    }

    auto runtime = std::unique_ptr<PythonRuntime>(new PythonRuntime(config));
    runtime->queues_ = std::make_unique<PythonRuntimeQueues>(config);
    return result<std::unique_ptr<PythonRuntime>>::make(std::move(runtime));
}

result<void> PythonRuntime::start(GatewayWakePort wake_port) noexcept {
    if (!wake_port) {
        return result<void>::make(
            error(errors::invalid_argument, "invalid GatewayWakePort"));
    }

    PythonRuntimeState expected = PythonRuntimeState::Created;
    if (!state_.compare_exchange_strong(expected, PythonRuntimeState::Starting,
                                        std::memory_order_acq_rel)) {
        return result<void>::make(error(errors::unknown, "runtime already started"));
    }

    // Create notifiers.
    auto dispatch_nf = NativeNotifier::create();
    if (!dispatch_nf.ok()) {
        state_.store(PythonRuntimeState::Failed, std::memory_order_release);
        return result<void>::make(
            error(errors::unknown, "failed to create dispatch notifier"));
    }

    auto completion_nf = NativeNotifier::create();
    if (!completion_nf.ok()) {
        state_.store(PythonRuntimeState::Failed, std::memory_order_release);
        return result<void>::make(
            error(errors::unknown, "failed to create completion notifier"));
    }

    dispatch_notifier_ = std::move(dispatch_nf.value());
    completion_notifier_ = std::move(completion_nf.value());
    wake_port_ = wake_port;

    state_.store(PythonRuntimeState::Running, std::memory_order_release);
    return result<void>::make();
}

void PythonRuntime::begin_draining() noexcept {
    PythonRuntimeState expected = PythonRuntimeState::Running;
    state_.compare_exchange_strong(expected, PythonRuntimeState::Draining,
                                   std::memory_order_acq_rel);
}

result<void> PythonRuntime::stop() noexcept {
    // Close admission first so no new submissions are accepted.
    admission_open_.store(false, std::memory_order_release);

    PythonRuntimeState current = state_.load(std::memory_order_acquire);

    // Already stopped — idempotent.
    if (current == PythonRuntimeState::Stopped) {
        return result<void>::make();
    }

    // Failed is also terminal.
    if (current == PythonRuntimeState::Failed) {
        return result<void>::make();
    }

    // Transition to Stopping, then Stopped.
    state_.store(PythonRuntimeState::Stopping, std::memory_order_release);

    // Close notifiers.
    if (dispatch_notifier_) {
        dispatch_notifier_->close();
    }
    if (completion_notifier_) {
        completion_notifier_->close();
    }

    // Clear the wake port.
    wake_port_ = GatewayWakePort{};

    state_.store(PythonRuntimeState::Stopped, std::memory_order_release);
    return result<void>::make();
}

std::optional<PythonActorLease> PythonRuntime::reserve_actor() noexcept {
    std::lock_guard<std::mutex> lock(actor_mutex_);

    if (actor_reservation_count_ >= config_.max_actor_bindings) {
        return std::nullopt;
    }

    uint64_t generation =
        actor_generation_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    ++actor_reservation_count_;

    return PythonActorLease(this, generation);
}

bool PythonRuntime::generation_matches(ActorId actor_id,
                                       uint64_t generation) const noexcept {
    std::lock_guard<std::mutex> lock(actor_mutex_);
    auto it = actor_generations_.find(actor_id);
    if (it == actor_generations_.end()) {
        return false;
    }
    return it->second == generation;
}

bool PythonRuntime::bind_actor(ActorId actor_id, uint64_t generation) noexcept {
    std::lock_guard<std::mutex> lock(actor_mutex_);
    auto result = actor_generations_.emplace(actor_id, generation);
    return result.second;
}

void PythonRuntime::release_actor(ActorId actor_id, uint64_t generation,
                                  bool was_bound) noexcept {
    std::lock_guard<std::mutex> lock(actor_mutex_);

    if (actor_reservation_count_ > 0) {
        --actor_reservation_count_;
    }

    if (was_bound) {
        auto it = actor_generations_.find(actor_id);
        // Only erase if the generation still matches — protects against
        // stale leases erasing a replacement generation.
        if (it != actor_generations_.end() && it->second == generation) {
            actor_generations_.erase(it);
        }
    }
}

// ── Queue submission ────────────────────────────────────────────────────────

bool PythonRuntime::try_push_dispatch(const PythonDispatchPtr& envelope) noexcept {
    if (state_.load(std::memory_order_acquire) != PythonRuntimeState::Running ||
        !admission_open_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!queues_->try_push_dispatch(envelope)) {
        return false;
    }
    if (dispatch_notifier_) {
        (void)dispatch_notifier_->signal();
    }
    return true;
}

bool PythonRuntime::try_push_command(const PythonCommandPtr& command) noexcept {
    if (state_.load(std::memory_order_acquire) != PythonRuntimeState::Running ||
        !admission_open_.load(std::memory_order_acquire)) {
        return false;
    }
    if (!queues_->try_push_command(command)) {
        return false;
    }
    // Invoke the wake port to notify the gateway actor.
    if (wake_port_) {
        wake_port_.wake(wake_port_.context);
    }
    return true;
}

bool PythonRuntime::try_push_completion(const PythonCompletionPtr& completion) noexcept {
    if (state_.load(std::memory_order_acquire) != PythonRuntimeState::Running ||
        !admission_open_.load(std::memory_order_acquire)) {
        return false;
    }

    // Generation check: for completions with a non-zero actor ID and
    // generation, verify the generation is current before accepting.
    if (completion && completion->actor.id.value() != 0 &&
        completion->generation != 0) {
        if (!generation_matches(completion->actor.id, completion->generation)) {
            stale_completion_rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    if (!queues_->try_push_completion(completion)) {
        return false;
    }
    if (completion_notifier_) {
        (void)completion_notifier_->signal();
    }
    return true;
}

// ── Notifier file descriptors ──────────────────────────────────────────────

int PythonRuntime::dispatch_read_fd() const noexcept {
    if (dispatch_notifier_ && dispatch_notifier_->valid()) {
        return dispatch_notifier_->read_fd();
    }
    return -1;
}

int PythonRuntime::completion_read_fd() const noexcept {
    if (completion_notifier_ && completion_notifier_->valid()) {
        return completion_notifier_->read_fd();
    }
    return -1;
}

uint64_t PythonRuntime::drain_dispatch_notification() noexcept {
    if (dispatch_notifier_) {
        return dispatch_notifier_->drain();
    }
    return 0;
}

uint64_t PythonRuntime::drain_completion_notification() noexcept {
    if (completion_notifier_) {
        return completion_notifier_->drain();
    }
    return 0;
}

// ── Observers ──────────────────────────────────────────────────────────────

const PythonRuntimeConfig& PythonRuntime::config() const noexcept {
    return config_;
}

PythonRuntimeSnapshot PythonRuntime::snapshot() const noexcept {
    PythonRuntimeSnapshot snap;
    snap.state = state_.load(std::memory_order_acquire);
    snap.queues = queues_->snapshot();
    snap.stale_completion_rejected =
        stale_completion_rejected_.load(std::memory_order_relaxed);

    // Phase 1C extended counters.
    snap.dispatch_rejected = dispatch_rejected_.load(std::memory_order_relaxed);
    snap.command_rejected = command_rejected_.load(std::memory_order_relaxed);
    snap.handler_exceptions = handler_exceptions_.load(std::memory_order_relaxed);
    snap.handler_cancelled = handler_cancelled_.load(std::memory_order_relaxed);
    snap.stale_completions = stale_completions_.load(std::memory_order_relaxed);
    snap.last_heartbeat_ns = last_heartbeat_ns_.load(std::memory_order_relaxed);

    // Compute readiness: Running + fresh heartbeat.
    snap.ready =
        (snap.state == PythonRuntimeState::Running && snap.last_heartbeat_ns > 0);

    snap.dispatch_notifier_fd = dispatch_read_fd();
    snap.completion_notifier_fd = completion_read_fd();

    {
        std::lock_guard<std::mutex> lock(actor_mutex_);
        snap.actor_bindings = actor_generations_.size();
    }

    return snap;
}

// ── Phase 1C reliability counters ─────────────────────────────────────────

void PythonRuntime::record_heartbeat(uint64_t now_ns) noexcept {
    last_heartbeat_ns_.store(now_ns, std::memory_order_relaxed);
}

void PythonRuntime::record_dispatch_rejected() noexcept {
    dispatch_rejected_.fetch_add(1, std::memory_order_relaxed);
}

void PythonRuntime::record_command_rejected() noexcept {
    command_rejected_.fetch_add(1, std::memory_order_relaxed);
}

void PythonRuntime::record_handler_exception() noexcept {
    handler_exceptions_.fetch_add(1, std::memory_order_relaxed);
}

void PythonRuntime::record_handler_cancelled() noexcept {
    handler_cancelled_.fetch_add(1, std::memory_order_relaxed);
}

void PythonRuntime::record_stale_completion() noexcept {
    stale_completions_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace hpactor::python
