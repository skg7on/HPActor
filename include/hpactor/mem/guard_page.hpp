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

#pragma once

#include <hpactor/mem/segment_provider.hpp>

#include <cstddef>
#include <cstdint>
#include <csignal>

namespace hpactor::mem {

// ---------------------------------------------------------------------------
// Guard page utilities for fat blocks (>4KB)
// ---------------------------------------------------------------------------

// Returns the system page size (cached).
size_t page_size() noexcept;

// Allocate a block with PROT_NONE guard pages at both ends.
// Returns pointer to usable memory (after leading guard page).
// On overflow/underflow, SIGSEGV fires instead of silent corruption.
// Caller must free via guarded_free().
void* guarded_alloc(size_t user_bytes) noexcept;

// Free a block allocated by guarded_alloc().
void guarded_free(void* user_ptr, size_t user_bytes) noexcept;

// ---------------------------------------------------------------------------
// SIGSEGV handler for memory corruption detection
// ---------------------------------------------------------------------------

// Install the corruption signal handler. Chains to any previous handler.
// When a guard page is hit, the handler identifies the owning actor (via
// SegmentProvider::lookup() on the fault address) and records a corruption
// event rather than crashing the process.
void install_corruption_handler() noexcept;

// Restore the previous signal handler.
void remove_corruption_handler() noexcept;

} // namespace hpactor::mem
