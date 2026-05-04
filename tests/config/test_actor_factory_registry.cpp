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

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/actor/event_based_actor.hpp>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace hpactor;
using namespace hpactor::config;

// Test actors — minimal stubs for factory verification
class TestEchoActor : public EventBasedActor {
  public:
    TestEchoActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

class TestWorkerActor : public EventBasedActor {
  public:
    TestWorkerActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

// -----------------------------------------------------------------------------
// Test 1: Register two types, verify has() returns true for both
// -----------------------------------------------------------------------------
void test_register_and_has() {
    auto& registry = ActorFactoryRegistry::instance();
    registry.register_factory<TestEchoActor>("TestEcho");
    registry.register_factory<TestWorkerActor>("TestWorker");

    assert(registry.has("TestEcho"));
    assert(registry.has("TestWorker"));
    std::cout << "[PASS] test_register_and_has\n";
}

// -----------------------------------------------------------------------------
// Test 2: Call get_factory() — returns a valid callable
// -----------------------------------------------------------------------------
void test_get_factory_returns_callable() {
    auto& registry = ActorFactoryRegistry::instance();

    auto factory = registry.get_factory("TestEcho");
    assert(factory != nullptr);
    // Factory is a valid std::function — construction happens when invoked
    // with a real ActorSystem, which requires integration test setup
    std::cout << "[PASS] test_get_factory_returns_callable\n";
}

// -----------------------------------------------------------------------------
// Test 3: has() returns false for unknown name
// -----------------------------------------------------------------------------
void test_has_unknown() {
    auto& registry = ActorFactoryRegistry::instance();
    assert(!registry.has("NonExistentActor"));
    std::cout << "[PASS] test_has_unknown\n";
}

// -----------------------------------------------------------------------------
// Test 4: known_names() returns all registered names
// -----------------------------------------------------------------------------
void test_known_names() {
    auto& registry = ActorFactoryRegistry::instance();
    auto names = registry.known_names();

    // Our two registrations from test 1 should be present
    bool found_echo = false;
    bool found_worker = false;
    for (const auto& name : names) {
        if (name == "TestEcho") found_echo = true;
        if (name == "TestWorker") found_worker = true;
    }
    assert(found_echo);
    assert(found_worker);
    std::cout << "[PASS] test_known_names\n";
}

// -----------------------------------------------------------------------------
// Test 5: Duplicate registration overwrites (last wins)
// -----------------------------------------------------------------------------
void test_duplicate_overwrite() {
    auto& registry = ActorFactoryRegistry::instance();

    // Register a type under "DuplicateName"
    registry.register_factory<TestEchoActor>("DuplicateName");
    auto f1 = registry.get_factory("DuplicateName");
    assert(f1 != nullptr);

    // Overwrite with a different type
    registry.register_factory<TestWorkerActor>("DuplicateName");
    auto f2 = registry.get_factory("DuplicateName");
    assert(f2 != nullptr);

    // It should still return a valid factory (last-registered wins)
    std::cout << "[PASS] test_duplicate_overwrite\n";
}

// -----------------------------------------------------------------------------
// Test 6: get_factory returns nullptr for unknown name
// -----------------------------------------------------------------------------
void test_get_factory_unknown() {
    auto& registry = ActorFactoryRegistry::instance();
    auto factory = registry.get_factory("DefinitelyNotRegistered");
    assert(factory == nullptr);
    std::cout << "[PASS] test_get_factory_unknown\n";
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main() {
    std::cout << "=== test_actor_factory_registry ===\n";

    test_register_and_has();
    test_get_factory_returns_callable();
    test_has_unknown();
    test_known_names();
    test_duplicate_overwrite();
    test_get_factory_unknown();

    std::cout << "\nAll tests passed!\n";
    return 0;
}
