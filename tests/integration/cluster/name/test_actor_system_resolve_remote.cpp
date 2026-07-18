// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

namespace hpactor::cluster::name {
namespace {

/// \brief Integration test for NameResolver fallback in
/// ActorSystem::resolve_actor().
///
/// (Full test implementation deferred to Task 14 — integration tests.)
TEST(ActorSystemResolveRemote, ResolveFallsThroughToNameResolver) {
    // Setup: create two ActorSystems on different ports, with NameResolver
    // configured. Register actor on system-1, resolve on system-2.
    // Verify system-2 gets an ActorRef whose is_local() is false.
    SUCCEED();
}

} // namespace
} // namespace hpactor::cluster::name
