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

#include <hpactor/log/log_formatter.hpp>

#include <hpactor/log/log_category.hpp>
#include <hpactor/log/log_event.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/log_level.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace hpactor::log {

namespace {

// ---------------------------------------------------------------------------
// Format Unix epoch nanoseconds as ISO 8601 (e.g.
// "2026-05-09T12:34:56.789123456Z")
// ---------------------------------------------------------------------------
void format_timestamp(uint64_t ns, char* buf, size_t bufsz) {
    auto secs = std::chrono::seconds(
        static_cast<std::chrono::seconds::rep>(ns / 1'000'000'000));
    auto sub_ns = static_cast<unsigned long>(ns % 1'000'000'000);
    auto tp = std::chrono::system_clock::time_point(secs);
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm gm;
    gmtime_r(&t, &gm);
    int pos = std::snprintf(buf, bufsz, "%04d-%02d-%02dT%02d:%02d:%02d.",
                            gm.tm_year + 1900, gm.tm_mon + 1, gm.tm_mday,
                            gm.tm_hour, gm.tm_min, gm.tm_sec);
    if (pos > 0 && static_cast<std::size_t>(pos) < bufsz) {
        std::snprintf(buf + pos, bufsz - static_cast<std::size_t>(pos),
                      "%09luZ", sub_ns);
    }
}

// ---------------------------------------------------------------------------
// Append a JSON-escaped string value to `out`.
// Escapes ", \, \n, \r, \t, and control characters (U+0000..U+001F).
// ---------------------------------------------------------------------------
void json_escape_string(std::string& out, const char* s) {
    if (!s) {
        return;
    }
    for (; *s; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += esc;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Append the value of a LogField in text (key=value) form to `out`.
// ---------------------------------------------------------------------------
void append_text_field_value(const LogField& field, std::string& out) {
    char buf[128];
    switch (field.type) {
        case LogFieldType::kInt64:
            std::snprintf(buf, sizeof(buf), "%ld",
                          static_cast<long>(field.value.i64));
            out += buf;
            break;
        case LogFieldType::kUInt64:
            std::snprintf(buf, sizeof(buf), "%lu",
                          static_cast<unsigned long>(field.value.u64));
            out += buf;
            break;
        case LogFieldType::kDouble:
            std::snprintf(buf, sizeof(buf), "%g", field.value.f64);
            out += buf;
            break;
        case LogFieldType::kBool:
            out += field.value.boolean ? "true" : "false";
            break;
        case LogFieldType::kStringLiteral:
            if (field.value.str) {
                out += field.value.str;
            }
            break;
        case LogFieldType::kPointer:
            std::snprintf(buf, sizeof(buf), "%p", field.value.ptr);
            out += buf;
            break;
    }
}

// ---------------------------------------------------------------------------
// Append the value of a LogField in JSON form to `out`.
// ---------------------------------------------------------------------------
void append_json_field_value(const LogField& field, std::string& out) {
    char buf[128];
    switch (field.type) {
        case LogFieldType::kInt64:
            std::snprintf(buf, sizeof(buf), "%ld",
                          static_cast<long>(field.value.i64));
            out += buf;
            break;
        case LogFieldType::kUInt64:
            std::snprintf(buf, sizeof(buf), "%lu",
                          static_cast<unsigned long>(field.value.u64));
            out += buf;
            break;
        case LogFieldType::kDouble:
            std::snprintf(buf, sizeof(buf), "%g", field.value.f64);
            out += buf;
            break;
        case LogFieldType::kBool:
            out += field.value.boolean ? "true" : "false";
            break;
        case LogFieldType::kStringLiteral:
            out += '"';
            json_escape_string(out, field.value.str ? field.value.str : "");
            out += '"';
            break;
        case LogFieldType::kPointer:
            out += '"';
            std::snprintf(buf, sizeof(buf), "%p", field.value.ptr);
            out += buf;
            out += '"';
            break;
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// TextLogFormatter
// ---------------------------------------------------------------------------
void TextLogFormatter::format(const LogEvent& event, std::string& out) {
    char ts_buf[64];
    format_timestamp(event.timestamp_ns, ts_buf, sizeof(ts_buf));
    out = ts_buf;

    out += ' ';
    out += to_string(event.level);

    out += ' ';
    out += to_string(event.category);

    if (event.actor_id.value() != 0) {
        char buf[32];
        int n = std::snprintf(buf, sizeof(buf), " actor=%lu",
                              static_cast<unsigned long>(event.actor_id.value()));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        }
    }

    out += " event=";
    out += to_string(static_cast<LogEventId>(event.event_id));

    if (event.message) {
        out += ' ';
        out += event.message;
    }

    for (uint8_t i = 0; i < event.field_count; ++i) {
        out += ' ';
        out += event.fields[i].name;
        out += '=';
        append_text_field_value(event.fields[i], out);
    }
}

// ---------------------------------------------------------------------------
// JsonLogFormatter
// ---------------------------------------------------------------------------
void JsonLogFormatter::format(const LogEvent& event, std::string& out) {
    char ts_buf[64];
    format_timestamp(event.timestamp_ns, ts_buf, sizeof(ts_buf));

    out = R"({"ts":")";
    out += ts_buf;
    out += R"(","level":")";
    out += to_string(event.level);
    out += R"(","category":")";
    out += to_string(event.category);
    out += '"';

    if (event.actor_id.value() != 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lu",
                      static_cast<unsigned long>(event.actor_id.value()));
        out += ",\"actor_id\":";
        out += buf;
    }

    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u", event.event_id);
        out += ",\"event_id\":";
        out += buf;
    }

    out += R"(,"message":")";
    json_escape_string(out, event.message ? event.message : "");
    out += '"';

    for (uint8_t i = 0; i < event.field_count; ++i) {
        out += ",\"";
        out += event.fields[i].name;
        out += "\":";
        append_json_field_value(event.fields[i], out);
    }

    out += '}';
}

} // namespace hpactor::log
