#include <hpactor/net/connection_pool.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test PoolConfig default values
    PoolConfig config;
    assert(config.min_connections == 1);
    assert(config.max_connections == 4);
    assert(config.max_pending == 1000);
    assert(config.max_attempts == 5);
    assert(config.initial_backoff.count() == 1000);
    assert(config.max_backoff.count() == 16000);

    // Test PoolStats initial state
    PoolStats stats;
    assert(stats.active_connections == 0);
    assert(stats.pending_messages == 0);
    assert(stats.reconnect_attempts == 0);
    assert(stats.is_connected == false);

    return 0;
}