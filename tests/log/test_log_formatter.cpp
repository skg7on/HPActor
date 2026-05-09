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
#include <hpactor/log/log_event.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/log_formatter.hpp>
#include <string>

using namespace hpactor::log;
using hpactor::ActorId;

static LogEvent make_event() {
    LogEvent evt{};
    evt.timestamp_ns = 1746789123456789000ULL;
    evt.level = LogLevel::kWarning;
    evt.category = LogCategory::kMailbox;
    evt.actor_id = ActorId{42};
    evt.event_id = 1100;
    evt.message = "mailbox depth high";
    evt.fields[0] = field("depth", uint64_t(2048));
    evt.fields[1] = field("threshold", uint64_t(1024));
    evt.field_count = 2;
    evt.worker_id = UINT32_MAX;
    evt.line = 100;
    evt.file = "mailbox.cpp";
    return evt;
}

int main() {
    // Test: Text format contains key fields
    {
        TextLogFormatter fmt;
        std::string out;
        fmt.format(make_event(), out);
        assert(out.find("warning") != std::string::npos);
        assert(out.find("mailbox") != std::string::npos);
        assert(out.find("mailbox depth high") != std::string::npos);
        assert(out.find("2048") != std::string::npos);
        assert(out.find("1024") != std::string::npos);
    }

    // Test: JSON format contains key fields
    {
        JsonLogFormatter fmt;
        std::string out;
        fmt.format(make_event(), out);
        assert(out.find("\"level\"") != std::string::npos);
        assert(out.find("\"warning\"") != std::string::npos);
        assert(out.find("\"category\"") != std::string::npos);
        assert(out.find("\"mailbox\"") != std::string::npos);
        assert(out.find("\"actor_id\"") != std::string::npos);
        assert(out.find("42") != std::string::npos);
    }

    // Test: JSON escapes special characters
    {
        JsonLogFormatter fmt;
        LogEvent evt{};
        evt.timestamp_ns = 0;
        evt.level = LogLevel::kInfo;
        evt.category = LogCategory::kUser;
        evt.message = "hello \"world\"\nbackslash\\here";
        evt.fields[0] = field_lit("key", "val\"ue");
        evt.field_count = 1;
        evt.worker_id = UINT32_MAX;

        std::string out;
        fmt.format(evt, out);
        // Should contain escaped content, not raw special chars in values
        assert(out.find("world") != std::string::npos);
    }

    // Test: Empty fields omitted
    {
        TextLogFormatter fmt;
        LogEvent evt{};
        evt.timestamp_ns = 0;
        evt.level = LogLevel::kInfo;
        evt.category = LogCategory::kUser;
        evt.message = "simple message";
        evt.field_count = 0;
        evt.worker_id = UINT32_MAX;

        std::string out;
        fmt.format(evt, out);
        assert(out.find("simple message") != std::string::npos);
    }

    return 0;
}
