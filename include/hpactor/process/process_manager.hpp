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

#pragma once

#include <functional>
#include <hpactor/process/process_config.hpp>
#include <hpactor/types/types.hpp> // for result<T>
#include <string>

namespace hpactor::process {

class ProcessManager {
  public:
    ProcessManager() = delete;

    static result<void> init(const ProcessConfig& config);
    static void notify_ready();
    static void notify_status(const std::string& status);
    static void notify_watchdog();
    static void notify_stopping();
    static void notify_stopped();
    static ProcessMode mode();
    static std::string format_notify_message(const std::string& msg);
    static result<void> install_signal_handlers(std::function<void()> on_terminate,
                                                std::function<void()> on_reload);
    static int wait_for_signal();

  private:
    static void daemonize();
    static void write_pidfile();
    static void remove_pidfile();
    static void send_notify(const std::string& msg);

    static ProcessConfig config_;
    static ProcessMode mode_;
    static bool daemonized_;
    static bool pidfile_written_;
};

} // namespace hpactor::process
