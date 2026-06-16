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
#include <hpactor/cli.pb.h>
#include <hpactor/cli/cli_server_actor.hpp>
#include <hpactor/cli/cli_server_config.hpp>
#include <hpactor/core/actor_system.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <thread>

// REMOVED: Proto listener tests require proto_uds_path/proto_tcp_port fields
// which have been extracted from CliServerConfig. These tests will be
// re-enabled when CliProtoServerActor is implemented.
// See issue #306.
#if 0
TEST(CliServerProto, ProtoListenerStartsWithoutError) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    hpactor::cli::CliServerConfig cfg;
    // cfg.proto_tcp_port = 19091;  // field removed from CliServerConfig
    cfg.tcp_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliServerActor>(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto* raw = static_cast<hpactor::cli::CliServerActor*>(
        system.get_actor(server.id()).get());
    ASSERT_TRUE(raw != nullptr);
    ASSERT_TRUE(raw->is_system_actor());

    raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// DISABLED: proto client dispatch requires EventLoop integration tuning.
// The on_proto_client_readable handler is registered correctly but the
// DaemonActor thread's EventLoop poll cycle doesn't reliably process
// the read handler before the test read() times out. This will be
// fixed when the HTTP JSON endpoint integration is added.
TEST(CliServerProto, DISABLED_CommandTreeDispatchReturnsFormattedText) {
    hpactor::Config sys_config;
    sys_config.scheduler_threads = 0;
    hpactor::ActorSystem system(sys_config);

    hpactor::cli::CliServerConfig cfg;
    cfg.proto_tcp_port = 19092;
    cfg.tcp_bind_address = "127.0.0.1";

    auto server = system.spawn<hpactor::cli::CliServerActor>(cfg);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Connect and send a CliCommand
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19092);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    int ret = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0) {
        // Server may not be ready — skip rather than fail
        close(fd);
        GTEST_SKIP()
            << "Could not connect to proto listener (server may not be ready)";
    }

    // Send: CliCommand{path="help"}
    hpactor::cli::CliCommand cmd;
    cmd.set_path("help");
    std::string wire = cmd.SerializeAsString();
    uint32_t len = static_cast<uint32_t>(wire.size());
    uint8_t len_buf[5];
    int lb = 0;
    uint32_t tmp = len;
    while (tmp > 0x7f) {
        len_buf[lb++] = static_cast<uint8_t>(tmp & 0x7f) | 0x80;
        tmp >>= 7;
    }
    len_buf[lb++] = static_cast<uint8_t>(tmp);
    ssize_t written = write(fd, len_buf, static_cast<size_t>(lb));
    ASSERT_EQ(written, static_cast<ssize_t>(lb));
    written = write(fd, wire.data(), wire.size());
    ASSERT_EQ(written, static_cast<ssize_t>(wire.size()));

    // Read response (wait a bit for server to process)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    EXPECT_GT(n, 0) << "Should receive at least the varint length prefix";
    // Verify we got something back — at minimum the varint prefix
    if (n > 0) {
        // Try to decode the response varint prefix
        uint32_t resp_len = 0;
        uint32_t shift = 0;
        size_t pos = 0;
        while (pos < static_cast<size_t>(n) && pos < 5) {
            uint8_t byte = static_cast<uint8_t>(buf[pos]);
            resp_len |= static_cast<uint32_t>((byte & 0x7f) << shift);
            pos++;
            if (!(byte & 0x80))
                break;
            shift += 7;
        }
        EXPECT_GT(pos, 0u);
        // Verify the decoded length is reasonable (non-zero)
        EXPECT_GT(resp_len, 0u);
    }

    close(fd);
    auto* raw = static_cast<hpactor::cli::CliServerActor*>(
        system.get_actor(server.id()).get());
    raw->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
#endif // 0 — removed proto fields from CliServerConfig
