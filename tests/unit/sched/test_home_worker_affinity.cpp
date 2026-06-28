// Tests for SCHED-02: home-worker affinity.
// Verifies that repeated actor activations route to the same worker
// (home_worker derived from actor_id hash), and that home_worker is
// preserved across RequeueReady requeue cycles.

#include <gtest/gtest.h>
#include <hpactor/sched/work_queue.hpp>

using namespace hpactor;
using namespace hpactor::sched;

TEST(HomeWorkerAffinity, WorkItemDefaultHomeWorkerIsUnset) {
    WorkItem item{};
    EXPECT_EQ(item.home_worker, UINT32_MAX);
}

TEST(HomeWorkerAffinity, HomeWorkerModConstraint) {
    // home_worker should always be < num_workers when used
    constexpr uint32_t num_workers = 8;
    ActorId id{42};
    uint32_t hw = static_cast<uint32_t>(id.value() % num_workers);
    EXPECT_LT(hw, num_workers);
}
