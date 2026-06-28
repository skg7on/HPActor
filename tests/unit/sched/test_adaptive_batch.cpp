// Tests for SCHED-05: adaptive batch continuation.
// Verifies correctness: all messages processed; batch doesn't break delivery.
// The performance aspect (fewer requeue round-trips) is validated by bench_caf.

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>

using namespace hpactor;

class BatchCountingActor : public EventBasedActor {
  public:
    BatchCountingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }
    int received() const {
        return count_.load();
    }
    void
    wait_for(int n,
             std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, timeout, [&] { return count_.load() >= n; });
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage&) {
            count_.fetch_add(1);
            cv_.notify_all();
        }};
    }

  private:
    std::atomic<int> count_{0};
    std::mutex mu_;
    std::condition_variable cv_;
};

class AdaptiveBatchTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg.scheduler_threads = 2;
        cfg.enable_network = false;
        cfg.enable_receptionist = false;
    }
    Config cfg;
};

TEST_F(AdaptiveBatchTest, AllMessagesDeliveredInBurst) {
    ActorSystem sys{cfg};
    auto actor = sys.spawn<BatchCountingActor>();
    auto* ca = static_cast<BatchCountingActor*>(actor.get().get());

    constexpr int kMsgs = 64;
    for (int i = 0; i < kMsgs; ++i)
        sys.deliver_local(actor.id(), TypedMessage{TypeTag::User, StreamBuffer{1}});

    ca->wait_for(kMsgs);
    EXPECT_EQ(ca->received(), kMsgs);
}

TEST_F(AdaptiveBatchTest, BurstLargerThanBatchLimitDeliveredCompletely) {
    ActorSystem sys{cfg};
    auto actor = sys.spawn<BatchCountingActor>();
    auto* ca = static_cast<BatchCountingActor*>(actor.get().get());

    constexpr int kMsgs = 200; // > kBatchLimit(64), exercises requeue path
    for (int i = 0; i < kMsgs; ++i)
        sys.deliver_local(actor.id(), TypedMessage{TypeTag::User, StreamBuffer{1}});

    ca->wait_for(kMsgs, std::chrono::milliseconds{10000});
    EXPECT_EQ(ca->received(), kMsgs);
}
