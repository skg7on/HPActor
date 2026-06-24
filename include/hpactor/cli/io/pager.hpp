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
#include <functional>
#include <string>

namespace hpactor {
namespace cli {

class OutputFormatter;

/// \brief Interactive paging state machine for multi-page CLI output.
///
/// Manages a current offset and page size. After each page is rendered,
/// the user can navigate (n/p), search, goto a page, or quit.
///
/// \note Thread affinity: called on the CLI daemon thread.
class Pager {
  public:
    /// \brief User actions recognized after a page prompt.
    enum class Action {
        Next,     ///< Advance to the next page.
        Previous, ///< Return to the previous page.
        Quit,     ///< Exit paging mode.
        Search,   ///< Search for text within output.
        Goto,     ///< Jump to a specific page number.
        Unknown,  ///< Unrecognized input.
    };

    /// \brief Construct a pager with the given page size.
    ///
    /// \param[in] page_size Number of items per page.
    explicit Pager(uint32_t page_size);

    /// \brief Render one page of output.
    ///
    /// Calls \p render with the current offset and limit, then writes
    /// the output. Prompts the user and processes navigation commands
    /// if more pages remain.
    ///
    /// \param[in] total_items Total number of items across all pages.
    /// \param[in] render Callback that writes rows for the given
    ///                   [offset, offset+limit) range.
    /// \param[in] output Formatter used to write the output.
    /// \retval true More pages remain after this one.
    /// \retval false This was the last page or the user quit.
    bool show_page(uint32_t total_items,
                   std::function<void(uint32_t offset, uint32_t limit)> render,
                   OutputFormatter* output);

    /// \brief Parse user input after a page prompt into an Action.
    ///
    /// \param[in] input Raw input string from the user.
    /// \param[out] arg Set to the argument for Search/Goto actions.
    /// \return The parsed action.
    Action parse_input(const std::string& input, std::string& arg);

    /// \brief One-based current page number.
    ///
    /// \return The current page number.
    uint32_t current_page() const {
        return current_offset_ / page_size_ + 1;
    }

    /// \brief Total number of pages.
    ///
    /// \return Ceiling of total_items / page_size.
    uint32_t total_pages() const;

    /// \brief Jump to a specific page.
    ///
    /// \param[in] page One-based page number (clamped to valid range).
    void goto_page(uint32_t page);

    /// \brief Advance to the next page, if any.
    void next_page();

    /// \brief Return to the previous page, if any.
    void prev_page();

  private:
    uint32_t page_size_;
    uint32_t current_offset_ = 0;
    uint32_t total_items_ = 0;
};

} // namespace cli
} // namespace hpactor
