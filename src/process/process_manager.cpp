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

#include <hpactor/process/process_config.hpp>
#include <hpactor/process/process_manager.hpp>

#include <cctype>
#include <cstring>
#include <string>

#ifdef __linux__
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

namespace hpactor::process {

ProcessConfig ProcessManager::config_{};
ProcessMode ProcessManager::mode_ = ProcessMode::Foreground;

ProcessMode ProcessConfig::parse_mode(const std::string& s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s)
        lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "systemd")
        return ProcessMode::Systemd;
    if (lower == "daemon")
        return ProcessMode::Daemon;
    return ProcessMode::Foreground;
}

std::string ProcessManager::format_notify_message(const std::string& msg) {
    std::string out;
    out.reserve(msg.size());
    for (char c : msg) {
        if (c == '\n' || c == '\r')
            continue;
        out.push_back(c);
    }
    return out;
}

result<void> ProcessManager::init(const ProcessConfig& config) {
    config_ = config;
    mode_ = config.mode;
    return result<void>::make();
}

void ProcessManager::notify_ready() {
    send_notify("READY=1");
}
void ProcessManager::notify_status(const std::string& status) {
    send_notify("STATUS=" + status);
}
void ProcessManager::notify_watchdog() {
    send_notify("WATCHDOG=1");
}
void ProcessManager::notify_stopping() {
    send_notify("STOPPING=1");
}
void ProcessManager::notify_stopped() {
    remove_pidfile();
}
ProcessMode ProcessManager::mode() {
    return mode_;
}

void ProcessManager::send_notify(const std::string& msg) {
    if (mode_ != ProcessMode::Systemd)
        return;
    const char* socket_path = config_.notify_socket.empty()
                                  ? getenv("NOTIFY_SOCKET")
                                  : config_.notify_socket.c_str();
    if (!socket_path || socket_path[0] == '\0')
        return;

    std::string clean = format_notify_message(msg);
    if (clean.empty())
        return;

#ifdef __linux__
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    sendto(fd, clean.c_str(), clean.size(), MSG_NOSIGNAL,
           reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    close(fd);
#endif
}

// Stubs for later tasks:
result<void> ProcessManager::install_signal_handlers(std::function<void()>,
                                                     std::function<void()>) {
    return result<void>::make();
}
int ProcessManager::wait_for_signal() {
    return -1;
}
void ProcessManager::daemonize() {}
void ProcessManager::write_pidfile() {}
void ProcessManager::remove_pidfile() {}

} // namespace hpactor::process
