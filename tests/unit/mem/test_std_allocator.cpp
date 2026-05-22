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

// Smoke test for MemStdAllocator — std::allocator-compatible adapter
// for the HPActor slab allocator.

#include <gtest/gtest.h>
#include <hpactor/mem/std_allocator.hpp>

#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <cstring>

using namespace hpactor;
using namespace hpactor::mem;

// ---------------------------------------------------------------------------
// Fixture: sets up a TLA for allocate_bytes to work
// ---------------------------------------------------------------------------
class MemStdAllocatorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tla_ = new ThreadLocalAllocator();
        set_thread_allocator(tla_);
    }

    void TearDown() override {
        delete tla_;
        tla_ = nullptr;
    }

    ThreadLocalAllocator* tla_ = nullptr;
};

// ---------------------------------------------------------------------------
// Basic allocation / deallocation
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, BasicAlloc) {
    MemStdAllocator<int> alloc(ActorId{42}, RegionType::kActor);

    int* p = alloc.allocate(10);
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 10; ++i) {
        p[i] = i * 100;
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(p[i], i * 100);
    }
    alloc.deallocate(p, 10);
}

// ---------------------------------------------------------------------------
// Default-constructed allocator
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, DefaultCtor) {
    MemStdAllocator<double> alloc;
    EXPECT_EQ(alloc.owner(), ActorId{});
    EXPECT_EQ(alloc.region(), RegionType::kActor);

    double* p = alloc.allocate(3);
    p[0] = 1.0;
    p[1] = 2.0;
    p[2] = 3.0;
    alloc.deallocate(p, 3);
}

// ---------------------------------------------------------------------------
// Rebind via conversion constructor
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, Rebind) {
    MemStdAllocator<int> int_alloc(ActorId{7});
    MemStdAllocator<char> char_alloc(int_alloc);

    EXPECT_EQ(char_alloc.owner(), ActorId{7});
    EXPECT_EQ(char_alloc.region(), RegionType::kActor);

    char* p = char_alloc.allocate(32);
    std::memset(p, 'X', 32);
    char_alloc.deallocate(p, 32);
}

// ---------------------------------------------------------------------------
// Equality semantics
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, Equality) {
    MemStdAllocator<int> a1(ActorId{1});
    MemStdAllocator<int> a2(ActorId{1});
    MemStdAllocator<int> a3(ActorId{2});

    EXPECT_TRUE(a1 == a2);
    EXPECT_FALSE(a1 != a2);
    EXPECT_TRUE(a1 != a3);
    EXPECT_FALSE(a1 == a3);

    // Rebound allocator compares equal to source
    MemStdAllocator<char> a4(a1);
    EXPECT_TRUE(a1 == a4);
}

// ---------------------------------------------------------------------------
// std::vector integration
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, VectorIntegration) {
    using Vec = std::vector<int, MemStdAllocator<int>>;

    Vec v(MemStdAllocator<int>(ActorId{99}));
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    EXPECT_EQ(v.size(), 3U);
    EXPECT_EQ(v[0], 10);
    EXPECT_EQ(v[1], 20);
    EXPECT_EQ(v[2], 30);
}

// ---------------------------------------------------------------------------
// std::deque integration
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, DequeIntegration) {
    using Deq = std::deque<int, MemStdAllocator<int>>;
    Deq d(MemStdAllocator<int>(ActorId{100}));
    d.push_back(1);
    d.push_front(0);
    d.push_back(2);
    EXPECT_EQ(d.size(), 3U);
}

// ---------------------------------------------------------------------------
// std::set integration (node-based container)
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, SetIntegration) {
    using Set = std::set<int, std::less<>, MemStdAllocator<int>>;
    Set s(MemStdAllocator<int>(ActorId{101}));
    s.insert(3);
    s.insert(1);
    s.insert(2);
    EXPECT_EQ(s.size(), 3U);
}

// ---------------------------------------------------------------------------
// std::map integration (node-based container)
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, MapIntegration) {
    using Map = std::map<int, std::string, std::less<>,
                         MemStdAllocator<std::pair<const int, std::string>>>;
    Map m(MemStdAllocator<std::pair<const int, std::string>>(ActorId{102}));
    m[1] = "one";
    m[2] = "two";
    EXPECT_EQ(m.size(), 2U);
    EXPECT_EQ(m[1], "one");
}

// ---------------------------------------------------------------------------
// std::unordered_map integration
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, UnorderedMapIntegration) {
    using UMap = std::unordered_map<int, int, std::hash<int>, std::equal_to<>,
                                    MemStdAllocator<std::pair<const int, int>>>;
    UMap um(MemStdAllocator<std::pair<const int, int>>(ActorId{103}));
    um[1] = 100;
    um[2] = 200;
    EXPECT_EQ(um.size(), 2U);
}

// ---------------------------------------------------------------------------
// Vector copy / move with allocator propagation
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, VectorPropagation) {
    using Vec = std::vector<int, MemStdAllocator<int>>;

    Vec v1(MemStdAllocator<int>(ActorId{200}));
    v1.push_back(42);

    // Copy: allocator propagates, new vector uses v1's allocator
    Vec v2(v1);
    EXPECT_EQ(v2.size(), 1U);
    EXPECT_EQ(v2[0], 42);
    EXPECT_TRUE(v2.get_allocator() == v1.get_allocator());

    // Move: allocator propagates
    Vec v3(std::move(v1));
    EXPECT_EQ(v3.size(), 1U);
    EXPECT_EQ(v3[0], 42);
}

// ---------------------------------------------------------------------------
// Zero-size allocation
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, ZeroAlloc) {
    MemStdAllocator<int> alloc(ActorId{1});
    int* p = alloc.allocate(0);
    // C++20 allows nullptr for zero-size allocation
    alloc.deallocate(p, 0); // should be no-op
}

// ---------------------------------------------------------------------------
// void* deallocation (nullptr)
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, NullDealloc) {
    MemStdAllocator<int> alloc(ActorId{1});
    alloc.deallocate(nullptr, 10); // should be no-op
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Oversized allocation (>4KB, falls back to malloc)
// ---------------------------------------------------------------------------
TEST_F(MemStdAllocatorTest, Oversized) {
    MemStdAllocator<int> alloc(ActorId{1});

    // Each int is 4 bytes, 4097 * 4 = 16388 > 4096 -> malloc fallback
    static constexpr size_t kBig = 4097;
    int* p = alloc.allocate(kBig);
    ASSERT_NE(p, nullptr);
    for (size_t i = 0; i < kBig; ++i) {
        p[i] = static_cast<int>(i);
    }
    alloc.deallocate(p, kBig);
}

// ---------------------------------------------------------------------------
// Trait verification (compile-time)
// ---------------------------------------------------------------------------
TEST(MemStdAllocatorCompileTimeTest, Traits) {
    using Alloc = MemStdAllocator<int>;
    using Traits = std::allocator_traits<Alloc>;

    // is_always_equal is false
    static_assert(!Traits::is_always_equal::value);

    // propagate traits are all true
    static_assert(Traits::propagate_on_container_copy_assignment::value);
    static_assert(Traits::propagate_on_container_move_assignment::value);
    static_assert(Traits::propagate_on_container_swap::value);

    // Pointer types default correctly
    static_assert(std::is_same_v<Traits::pointer, int*>);
    static_assert(std::is_same_v<Traits::const_pointer, const int*>);
}
