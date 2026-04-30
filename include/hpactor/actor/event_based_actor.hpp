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

#include <hpactor/actor/local_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#if HPACTOR_SUPPORT_COROUTINES
#    include <hpactor/sched/actor_coroutine.hpp>
#    include <hpactor/sched/coroutine_awaiters.hpp>
#    include <hpactor/sched/coroutine_task.hpp>
#endif

namespace hpactor {

// Forward declarations
class ProtoTypeRegistry;

// Internal handler storage — type-erased to avoid template bloat in the map
struct ProtoHandler {
    std::string type_name;

    ProtoHandler() = default;
    ProtoHandler(ProtoHandler&&) = default;
    ProtoHandler& operator=(ProtoHandler&&) = default;
    ProtoHandler(const ProtoHandler&) = delete;
    ProtoHandler& operator=(const ProtoHandler&) = delete;

    // Deserialize bytes into a shared_ptr<void> holding the concrete protobuf type
    std::function<std::shared_ptr<void>(const bytes&)> deserialize;

    // Invoke the handler with a deserialized message.
    // Returns serialized response bytes (empty for fire-and-forget).
    std::function<bytes(std::shared_ptr<void>)> invoke;
};

// -----------------------------------------------------------------------------
// EventBasedActor - cooperatively scheduled actor with behavior-based
// handling, proto handler dispatch, and optional coroutine support (C++20)
// -----------------------------------------------------------------------------
class EventBasedActor : public LocalActor {
  public:
    void become(Behavior bh);
    void become_empty();

    void receive(TypedMessage& msg) override;

    // Type query for safe downcasting without RTTI
    bool is_event_based_actor() const override {
        return true;
    }

    // Proto handler registration (absorbed from ProtoActor)
    // Users override register_handlers() and call these in the override.

    // Register a fire-and-forget handler for a protobuf message type
    template<typename ProtoMsgT>
    void on(std::function<void(const ProtoMsgT&)> handler) {
        TypeTag tag = type_tag_for<ProtoMsgT>();
        auto handler_ptr = std::make_shared<
            std::function<void(const ProtoMsgT&)>>(std::move(handler));

        ProtoHandler entry;
        entry.type_name = ProtoMsgT().GetTypeName();
        entry.deserialize = [](const bytes& data) -> std::shared_ptr<void> {
            auto msg = std::make_shared<ProtoMsgT>();
            if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                return nullptr;
            }
            return msg;
        };
        entry.invoke = [handler_ptr](std::shared_ptr<void> raw) -> bytes {
            auto& msg = *static_cast<ProtoMsgT*>(raw.get());
            (*handler_ptr)(msg);
            return {};
        };

        proto_handlers_[tag] = std::move(entry);
    }

    // Register a request-response handler for protobuf types
    template<typename ReqT, typename ResT>
    void on_request(std::function<ResT(const ReqT&)> handler) {
        TypeTag tag = type_tag_for<ReqT>();
        auto handler_ptr = std::make_shared<
            std::function<ResT(const ReqT&)>>(std::move(handler));

        ProtoHandler entry;
        entry.type_name = ReqT().GetTypeName();
        entry.deserialize = [](const bytes& data) -> std::shared_ptr<void> {
            auto msg = std::make_shared<ReqT>();
            if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                return nullptr;
            }
            return msg;
        };
        entry.invoke = [handler_ptr](std::shared_ptr<void> raw) -> bytes {
            auto& req = *static_cast<ReqT*>(raw.get());
            ResT res = (*handler_ptr)(req);
            bytes result(res.ByteSizeLong());
            res.SerializeToArray(result.data(),
                                 static_cast<int>(result.size()));
            return result;
        };

        proto_handlers_[tag] = std::move(entry);
    }

    // Dispatch an incoming protobuf message by TypeTag
    void on_proto_message(TypeTag tag, const bytes& payload);

    // Check if this actor can handle a given TypeTag
    [[nodiscard]] bool handles(TypeTag tag) const {
        return proto_handlers_.find(tag) != proto_handlers_.end();
    }

