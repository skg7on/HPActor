#include <hpactor/net/endpoint_circuit_breaker.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace hpactor::net {
namespace {

using namespace std::chrono_literals;

TEST(EndpointCircuitBreakerTest, ClosedAllowsAllSends) {
    EndpointCircuitBreakerConfig cfg;
    EndpointCircuitBreaker cb(cfg);
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
    EXPECT_TRUE(cb.allow_send());
    EXPECT_TRUE(cb.allow_send());
    EXPECT_TRUE(cb.allow_send());
}

TEST(EndpointCircuitBreakerTest, OpensAfterThresholdFailures) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
    cb.record_failure(); // 3rd failure -> Open
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
}

TEST(EndpointCircuitBreakerTest, OpenRejectsAllSends) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = 60000ms; // very long cooldown
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
    EXPECT_FALSE(cb.allow_send());
    EXPECT_FALSE(cb.allow_send());
}

TEST(EndpointCircuitBreakerTest, CooldownBeforeHalfOpen) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = 1ms;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
    std::this_thread::sleep_for(5ms);
    EXPECT_TRUE(cb.allow_send()); // probe transitions to HalfOpen
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::HalfOpen);
}

TEST(EndpointCircuitBreakerTest, HalfOpenProbeLimit) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = 1ms;
    cfg.half_open_probe_limit = 2;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    std::this_thread::sleep_for(5ms);
    EXPECT_TRUE(cb.allow_send());  // probe 1
    EXPECT_TRUE(cb.allow_send());  // probe 2
    EXPECT_FALSE(cb.allow_send()); // beyond probe limit
}

TEST(EndpointCircuitBreakerTest, HalfOpenSuccessCloses) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = 1ms;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    std::this_thread::sleep_for(5ms);
    EXPECT_TRUE(cb.allow_send()); // enter HalfOpen
    cb.record_success();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
}

TEST(EndpointCircuitBreakerTest, HalfOpenFailureReopens) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = 1ms;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    std::this_thread::sleep_for(5ms);
    cb.allow_send();     // enter HalfOpen
    cb.record_failure(); // fail in HalfOpen -> back to Open
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
}

TEST(EndpointCircuitBreakerTest, SuccessResetsFailureCount) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    cb.record_failure(); // 2 failures
    cb.record_success(); // reset
    EXPECT_EQ(cb.failure_count(), 0u);
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
    cb.record_failure(); // 3rd since last reset -> open
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
}

TEST(EndpointCircuitBreakerTest, OperatorReset) {
    EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    EndpointCircuitBreaker cb(cfg);
    cb.record_failure();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Open);
    cb.reset();
    EXPECT_EQ(cb.state(), EndpointCircuitBreaker::State::Closed);
    EXPECT_EQ(cb.failure_count(), 0u);
}

} // anonymous namespace
} // namespace hpactor::net
