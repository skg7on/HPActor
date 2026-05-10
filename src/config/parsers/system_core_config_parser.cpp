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

#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>

namespace hpactor::config {
namespace {

class SystemCoreConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.core";
    static constexpr int kOrder = 0;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& st, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        out.version = st.read_string("version", "1.0");
        out.scheduler_threads = st.read_uint32("scheduler_threads", 4);
        out.max_queue_depth = st.read_uint32("max_queue_depth", 1024);
        out.default_mailbox_size = st.read_uint32("default_mailbox_size", 1024);
        out.enable_network = st.read_bool("enable_network");
        out.tcp_port = static_cast<uint16_t>(st.read_uint32("tcp_port"));
        out.spawn_timeout_ms = st.read_uint32("spawn_timeout_ms", 5000);
        out.enable_http_gateway = st.read_bool("enable_http_gateway");
        out.http_bind_host = st.read_string("http_bind_host", "0.0.0.0");
        out.http_port = static_cast<uint16_t>(st.read_uint32("http_port", 8080));
        out.http_max_connections = st.read_uint32("http_max_connections", 1000);
        out.http_max_request_size =
            st.read_uint32("http_max_request_size", 1048576);
        out.http_reply_timeout_ms = st.read_uint32("http_reply_timeout_ms", 5000);
        out.use_coroutines = st.read_bool("use_coroutines");

        // imports
        st.for_each_string_array("imports", [&](std::string_view val) {
            out.imports.emplace_back(val);
        });

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<SystemCoreConfigParser> kRegisterSystemCoreConfigParser;

} // anonymous namespace
} // namespace hpactor::config
