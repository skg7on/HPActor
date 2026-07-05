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

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <memory>
#include <vector>

namespace hpactor::testing {

/// Internal actor used by TestProbe to collect messages.
class ProbeActor : public EventBasedActor {
  public:
    ProbeActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{
            [this](TypedMessage& msg) { queue_.push_back(std::move(msg)); }};
    }

    const std::vector<TypedMessage>& queue() const {
        return queue_;
    }
    std::vector<TypedMessage>& mutable_queue() {
        return queue_;
    }
    size_t queue_size() const {
        return queue_.size();
    }
    void drain() {
        queue_.clear();
    }

  private:
    std::vector<TypedMessage> queue_;
};

/// Lightweight probe actor that receives messages, queues them,
/// and provides assertion helpers. Use with SchedulerTestDriver
/// for deterministic testing.
class TestProbe {
  public:
    explicit TestProbe(ActorSystem& system) {
        actor_ = system.spawn<ProbeActor>();
        addr_ = actor_.get()->address();
    }

    /// The ActorAddress to send messages to.
    const ActorAddress& address() const {
        return addr_;
    }

    /// Drain the probe's queue.
    void clear() {
        probe().drain();
    }

    /// Raw queue access.
    const std::vector<TypedMessage>& queue() const {
        return probe().queue();
    }
    size_t queue_size() const {
        return probe().queue_size();
    }

    /// Assert no message of TypeTag \p tag is in the queue.
    void expect_no_message(TypeTag tag) const {
        for (auto& msg : probe().queue()) {
            if (msg.type_id() == tag) {
                fprintf(stderr,
                        "FAIL: Expected no message of tag 0x%x in probe queue\n",
                        static_cast<uint32_t>(tag));
                std::abort();
            }
        }
    }

    /// Search for a message matching a predicate.
    template <typename T, typename Predicate>
    const T* fish_for_message(Predicate pred, TypeTag tag) const {
        for (auto& msg : probe().queue()) {
            if (msg.type_id() == tag) {
                auto proto = msg.as<T>();
                if (proto && pred(*proto)) {
                    return proto.get();
                }
            }
        }
        return nullptr;
    }

  private:
    const ProbeActor& probe() const {
        return static_cast<const ProbeActor&>(*actor_.get());
    }
    ProbeActor& probe() {
        return static_cast<ProbeActor&>(*actor_.get());
    }

    Actor actor_;
    ActorAddress addr_;
};

} // namespace hpactor::testing
