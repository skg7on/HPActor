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

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include "native_system.hpp"

namespace py = pybind11;
using namespace hpactor;

namespace {

py::dict make_valid_config() {
    auto cfg = py::dict();
    cfg["dispatch_queue_capacity"] = 65536;
    cfg["command_queue_capacity"] = 16384;
    cfg["completion_queue_capacity"] = 16384;
    cfg["max_actor_bindings"] = 65536;
    cfg["max_dispatch_per_tick"] = 256;
    cfg["max_commands_per_turn"] = 256;
    cfg["loop_lag_unready_ms"] = 5000;
    cfg["handler_shutdown_timeout_ms"] = 10000;
    cfg["trace_handler_spans"] = true;
    return cfg;
}

} // namespace

class Pybind11NativeSystemTest : public ::testing::Test {
  protected:
    void SetUp() override {
        guard_ = std::make_unique<py::scoped_interpreter>();
    }
    void TearDown() override {
        guard_.reset();
    }

  private:
    std::unique_ptr<py::scoped_interpreter> guard_;
};

TEST_F(Pybind11NativeSystemTest, ConfigDictConstructsValidObject) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    // FDs are valid only after start(); before start they are -1
    // Construction itself succeeds with valid config
    EXPECT_TRUE(true);
}

TEST_F(Pybind11NativeSystemTest, StartStopLifecycle) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    EXPECT_TRUE(obj.start());
    EXPECT_TRUE(obj.stop());
    EXPECT_TRUE(obj.stop()); // idempotent
}

TEST_F(Pybind11NativeSystemTest, ApplicationOriginReturnsValidTuple) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    ASSERT_TRUE(obj.start());
    auto origin = obj.application_origin();
    EXPECT_EQ(py::len(origin), 6);
    // (family, packed_address, port, actor_type, actor_id, incarnation)
    // Application bridge is on loopback (127.0.0.1) = IPv4 family
    EXPECT_EQ(origin[py::int_(0)].cast<int>(), 4);
    // actor_id > 0
    EXPECT_GT(origin[py::int_(4)].cast<uint64_t>(), 0);
    EXPECT_TRUE(obj.stop());
}

TEST_F(Pybind11NativeSystemTest, DrainDispatchReturnsEmptyListWhenIdle) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    ASSERT_TRUE(obj.start());
    auto dispatches = obj.drain_dispatch(16);
    EXPECT_EQ(py::len(dispatches), 0);
    EXPECT_TRUE(obj.stop());
}

TEST_F(Pybind11NativeSystemTest, SnapshotReturnsDict) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    ASSERT_TRUE(obj.start());
    auto snap = obj.snapshot();
    EXPECT_TRUE(snap.contains("state"));
    EXPECT_TRUE(snap.contains("dispatch_depth"));
    EXPECT_TRUE(snap.contains("command_depth"));
    EXPECT_TRUE(snap.contains("completion_depth"));
    EXPECT_TRUE(obj.stop());
}

TEST_F(Pybind11NativeSystemTest, GuardConvertsErrorToFalse) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    ASSERT_TRUE(obj.start());
    // Pass invalid address tuple (wrong size) — returns false, not throw
    auto bad_address = py::make_tuple(0);
    bool result = obj.stop_bridge(bad_address);
    EXPECT_FALSE(result);
    EXPECT_NE(PyErr_Occurred(), nullptr);
    PyErr_Clear();
    EXPECT_TRUE(obj.stop());
}
