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

#include <csignal>
#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

// ---------------------------------------------------------------------------
// Guard page utilities for fat blocks (>4KB)
// ---------------------------------------------------------------------------

/// \brief Return the system page size (cached after first call).
///
/// \return The page size in bytes (e.g. 4096 on x86-64, 16384 on Apple
/// Silicon).
size_t page_size() noexcept;

/// \brief Allocate a block with \c PROT_NONE guard pages at both ends.
///
/// Returns a pointer to usable memory positioned after the leading guard page.
/// On overflow or underflow the guard page triggers \c SIGSEGV instead of
/// silent memory corruption.
///
/// \param[in] user_bytes Number of usable bytes requested.
/// \return Pointer to the usable memory region, or \c nullptr on failure.
/// \note Caller must free via \c guarded_free().
void* guarded_alloc(size_t user_bytes) noexcept;

/// \brief Free a block previously allocated by \c guarded_alloc().
///
/// \param[in] user_ptr Pointer returned by \c guarded_alloc().
/// \param[in] user_bytes The same size passed to \c guarded_alloc().
void guarded_free(void* user_ptr, size_t user_bytes) noexcept;

// ---------------------------------------------------------------------------
// SIGSEGV handler for memory corruption detection
// ---------------------------------------------------------------------------

/// \brief Install the corruption signal handler for \c SIGSEGV / \c SIGBUS.
///
/// Chains to any previously installed handler. When a guard page is hit, the
/// handler identifies the owning actor via \c SegmentProvider::lookup() on the
/// fault address and records a corruption event rather than crashing the
/// process.
///
/// \note The handler uses only \c write() for logging — never the async-unsafe
///       logger — to avoid CAS-atomics deadlock in signal context.
void install_corruption_handler() noexcept;

/// \brief Set a pre-opened file descriptor for signal-safe logging of guard
/// page violations.
///
/// \param[in] fd File descriptor opened for writing (e.g. via \c open()).
void set_guard_page_log_fd(int fd) noexcept;

/// \brief Restore the previous signal handler, removing the corruption handler.
void remove_corruption_handler() noexcept;

} // namespace hpactor::mem
