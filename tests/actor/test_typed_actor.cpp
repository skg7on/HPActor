#include <cassert>
#include <hpactor/typed_behavior.hpp>
#include <hpactor/actor/typed_actor.hpp>

// Message struct definitions for typed actor
struct add_message {
    int a;
    int b;
};

struct subtract_message {
    int a;
    int b;
};

struct shutdown_message {};

// Typed actor using message struct signatures
using calculator_actor = hpactor::typed_actor<
    hpactor::result<int>(add_message),
    hpactor::result<int>(subtract_message),
    hpactor::result<void>(shutdown_message)
>;

void test_typed_actor_definition() {
    static_assert(sizeof(calculator_actor) > 0, "typed_actor should be instantiable");
}

void test_handler_type() {
    using handler = hpactor::handler_type<hpactor::result<int>(add_message)>;
    static_assert(std::is_same_v<typename handler::result, int>, "result type should be int");
}

void test_typed_behavior() {
    hpactor::TypedBehavior<hpactor::result<int>(add_message)> bh;
    (void)bh;
    assert(true);
}

int main() {
    test_typed_actor_definition();
    test_handler_type();
    test_typed_behavior();
    return 0;
}