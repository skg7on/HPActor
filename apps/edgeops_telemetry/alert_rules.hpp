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

#pragma once

#include <apps/edgeops_telemetry/messages.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace hpactor::apps::edgeops_telemetry {

struct ThresholdRule {
    SensorType sensor_type = SensorType::Temperature;
    int64_t high_milli = 0;
    std::string reason;

    bool evaluate(const NormalizedReadingPayload& reading,
                  AlertRaisedPayload& alert) const {
        if (reading.sensor_type != sensor_type || reading.reading_milli < high_milli)
            return false;
        alert.device_id = reading.device_id;
        alert.site_id = reading.site_id;
        alert.sensor_type = reading.sensor_type;
        alert.sequence = reading.sequence;
        alert.reading_milli = reading.reading_milli;
        alert.threshold_milli = high_milli;
        alert.reason = reason;
        return true;
    }
};

struct RateOfChangeRule {
    SensorType sensor_type = SensorType::Temperature;
    int64_t delta_milli = 0;
    std::string reason;

    bool evaluate(const NormalizedReadingPayload& previous,
                  const NormalizedReadingPayload& current,
                  AlertRaisedPayload& alert) const {
        if (previous.sensor_type != sensor_type ||
            current.sensor_type != sensor_type ||
            previous.device_id != current.device_id)
            return false;
        int64_t delta = current.reading_milli - previous.reading_milli;
        if (delta < 0)
            delta = -delta;
        if (delta < delta_milli)
            return false;
        alert.device_id = current.device_id;
        alert.site_id = current.site_id;
        alert.sensor_type = current.sensor_type;
        alert.sequence = current.sequence;
        alert.reading_milli = delta;
        alert.threshold_milli = delta_milli;
        alert.reason = reason;
        return true;
    }
};

} // namespace hpactor::apps::edgeops_telemetry
