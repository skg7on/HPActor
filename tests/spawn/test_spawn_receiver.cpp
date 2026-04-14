#include <hpactor/actor/spawn_receiver.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/core/actor_system.hpp>

#include <iostream>
#include <cassert>

using namespace hpactor;

int main() {
    // Basic compilation test - SpawnReceiver requires full ActorSystem
    Config config;
    ActorSystem system{config};
    ActorTypeRegistry registry;

    // SpawnReceiver can't be easily unit tested without transport mock
    // This is more of an integration test
    std::cout << "SpawnReceiver compiled successfully\n";
    return 0;
}