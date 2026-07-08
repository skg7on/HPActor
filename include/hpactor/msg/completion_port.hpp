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

#include <memory>
#include <utility>

namespace hpactor {

/// \brief Fixed function-pointer completion port for move-only values.
///
/// Provides a non-blocking, exception-free alternative to \c std::function
/// for one-shot completion notifications. The port consists of an opaque
/// context pointer, a typed function pointer, and an optional keepalive
/// shared pointer that travels with the port to keep heap-allocated
/// callback state alive through invocation.
///
/// \tparam T The value type passed to the completion callback. Must be
///           move-constructible.
template <typename T> struct CompletionPort final {
    /// \brief Signature of the completion callback.
    ///
    /// \param[in] context The opaque context pointer.
    /// \param[in] value   The result value (moved into the callback).
    using CompleteFn = void (*)(void* context, T value) noexcept;

    /// Opaque context pointer passed as the first argument to \ref complete.
    void* context{nullptr};

    /// Function pointer invoked with the context and the result value.
    CompleteFn complete{nullptr};

    /// Optional keepalive that travels with the port. The callback may
    /// receive a copy so that heap-owned completion state outlives the
    /// bridge or stack frame that created the port.
    std::shared_ptr<void> keepalive;

    /// \brief Check whether this port holds a valid callback.
    ///
    /// \return true if both \ref context and \ref complete are non-null.
    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && complete != nullptr;
    }

    /// \brief Invoke the completion callback with a value.
    ///
    /// The callback receives the \ref context pointer and the moved value.
    /// The \ref keepalive is captured by copy in the calling scope to
    /// ensure it survives until the callback returns; this copy is
    /// destroyed after the call returns.
    ///
    /// \param[in] value The value to pass to the callback.
    void operator()(T value) const noexcept {
        auto keep = keepalive; // hold keepalive across the call
        complete(context, std::move(value));
        (void)keep;
    }
};

} // namespace hpactor
