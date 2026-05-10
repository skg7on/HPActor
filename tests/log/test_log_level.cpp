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

#include <cassert>
#include <cstring>
#include <hpactor/log/log_level.hpp>

int main() {
    using namespace hpactor::log;

    // Test to_string
    assert(std::strcmp(to_string(LogLevel::kCritical), "critical") == 0);
    assert(std::strcmp(to_string(LogLevel::kError), "error") == 0);
    assert(std::strcmp(to_string(LogLevel::kWarning), "warning") == 0);
    assert(std::strcmp(to_string(LogLevel::kInfo), "info") == 0);
    assert(std::strcmp(to_string(LogLevel::kDebug), "debug") == 0);
    assert(std::strcmp(to_string(LogLevel::kTrace), "trace") == 0);
    assert(std::strcmp(to_string(LogLevel::kOff), "off") == 0);

    // Test parse_level success
    assert(parse_level("critical").has_value());
    assert(parse_level("critical").value() == LogLevel::kCritical);
    assert(parse_level("error").value() == LogLevel::kError);
    assert(parse_level("warning").value() == LogLevel::kWarning);
    assert(parse_level("info").value() == LogLevel::kInfo);
    assert(parse_level("debug").value() == LogLevel::kDebug);
    assert(parse_level("trace").value() == LogLevel::kTrace);
    assert(parse_level("off").value() == LogLevel::kOff);

    // Test parse_level failure
    assert(!parse_level("invalid").has_value());

    // Test ordering
    assert(static_cast<uint8_t>(LogLevel::kCritical) <
           static_cast<uint8_t>(LogLevel::kError));
    assert(static_cast<uint8_t>(LogLevel::kDebug) <
           static_cast<uint8_t>(LogLevel::kTrace));

    // Test enabled/disabled relationships
    assert(static_cast<uint8_t>(LogLevel::kInfo) <=
           static_cast<uint8_t>(LogLevel::kDebug));
    assert(static_cast<uint8_t>(LogLevel::kDebug) >
           static_cast<uint8_t>(LogLevel::kInfo));

    return 0;
}
