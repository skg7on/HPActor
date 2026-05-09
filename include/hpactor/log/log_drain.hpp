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

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hpactor::log {

class LogRingBuffer;
class ILogFormatter;
class ILogSink;
struct LogConfig;

class LogDrain {
  public:
    LogDrain(LogRingBuffer& buffer, ILogFormatter& formatter,
             std::vector<std::unique_ptr<ILogSink>> sinks,
             const LogConfig& config) noexcept;

    ~LogDrain();

    LogDrain(const LogDrain&) = delete;
    LogDrain& operator=(const LogDrain&) = delete;

    void start();
    void stop() noexcept;
    void nudge() noexcept;

    uint64_t sink_errors() const noexcept {
        return sink_errors_.load();
    }

  private:
    void run();

    LogRingBuffer& buffer_;
    ILogFormatter& formatter_;
    std::vector<std::unique_ptr<ILogSink>> sinks_;
    const LogConfig& config_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> sink_errors_{0};
};

} // namespace hpactor::log
