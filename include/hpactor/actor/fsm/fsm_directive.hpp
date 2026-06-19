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

#include <cstdint>
#include <utility>

namespace hpactor {

/// \brief Return type from FSM message handlers, controlling state transitions.
///
/// Handlers registered via \c StateBuilder::on<T>() or
/// \c StateBuilder::on_raw() return an \c FsmDirective that instructs the
/// FSM runtime how to proceed:
/// - \c Stay: remain in the current state, optionally updating data.
/// - \c GoTo: transition to a new state, optionally with new data.
/// - \c Stop: terminate the FSM (behavior becomes no-op).
///
/// \tparam S The state enum type (must be copy/move constructible and
///           equality-comparable).
/// \tparam D The state data type (must be copy/move constructible).
template <typename S, typename D> struct FsmDirective {
    /// \brief The kind of transition to perform.
    enum class Kind : uint8_t {
        Stay, ///< Remain in the current state.
        GoTo, ///< Transition to \c target_state.
        Stop  ///< Terminate the FSM.
    };

    Kind kind{Kind::Stay};
    S target_state{};        ///< Valid for \c GoTo; the state to enter.
    D target_data{};         ///< Valid for \c GoTo and \c Stay (when
                             ///  \c update_data is true).
    bool update_data{false}; ///< When \c true, replace current data with
                             ///  \c target_data.

    /// \brief Remain in the current state without modifying data.
    ///
    /// Resets the idle timeout for the current state (if configured).
    /// \return A \c Stay directive with no data update.
    static FsmDirective stay() noexcept {
        FsmDirective d;
        d.kind = Kind::Stay;
        return d;
    }

    /// \brief Remain in the current state and replace the state data.
    ///
    /// Resets the idle timeout for the current state (if configured).
    /// \param[in] new_data The new state data to install.
    /// \return A \c Stay directive with \c update_data set.
    static FsmDirective stay(D new_data) {
        FsmDirective d;
        d.kind = Kind::Stay;
        d.target_data = std::move(new_data);
        d.update_data = true;
        return d;
    }

    /// \brief Transition to \p state, keeping the current data.
    ///
    /// Cancels the current state's timeout and schedules the target state's
    /// timeout (if configured).
    /// \param[in] state The target state to transition to.
    /// \return A \c GoTo directive.
    static FsmDirective go_to(S state) noexcept {
        FsmDirective d;
        d.kind = Kind::GoTo;
        d.target_state = std::move(state);
        return d;
    }

    /// \brief Transition to \p state with new data.
    ///
    /// Cancels the current state's timeout and installs \p data.
    /// \param[in] state The target state to transition to.
    /// \param[in] data The new state data.
    /// \return A \c GoTo directive carrying new data.
    static FsmDirective go_to(S state, D data) {
        FsmDirective d;
        d.kind = Kind::GoTo;
        d.target_state = std::move(state);
        d.target_data = std::move(data);
        return d;
    }

    /// \brief Terminate the FSM.
    ///
    /// The behavior becomes a no-op. All subsequent messages are dropped.
    /// \return A \c Stop directive.
    static FsmDirective stop() noexcept {
        FsmDirective d;
        d.kind = Kind::Stop;
        return d;
    }
};

} // namespace hpactor
