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

#include <cstdio>
#include <hpactor/log/log_sink.hpp>

namespace hpactor::log {

class StderrSink : public ILogSink {
  public:
    result<void> write(std::string_view line) noexcept override {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
        return result<void>::make();
    }

    result<void> flush() noexcept override {
        std::fflush(stderr);
        return result<void>::make();
    }
};

std::unique_ptr<ILogSink> make_stderr_sink() {
    return std::make_unique<StderrSink>();
}

} // namespace hpactor::log
