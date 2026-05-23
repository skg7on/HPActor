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
#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/pretty_formatter.hpp>

using namespace hpactor::cli;

TEST(PagerTest, PageCalculation) {
    Pager pager(50);
    EXPECT_EQ(pager.current_page(), 1);
    pager.goto_page(3);
    EXPECT_EQ(pager.current_page(), 3);
}

TEST(PagerTest, Clamp) {
    Pager pager(50);
    pager.goto_page(100);
    EXPECT_GE(pager.current_page(), 1);
}

TEST(PagerTest, ParseInputNext) {
    Pager pager(50);
    std::string arg;
    EXPECT_EQ(pager.parse_input("", arg), Pager::Action::Next);
    EXPECT_EQ(pager.parse_input("n", arg), Pager::Action::Next);
    EXPECT_EQ(pager.parse_input("next", arg), Pager::Action::Next);
}

TEST(PagerTest, ParseInputPrev) {
    Pager pager(50);
    std::string arg;
    EXPECT_EQ(pager.parse_input("p", arg), Pager::Action::Previous);
    EXPECT_EQ(pager.parse_input("prev", arg), Pager::Action::Previous);
}

TEST(PagerTest, ParseInputQuit) {
    Pager pager(50);
    std::string arg;
    EXPECT_EQ(pager.parse_input("q", arg), Pager::Action::Quit);
    EXPECT_EQ(pager.parse_input("quit", arg), Pager::Action::Quit);
}

TEST(PagerTest, ParseInputSearch) {
    Pager pager(50);
    std::string arg;
    EXPECT_EQ(pager.parse_input("/Worker", arg), Pager::Action::Search);
    EXPECT_EQ(arg, "Worker");
}

TEST(PagerTest, ParseInputGoto) {
    Pager pager(50);
    std::string arg;
    // Page numbers work only after show_page is called (total_items_ is set)
    // Without show_page, total_pages() returns 1, so g3 would go to page 1
    // Just test that the action is Goto
    EXPECT_EQ(pager.parse_input("g5", arg), Pager::Action::Goto);
    EXPECT_EQ(arg, "5");
}

TEST(PagerTest, ShowPageOutput) {
    Pager pager(3);
    PrettyFormatter fmt;

    std::string rendered;
    bool more = pager.show_page(
        10,
        [&](uint32_t offset, uint32_t limit) {
            char buf[64];
            snprintf(buf, sizeof(buf), "offset=%u limit=%u", offset, limit);
            rendered = buf;
        },
        &fmt);

    EXPECT_TRUE(more);
    EXPECT_EQ(rendered, "offset=0 limit=3");

    auto out = fmt.finalize();
    EXPECT_NE(out.find("Page 1 of 4"), std::string::npos);
    EXPECT_NE(out.find("[n]ext"), std::string::npos);
}

TEST(PagerTest, ShowPageLastPage) {
    Pager pager(3);
    PrettyFormatter fmt;

    // Navigate to last page manually
    bool more =
        pager.show_page(3, [&](uint32_t /*offset*/, uint32_t /*limit*/) {}, &fmt);
    EXPECT_FALSE(more); // no more pages when total == page_size

    auto out = fmt.finalize();
    EXPECT_NE(out.find("Page 1 of 1"), std::string::npos);
}
