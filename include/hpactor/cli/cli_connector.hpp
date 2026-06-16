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

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace hpactor {

namespace net {
class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;
class TcpTransport;
} // namespace net

namespace cli {

/// \brief Lightweight wrapper around \c net::TcpTransport for CLI connections.
///
/// Uses \c TcpTransport::connect() and \c TcpTransport::connect_unix_domain()
/// for async socket creation and non-blocking connect completion, then
/// exposes the connected file descriptor for direct varint-prefixed
/// protobuf I/O (bypassing the WireFrameConnection layer).
class CliConnector {
  public:
    CliConnector();
    ~CliConnector();

    CliConnector(const CliConnector&) = delete;
    CliConnector& operator=(const CliConnector&) = delete;

    /// \brief Connect to host:port via TcpTransport::connect().
    /// \return Connected fd (>= 0), or -1 on failure / timeout.
    int connect_tcp(const std::string& host, uint16_t port,
                    std::chrono::milliseconds timeout);

    /// \brief Connect to a Unix domain socket via
    ///        TcpTransport::connect_unix_domain().
    /// \return Connected fd (>= 0), or -1 on failure / timeout.
    int connect_uds(const std::string& path, std::chrono::milliseconds timeout);

    /// \brief Close the connection and release transport resources.
    void disconnect();

    /// \brief The connected fd, or -1 if not connected.
    int fd() const {
        return fd_;
    }

  private:
    std::unique_ptr<net::TcpTransport> transport_;
    net::ConnectionPtr conn_;
    int fd_ = -1;
};

} // namespace cli
} // namespace hpactor
