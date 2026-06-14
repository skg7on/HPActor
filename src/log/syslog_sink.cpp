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

#include <hpactor/log/syslog_sink.hpp>

#include <string>
#include <syslog.h>

namespace hpactor::log {

SyslogSink::SyslogSink(const std::string& ident)
    : ident_(ident), opened_(false) {
    ::openlog(ident_.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
    opened_ = true;
}

SyslogSink::~SyslogSink() {
    if (opened_) {
        ::closelog();
        opened_ = false;
    }
}

result<void> SyslogSink::write(std::string_view line) noexcept {
    if (!opened_ || line.empty()) {
        return result<void>::make();
    }

    int priority = LOG_INFO;

    // Map log level prefixes to syslog priorities
    if (line.size() >= 7) {
        if (line.substr(0, 7) == "[ERROR]") {
            priority = LOG_ERR;
        } else if (line.substr(0, 7) == "[FATAL]") {
            priority = LOG_CRIT;
        } else if (line.substr(0, 7) == "[DEBUG]") {
            priority = LOG_DEBUG;
        }
    }
    if (priority == LOG_INFO && line.size() >= 6) {
        if (line.substr(0, 6) == "[WARN]") {
            priority = LOG_WARNING;
        } else if (line.substr(0, 6) == "[CRIT]") {
            priority = LOG_CRIT;
        }
    }
    if (priority == LOG_INFO && line.size() >= 10) {
        if (line.substr(0, 10) == "[CRITICAL]") {
            priority = LOG_CRIT;
        }
    }

    ::syslog(priority, "%.*s", static_cast<int>(line.size()), line.data());
    return result<void>::make();
}

result<void> SyslogSink::flush() noexcept {
    // syslog does not require explicit flushing
    return result<void>::make();
}

std::unique_ptr<ILogSink> make_syslog_sink(const std::string& ident) {
    return std::make_unique<SyslogSink>(ident);
}

} // namespace hpactor::log
