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

#include <gtest/gtest.h>
#include <hpactor/cli/config/cli_legacy_server_config.hpp>

using namespace hpactor::cli;

TEST(CliLegacyServerConfigTest, Defaults) {
    CliLegacyServerConfig cfg;
    EXPECT_TRUE(cfg.uds_listen_path.empty());
    EXPECT_EQ(cfg.tcp_listen_port, 0);
    EXPECT_EQ(cfg.max_sessions, 16u);
    EXPECT_EQ(cfg.session_timeout.count(), 300000);
    EXPECT_EQ(cfg.uds_socket_mode, 0660u);
}

TEST(CliLegacyServerConfigTest, TcpDefaultsToLocalhost) {
    CliLegacyServerConfig cfg;
    EXPECT_EQ(cfg.tcp_bind_address, "127.0.0.1");
    EXPECT_EQ(cfg.default_format, "pretty");
    EXPECT_EQ(cfg.page_size, 50u);
}
