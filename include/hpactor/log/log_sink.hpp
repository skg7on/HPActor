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

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "hpactor/types/types.hpp" // for result<T>

namespace hpactor::log {

struct LogConfig;
struct RotatingFileConfig;

class ILogSink {
  public:
    virtual ~ILogSink() = default;
    virtual result<void> write(std::string_view line) noexcept = 0;
    virtual result<void> flush() noexcept = 0;
};

// In-memory sink for tests
class MemorySink : public ILogSink {
  public:
    result<void> write(std::string_view line) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.emplace_back(line);
        return result<void>::make();
    }

    result<void> flush() noexcept override {
        return result<void>::make();
    }

    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

// Factory functions (implemented in respective .cpp files)
std::unique_ptr<ILogSink> make_stderr_sink();
std::unique_ptr<ILogSink> make_file_sink(const std::string& path);
std::unique_ptr<ILogSink> make_rotating_file_sink(const RotatingFileConfig& cfg);

} // namespace hpactor::log
