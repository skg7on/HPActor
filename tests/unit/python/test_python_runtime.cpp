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

#include <hpactor/python/python_runtime.hpp>

#include <memory>

using namespace hpactor;

namespace {
struct WakeProbe {
    size_t calls{0};
    static bool wake(void* context) noexcept {
        ++static_cast<WakeProbe*>(context)->calls;
        return true;
    }
};
} // namespace

TEST(PythonRuntimeTest, LifecycleIsExplicitAndStopIsIdempotent) {
    auto created = python::PythonRuntime::create({});
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Created);

    WakeProbe probe;
    ASSERT_TRUE(runtime->start({&probe, &WakeProbe::wake}).ok());
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Running);
    EXPECT_GE(runtime->dispatch_read_fd(), 0);
    EXPECT_GE(runtime->completion_read_fd(), 0);
    runtime->begin_draining();
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Draining);
    EXPECT_TRUE(runtime->stop().ok());
    EXPECT_TRUE(runtime->stop().ok());
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Stopped);
    EXPECT_EQ(runtime->dispatch_read_fd(), -1);
    EXPECT_EQ(runtime->completion_read_fd(), -1);
}

TEST(PythonRuntimeTest, ActorLeasesAreBoundedAndGenerational) {
    python::PythonRuntimeConfig cfg;
    cfg.max_actor_bindings = 1;
    auto created = python::PythonRuntime::create(cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());

    auto first = runtime->reserve_actor();
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(runtime->reserve_actor().has_value());
    ASSERT_TRUE(first->bind(ActorId{42}));
    const uint64_t first_generation = first->generation();
    EXPECT_TRUE(runtime->generation_matches(ActorId{42}, first_generation));
    first.reset();

    auto replacement = runtime->reserve_actor();
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(replacement->bind(ActorId{42}));
    EXPECT_GT(replacement->generation(), first_generation);
    EXPECT_FALSE(runtime->generation_matches(ActorId{42}, first_generation));
}

TEST(PythonRuntimeTest, RejectsCompletionForReplacedGeneration) {
    python::PythonRuntimeConfig cfg;
    cfg.max_actor_bindings = 1;
    auto created = python::PythonRuntime::create(cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());
    WakeProbe probe;
    ASSERT_TRUE(runtime->start({&probe, &WakeProbe::wake}).ok());

    auto old_lease = runtime->reserve_actor();
    ASSERT_TRUE(old_lease.has_value());
    ASSERT_TRUE(old_lease->bind(ActorId{42}));
    const uint64_t old_generation = old_lease->generation();
    old_lease->reset();

    auto replacement = runtime->reserve_actor();
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(replacement->bind(ActorId{42}));
    auto stale = std::make_shared<python::PythonCompletion>();
    stale->actor.id = ActorId{42};
    stale->generation = old_generation;
    EXPECT_FALSE(runtime->try_push_completion(stale));
    EXPECT_EQ(runtime->snapshot().stale_completion_rejected, 1u);
    EXPECT_EQ(runtime->snapshot().queues.completion_depth, 0u);
    EXPECT_TRUE(runtime->stop().ok());
}
