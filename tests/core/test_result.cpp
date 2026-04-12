#include <cassert>
#include <hpactor/types.hpp>

void test_result_value() {
    auto r = hpactor::result<int>::make(42);
    assert(r.has_value());
    assert(r.value() == 42);
}

void test_result_error() {
    auto r = hpactor::result<int>::make(hpactor::error{1, "test"});
    assert(!r.has_value());
    assert(r.error().code() == 1);
}

void test_result_void_success() {
    auto r = hpactor::result<void>::make();
    assert(r.has_value());
}

void test_result_void_error() {
    auto r = hpactor::result<void>::make(hpactor::error{42, "specific error"});
    assert(!r.has_value());
    assert(r.error().code() == 42);
}

int main() {
    test_result_value();
    test_result_error();
    test_result_void_success();
    test_result_void_error();
    return 0;
}