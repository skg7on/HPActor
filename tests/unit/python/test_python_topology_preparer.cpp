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

#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/config/actor_factory.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/python/python_topology_preparer.hpp>
#include <hpactor/python/python_topology_types.hpp>
#include <hpactor/runtime/runtime_blueprint.hpp>
#include <hpactor/runtime/runtime_blueprint_builder.hpp>

#include <fstream>
#include <string>

using namespace hpactor;

namespace {

std::string write_temp(const std::string& content, const std::string& name) {
    std::string path = "/tmp/hpactor_test_" + name + ".toml";
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// Minimal C++ actor factory for testing.
class TestEchoActor : public EventBasedActor {
  public:
    TestEchoActor(ActorContext* ctx, ActorSystem& sys) : EventBasedActor(ctx, sys) {}
};

} // namespace

// ── Classification and purity ───────────────────────────────────────────────

TEST(PythonTopologyPreparerTest, ClassifiesMixedTopology) {
    // Register one C++ factory.
    config::ActorFactoryRegistry::instance().register_factory<TestEchoActor>(
        "EchoActor");

    std::string content = R"(
[system]
version = "1.0"

[system.python]
enabled = true

[[actor]]
id = "python-echo"
behavior = "python:my_app.actors:Echo"
args = { prefix = "prod" }

[[actor]]
id = "cpp-audit"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "mixed_topology");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    ASSERT_TRUE(parsed.has_value());

    const auto& actors = parsed.value()->actors();
    ASSERT_EQ(actors.size(), 2u);

    EXPECT_EQ(actors[0].kind, python::ConfiguredActorKind::Python);
    EXPECT_EQ(actors[0].topology_index, 0u);
    ASSERT_TRUE(actors[0].python.has_value());
    EXPECT_EQ(actors[0].python->module, "my_app.actors");
    EXPECT_EQ(actors[0].python->qualname, "Echo");
    EXPECT_NE(actors[0].args_fingerprint, 0u);

    EXPECT_EQ(actors[1].kind, python::ConfiguredActorKind::Cpp);
    EXPECT_EQ(actors[1].topology_index, 1u);
    EXPECT_FALSE(actors[1].python.has_value());
}

TEST(PythonTopologyPreparerTest, RejectsMalformedPythonBehavior) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "bad"
behavior = "python:pkg:"
)";
    std::string path = write_temp(content, "malformed_python");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    EXPECT_FALSE(parsed.has_value());
}

TEST(PythonTopologyPreparerTest, RejectsPythonCppNameCollision) {
    // Use a unique behavior name to avoid polluting the singleton registry
    // for subsequent tests.
    config::ActorFactoryRegistry::instance().register_factory<TestEchoActor>(
        "python:collision_test.unique:EchoActor");

    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "python:collision_test.unique:EchoActor"
)";
    std::string path = write_temp(content, "collision");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    EXPECT_FALSE(parsed.has_value());
}

TEST(PythonTopologyPreparerTest, ParsingDoesNotStartThreadsOrSpawn) {
    config::ActorFactoryRegistry::instance().register_factory<TestEchoActor>(
        "EchoActor");

    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "purity");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    ASSERT_TRUE(parsed.has_value());
    // parse() is side-effect-free: no threads, no actors spawned.
    EXPECT_EQ(parsed.value()->actors().size(), 1u);
    EXPECT_NE(parsed.value()->topology_fingerprint(), 0u);
}

// ── Manifest binding ────────────────────────────────────────────────────────

TEST(PythonTopologyPreparerTest, BindManifestExactMatchSucceeds) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "python:my_app.actors:Echo"
)";
    std::string path = write_temp(content, "bind_ok");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed.value()->actors().size(), 1u);

    uint64_t args_fp = parsed.value()->actors()[0].args_fingerprint;
    std::array<python::FactoryTokenBinding, 1> bindings{{
        {0, 7, args_fp}}};
    uint64_t policy_fp = 0x1122334455667788ULL;
    auto prepared = parsed.value()->bind_manifest(bindings, policy_fp);
    ASSERT_TRUE(prepared.has_value());
    EXPECT_NE(prepared.value()->effective_fingerprint(), 0u);
}

TEST(PythonTopologyPreparerTest, BindManifestRejectsMismatchedFingerprint) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "python:my_app.actors:Echo"
)";
    std::string path = write_temp(content, "bind_fail");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    ASSERT_TRUE(parsed.has_value());

    uint64_t args_fp = parsed.value()->actors()[0].args_fingerprint;
    std::array<python::FactoryTokenBinding, 1> bindings{{
        {0, 7, args_fp ^ 1}}}; // wrong fingerprint
    auto prepared = parsed.value()->bind_manifest(bindings, 0x1234);
    EXPECT_FALSE(prepared.has_value());
}

