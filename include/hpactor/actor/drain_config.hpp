// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <hpactor/actor/drain_policy.hpp>

namespace hpactor {

struct DrainConfig {
    DrainPolicy policy{DrainPolicy::Drain};
    std::chrono::milliseconds timeout{30'000};
};

} // namespace hpactor
