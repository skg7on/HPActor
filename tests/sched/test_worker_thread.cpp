// tests/sched/test_worker_thread.cpp
#include <cassert>
#include <cstdio>
#include <hpactor/sched/coroutine_frame_pool.hpp>
#include <hpactor/sched/worker_thread.hpp>

using hpactor::ActorId;
using namespace hpactor::sched;

void test_config_defaults() {
    WorkerThread::Config cfg;
    assert(cfg.worker_index == 0);
    assert(cfg.priority_levels == 4);
    assert(cfg.steal_threshold == 10);
    assert(cfg.victim_scan_limit == 4);
    printf("  PASSED test_config_defaults\n");
}

void test_index() {
    WorkerThread::Config cfg;
    cfg.worker_index = 42;
    WorkerThread worker(cfg);
    assert(worker.index() == 42);
    printf("  PASSED test_index\n");
}

void test_start_stop() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    assert(!worker.is_running());
    worker.start();
    assert(worker.is_running());
    worker.stop();
    assert(!worker.is_running());
    printf("  PASSED test_start_stop\n");
}

void test_double_start() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    worker.start();
    assert(worker.is_running());
    worker.start();
    assert(worker.is_running());
    worker.stop();
    printf("  PASSED test_double_start\n");
}

void test_push_pop() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem item;
    item.actor = ActorId{0};
    worker.push(0, item);
    WorkItem out;
    assert(worker.pop(out));
    assert(out.actor == ActorId{0});
    printf("  PASSED test_push_pop\n");
}

void test_pop_empty() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem out;
    assert(!worker.pop(out));
    printf("  PASSED test_pop_empty\n");
}

void test_steal() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem item1;
    item1.actor = ActorId{1};
    worker.push(0, item1);
    WorkItem item2;
    item2.actor = ActorId{2};
    worker.push(1, item2);
    WorkItem stolen;
    assert(worker.steal(stolen));
    assert(stolen.actor == ActorId{1});
    WorkItem popped;
    assert(worker.pop(popped));
    assert(popped.actor == ActorId{2});
    printf("  PASSED test_steal\n");
}

void test_steal_empty() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem out;
    assert(!worker.steal(out));
    printf("  PASSED test_steal_empty\n");
}

void test_depth() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem item;
    item.actor = ActorId{0};
    worker.push(0, item);
    assert(worker.depth() >= 1);
    printf("  PASSED test_depth\n");
}

void test_donation_count() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    assert(worker.donation_count() == 0);
    worker.increment_donations();
    assert(worker.donation_count() == 1);
    worker.increment_donations();
    worker.increment_donations();
    assert(worker.donation_count() == 3);
    printf("  PASSED test_donation_count\n");
}

void test_acquire_release_frame() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    assert(worker.acquire_frame() == nullptr);
    CoroutineFramePool pool(4, 1024);
    worker.set_frame_pool(&pool);
    auto* f = worker.acquire_frame();
    assert(f != nullptr && f->in_use);
    worker.release_frame(f);
    assert(!f->in_use);
    worker.release_frame(nullptr);
    printf("  PASSED test_acquire_release_frame\n");
}

void test_process_noop() {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem item;
    item.actor = ActorId{0};
    worker.process(item);
    printf("  PASSED test_process_noop\n");
}

int main() {
    printf("WorkerThread tests:\n");
    test_config_defaults();
    test_index();
    test_start_stop();
    test_double_start();
    test_push_pop();
    test_pop_empty();
    test_steal();
    test_steal_empty();
    test_depth();
    test_donation_count();
    test_acquire_release_frame();
    test_process_noop();
    printf("All WorkerThread tests PASSED\n");
    return 0;
}