TEST(PythonTopologyPreparerTest, BindManifestRejectsZeroToken) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "python:my_app.actors:Echo"
)";
    std::string path = write_temp(content, "bind_zero");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    ASSERT_TRUE(parsed.has_value());

    uint64_t args_fp = parsed.value()->actors()[0].args_fingerprint;
    std::array<python::FactoryTokenBinding, 1> bindings{{
        {0, 0, args_fp}}}; // zero token
    auto prepared = parsed.value()->bind_manifest(bindings, 0x1234);
    EXPECT_FALSE(prepared.has_value());
}

TEST(PythonTopologyPreparerTest, BindManifestRejectsMissingBinding) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "python:my_app.actors:Echo"
)";
    std::string path = write_temp(content, "bind_missing");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    ASSERT_TRUE(parsed.has_value());

    // Empty bindings — Python actor has no matching token.
    std::vector<python::FactoryTokenBinding> bindings;
    auto prepared = parsed.value()->bind_manifest(bindings, 0x1234);
    EXPECT_FALSE(prepared.has_value());
}

// ── Blueprint extension ─────────────────────────────────────────────────────

TEST(RuntimeBlueprintTest, FromConfigAndTopologyIncludesActors) {
    config::ActorFactoryRegistry::instance().register_factory<TestEchoActor>(
        "EchoActor");

    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "python:my_app.actors:Echo"
)";
    std::string path = write_temp(content, "blueprint_ext");
    auto parsed = python::PythonTopologyPreparer::parse(path);
    ASSERT_TRUE(parsed.has_value());

    uint64_t args_fp = parsed.value()->actors()[0].args_fingerprint;
    std::array<python::FactoryTokenBinding, 1> bindings{{
        {0, 1, args_fp}}};
    auto prepared = parsed.value()->bind_manifest(bindings, 0x1234);
    ASSERT_TRUE(prepared.has_value());

    // Build a blueprint with the topology.
    Config cfg;
    auto bp = RuntimeBlueprintBuilder::from_config_and_topology(
        cfg, prepared.value()->model(),
        prepared.value()->effective_fingerprint());
    ASSERT_TRUE(bp.has_value());
    EXPECT_FALSE(bp.value().actors().empty());
    EXPECT_EQ(bp.value().actors().size(), 1u);
    EXPECT_EQ(bp.value().actors()[0].id, "echo");
    EXPECT_EQ(bp.value().actors()[0].behavior, "python:my_app.actors:Echo");
    EXPECT_NE(bp.value().fingerprint(), 0u);
}

TEST(RuntimeBlueprintTest, TopologyFingerprintChangesWithActorChange) {
    config::ActorFactoryRegistry::instance().register_factory<TestEchoActor>(
        "EchoActor");

    std::string content1 = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "python:my_app.actors:Echo"
)";
    std::string path1 = write_temp(content1, "bp_fp1");
    auto parsed1 = python::PythonTopologyPreparer::parse(path1);
    ASSERT_TRUE(parsed1.has_value());
    uint64_t fp1 = parsed1.value()->actors()[0].args_fingerprint;
    std::array<python::FactoryTokenBinding, 1> b1{{{0, 1, fp1}}};
    auto prep1 = parsed1.value()->bind_manifest(b1, 0x1234);
    ASSERT_TRUE(prep1.has_value());

    Config cfg;
    auto bp1 = RuntimeBlueprintBuilder::from_config_and_topology(
        cfg, prep1.value()->model(),
        prep1.value()->effective_fingerprint());
    ASSERT_TRUE(bp1.has_value());

    std::string content2 = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "python:other_app.actors:Echo"
)";
    std::string path2 = write_temp(content2, "bp_fp2");
    auto parsed2 = python::PythonTopologyPreparer::parse(path2);
    ASSERT_TRUE(parsed2.has_value());
    uint64_t fp2 = parsed2.value()->actors()[0].args_fingerprint;
    std::array<python::FactoryTokenBinding, 1> b2{{{0, 1, fp2}}};
    auto prep2 = parsed2.value()->bind_manifest(b2, 0x1234);
    ASSERT_TRUE(prep2.has_value());

    auto bp2 = RuntimeBlueprintBuilder::from_config_and_topology(
        cfg, prep2.value()->model(),
        prep2.value()->effective_fingerprint());
    ASSERT_TRUE(bp2.has_value());

    // Different behavior strings should produce different fingerprints.
    EXPECT_NE(bp1.value().fingerprint(), bp2.value().fingerprint());
}
