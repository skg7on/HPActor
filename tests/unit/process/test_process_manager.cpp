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
#include <hpactor/process/process_config.hpp>
#include <hpactor/process/process_manager.hpp>

#include <string>

using namespace hpactor::process;

TEST(ProcessModeTest, ParseForeground) {
    EXPECT_EQ(ProcessConfig::parse_mode("foreground"), ProcessMode::Foreground);
    EXPECT_EQ(ProcessConfig::parse_mode("FOREGROUND"), ProcessMode::Foreground);
    EXPECT_EQ(ProcessConfig::parse_mode("Foreground"), ProcessMode::Foreground);
}

TEST(ProcessModeTest, ParseSystemd) {
    EXPECT_EQ(ProcessConfig::parse_mode("systemd"), ProcessMode::Systemd);
    EXPECT_EQ(ProcessConfig::parse_mode("SYSTEMD"), ProcessMode::Systemd);
}

TEST(ProcessModeTest, ParseDaemon) {
    EXPECT_EQ(ProcessConfig::parse_mode("daemon"), ProcessMode::Daemon);
    EXPECT_EQ(ProcessConfig::parse_mode("DAEMON"), ProcessMode::Daemon);
}

TEST(ProcessModeTest, ParseUnknownDefaultsToForeground) {
    EXPECT_EQ(ProcessConfig::parse_mode("bogus"), ProcessMode::Foreground);
    EXPECT_EQ(ProcessConfig::parse_mode(""), ProcessMode::Foreground);
}

TEST(ProcessModeTest, DefaultConfigIsForeground) {
    ProcessConfig cfg;
    EXPECT_EQ(cfg.mode, ProcessMode::Foreground);
    EXPECT_EQ(cfg.watchdog_interval.count(), 0);
    EXPECT_TRUE(cfg.pidfile_path.empty());
}

TEST(SystemdNotifyTest, FormatsReadyMessage) {
    std::string msg = ProcessManager::format_notify_message("READY=1");
    EXPECT_EQ(msg, "READY=1");
}

TEST(SystemdNotifyTest, FormatsWatchdogMessage) {
    std::string msg = ProcessManager::format_notify_message("WATCHDOG=1");
    EXPECT_EQ(msg, "WATCHDOG=1");
}

TEST(SystemdNotifyTest, FormatsStatusMessage) {
    std::string msg =
        ProcessManager::format_notify_message("STATUS=Running 42 actors");
    EXPECT_NE(msg.find("STATUS=Running 42 actors"), std::string::npos);
}

TEST(SystemdNotifyTest, FormatsStoppingMessage) {
    std::string msg = ProcessManager::format_notify_message("STOPPING=1");
    EXPECT_EQ(msg, "STOPPING=1");
}

TEST(SystemdNotifyTest, RejectsNewlines) {
    std::string msg = ProcessManager::format_notify_message("READY=1\nBAD=1");
    EXPECT_EQ(msg.find('\n'), std::string::npos);
}

#ifdef __linux__
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>

TEST(SystemdNotifyTest, SendNotifyWritesToSocket) {
    std::string sock_path = "/tmp/test_notify_" + std::to_string(getpid()) + ".sock";
    unlink(sock_path.c_str());

    int recv_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(recv_fd, 0);

    struct sockaddr_un bind_addr{};
    bind_addr.sun_family = AF_UNIX;
    strncpy(bind_addr.sun_path, sock_path.c_str(), sizeof(bind_addr.sun_path) - 1);
    ASSERT_EQ(bind(recv_fd, reinterpret_cast<struct sockaddr*>(&bind_addr),
                   sizeof(bind_addr)),
              0);

    ProcessConfig cfg;
    cfg.mode = ProcessMode::Systemd;
    cfg.notify_socket = sock_path;
    ProcessManager::init(cfg);

    ProcessManager::notify_ready();

    char buf[256] = {};
    ssize_t n =
        recvfrom(recv_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT, nullptr, nullptr);
    EXPECT_GT(n, 0);
    if (n > 0) {
        std::string received(buf, static_cast<size_t>(n));
        EXPECT_EQ(received, "READY=1");
    }

    close(recv_fd);
    unlink(sock_path.c_str());
}
#endif
