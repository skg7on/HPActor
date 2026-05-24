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

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pager.hpp>

namespace hpactor {
namespace cli {

Pager::Pager(uint32_t page_size) : page_size_(page_size) {}

bool Pager::show_page(uint32_t total_items,
                      std::function<void(uint32_t offset, uint32_t limit)> render,
                      OutputFormatter* output) {
    total_items_ = total_items;
    uint32_t start = current_offset_;
    uint32_t end = std::min(start + page_size_, total_items_);

    render(start, end - start);

    char buf[128];
    int n = snprintf(buf, sizeof(buf), "Page %u of %u (%u-%u of %u)",
                     current_page(), total_pages(), start + 1, end, total_items_);
    output->raw(std::string(buf, static_cast<size_t>(n)));
    output->raw("[n]ext, [p]rev, [q]uit, /search, g<num>");

    return end < total_items_;
}

uint32_t Pager::total_pages() const {
    if (total_items_ == 0)
        return 1;
    return (total_items_ + page_size_ - 1) / page_size_;
}

void Pager::goto_page(uint32_t page) {
    if (total_items_ > 0) {
        page = std::max(page, 1u);
        page = std::min(page, total_pages());
    }
    current_offset_ = (page - 1) * page_size_;
}

void Pager::next_page() {
    if (current_offset_ + page_size_ < total_items_) {
        current_offset_ += page_size_;
    }
}

void Pager::prev_page() {
    if (current_offset_ >= page_size_) {
        current_offset_ -= page_size_;
    }
}

Pager::Action Pager::parse_input(const std::string& input, std::string& arg) {
    if (input.empty())
        return Action::Next;
    if (input == "n" || input == "next")
        return Action::Next;
    if (input == "p" || input == "prev")
        return Action::Previous;
    if (input == "q" || input == "quit")
        return Action::Quit;
    if (input == "f" || input == "first") {
        goto_page(1);
        return Action::Goto;
    }
    if (input == "l" || input == "last") {
        goto_page(total_pages());
        return Action::Goto;
    }
    if (!input.empty() && input[0] == '/') {
        arg = input.substr(1);
        return Action::Search;
    }
    if (!input.empty() && input[0] == 'g') {
        arg = input.substr(1);
        uint32_t page = 0;
        auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), page);
        if (ec == std::errc{})
            goto_page(page);
        return Action::Goto;
    }
    return Action::Unknown;
}

} // namespace cli
} // namespace hpactor