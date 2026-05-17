// tests/sched/test_coroutine_frame_pool.cpp
#include <hpactor/sched/coroutine_frame_pool.hpp>

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace hpactor::sched;

void test_acquire_release() {
    CoroutineFramePool pool(4, 1024);
    assert(pool.total() == 4);
    assert(pool.available() == 4);
    assert(!pool.empty());

    auto* frame = pool.acquire();
    assert(frame != nullptr);
    assert(frame->in_use);
    assert(frame->stack_ptr != nullptr);
    assert(frame->stack_size == 1024);
    assert(pool.available() == 3);

    pool.release(frame);
    assert(!frame->in_use);
    assert(pool.available() == 4);
    assert(pool.total() == 4);

    printf("  PASSED test_acquire_release\n");
}

void test_exhaustion() {
    CoroutineFramePool pool(2, 1024);
    auto* f1 = pool.acquire();
    auto* f2 = pool.acquire();
    assert(f1 != nullptr && f2 != nullptr);
    assert(pool.available() == 0);
    assert(pool.empty());

    auto* f3 = pool.acquire();
    assert(f3 == nullptr);

    pool.release(f1);
    assert(!pool.empty());
    auto* f4 = pool.acquire();
    assert(f4 != nullptr);

    pool.release(f2);
    pool.release(f4);

    printf("  PASSED test_exhaustion\n");
}

void test_release_nullptr() {
    CoroutineFramePool pool(1, 1024);
    pool.release(nullptr);
    assert(pool.available() == 1);
    printf("  PASSED test_release_nullptr\n");
}

void test_double_release() {
    CoroutineFramePool pool(1, 1024);
    auto* f = pool.acquire();
    assert(f != nullptr);
    pool.release(f);
    pool.release(f);
    assert(pool.available() == 1);
    printf("  PASSED test_double_release\n");
}

void test_reacquire() {
    CoroutineFramePool pool(4, 2048);
    auto* f1 = pool.acquire();
    void* stack_ptr = f1->stack_ptr;
    pool.release(f1);
    auto* f2 = pool.acquire();
    assert(f2 == f1);
    assert(f2->stack_ptr == stack_ptr);
    pool.release(f2);
    printf("  PASSED test_reacquire\n");
}

void test_concurrent_stress() {
    CoroutineFramePool pool(16, 4096);
    const int num_threads = 4;
    const int iterations = 1000;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&pool]() {
            for (int i = 0; i < iterations; ++i) {
                auto* f = pool.acquire();
                if (f) {
                    f->stack_ptr[0] = std::byte{42};
                    f->stack_ptr[f->stack_size - 1] = std::byte{99};
                    pool.release(f);
                }
            }
        });
    }

    for (auto& th : threads)
        th.join();
    assert(pool.available() == 16);
    printf("  PASSED test_concurrent_stress\n");
}

void test_empty_true() {
    CoroutineFramePool pool(1, 1024);
    assert(!pool.empty());
    auto* f = pool.acquire();
    assert(pool.empty());
    pool.release(f);
    assert(!pool.empty());
    printf("  PASSED test_empty_true\n");
}

void test_constructor_params() {
    CoroutineFramePool pool(8, 1024);
    assert(pool.total() == 8);
    assert(pool.available() == 8);
    assert(pool.stack_size() == 1024);

    CoroutineFramePool pool2(4);
    assert(pool2.total() == 4);
    assert(pool2.stack_size() == 8 * 1024);
    printf("  PASSED test_constructor_params\n");
}

void test_acquire_releases_different_frames() {
    CoroutineFramePool pool(4, 1024);
    auto* f1 = pool.acquire();
    auto* f2 = pool.acquire();
    auto* f3 = pool.acquire();
    auto* f4 = pool.acquire();
    assert(f1 != nullptr && f2 != nullptr && f3 != nullptr && f4 != nullptr);
    assert(f1->stack_ptr != f2->stack_ptr);
    assert(f1->stack_ptr != f3->stack_ptr);
    assert(f1->stack_ptr != f4->stack_ptr);
    assert(f2->stack_ptr != f3->stack_ptr);
    assert(f2->stack_ptr != f4->stack_ptr);
    assert(f3->stack_ptr != f4->stack_ptr);
    pool.release(f1);
    pool.release(f2);
    pool.release(f3);
    pool.release(f4);
    printf("  PASSED test_acquire_releases_different_frames\n");
}

int main() {
    printf("CoroutineFramePool tests:\n");
    test_acquire_release();
    test_exhaustion();
    test_release_nullptr();
    test_double_release();
    test_reacquire();
    test_concurrent_stress();
    test_empty_true();
    test_constructor_params();
    test_acquire_releases_different_frames();
    printf("All CoroutineFramePool tests PASSED\n");
    return 0;
}
