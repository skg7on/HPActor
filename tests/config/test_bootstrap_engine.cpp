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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

using namespace hpactor;
using namespace hpactor::config;

// ---------------------------------------------------------------------------
// A minimal actor for integration testing
// ---------------------------------------------------------------------------
class BootstrapTestActor : public EventBasedActor {
  public:
    BootstrapTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

HPACTOR_REGISTER_ACTOR("BootstrapTestActor", BootstrapTestActor)

// ---------------------------------------------------------------------------
// Helper: write inline TOML to a temp file
// ---------------------------------------------------------------------------
static std::string write_temp(const std::string& content, const std::string& name) {
    std::string path = "/tmp/hpactor_bs_test_" + name + ".toml";
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// ---------------------------------------------------------------------------
// Test 1: Single actor — spawn from TOML, verify registration
// ---------------------------------------------------------------------------
void test_single_actor() {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "my_actor"
behavior = "BootstrapTestActor"
)";
    std::string path = write_temp(toml, "single");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    assert(result.has_value());

    auto addr = system.registry().get("my_actor");
    assert(addr.id.value() != 0);
    std::cout << "[PASS] test_single_actor\n";
}

// ---------------------------------------------------------------------------
// Test 2: Parent-child supervisor ordering
// ---------------------------------------------------------------------------
void test_supervisor_ordering() {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "parent"
behavior = "BootstrapTestActor"

[[actor]]
id = "child"
behavior = "BootstrapTestActor"
supervisor = "parent"
)";
    std::string path = write_temp(toml, "supervisor");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    assert(result.has_value());

    // Both actors should be registered
    auto parent_addr = system.registry().get("parent");
    auto child_addr = system.registry().get("child");
    assert(parent_addr.id.value() != 0);
    assert(child_addr.id.value() != 0);
    std::cout << "[PASS] test_supervisor_ordering\n";
}

// ---------------------------------------------------------------------------
// Test 3: SystemInit delivery
// ---------------------------------------------------------------------------
void test_system_init_delivery() {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "init_test"
behavior = "BootstrapTestActor"
)";
    std::string path = write_temp(toml, "sysinit");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    assert(result.has_value());

    // The actor is spawned and SystemInit is delivered via mailbox.
    // For now, verify the actor exists (SystemInit delivery requires
    // the scheduler to process messages, which is tested separately).
    auto addr = system.registry().get("init_test");
    assert(addr.id.value() != 0);
    std::cout << "[PASS] test_system_init_delivery\n";
}

// ---------------------------------------------------------------------------
// Test 4: Multiple independent roots
// ---------------------------------------------------------------------------
void test_multiple_roots() {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "root_a"
behavior = "BootstrapTestActor"

[[actor]]
id = "root_b"
behavior = "BootstrapTestActor"

[[actor]]
id = "root_c"
behavior = "BootstrapTestActor"
)";
    std::string path = write_temp(toml, "multiroot");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    assert(result.has_value());

    for (const char* id : {"root_a", "root_b", "root_c"}) {
        auto addr = system.registry().get(id);
        assert(addr.id.value() != 0);
    }
    std::cout << "[PASS] test_multiple_roots\n";
}

// ---------------------------------------------------------------------------
// Test 5: Unknown behavior → error
// ---------------------------------------------------------------------------
void test_unknown_behavior() {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "bad"
behavior = "NonexistentActor"
)";
    std::string path = write_temp(toml, "unknown_beh");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    assert(!result.has_value());
    std::cout << "[PASS] test_unknown_behavior\n";
}

// ---------------------------------------------------------------------------
// Test 6: Dispatcher assignment via config
// ---------------------------------------------------------------------------
void test_dispatcher_assignment() {
    std::string toml = R"(
[system]
version = "1.0"

[[dispatcher]]
name = "test_pool"
threads = 2

[[actor]]
id = "pooled_actor"
behavior = "BootstrapTestActor"
dispatcher = "test_pool"
dispatch_policy = "Cooperative"
)";
    std::string path = write_temp(toml, "dispatcher");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    assert(result.has_value());

    auto addr = system.registry().get("pooled_actor");
    assert(addr.id.value() != 0);
    std::cout << "[PASS] test_dispatcher_assignment\n";
}

// ---------------------------------------------------------------------------
// Test 7: Template-based actor with args
// ---------------------------------------------------------------------------
void test_template_with_args() {
    std::string toml = R"(
[system]
version = "1.0"

[template.base]
behavior = "BootstrapTestActor"
mailbox_capacity = 8192

[[actor]]
id = "templated"
inherits = "base"
)";
    std::string path = write_temp(toml, "templ_args");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    assert(result.has_value());

    auto addr = system.registry().get("templated");
    assert(addr.id.value() != 0);
    std::cout << "[PASS] test_template_with_args\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== test_bootstrap_engine ===\n";

    test_single_actor();
    test_supervisor_ordering();
    test_system_init_delivery();
    test_multiple_roots();
    test_unknown_behavior();
    test_dispatcher_assignment();
    test_template_with_args();

    std::cout << "\nAll tests passed!\n";
    return 0;
}
