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

#include <gtest/gtest.h>

#include <scheduler_test_driver.hpp>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/mailbox/delivery_pipeline.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/python/python_bridge_actor.hpp>
#include <hpactor/python/python_runtime.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;

namespace {
struct BridgeWakeProbe {
    size_t calls{0};
    static bool wake(void* context) noexcept {
        ++static_cast<BridgeWakeProbe*>(context)->calls;
        return true;
    }
};
} // namespace

TEST(PythonBridgeActorTest, TransfersCompleteEnvelopeOnActorTurn) {
    // Create runtime first so it outlives the actor system.
    // The PythonBridgeActor holds a reference to the runtime, and its
    // lease destructor accesses the runtime during ActorSystem teardown.
    auto created = python::PythonRuntime::create({});
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());

    BridgeWakeProbe probe;
    ASSERT_TRUE(runtime->start({&probe, &BridgeWakeProbe::wake}).ok());
    auto lease = runtime->reserve_actor();
    ASSERT_TRUE(lease.has_value());

    Config cfg;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto bridge =
        system.spawn<python::PythonBridgeActor>(*runtime, std::move(*lease));

    TypedMessage message(TypeTag::User, StreamBuffer{1, 2, 3});
    message.set_sender_address(
        ActorAddress{LocalEndpoint, ActorType{7}, ActorId{99}, 4});
    message.set_message_id(501);
    message.set_ask_message_id(601);
    mailbox::DeliveryOptions options;
    options.message_id = 501;
    options.flags = 0x40;
    auto result = system.try_deliver_local(bridge.id(), std::move(message), 2,
                                           INT64_MAX, options);
    ASSERT_TRUE(result.accepted()) << "code=" << static_cast<int>(result.code);
    ASSERT_TRUE(driver.drain_until(
        [&] { return runtime->snapshot().queues.dispatch_depth == 1; }));

    python::PythonDispatchEnvelope observed;
    bool got_envelope{false};
    ASSERT_EQ(runtime->drain_dispatch(1,
                                      [&](const auto& item) {
                                          observed = item;
                                          got_envelope = true;
                                      }),
              1u);
    ASSERT_TRUE(got_envelope);
    EXPECT_EQ(observed.actor.id, bridge.id());
    EXPECT_EQ(observed.sender.id, ActorId(99));
    EXPECT_EQ(observed.message_id, MessageId(501));
    EXPECT_EQ(observed.ask_message_id, 601u);
    EXPECT_EQ(observed.priority, 2u);
    EXPECT_EQ(observed.flags, 0x40u);
    EXPECT_EQ(observed.deadline_ns, INT64_MAX);
    EXPECT_EQ(observed.payload, (StreamBuffer{1, 2, 3}));

    // Stop the runtime before destroying the system so the lease
    // release during ActorSystem teardown is a no-op on a stopped runtime.
    EXPECT_TRUE(runtime->stop().ok());
}
