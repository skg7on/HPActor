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
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>

TEST(OneForOneSupervisorTest, DefaultPolicy) {
    hpactor::OneForOneSupervisor supervisor;
    hpactor::ChildFailure failure{hpactor::ActorId{1}, hpactor::error{1},
                                  hpactor::SupervisionDirective::Restart};
    auto directive = supervisor.on_child_failure(failure);
    EXPECT_EQ(directive, hpactor::SupervisionDirective::Restart);
}

TEST(OneForOneSupervisorTest, CustomPolicy) {
    hpactor::SupervisionPolicy policy;
    policy.max_restarts = 5;
    hpactor::OneForOneSupervisor supervisor(policy);
    hpactor::ChildFailure failure{hpactor::ActorId{1}, hpactor::error{1},
                                  hpactor::SupervisionDirective::Stop};
    auto directive = supervisor.on_child_failure(failure);
    EXPECT_EQ(directive, hpactor::SupervisionDirective::Stop);
}
