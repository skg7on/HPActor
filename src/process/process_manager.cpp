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
#include <csignal>
#include <cstring>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#ifdef __linux__
#    include <sys/signalfd.h>
#endif

namespace hpactor::process {

ProcessConfig ProcessManager::config_{};
ProcessMode ProcessManager::mode_ = ProcessMode::Foreground;

namespace {
#ifdef __linux__
int signal_fd_ = -1;
#endif
std::function<void()> on_terminate_fn_;
std::function<void()> on_reload_fn_;
} // anonymous namespace

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
    if (mode_ == ProcessMode::Daemon) {
        daemonize(); // does not return in parent
    }
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

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    // Close-on-exec (portable — SOCK_CLOEXEC is Linux-only).
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0)
        fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);

#ifdef __APPLE__
    // macOS: suppress SIGPIPE on sendto to a closed socket.
    int nosigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    sendto(fd, clean.c_str(), clean.size(),
#ifdef MSG_NOSIGNAL
           MSG_NOSIGNAL,
#else
           0,
#endif
           reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    close(fd);
}

// --- Daemonization ---

void ProcessManager::daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        _exit(1);
    }
    if (pid > 0) {
        _exit(0);
    }

    if (setsid() < 0) {
        _exit(1);
    }

    pid = fork();
    if (pid < 0) {
        _exit(1);
    }
    if (pid > 0) {
        _exit(0);
    }

    if (!config_.working_directory.empty()) {
        chdir(config_.working_directory.c_str());
    }
    umask(0);

    if (config_.redirect_stdio) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
    }

    write_pidfile();
}

void ProcessManager::write_pidfile() {
    if (config_.pidfile_path.empty()) {
        return;
    }
    std::string dir;
    auto slash = config_.pidfile_path.rfind('/');
    if (slash != std::string::npos) {
        dir = config_.pidfile_path.substr(0, slash);
        mkdir(dir.c_str(), 0755);
    }
    std::string tmp = config_.pidfile_path + ".tmp";
    {
        std::ofstream ofs(tmp);
        if (!ofs) {
            return;
        }
        ofs << getpid() << "\n" << std::flush;
    }
    rename(tmp.c_str(), config_.pidfile_path.c_str());
}

void ProcessManager::remove_pidfile() {
    if (!config_.pidfile_path.empty()) {
        unlink(config_.pidfile_path.c_str());
    }
}

// --- Signal Handling ---

result<void>
ProcessManager::install_signal_handlers(std::function<void()> on_terminate,
                                        std::function<void()> on_reload) {
    on_terminate_fn_ = std::move(on_terminate);
    on_reload_fn_ = std::move(on_reload);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);

    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        return result<void>::make(
            error(errors::invalid_argument, "Failed to block signals"));
    }

#ifdef __linux__
    signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ < 0) {
        return result<void>::make(
            error(errors::invalid_argument, "Failed to create signalfd"));
    }
#endif
    return result<void>::make();
}

int ProcessManager::wait_for_signal() {
#ifdef __linux__
    if (signal_fd_ < 0) {
        return -1;
    }
    struct signalfd_siginfo ssi{};
    ssize_t n = read(signal_fd_, &ssi, sizeof(ssi));
    if (n != static_cast<ssize_t>(sizeof(ssi))) {
        return -1;
    }
    switch (ssi.ssi_signo) {
        case SIGTERM:
        case SIGINT:
            if (on_terminate_fn_) {
                on_terminate_fn_();
            }
            break;
        case SIGHUP:
            if (on_reload_fn_) {
                on_reload_fn_();
            }
            break;
    }
    return static_cast<int>(ssi.ssi_signo);
#else
    return -1;
#endif
}

} // namespace hpactor::process
