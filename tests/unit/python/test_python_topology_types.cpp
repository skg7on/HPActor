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

#include <hpactor/config/python_binding_config.hpp>
#include <hpactor/config/topology_model.hpp>
#include <hpactor/python/python_topology_types.hpp>

using namespace hpactor;

// ── Behavior reference grammar ──────────────────────────────────────────────

TEST(PythonTopologyTypesTest, ParsesApprovedBehaviorReference) {
    auto parsed = python::parse_python_behavior_ref(
        "python:my_app.workers:Workers.Ingest");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().module, "my_app.workers");
    EXPECT_EQ(parsed.value().qualname, "Workers.Ingest");
}

TEST(PythonTopologyTypesTest, ParsesSimpleBehaviorReference) {
    auto parsed = python::parse_python_behavior_ref("python:myapp:Echo");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().module, "myapp");
    EXPECT_EQ(parsed.value().qualname, "Echo");
}

TEST(PythonTopologyTypesTest, RejectsNonPythonPrefix) {
    EXPECT_FALSE(python::parse_python_behavior_ref("worker").has_value());
    EXPECT_FALSE(python::parse_python_behavior_ref("cpp:worker").has_value());
    EXPECT_FALSE(python::parse_python_behavior_ref("").has_value());
}

TEST(PythonTopologyTypesTest, RejectsPathRelativeAndLocalReferences) {
    for (std::string_view value : {
             "python:.actors:Echo",
             "python:/tmp/actors.py:Echo",
             "python:pkg.actors:<locals>.Echo",
             "python:pkg:Echo:Extra",
             "python:pkg-name:Echo",
             "python:pkg:",
             "python::Echo",
         }) {
        EXPECT_FALSE(python::parse_python_behavior_ref(value).has_value())
            << value;
    }
}

TEST(PythonTopologyTypesTest, RejectsBoundaryViolations) {
    // Module > 255 bytes
    std::string long_module(256, 'a');
    auto ref = python::parse_python_behavior_ref(
        "python:" + long_module + ":Echo");
    EXPECT_FALSE(ref.has_value());

    // Qualname > 255 bytes
    std::string long_qualname(256, 'B');
    ref = python::parse_python_behavior_ref(
        "python:pkg:" + long_qualname);
    EXPECT_FALSE(ref.has_value());

    // Total behavior > 518 bytes (519 = rejected)
    std::string mod(255, 'm');
    std::string qn(256, 'q');
    std::string full = "python:" + mod + ":" + qn;
    EXPECT_GT(full.size(), 518u);
    ref = python::parse_python_behavior_ref(full);
    EXPECT_FALSE(ref.has_value());
}

TEST(PythonTopologyTypesTest, AcceptsMaximumBoundaryValues) {
    // 255-byte module, 255-byte qualname, total < 518
    std::string mod(255, 'm');
    std::string qn(255, 'q');
    std::string full = "python:" + mod + ":" + qn;
    // "python:" is 7 bytes + 1 colon = 8 + 255 + 255 = 518 exactly
    EXPECT_EQ(full.size(), 518u);
    auto ref = python::parse_python_behavior_ref(full);
    EXPECT_TRUE(ref.has_value());
}

// ── Actor argument validation ───────────────────────────────────────────────

TEST(PythonTopologyTypesTest, AcceptsValidStringArguments) {
    config::ActorDef def;
    def.id = "echo";
    def.behavior = "python:pkg.actors:Echo";
    def.args["prefix"] = "prod";
    def.args["count"] = "42";
    EXPECT_TRUE(python::validate_python_actor_args(def).has_value());
}

TEST(PythonTopologyTypesTest, RejectsHpactorReservedKeys) {
    config::ActorDef def;
    def.id = "echo";
    def.behavior = "python:pkg.actors:Echo";
    def.args["__hpactor_token"] = "secret";
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());

    def.args.clear();
    def.args["__hpactor_internal"] = "1";
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());
}

