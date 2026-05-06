// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <functional>
#include <string>

namespace hpactor {
namespace cli {

class OutputFormatter;

class Pager {
public:
    enum class Action { Next, Previous, Quit, Search, Goto, Unknown };

    Pager(uint32_t page_size);

    // Show one page. Calls render(offset, limit) to get the rows.
    // Returns true if there are more pages.
    bool show_page(uint32_t total_items,
                   std::function<void(uint32_t offset, uint32_t limit)> render,
                   OutputFormatter* output);

    // Parse user input after a page prompt.
    Action parse_input(const std::string& input, std::string& arg);

    uint32_t current_page() const { return current_offset_ / page_size_ + 1; }
    uint32_t total_pages() const;
    void goto_page(uint32_t page);
    void next_page();
    void prev_page();

private:
    uint32_t page_size_;
    uint32_t current_offset_ = 0;
    uint32_t total_items_ = 0;
};

}  // namespace cli
}  // namespace hpactor
