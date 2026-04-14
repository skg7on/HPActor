#include <cassert>
#include <hpactor/actor/message.hpp>
#include <string>

struct TestPayload {
    int value;
    std::string data;
};

int main() {
    // Test default construction
    hpactor::Message<TestPayload> msg;
    // Test with payload
    hpactor::Message<TestPayload> msg2{TestPayload{42, "hello"}};
    assert(msg2.payload().value == 42);
    assert(msg2.payload().data == "hello");
    // Test move semantics
    TestPayload p{100, "moved"};
    hpactor::Message<TestPayload> msg3{std::move(p)};
    assert(msg3.payload().value == 100);
    return 0;
}