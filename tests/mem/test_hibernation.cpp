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

#include <hpactor/mem/hibernation_registry.hpp>
#include <hpactor/mem/hibernatable.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <sys/mman.h>

// Example hibernatable actor state
struct TestState {
    uint64_t counter;
    uint64_t values[4];
};

class TestHibernatableActor : public hpactor::mem::Hibernatable {
  public:
    explicit TestHibernatableActor(const TestState& s) : state_(s) {}

    size_t serialized_size() const override { return sizeof(TestState); }

    void serialize_to(std::span<std::byte> buffer) const override {
        std::memcpy(buffer.data(), &state_, sizeof(TestState));
    }

    void deserialize_from(std::span<const std::byte> buffer) override {
        std::memcpy(&state_, buffer.data(), sizeof(TestState));
    }

    TestState& state() { return state_; }

  private:
    TestState state_;
};

int main() {
    using namespace hpactor::mem;

    // Test basic store/load cycle
    {
        auto& reg = HibernationRegistry::instance();
        size_t before = reg.count();

        // Allocate a hibernation buffer via mmap
        size_t buf_size = sizeof(TestState);
        void* buf = mmap(nullptr, buf_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(buf != MAP_FAILED);

        // Create test actor state and serialize
        TestState ts{42, {1, 2, 3, 4}};
        std::memcpy(buf, &ts, sizeof(ts));

        // Store in registry
        HibernationBuffer hb{buf, buf_size, 0, 0};
        hpactor::ActorId aid{100};
        reg.store(aid, hb);
        assert(reg.contains(aid));
        assert(reg.count() == before + 1);
        assert(reg.total_bytes() == buf_size);

        // Load back
        HibernationBuffer loaded = reg.load(aid);
        assert(loaded.ptr == buf);
        assert(loaded.size == buf_size);
        assert(!reg.contains(aid)); // removed after load

        // Deserialize and verify
        TestState restored;
        std::memcpy(&restored, loaded.ptr, sizeof(restored));
        assert(restored.counter == 42);
        assert(restored.values[0] == 1);
        assert(restored.values[3] == 4);

        // Clean up the mmap'd buffer (registry gave up ownership)
        munmap(loaded.ptr, loaded.size);
    }

    // Test Hibernatable interface
    {
        TestState ts{99, {10, 20, 30, 40}};
        TestHibernatableActor actor(ts);

        assert(actor.serialized_size() == sizeof(TestState));

        // Serialize
        std::vector<std::byte> buffer(actor.serialized_size());
        actor.serialize_to(buffer);

        // Modify state
        actor.state().counter = 0;

        // Deserialize
        actor.deserialize_from(buffer);
        assert(actor.state().counter == 99);
        assert(actor.state().values[2] == 30);
    }

    std::cout << "test_hibernation: PASS\n";
    return 0;
}