#if HPACTOR_SUPPORT_COROUTINES
    // Coroutine support (C++20 only)
    virtual sched::CoroutineTask act() {
        co_return;
    }

    sched::ActorCoroutine& get_actor_coroutine() {
        return actor_coroutine_;
    }
    const sched::ActorCoroutine& get_actor_coroutine() const {
        return actor_coroutine_;
    }
    void set_actor_coroutine(sched::ActorCoroutine&& coroutine) {
        actor_coroutine_ = std::move(coroutine);
    }

    std::coroutine_handle<sched::CoroutinePromise> get_coro_handle() {
        return coro_handle_;
    }
    void set_coro_handle(std::coroutine_handle<sched::CoroutinePromise> h) {
        coro_handle_ = h;
    }

    sched::MailboxAwaiter<TypedMessage> make_mailbox_awaiter() {
        return sched::MailboxAwaiter<TypedMessage>{
            coro_handle_.promise(), mailbox_};
    }

    void ensure_coroutine_started() {
        if (!actor_coroutine_) {
            auto task = act();
            if (task) {
                coro_handle_ = task.handle();
                actor_coroutine_ = sched::ActorCoroutine{std::move(task), id()};

                if (mailbox_) {
                    // Continuation callback disabled: direct resume races with
                    // the scheduler's execute_actor state machine, causing
                    // await_suspend CAS(kRunning→kIdle) to fail and the
                    // coroutine to busy-loop. Wakeups go through notify_ready()
                    // which transitions state correctly.
                    //
                    // auto* coro_ptr = &actor_coroutine_;
                    // mailbox_->set_continuation_callback([coro_ptr]() {
                    //     if (!coro_ptr->done()) {
                    //         coro_ptr->promise().notify_mailbox_nonempty();
                    //     }
                    // });
                }
            }
        }
    }

#else // !HPACTOR_SUPPORT_COROUTINES
    void ensure_coroutine_started() {}
#endif // HPACTOR_SUPPORT_COROUTINES

    sched::IScheduler* get_scheduler() {
        return scheduler_;
    }

    mailbox::MPSCActorMailbox<TypedMessage>* get_mailbox() {
        return mailbox_;
    }

    bool mailbox_has_messages() const {
        return mailbox_ && !mailbox_->empty();
    }
    bool mailbox_is_empty() const {
        return !mailbox_ || mailbox_->empty();
    }

    void set_scheduler(sched::IScheduler* scheduler) override {
        scheduler_ = scheduler;
    }
    void
    set_mailbox(mailbox::MPSCActorMailbox<TypedMessage>* mailbox) override {
        mailbox_ = mailbox;
    }

  protected:
    virtual Behavior make_behavior() {
        return {};
    }

    // Users override this to call on<T>() / on_request<ReqT,ResT>()
    virtual void register_handlers() {}

    // Called by the framework after construction to set up handlers
    void initialize_proto_handlers();

    void on_activate() override;
    void on_deactivate() override;

    // Get TypeTag for a protobuf type from the system registry
    template<typename ProtoMsgT>
    TypeTag type_tag_for() const {
        return system().proto_registry().lookup<ProtoMsgT>();
    }

  public:
    virtual void on_exit();

    void set_exit_reason(uint32_t code) { exit_reason_ = code; }
    uint32_t exit_reason() const { return exit_reason_; }

    EventBasedActor(ActorContext* ctx, ActorSystem& sys);

  private:
#if HPACTOR_SUPPORT_COROUTINES
    sched::ActorCoroutine actor_coroutine_;
    std::coroutine_handle<sched::CoroutinePromise> coro_handle_;
#endif
    Behavior behavior_;
    uint32_t exit_reason_ = 0;
    mailbox::MPSCActorMailbox<TypedMessage>* mailbox_ = nullptr;
    sched::IScheduler* scheduler_ = nullptr;

    bool handlers_initialized_ = false;
    std::unordered_map<TypeTag, ProtoHandler> proto_handlers_;
};

} // namespace hpactor
