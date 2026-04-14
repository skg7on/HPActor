#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <type_traits>

using namespace hpactor;

// Test that AbstractActor has the required interface
void test_abstract_actor_interface() {
    // Abstract actor cannot be instantiated directly
    // Test that it has the required virtual interface
    static_assert(sizeof(hpactor::AbstractActor) > 0, "AbstractActor should not be empty");

    // Check that AbstractActor is abstract (has pure virtual methods)
    static_assert(!std::is_default_constructible_v<AbstractActor>,
                  "AbstractActor should not be default constructible");

    // Check that it inherits from enable_shared_from_this
    static_assert(
        std::is_base_of_v<std::enable_shared_from_this<AbstractActor>, AbstractActor>,
        "AbstractActor must inherit from enable_shared_from_this");
}

void test_message_variant_types() {
    // Test that MessageVariant can hold all required message types
    MessageVariant msg = down_msg{{}, error{}};
    assert(std::holds_alternative<down_msg>(msg));

    msg = exit_msg{{}, error{}};
    assert(std::holds_alternative<exit_msg>(msg));

    msg = link_msg{{}};
    assert(std::holds_alternative<link_msg>(msg));

    msg = unlink_msg{{}};
    assert(std::holds_alternative<unlink_msg>(msg));
}

int main() {
    test_abstract_actor_interface();
    test_message_variant_types();

    return 0;
}
