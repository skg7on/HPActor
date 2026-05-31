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

#include <string>

namespace hpactor::apps::edgeops_telemetry {

class RollupAccumulator {
  public:
    RollupAccumulator(std::string site_id, SensorType sensor_type)
        : site_id_(std::move(site_id)), sensor_type_(sensor_type) {}

    void add(const NormalizedReadingPayload& reading) {
        if (count_ == 0) {
            min_milli_ = reading.reading_milli;
            max_milli_ = reading.reading_milli;
        } else {
            if (reading.reading_milli < min_milli_)
                min_milli_ = reading.reading_milli;
            if (reading.reading_milli > max_milli_)
                max_milli_ = reading.reading_milli;
        }
        sum_milli_ += reading.reading_milli;
        ++count_;
    }

    WindowRollupPayload
    finish(uint64_t window_start_ns, uint64_t window_end_ns) const {
        WindowRollupPayload payload;
        payload.site_id = site_id_;
        payload.sensor_type = sensor_type_;
        payload.window_start_ns = window_start_ns;
        payload.window_end_ns = window_end_ns;
        payload.count = count_;
        payload.min_milli = count_ == 0 ? 0 : min_milli_;
        payload.max_milli = count_ == 0 ? 0 : max_milli_;
        payload.sum_milli = sum_milli_;
        payload.average_milli =
            count_ == 0 ? 0 : sum_milli_ / static_cast<int64_t>(count_);
        return payload;
    }

    uint32_t count() const {
        return count_;
    }

  private:
    std::string site_id_;
    SensorType sensor_type_;
    uint32_t count_ = 0;
    int64_t min_milli_ = 0;
    int64_t max_milli_ = 0;
    int64_t sum_milli_ = 0;
};

} // namespace hpactor::apps::edgeops_telemetry
