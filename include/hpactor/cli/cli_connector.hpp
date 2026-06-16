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
#include <string>

namespace hpactor {
namespace cli {

/// \brief Non-blocking TCP/UDS socket factory with async connect via EventLoop.
///
/// Extracts socket-creation and async-connect logic from
/// \c TcpTransport::connect() and \c TcpTransport::complete_connect() to
/// provide reusable async-connect helpers for CLI clients that use
/// varint-length-prefixed protobuf framing (unlike TcpTransport which uses
/// WireFrameConnection with HPAC magic + big-endian length framing).
///
/// Internally creates a temporary \c net::EventLoop to perform the
/// non-blocking connect with write_handler + SO_ERROR completion detection
/// and a configurable timeout.
class CliConnector {
  public:
    CliConnector() = delete;

    /// \brief Create a non-blocking TCP socket and connect to host:port.
    ///
    /// Sets \c TCP_NODELAY and \c O_NONBLOCK, initiates \c ::connect(),
    /// and waits for the TCP handshake to complete via EventLoop
    /// write_handler + \c SO_ERROR.
    ///
    /// \param[in] host    Target hostname or IPv4 address.
    /// \param[in] port    Target TCP port.
    /// \param[in] timeout Maximum time to wait for connect completion.
    /// \return Connected fd (>= 0), or -1 on failure / timeout.
    static int connect_tcp(const std::string& host, uint16_t port,
                           std::chrono::milliseconds timeout);

    /// \brief Create a non-blocking Unix domain socket and connect to path.
    ///
    /// Sets \c O_NONBLOCK, initiates \c ::connect(), and waits for
    /// completion via EventLoop write_handler + \c SO_ERROR.
    ///
    /// \param[in] path    Unix domain socket path.
    /// \param[in] timeout Maximum time to wait for connect completion.
    /// \return Connected fd (>= 0), or -1 on failure / timeout.
    static int
    connect_uds(const std::string& path, std::chrono::milliseconds timeout);
};

} // namespace cli
} // namespace hpactor
