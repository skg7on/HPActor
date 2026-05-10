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

#include <fstream>
#include <hpactor/log/log_sink.hpp>
#include <mutex>

namespace hpactor::log {

class FileSink : public ILogSink {
  public:
    explicit FileSink(const std::string& path)
        : file_(path, std::ios::app | std::ios::out) {}

    result<void> write(std::string_view line) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open())
            return result<void>::make(error(errors::unknown, "file sink not "
                                                             "open"));
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        file_.put('\n');
        if (file_.fail())
            return result<void>::make(error(errors::unknown, "file sink write "
                                                             "failed"));
        return result<void>::make();
    }

    result<void> flush() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open())
            file_.flush();
        return result<void>::make();
    }

  private:
    std::ofstream file_;
    std::mutex mutex_;
};

std::unique_ptr<ILogSink> make_file_sink(const std::string& path) {
    return std::make_unique<FileSink>(path);
}

} // namespace hpactor::log
