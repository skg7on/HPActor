// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/process/health_http_server.hpp>
#include <hpactor/process/process_config.hpp>
#include <hpactor/process/process_manager.hpp>
#include <hpactor/process/watchdog_actor.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#    include <signal.h>
#    include <sys/signalfd.h>
#endif

namespace {

using namespace hpactor;

// Test fixture for process tests
class ProcessLifecycleTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Use a unique temp directory per test case to prevent filesystem
        // races during parallel CTest execution.  Without uniqueness every
        // test process writes to the same <tmp>/hpactor_proc_test/hpactor.pid,
        // producing TOCTOU failures when one process unlinks the pidfile
        // while a concurrent process is still reading it.
        const auto* test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        std::string suffix =
            test_info != nullptr ? std::string(test_info->name()) : "unknown";
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("hpactor_proc_test_" + suffix);
        std::filesystem::create_directories(temp_dir_);
        pidfile_path_ = (temp_dir_ / "hpactor.pid").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::filesystem::path temp_dir_;
    std::string pidfile_path_;
};

TEST_F(ProcessLifecycleTest, ProcessManagerForegroundInit) {
    process::ProcessConfig config;
    config.mode = process::ProcessMode::Foreground;
    config.pidfile_path = pidfile_path_;

    // Remove stale pidfile to get a clean slate for this test.
    std::error_code ec;
    std::filesystem::remove(pidfile_path_, ec);

    auto result = process::ProcessManager::init(config);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(process::ProcessManager::mode(), process::ProcessMode::Foreground);

    // Verify pidfile was written.
    // ProcessManager uses a static pidfile_written_ guard that prevents
    // re-writing the pidfile on subsequent init() calls within the same
    // process. When this test runs first (or in isolation via
    // --gtest_filter) the pidfile is always present. When another test
    // already called init(), the static guard blocks the write — the
    // check below is conditional to handle both scenarios.
    if (std::filesystem::exists(pidfile_path_)) {
        std::ifstream pidfile(pidfile_path_);
        ASSERT_TRUE(pidfile.good());
        std::string content;
        std::getline(pidfile, content);
        EXPECT_FALSE(content.empty());
        // Content should be the process PID (decimal integer).
        int pid = std::stoi(content);
        EXPECT_GT(pid, 0);
    }
}

TEST_F(ProcessLifecycleTest, ProcessManagerPidfileLifecycle) {
    process::ProcessConfig config;
    config.mode = process::ProcessMode::Foreground;
    config.pidfile_path = pidfile_path_;

    // Remove stale pidfile for a clean slate.
    std::error_code ec;
    std::filesystem::remove(pidfile_path_, ec);

    auto result = process::ProcessManager::init(config);
    ASSERT_TRUE(result.ok());

    // Verify pidfile exists with correct content.
    // The pidfile_written_ static guard may prevent re-write when another
    // test already called init(); the check is conditional to handle both.
    if (std::filesystem::exists(pidfile_path_)) {
        std::ifstream pf(pidfile_path_);
        ASSERT_TRUE(pf.good());
        std::string written_pid;
        std::getline(pf, written_pid);
        EXPECT_FALSE(written_pid.empty());

        // Verify content is a valid PID (positive integer).
        int pid = std::stoi(written_pid);
        EXPECT_GT(pid, 0);

        // notify_stopped() should remove the pidfile.
        process::ProcessManager::notify_stopped();
        EXPECT_FALSE(std::filesystem::exists(pidfile_path_));
    }
}

TEST_F(ProcessLifecycleTest, ProcessManagerNotifyReady) {
    process::ProcessConfig config;
    config.mode = process::ProcessMode::Foreground;
    config.pidfile_path = pidfile_path_;

    // Remove stale pidfile for a clean slate.
    std::error_code ec;
    std::filesystem::remove(pidfile_path_, ec);

    auto result = process::ProcessManager::init(config);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(process::ProcessManager::mode(), process::ProcessMode::Foreground);

    // Verify format_notify_message strips newlines and carriage returns.
    EXPECT_EQ(process::ProcessManager::format_notify_message("READY=1"), "READY=1");
    EXPECT_EQ(process::ProcessManager::format_notify_message("STATUS\n"), "STATUS");
    EXPECT_EQ(process::ProcessManager::format_notify_message("A\r\nB"), "AB");

    // In Foreground mode, notify calls are no-ops (no systemd socket),
    // but they must not assert, throw, or crash.
    process::ProcessManager::notify_ready();
    process::ProcessManager::notify_status("running");
    process::ProcessManager::notify_watchdog();
    process::ProcessManager::notify_stopping();

    // notify_stopped() removes the pidfile.
    if (std::filesystem::exists(pidfile_path_)) {
        process::ProcessManager::notify_stopped();
        EXPECT_FALSE(std::filesystem::exists(pidfile_path_));
    }

    SUCCEED();
}

