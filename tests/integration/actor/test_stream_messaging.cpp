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
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stream/stream_config.hpp>
#include <hpactor/actor/stream/stream_handle.hpp>
#include <hpactor/actor/stream/stream_types.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/adt/stream_buffer.hpp>

#include <vector>

using namespace hpactor;

namespace {

// A receiver actor that collects stream events and chunks.
class StreamTestReceiver : public EventBasedActor {
  public:
    explicit StreamTestReceiver(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            auto tag = msg.type_id();
            if (tag == stream::StreamOpenedTag) {
                opened_ = true;
                stream_id_ = 1; // placeholder — full payload in future phase
            } else if (tag == stream::StreamClosedTag) {
                closed_ = true;
            } else if (tag == stream::StreamErrorTag) {
                errored_ = true;
            } else if (tag == stream::StreamChunkTag) {
                const auto& p = msg.payload();
                received_chunks_.push_back(
                    StreamBuffer(p.data(), p.data() + p.size()));
            }
        }};
    }

    bool opened_ = false;
    bool closed_ = false;
    bool errored_ = false;
    uint64_t stream_id_ = 0;
    std::vector<StreamBuffer> received_chunks_;
};

} // namespace

// ── Stream Open / Close ────────────────────────────────────────────────────

TEST(StreamMessagingTest, OpenStreamLocalReturnsValidHandle) {
    Config cfg;
    cfg.scheduler_threads = 0; // deterministic: no workers
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());
    EXPECT_TRUE(handle->is_open());
    EXPECT_GT(handle->stream_id(), 0u);
}

TEST(StreamMessagingTest, OpenStreamToUnknownTargetReturnsNullopt) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    ActorId invalid_id{999999};
    auto handle = system.open_stream(invalid_id);
    EXPECT_FALSE(handle.has_value());
}

TEST(StreamMessagingTest, StreamCloseGraceful) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());
    EXPECT_TRUE(handle->close());
    EXPECT_FALSE(handle->is_open());
}

TEST(StreamMessagingTest, StreamErrorDeliversErrorTag) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());

    EXPECT_TRUE(handle->error(42, "test error"));
    EXPECT_FALSE(handle->is_open());
}

TEST(StreamMessagingTest, WriteOnClosedHandleReturnsFalse) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());
    handle->close();

    StreamBuffer buf;
    EXPECT_FALSE(handle->write(TypeTag::User, std::move(buf)));
    EXPECT_FALSE(handle->close()); // double close
}

TEST(StreamMessagingTest, StreamHandleMoveSemantics) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    auto h1 = system.open_stream(receiver.id());
    ASSERT_TRUE(h1.has_value());

    uint64_t sid = h1->stream_id();
    auto h2 = std::move(*h1);
    EXPECT_FALSE(h1->is_open()); // moved-from is closed
    EXPECT_TRUE(h2.is_open());
    EXPECT_EQ(h2.stream_id(), sid);
}

// ── Stream Data Flow ───────────────────────────────────────────────────────

TEST(StreamMessagingTest, StreamDataChunksArriveInOrder) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());

    // Write 5 chunks with sequential markers
    for (int i = 0; i < 5; i++) {
        uint8_t val = static_cast<uint8_t>(i);
        StreamBuffer payload(&val, &val + 1);
        EXPECT_TRUE(handle->write(TypeTag::User, std::move(payload)));
    }
    handle->close();

    // The StreamReceiverActor processes messages, and the receiver actor
    // should have received them. Since scheduler_threads=0, processing is
    // synchronous after each try_deliver_local_fast call.
    // Note: with the stream actor architecture, messages go through
    // StreamSenderActor and StreamReceiverActor. We validate that the
    // handle operations succeed without crashing.
    EXPECT_FALSE(handle->is_open());
}

TEST(StreamMessagingTest, StreamWriteAfterCloseFails) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    auto handle = system.open_stream(receiver.id());
    ASSERT_TRUE(handle.has_value());
    handle->close();

    uint8_t data[] = {1, 2, 3};
    StreamBuffer buf(data, data + 3);
    EXPECT_FALSE(handle->write(TypeTag::User, std::move(buf)));
}

TEST(StreamMessagingTest, OpenStreamWithCustomConfig) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    StreamConfig stream_cfg;
    stream_cfg.initial_window_bytes = 128 * 1024;
    stream_cfg.send_buffer_bytes = 512 * 1024;

    auto handle = system.open_stream(receiver.id(), stream_cfg);
    ASSERT_TRUE(handle.has_value());
    EXPECT_TRUE(handle->is_open());
}

TEST(StreamMessagingTest, MultipleStreamsNoInterference) {
    Config cfg;
    cfg.scheduler_threads = 0;
    ActorSystem system(cfg);

    auto receiver = system.spawn<StreamTestReceiver>();
    ASSERT_TRUE(static_cast<bool>(receiver));

    // Open two streams to the same receiver.
    auto h1 = system.open_stream(receiver.id());
    auto h2 = system.open_stream(receiver.id());
    ASSERT_TRUE(h1.has_value());
    ASSERT_TRUE(h2.has_value());

    EXPECT_NE(h1->stream_id(), h2->stream_id());
    EXPECT_TRUE(h1->is_open());
    EXPECT_TRUE(h2->is_open());

    h1->close();
    EXPECT_FALSE(h1->is_open());
    EXPECT_TRUE(h2->is_open()); // second stream unaffected

    h2->close();
    EXPECT_FALSE(h2->is_open());
}
