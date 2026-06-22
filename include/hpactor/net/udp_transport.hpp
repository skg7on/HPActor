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

#include <hpactor/net/event_loop.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hpactor::net {

/// \brief Abstract UDP transport interface.
///
/// Decouples gossip membership from the concrete UDP I/O implementation,
/// enabling test doubles and platform-specific transports.
///
/// \note Thread safety: Called from the event loop thread.
class IUdpTransport {
  public:
    virtual ~IUdpTransport() = default;

    /// \brief Bind to a local UDP port.
    ///
    /// \param[in] port Port number to bind.
    /// \return \c true on success.
    virtual bool bind(uint16_t port) = 0;

    /// \brief Send a datagram.
    ///
    /// \param[in] data Payload to send.
    /// \param[in] dest Destination endpoint.
    virtual void send(const StreamBuffer& data, const EndPoint& dest) = 0;

    /// \brief Close the socket and release resources.
    virtual void close() = 0;

    /// \brief Callback for incoming datagrams.
    ///
    /// \param[in] data Received payload.
    /// \param[in] host Source hostname/IP.
    /// \param[in] port Source port.
    using ReceiveCallback =
        std::function<void(const StreamBuffer&, const std::string&, uint16_t)>;

    /// \brief Register a callback for incoming datagrams.
    ///
    /// \param[in] cb Callback invoked on each received datagram.
    /// \note Only one callback is stored; subsequent calls replace the
    ///       previous registration.
    virtual void set_receive_callback(ReceiveCallback cb) = 0;
};

/// \brief Test double for \c IUdpTransport.
///
/// Records sent packets in \c sent_packets and allows injection of
/// received packets via \c inject_packet(). Used in gossip membership
/// unit tests.
class FakeUdpTransport : public IUdpTransport {
  public:
    /// \brief A recorded sent datagram.
    struct SentPacket {
        StreamBuffer data;
        EndPoint dest;
    };

    /// \brief History of all sent packets (cleared on \c close() or
    ///        \c clear_sent()).
    std::vector<SentPacket> sent_packets;

    bool bind(uint16_t /*port*/) override {
        return true;
    }
    void send(const StreamBuffer& data, const EndPoint& dest) override {
        sent_packets.push_back({data, dest});
    }
    void close() override {
        sent_packets.clear();
    }
    void set_receive_callback(ReceiveCallback cb) override {
        receive_cb_ = std::move(cb);
    }

    /// \brief Simulate an incoming datagram.
    ///
    /// Invokes the registered \c ReceiveCallback synchronously.
    /// \param[in] data Payload.
    /// \param[in] src_host Source hostname.
    /// \param[in] src_port Source port.
    void inject_packet(const StreamBuffer& data, const std::string& src_host,
                       uint16_t src_port) {
        if (receive_cb_) {
            receive_cb_(data, src_host, src_port);
        }
    }

    /// \brief Clear the sent packet history.
    void clear_sent() {
        sent_packets.clear();
    }

  private:
    ReceiveCallback receive_cb_;
};

/// \brief Production UDP transport using kernel sockets.
///
/// Binds a UDP socket and performs async I/O via the provided
/// \c EventLoop.
///
/// \note Thread safety: Called from the event loop thread.
class RealUdpTransport : public IUdpTransport {
  public:
    /// \brief Construct with an event loop for async I/O.
    ///
    /// \param[in] loop Owning \c EventLoop (must outlive this transport).
    explicit RealUdpTransport(EventLoop* loop);
    ~RealUdpTransport() override;

    bool bind(uint16_t port) override;
    void send(const StreamBuffer& data, const EndPoint& dest) override;
    void close() override;
    void set_receive_callback(ReceiveCallback cb) override;

  private:
    EventLoop* loop_;
    int sock_ = -1;
    ReceiveCallback receive_cb_;
    std::vector<uint8_t> recv_buffer_;
};

} // namespace hpactor::net
