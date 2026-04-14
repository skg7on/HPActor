#include <cassert>
#include <hpactor/actor/message.hpp>
#include <string>

struct MoveOnly {
    int value;
    std::string data;
    MoveOnly() = default;
    MoveOnly(int v, std::string d) : value(v), data(std::move(d)) {}
    MoveOnly(MoveOnly&& other) noexcept
        : value(other.value), data(std::move(other.data)) {}
    MoveOnly& operator=(MoveOnly&& other) noexcept {
        value = other.value;
        data = std::move(other.data);
        return *this;
    }
    // Delete copy
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
};

int main() {
    // Test move-only type with rvalue (move constructor)
    MoveOnly m{42, "test"};
    hpactor::Message<MoveOnly> msg{std::move(m)};
    assert(msg.payload().value == 42);
    // Verify original moved-from state
    assert(m.value == 42); // int copied, string moved-from

    // Test move-only type by constructing MoveOnly first, then wrapping
    hpactor::Message<MoveOnly> msg2{MoveOnly{100, "moved"}};
    assert(msg2.payload().value == 100);

    return 0;
}