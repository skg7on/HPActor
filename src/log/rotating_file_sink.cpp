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
#include <fstream>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_sink.hpp>
#include <mutex>

namespace hpactor::log {

class RotatingFileSink : public ILogSink {
  public:
    explicit RotatingFileSink(const RotatingFileConfig& cfg)
        : cfg_(cfg), file_(cfg.path, std::ios::app | std::ios::out) {
        if (file_.is_open()) {
            file_.seekp(0, std::ios::end);
            bytes_written_ = static_cast<uint64_t>(file_.tellp());
        }
    }

    result<void> write(std::string_view line) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open())
            return result<void>::make(error(errors::unknown, "rotating file "
                                                             "sink not open"));
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        file_.put('\n');
        if (file_.fail())
            return result<void>::make(error(errors::unknown, "rotating file "
                                                             "sink write "
                                                             "failed"));
        bytes_written_ += line.size() + 1;

        if (bytes_written_ >= cfg_.max_bytes) {
            rotate();
        }
        return result<void>::make();
    }

    result<void> flush() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open())
            file_.flush();
        return result<void>::make();
    }

  private:
    void rotate() {
        file_.close();
        // Remove oldest file
        std::string oldest = cfg_.path + "." + std::to_string(cfg_.max_files);
        std::remove(oldest.c_str());
        // Rotate: file.log.N -> file.log.(N+1), file.log -> file.log.1
        for (int i = static_cast<int>(cfg_.max_files) - 1; i >= 1; --i) {
            std::string from = cfg_.path + "." + std::to_string(i);
            std::string to = cfg_.path + "." + std::to_string(i + 1);
            std::rename(from.c_str(), to.c_str());
        }
        std::rename(cfg_.path.c_str(), (cfg_.path + ".1").c_str());
        // Open fresh file
        file_.open(cfg_.path, std::ios::out | std::ios::trunc);
        bytes_written_ = 0;
    }

    RotatingFileConfig cfg_;
    std::ofstream file_;
    std::mutex mutex_;
    uint64_t bytes_written_ = 0;
};

std::unique_ptr<ILogSink> make_rotating_file_sink(const RotatingFileConfig& cfg) {
    return std::make_unique<RotatingFileSink>(cfg);
}

} // namespace hpactor::log
