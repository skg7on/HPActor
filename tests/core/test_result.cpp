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

#include <cassert>
#include <hpactor/types/types.hpp>

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