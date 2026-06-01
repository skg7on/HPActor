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

#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>

#include <string>

namespace hpactor::config {
namespace {

class TransportOutboundConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.transport";
    static constexpr int kOrder = 95;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto transport = system.table("transport");
        if (!transport.valid())
            return result<void>::make();

        auto ob = transport.table("outbound");
        if (!ob.valid())
            return result<void>::make();

        auto& limits = out.transport_outbound_limits;
        limits.max_messages = ob.read_uint32("max_queued_messages", 1000);
        limits.max_bytes = static_cast<uint64_t>(
            ob.value("max_queued_bytes").as_int64(16 * 1024 * 1024));
        limits.control_lane_reserve = ob.read_uint32("control_lane_reserve", 64);
        limits.reliable_headroom_pct =
            ob.read_double("reliable_headroom_pct", 0.20);
        limits.high_watermark = ob.read_double("high_watermark", 0.70);
        limits.critical_watermark = ob.read_double("critical_watermark", 0.90);
        limits.low_watermark = ob.read_double("low_watermark", 0.50);
        limits.drain_rate_ema_alpha = ob.read_double("drain_rate_ema_alpha", 0.20);

        // Validate watermark ordering
        if (limits.low_watermark < 0.0)
            limits.low_watermark = 0.50;
        if (limits.high_watermark < limits.low_watermark)
            limits.high_watermark = 0.70;
        if (limits.critical_watermark < limits.high_watermark ||
            limits.critical_watermark > 1.0)
            limits.critical_watermark = 0.90;

        auto& cb_cfg = out.transport_circuit_breaker;
        cb_cfg.failure_threshold = ob.read_uint32("circuit_failure_threshold", 5);
        cb_cfg.cooldown = std::chrono::milliseconds(
            ob.read_uint32("circuit_cooldown_ms", 30000));
        cb_cfg.half_open_probe_limit =
            ob.read_uint32("circuit_half_open_probe_limit", 1);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<TransportOutboundConfigParser> kRegisterTransportOutboundConfigParser;

} // anonymous namespace
} // namespace hpactor::config
