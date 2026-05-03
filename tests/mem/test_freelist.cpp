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

#include <hpactor/mem/freelist.hpp>

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

struct TestNode {
    TestNode* next;
    uint32_t value;
};

int main() {
    using namespace hpactor::mem;

    // Test basic push/pop (LIFO)
    {
        FreeList<TestNode> fl;
        assert(fl.empty());

        TestNode a{nullptr, 1};
        TestNode b{nullptr, 2};
        TestNode c{nullptr, 3};

        fl.push(&a);
        assert(!fl.empty());
        fl.push(&b);
        fl.push(&c);

        TestNode* n = fl.pop();
        assert(n != nullptr);
        assert(n->value == 3); // LIFO
        n = fl.pop();
        assert(n != nullptr);
        assert(n->value == 2);
        n = fl.pop();
        assert(n != nullptr);
        assert(n->value == 1);
        assert(fl.empty());
        assert(fl.pop() == nullptr);
    }

    // Test concurrent push/pop across 4 threads
    {
        FreeList<TestNode> fl;
        constexpr int kNumItems = 10000;

        std::atomic<int> push_count{0};
        std::atomic<int> pop_count{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&]() {
                // Push items
                for (int i = 0; i < kNumItems / 4; ++i) {
                    auto* node = new TestNode{nullptr, static_cast<uint32_t>(i)};
                    fl.push(node);
                    push_count.fetch_add(1);
                }
                // Pop items
                while (pop_count.load() < kNumItems) {
                    TestNode* n = fl.pop();
                    if (n) {
                        pop_count.fetch_add(1);
                        delete n;
                    }
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }

        assert(pop_count.load() == kNumItems);
        assert(fl.empty());
    }

    std::cout << "test_freelist: PASS\n";
    return 0;
}
