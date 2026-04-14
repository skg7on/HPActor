// tests/sched/test_multi_priority_work_queue.cpp
#include <cassert>
#include <hpactor/sched/work_queue.hpp>
#include <vector>

int main() {
    hpactor::sched::MultiPriorityWorkQueue q(4);

    // Test: pop returns false on empty
    hpactor::sched::WorkItem out;
    assert(!q.pop(out));

    // Test: push/pop round-trip
    hpactor::sched::WorkItem item;
    item.actor = hpactor::ActorId{1};
    item.deadline_ns = 1000;
    item.sequence = 1;

    q.push(0, item);
    assert(q.depth_approx() == 1);
    assert(q.pop(out));
    assert(out.actor.value() == 1);
    assert(q.depth_approx() == 0);

    // Test: priority ordering — higher priority returned first
    hpactor::sched::WorkItem lo, hi;
    lo.actor = hpactor::ActorId{1}; lo.deadline_ns = 2000; lo.sequence = 1;
    hi.actor = hpactor::ActorId{2}; hi.deadline_ns = 1000; hi.sequence = 2;

    q.push(3, lo);   // low priority (3)
    q.push(0, hi);   // high priority (0)
    q.push(2, lo);   // also low

    assert(q.pop(out));
    assert(out.actor.value() == 2);  // high priority first
    assert(q.pop(out));
    assert(out.actor.value() == 1);  // priority 2 before priority 3
    assert(q.pop(out));
    assert(out.actor.value() == 1);  // last remaining

    return 0;
}