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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/ref/actor_proxy.hpp>

#include <chrono>
#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::mailbox;
using namespace hpactor::net;

namespace {

/// Configurable mock transport for testing TransportSendResult →
/// DeliveryResult mapping.
class ConfigurableMockTransport : public Transport {
  public:
    explicit ConfigurableMockTransport(TransportSendResult result)
        : result_(result) {}
    void set_result(TransportSendResult r) {
        result_ = r;
    }

    TransportSendResult try_send(const ActorAddress&, const StreamBuffer&) override {
        return result_;
    }
    ConnectionPtr connect(EndPoint, const std::string&, uint16_t) override {
        return nullptr;
    }
    ConnectionPtr connect(EndPoint) override {
        return nullptr;
    }
    void listen(uint16_t) override {}
    void stop_listening() override {}
    bool is_connected(EndPoint) const override {
        return true;
    }
    EndPoint endpoint() const override {
        return LocalEndpoint;
    }
    void close_connection(EndPoint) override {}
    void set_rpc_handler(rpc_response_handler) override {}

  private:
    TransportSendResult result_;
};

class RemoteDeliveryResultTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(cfg);
        mock_transport_ =
            std::make_shared<ConfigurableMockTransport>(TransportSendResult::Sent);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(10);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }

    std::unique_ptr<ActorSystem> system_;
    std::shared_ptr<ConfigurableMockTransport> mock_transport_;
};

// ── TransportSendResult → DeliveryStatus mapping ─────────────────────────

TEST_F(RemoteDeliveryResultTest, SentMapsToAccepted) {
    mock_transport_->set_result(TransportSendResult::Sent);
    auto dr = DeliveryResult::from_transport(TransportSendResult::Sent,
                                             ActorAddress{}, MessageId{1});
    EXPECT_EQ(dr.status, DeliveryStatus::Accepted);
    EXPECT_TRUE(dr.ok());
    EXPECT_FALSE(dr.retryable());
    EXPECT_EQ(dr.message_id, MessageId{1});
}

TEST_F(RemoteDeliveryResultTest, NotConnectedMapsToRemoteUnavailable) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::NotConnected,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable());
    EXPECT_FALSE(dr.ok());
}

TEST_F(RemoteDeliveryResultTest, QueueFullMapsToRemoteUnavailable) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::QueueFull,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable());
}

TEST_F(RemoteDeliveryResultTest, CircuitOpenMapsToRemoteUnavailable) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::CircuitOpen,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::RemoteUnavailable);
    EXPECT_TRUE(dr.retryable());
}

TEST_F(RemoteDeliveryResultTest, EncodeErrorMapsToSerializationError) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::EncodeError,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::SerializationError);
    EXPECT_FALSE(dr.retryable());
}

TEST_F(RemoteDeliveryResultTest, ShuttingDownMapsToShuttingDown) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::ShuttingDown,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::ShuttingDown);
    EXPECT_FALSE(dr.retryable());
}

TEST_F(RemoteDeliveryResultTest, WriteErrorMapsToTransportError) {
    auto dr = DeliveryResult::from_transport(TransportSendResult::WriteError,
                                             ActorAddress{}, {});
    EXPECT_EQ(dr.status, DeliveryStatus::TransportError);
    EXPECT_TRUE(dr.retryable());
}

// ── from_transport preserves metadata ────────────────────────────────────

TEST_F(RemoteDeliveryResultTest, FromTransportPreservesTarget) {
    Ipv4Endpoint ep{0x7F000001, 9090};
    ActorAddress addr{EndPoint{ep}, ActorType{0}, ActorId{99}, 0};
    auto dr = DeliveryResult::from_transport(TransportSendResult::Sent, addr,
                                             MessageId{42});
    EXPECT_EQ(dr.target, addr);
    EXPECT_EQ(dr.message_id, MessageId{42});
    EXPECT_EQ(dr.status, DeliveryStatus::Accepted);
}

// ── try_reply returns DeliveryResult ──────────────────────────────────────

TEST_F(RemoteDeliveryResultTest, TryReplyReturnsDeliveryResult) {
    auto sender = system_->spawn<EventBasedActor>();
    auto target = system_->spawn<EventBasedActor>();

    ActorContext ctx(sender, system_.get());
    // Set up a mock sender address so try_reply has something to reply to.
    // try_reply uses current_message()->sender which is only set during
    // message processing. Since scheduler_threads=0, we test that try_reply
    // returns NoRoute when there's no current message.
    auto result = ctx.try_reply(TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_FALSE(result.get().ok());
    EXPECT_EQ(result.get().status, DeliveryStatus::NoRoute);
}

} // namespace