TEST(PythonTopologyTypesTest, RejectsNonIdentifierKeys) {
    config::ActorDef def;
    def.id = "echo";
    def.behavior = "python:pkg.actors:Echo";
    def.args["not-valid"] = "value";
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());

    def.args.clear();
    def.args["123numeric"] = "value";
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());

    def.args.clear();
    def.args[""] = "value";
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());
}

TEST(PythonTopologyTypesTest, RejectsKeyAndValueSizeViolations) {
    config::ActorDef def;
    def.id = "echo";
    def.behavior = "python:pkg.actors:Echo";

    // Key > 128 bytes
    def.args[std::string(129, 'k')] = "v";
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());

    // Value > 4096 bytes
    def.args.clear();
    def.args["valid_key"] = std::string(4097, 'v');
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());
}

TEST(PythonTopologyTypesTest, RejectsArgumentCountAndSizeViolations) {
    config::ActorDef def;
    def.id = "echo";
    def.behavior = "python:pkg.actors:Echo";

    // > 128 arguments
    for (int i = 0; i < 129; ++i) {
        def.args["arg_" + std::to_string(i)] = "v";
    }
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());

    // Combined key/value > 64 KiB
    def.args.clear();
    for (int i = 0; i < 80; ++i) {
        // 80 * ~820 bytes = ~64 KiB
        def.args["a" + std::to_string(i)] = std::string(820, 'x');
    }
    EXPECT_FALSE(python::validate_python_actor_args(def).has_value());
}

// ── Argument fingerprint ────────────────────────────────────────────────────

TEST(PythonTopologyTypesTest, FingerprintIsDeterministic) {
    config::ActorDef def;
    def.args["b"] = "2";
    def.args["a"] = "1";
    def.args["c"] = "3";
    uint64_t fp1 = python::fingerprint_python_actor_args(def);
    uint64_t fp2 = python::fingerprint_python_actor_args(def);
    EXPECT_EQ(fp1, fp2);
}

TEST(PythonTopologyTypesTest, FingerprintChangesWithContent) {
    config::ActorDef def1;
    def1.args["a"] = "1";
    config::ActorDef def2;
    def2.args["a"] = "2";
    EXPECT_NE(python::fingerprint_python_actor_args(def1),
              python::fingerprint_python_actor_args(def2));
}

TEST(PythonTopologyTypesTest, FingerprintIsOrderIndependent) {
    config::ActorDef def1;
    def1.args["b"] = "2";
    def1.args["a"] = "1";
    config::ActorDef def2;
    def2.args["a"] = "1";
    def2.args["b"] = "2";
    EXPECT_EQ(python::fingerprint_python_actor_args(def1),
              python::fingerprint_python_actor_args(def2));
}

TEST(PythonTopologyTypesTest, FingerprintEmptyArgsIsNonZero) {
    config::ActorDef def;
    // No args
    uint64_t fp = python::fingerprint_python_actor_args(def);
    EXPECT_NE(fp, 0u);
}

// ── Topology timeout config ─────────────────────────────────────────────────

TEST(PythonBindingConfigTest, TopologyTimeoutDefaults) {
    config::PythonBindingConfig cfg;
    EXPECT_EQ(cfg.topology_start_timeout_ms, 30000u);
}

TEST(PythonBindingConfigTest, TopologyTimeoutValidation) {
    config::PythonBindingConfig cfg;

    // Valid: 100 ms
    cfg.topology_start_timeout_ms = 100;
    EXPECT_TRUE(cfg.validate().ok());

    // Valid: 300,000 ms
    cfg.topology_start_timeout_ms = 300000;
    EXPECT_TRUE(cfg.validate().ok());

    // Invalid: 99 ms (below minimum)
    cfg.topology_start_timeout_ms = 99;
    EXPECT_FALSE(cfg.validate().ok());

    // Invalid: 300,001 ms (above maximum)
    cfg.topology_start_timeout_ms = 300001;
    EXPECT_FALSE(cfg.validate().ok());
}
