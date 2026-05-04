// Smoke test for MemStdAllocator — std::allocator-compatible adapter
// for the HPActor slab allocator.

#include <hpactor/mem/std_allocator.hpp>

#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace hpactor;
using namespace hpactor::mem;

// ---------------------------------------------------------------------------
// Basic allocation / deallocation
// ---------------------------------------------------------------------------
static void test_basic_alloc() {
    MemStdAllocator<int> alloc(ActorId{42}, RegionType::kActor);

    int* p = alloc.allocate(10);
    assert(p != nullptr);
    for (int i = 0; i < 10; ++i) {
        p[i] = i * 100;
    }
    for (int i = 0; i < 10; ++i) {
        assert(p[i] == i * 100);
    }
    alloc.deallocate(p, 10);
}

// ---------------------------------------------------------------------------
// Default-constructed allocator
// ---------------------------------------------------------------------------
static void test_default_ctor() {
    MemStdAllocator<double> alloc;
    assert(alloc.owner() == ActorId{});
    assert(alloc.region() == RegionType::kActor);

    double* p = alloc.allocate(3);
    p[0] = 1.0;
    p[1] = 2.0;
    p[2] = 3.0;
    alloc.deallocate(p, 3);
}

// ---------------------------------------------------------------------------
// Rebind via conversion constructor
// ---------------------------------------------------------------------------
static void test_rebind() {
    MemStdAllocator<int> int_alloc(ActorId{7});
    MemStdAllocator<char> char_alloc(int_alloc);

    assert(char_alloc.owner() == ActorId{7});
    assert(char_alloc.region() == RegionType::kActor);

    char* p = char_alloc.allocate(32);
    std::memset(p, 'X', 32);
    char_alloc.deallocate(p, 32);
}

// ---------------------------------------------------------------------------
// Equality semantics
// ---------------------------------------------------------------------------
static void test_equality() {
    MemStdAllocator<int> a1(ActorId{1});
    MemStdAllocator<int> a2(ActorId{1});
    MemStdAllocator<int> a3(ActorId{2});

    assert(a1 == a2);
    assert(!(a1 != a2));
    assert(a1 != a3);
    assert(!(a1 == a3));

    // Rebound allocator compares equal to source
    MemStdAllocator<char> a4(a1);
    assert(a1 == a4);
}

// ---------------------------------------------------------------------------
// std::vector integration
// ---------------------------------------------------------------------------
static void test_vector() {
    using Vec = std::vector<int, MemStdAllocator<int>>;

    Vec v(MemStdAllocator<int>(ActorId{99}));
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    assert(v.size() == 3);
    assert(v[0] == 10);
    assert(v[1] == 20);
    assert(v[2] == 30);
}

// ---------------------------------------------------------------------------
// std::deque integration
// ---------------------------------------------------------------------------
static void test_deque() {
    using Deq = std::deque<int, MemStdAllocator<int>>;
    Deq d(MemStdAllocator<int>(ActorId{100}));
    d.push_back(1);
    d.push_front(0);
    d.push_back(2);
    assert(d.size() == 3);
}

// ---------------------------------------------------------------------------
// std::set integration (node-based container)
// ---------------------------------------------------------------------------
static void test_set() {
    using Set = std::set<int, std::less<>, MemStdAllocator<int>>;
    Set s(MemStdAllocator<int>(ActorId{101}));
    s.insert(3);
    s.insert(1);
    s.insert(2);
    assert(s.size() == 3);
}

// ---------------------------------------------------------------------------
// std::map integration (node-based container)
// ---------------------------------------------------------------------------
static void test_map() {
    using Map = std::map<int, std::string, std::less<>,
                         MemStdAllocator<std::pair<const int, std::string>>>;
    Map m(MemStdAllocator<std::pair<const int, std::string>>(ActorId{102}));
    m[1] = "one";
    m[2] = "two";
    assert(m.size() == 2);
    assert(m[1] == "one");
}

// ---------------------------------------------------------------------------
// std::unordered_map integration
// ---------------------------------------------------------------------------
static void test_unordered_map() {
    using UMap = std::unordered_map<int, int, std::hash<int>, std::equal_to<>,
                                    MemStdAllocator<std::pair<const int, int>>>;
    UMap um(MemStdAllocator<std::pair<const int, int>>(ActorId{103}));
    um[1] = 100;
    um[2] = 200;
    assert(um.size() == 2);
}

// ---------------------------------------------------------------------------
// Vector copy / move with allocator propagation
// ---------------------------------------------------------------------------
static void test_vector_propagation() {
    using Vec = std::vector<int, MemStdAllocator<int>>;

    Vec v1(MemStdAllocator<int>(ActorId{200}));
    v1.push_back(42);

    // Copy: allocator propagates, new vector uses v1's allocator
    Vec v2(v1);
    assert(v2.size() == 1);
    assert(v2[0] == 42);
    assert(v2.get_allocator() == v1.get_allocator());

    // Move: allocator propagates
    Vec v3(std::move(v1));
    assert(v3.size() == 1);
    assert(v3[0] == 42);
}

// ---------------------------------------------------------------------------
// Zero-size allocation
// ---------------------------------------------------------------------------
static void test_zero_alloc() {
    MemStdAllocator<int> alloc(ActorId{1});
    int* p = alloc.allocate(0);
    // C++20 allows nullptr for zero-size allocation
    alloc.deallocate(p, 0); // should be no-op
}

// ---------------------------------------------------------------------------
// void* deallocation (nullptr)
// ---------------------------------------------------------------------------
static void test_null_dealloc() {
    MemStdAllocator<int> alloc(ActorId{1});
    alloc.deallocate(nullptr, 10); // should be no-op
}

// ---------------------------------------------------------------------------
// Oversized allocation (>4KB, falls back to malloc)
// ---------------------------------------------------------------------------
static void test_oversized() {
    MemStdAllocator<int> alloc(ActorId{1});

    // Each int is 4 bytes, 4097 * 4 = 16388 > 4096 → malloc fallback
    static constexpr size_t kBig = 4097;
    int* p = alloc.allocate(kBig);
    assert(p != nullptr);
    for (size_t i = 0; i < kBig; ++i) {
        p[i] = static_cast<int>(i);
    }
    alloc.deallocate(p, kBig);
}

// ---------------------------------------------------------------------------
// Trait verification (compile-time)
// ---------------------------------------------------------------------------
static void test_traits() {
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

// ---------------------------------------------------------------------------
int main() {
    // Set up a minimal TLA so allocate_bytes works
    ThreadLocalAllocator tla;
    set_thread_allocator(&tla);

    test_basic_alloc();
    test_default_ctor();
    test_rebind();
    test_equality();
    test_vector();
    test_deque();
    test_set();
    test_map();
    test_unordered_map();
    test_vector_propagation();
    test_zero_alloc();
    test_null_dealloc();
    test_oversized();
    test_traits();

    std::printf("MemStdAllocator smoke test PASSED\n");
    return 0;
}