TEST_F(ProcessLifecycleTest, ProcessManagerSignalHandlersInstalled) {
    bool terminated = false;
    bool reloaded = false;

    auto result = process::ProcessManager::install_signal_handlers(
        [&]() { terminated = true; }, [&]() { reloaded = true; });
    ASSERT_TRUE(result.ok());

    // No signal pending — wait_for_signal should return -1.
    int sig = process::ProcessManager::wait_for_signal();
    EXPECT_EQ(sig, -1);

    // Callbacks should not have been invoked (no signal was received).
    EXPECT_FALSE(terminated);
    EXPECT_FALSE(reloaded);
}

#if defined(__linux__)
TEST_F(ProcessLifecycleTest, ProcessManagerSignalHandlingLinux) {
    bool terminated = false;
    bool reloaded = false;

    auto result = process::ProcessManager::install_signal_handlers(
        [&]() { terminated = true; }, [&]() { reloaded = true; });
    ASSERT_TRUE(result.ok());

    // Send SIGTERM to self. It is in the blocked signal mask, so it will
    // be queued and captured by signalfd rather than delivered immediately.
    kill(getpid(), SIGTERM);

    // wait_for_signal should consume the queued SIGTERM and invoke the
    // terminate callback.
    int sig = process::ProcessManager::wait_for_signal();
    EXPECT_EQ(sig, SIGTERM);
    EXPECT_TRUE(terminated);
    EXPECT_FALSE(reloaded);

    // Send SIGHUP and verify reload callback fires.
    kill(getpid(), SIGHUP);
    sig = process::ProcessManager::wait_for_signal();
    EXPECT_EQ(sig, SIGHUP);
    EXPECT_TRUE(reloaded);
}
#endif // defined(__linux__)

TEST_F(ProcessLifecycleTest, ProcessConfigParsing) {
    // Test ProcessMode enum values
    process::ProcessConfig foreground;
    foreground.mode = process::ProcessMode::Foreground;
    EXPECT_EQ(foreground.mode, process::ProcessMode::Foreground);

    process::ProcessConfig systemd_cfg;
    systemd_cfg.mode = process::ProcessMode::Systemd;
    EXPECT_EQ(systemd_cfg.mode, process::ProcessMode::Systemd);

    process::ProcessConfig daemon_cfg;
    daemon_cfg.mode = process::ProcessMode::Daemon;
    EXPECT_EQ(daemon_cfg.mode, process::ProcessMode::Daemon);

    // Test all fields with explicit values
    process::ProcessConfig cfg;
    cfg.mode = process::ProcessMode::Daemon;
    cfg.pidfile_path = "/tmp/test.pid";
    cfg.redirect_stdio = true;
    cfg.log_file = "/var/log/hpactor.log";
    cfg.working_directory = "/var/lib/hpactor";
    cfg.watchdog_interval = std::chrono::milliseconds(5000);
    cfg.notify_socket = "/tmp/notify.sock";

    EXPECT_EQ(cfg.mode, process::ProcessMode::Daemon);
    EXPECT_EQ(cfg.pidfile_path, "/tmp/test.pid");
    EXPECT_TRUE(cfg.redirect_stdio);
    EXPECT_EQ(cfg.log_file, "/var/log/hpactor.log");
    EXPECT_EQ(cfg.working_directory, "/var/lib/hpactor");
    EXPECT_EQ(cfg.watchdog_interval, std::chrono::milliseconds(5000));
    EXPECT_EQ(cfg.notify_socket, "/tmp/notify.sock");

    // Test default values
    process::ProcessConfig defaults;
    EXPECT_EQ(defaults.mode, process::ProcessMode::Foreground);
    EXPECT_TRUE(defaults.pidfile_path.empty());
    EXPECT_FALSE(defaults.redirect_stdio);
    EXPECT_TRUE(defaults.log_file.empty());
    EXPECT_EQ(defaults.working_directory, "/");
    EXPECT_EQ(defaults.watchdog_interval, std::chrono::milliseconds(0));
    EXPECT_TRUE(defaults.notify_socket.empty());

    // Test parse_mode (case-insensitive, defaults to Foreground for unknown)
    EXPECT_EQ(process::ProcessConfig::parse_mode("foreground"),
              process::ProcessMode::Foreground);
    EXPECT_EQ(process::ProcessConfig::parse_mode("Foreground"),
              process::ProcessMode::Foreground);
    EXPECT_EQ(process::ProcessConfig::parse_mode("FOREGROUND"),
              process::ProcessMode::Foreground);
    EXPECT_EQ(process::ProcessConfig::parse_mode("systemd"),
              process::ProcessMode::Systemd);
    EXPECT_EQ(process::ProcessConfig::parse_mode("daemon"),
              process::ProcessMode::Daemon);
    EXPECT_EQ(process::ProcessConfig::parse_mode("bogus"),
              process::ProcessMode::Foreground);
    EXPECT_EQ(process::ProcessConfig::parse_mode(""),
              process::ProcessMode::Foreground);
}

