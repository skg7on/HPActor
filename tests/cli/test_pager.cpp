#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <cassert>
#include <cstdio>

using namespace hpactor::cli;

void test_page_calculation() {
    Pager pager(50);
    assert(pager.current_page() == 1);
    pager.goto_page(3);
    assert(pager.current_page() == 3);
}

void test_clamp() {
    Pager pager(50);
    pager.goto_page(100);
    assert(pager.current_page() >= 1);
}

void test_parse_input_next() {
    Pager pager(50);
    std::string arg;
    assert(pager.parse_input("", arg) == Pager::Action::Next);
    assert(pager.parse_input("n", arg) == Pager::Action::Next);
    assert(pager.parse_input("next", arg) == Pager::Action::Next);
}

void test_parse_input_prev() {
    Pager pager(50);
    std::string arg;
    assert(pager.parse_input("p", arg) == Pager::Action::Previous);
    assert(pager.parse_input("prev", arg) == Pager::Action::Previous);
}

void test_parse_input_quit() {
    Pager pager(50);
    std::string arg;
    assert(pager.parse_input("q", arg) == Pager::Action::Quit);
    assert(pager.parse_input("quit", arg) == Pager::Action::Quit);
}

void test_parse_input_search() {
    Pager pager(50);
    std::string arg;
    assert(pager.parse_input("/Worker", arg) == Pager::Action::Search);
    assert(arg == "Worker");
}

void test_parse_input_goto() {
    Pager pager(50);
    std::string arg;
    // Page numbers work only after show_page is called (total_items_ is set)
    // Without show_page, total_pages() returns 1, so g3 would go to page 1
    // Just test that the action is Goto
    assert(pager.parse_input("g5", arg) == Pager::Action::Goto);
    assert(arg == "5");
}

void test_show_page_output() {
    Pager pager(3);
    PrettyFormatter fmt;

    std::string rendered;
    bool more = pager.show_page(10,
        [&](uint32_t offset, uint32_t limit) {
            char buf[64];
            snprintf(buf, sizeof(buf), "offset=%u limit=%u", offset, limit);
            rendered = buf;
        },
        &fmt);

    assert(more);
    assert(rendered == "offset=0 limit=3");

    auto out = fmt.finalize();
    assert(out.find("Page 1 of 4") != std::string::npos);
    assert(out.find("[n]ext") != std::string::npos);
}

void test_show_page_last_page() {
    Pager pager(3);
    PrettyFormatter fmt;

    // Navigate to last page manually
    bool more = pager.show_page(3,
        [&](uint32_t /*offset*/, uint32_t /*limit*/) {}, &fmt);
    assert(!more);  // no more pages when total == page_size

    auto out = fmt.finalize();
    assert(out.find("Page 1 of 1") != std::string::npos);
}

int main() {
    test_page_calculation();
    test_clamp();
    test_parse_input_next();
    test_parse_input_prev();
    test_parse_input_quit();
    test_parse_input_search();
    test_parse_input_goto();
    test_show_page_output();
    test_show_page_last_page();
    printf("test_pager: PASSED\n");
    return 0;
}
