// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#include <hpactor/mem/object_pool.hpp>

#include <atomic>
#include <gtest/gtest.h>

using namespace hpactor::mem;

namespace {

struct TestObj {
    int value = 0;
    TestObj() = default;
    explicit TestObj(int v) : value(v) {}
};

} // namespace

// ── Acquire and release round-trip ────────────────────────────────
TEST(ObjectPoolTest, AcquireReleaseRoundTrip) {
    ObjectPool<TestObj, 16> pool;
    pool.prefill(8);

    auto* obj = pool.try_acquire();
    ASSERT_NE(obj, nullptr);
    obj->value = 42;
    pool.release(obj);

    auto* obj2 = pool.try_acquire();
    ASSERT_NE(obj2, nullptr);
    EXPECT_EQ(obj2->value, 42); // same object, value preserved
}

// ── Exhaustion returns nullptr ────────────────────────────────────
TEST(ObjectPoolTest, ExhaustionReturnsNullptr) {
    ObjectPool<TestObj, 16> pool;
    pool.prefill(2);

    auto* a = pool.try_acquire();
    auto* b = pool.try_acquire();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    auto* c = pool.try_acquire();
    EXPECT_EQ(c, nullptr) << "pool should be exhausted";
}

// ── Release to pool within capacity grows available count ─────────
TEST(ObjectPoolTest, ReleaseBelowCapacity) {
    ObjectPool<TestObj, 16> pool;
    pool.prefill(4);
    EXPECT_EQ(pool.available(), 4u);

    TestObj extra(99);
    pool.release(&extra); // pool has room (4 < 16)
    EXPECT_EQ(pool.available(), 5u);
}

// ── Prefill size matches available count ──────────────────────────
TEST(ObjectPoolTest, PrefillCount) {
    ObjectPool<TestObj, 16> pool;
    EXPECT_EQ(pool.available(), 0u);
    pool.prefill(8);
    EXPECT_EQ(pool.available(), 8u);
}

// ── Acquire after release to previously exhausted pool ─────────────
TEST(ObjectPoolTest, AcquireAfterReleaseRefills) {
    ObjectPool<TestObj, 16> pool;
    pool.prefill(1);

    auto* a = pool.try_acquire();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    pool.release(a);
    EXPECT_EQ(pool.available(), 1u);

    auto* b = pool.try_acquire();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b, a) << "released object should be re-acquired";
}