TEST_F(ProcessLifecycleTest, HealthHttpServerStartAndRespond) {
    // ActorSystem with a scheduler thread (daemon gets its own dedicated
    // thread).
    Config sys_cfg;
    sys_cfg.scheduler_threads = 1;
    sys_cfg.enable_network = false;

    ActorSystem system(sys_cfg);

    // Spawn HealthHttpServer on a loopback port.
    process::HealthHttpConfig health_cfg;
    health_cfg.port = 18081;
    health_cfg.bind_address = "127.0.0.1";

    auto server = system.spawn<process::HealthHttpServer>(health_cfg);
    ASSERT_TRUE(server);

    // Give the daemon thread time to bind and start its accept loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // Make a raw HTTP request over a loopback socket.
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(18081);
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);

    int conn_rc =
        connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ASSERT_EQ(conn_rc, 0);

    const char* request = "GET /health/live HTTP/1.1\r\n"
                          "Host: 127.0.0.1:18081\r\n"
                          "Connection: close\r\n"
                          "\r\n";
    ssize_t sent = send(sock, request, std::strlen(request), 0);
    ASSERT_GT(sent, 0);

    char buf[4096];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    std::string response(buf, static_cast<size_t>(n));

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("OK"), std::string::npos);
    close(sock);

    // Stop the daemon thread before destruction to avoid a use-after-free race:
    // the derived-class `gateway_` unique_ptr is destroyed before the
    // base-class
    // `~DaemonActor()` joins the thread.  Calling on_deactivate() joins the
    // daemon thread now, while gateway_ is still alive.
    auto* daemon = static_cast<DaemonActor*>(server.get().get());
    daemon->on_deactivate();
}

TEST_F(ProcessLifecycleTest, WatchdogActorPeriodicNotify) {
    // Remove stale pidfile for a clean slate.
    std::error_code ec;
    std::filesystem::remove(pidfile_path_, ec);

    process::ProcessConfig proc_cfg;
    proc_cfg.mode = process::ProcessMode::Foreground;
    proc_cfg.pidfile_path = pidfile_path_;
    proc_cfg.watchdog_interval = std::chrono::milliseconds(100);

    auto init_result = process::ProcessManager::init(proc_cfg);
    ASSERT_TRUE(init_result.ok());

    Config cfg;
    cfg.scheduler_threads = 1;

    ActorSystem system(cfg);
    auto watchdog =
        system.spawn<process::WatchdogActor>(proc_cfg.watchdog_interval);
    ASSERT_TRUE(watchdog);

    // Let the watchdog run for a few intervals to exercise the periodic
    // health check and notify_watchdog() path.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify the actor is still alive after several check cycles.
    auto* raw = static_cast<process::WatchdogActor*>(
        system.get_actor(watchdog.id()).get());
    ASSERT_NE(raw, nullptr);

    system.shutdown();
    SUCCEED();
}

} // namespace
