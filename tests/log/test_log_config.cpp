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
#include <hpactor/log/log_config.hpp>

using namespace hpactor::log;

int main() {
    // Test defaults
    {
        LogConfig cfg{};
        assert(cfg.enabled);
        assert(cfg.default_level == LogLevel::kInfo);
        assert(cfg.format == LogFormat::kJson);
        assert(cfg.ring_buffer_capacity == 65536);
        assert(cfg.flush_on_level == LogLevel::kError);
    }

    // Test category level overrides
    {
        LogConfig cfg{};
        cfg.default_level = LogLevel::kInfo;
        cfg.levels[static_cast<size_t>(LogCategory::kMailbox)] = LogLevel::kWarning;
        cfg.levels[static_cast<size_t>(LogCategory::kMemory)] = LogLevel::kWarning;
        cfg.levels[static_cast<size_t>(LogCategory::kNetwork)] = LogLevel::kWarning;

        assert(cfg.levels[static_cast<size_t>(LogCategory::kMailbox)] ==
               LogLevel::kWarning);
        assert(cfg.levels[static_cast<size_t>(LogCategory::kActor)] ==
               LogLevel::kCritical);
    }

    // Test disabled
    {
        LogConfig cfg{};
        cfg.enabled = false;
        assert(!cfg.enabled);
    }

    return 0;
}
