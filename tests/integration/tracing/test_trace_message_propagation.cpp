#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>

using namespace hpactor;

class CaptureActor final : public EventBasedActor {
  public:
    CaptureActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            saw_trace = msg.has_trace_context();
            if (saw_trace) {
                trace = msg.trace_context();
            }
        });
    }

    bool saw_trace{false};
    TraceContext trace{};
};

TEST(TraceMessagePropagationTest, TraceContextPropagatesOnSend) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.tracing.enabled = true;
    cfg.tracing.exporter = tracing::TraceExporterKind::kMemory;
    cfg.tracing.sampler = tracing::SamplerKind::kAlwaysOn;
    cfg.tracing.create_roots_for_actor_context_sends = true;
    ActorSystem system(cfg);

    auto sender = system.spawn<EventBasedActor>();
    auto receiver = system.spawn<CaptureActor>();
    ActorContext ctx(sender, &system);

    TypedMessage msg(TypeTag::User, StreamBuffer{7});
    ctx.send(receiver.address(), std::move(msg));

    auto* mailbox = system.get_mailbox(receiver.id());
    TypedMessage received;
    ASSERT_TRUE(mailbox->try_pop(received));
    receiver.get()->receive(received);

    auto* cap = static_cast<CaptureActor*>(receiver.get().get());
    ASSERT_TRUE(cap->saw_trace);
    EXPECT_TRUE(cap->trace.valid());

    system.trace_manager()->force_flush();
    EXPECT_EQ(system.trace_manager()->spans_dropped(), 0u);
}
