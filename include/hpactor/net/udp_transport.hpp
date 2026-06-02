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

// IUdpTransport — abstract UDP I/O

class IUdpTransport {
  public:
    virtual ~IUdpTransport() = default;

    virtual bool bind(uint16_t port) = 0;
    virtual void send(const StreamBuffer& data, const EndPoint& dest) = 0;
    virtual void close() = 0;

    using ReceiveCallback =
        std::function<void(const StreamBuffer&, const std::string&, uint16_t)>;
    virtual void set_receive_callback(ReceiveCallback) = 0;
};

// FakeUdpTransport — test double

class FakeUdpTransport : public IUdpTransport {
  public:
    struct SentPacket {
        StreamBuffer data;
        EndPoint dest;
    };
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

    void inject_packet(const StreamBuffer& data, const std::string& src_host,
                       uint16_t src_port) {
        if (receive_cb_) {
            receive_cb_(data, src_host, src_port);
        }
    }
    void clear_sent() {
        sent_packets.clear();
    }

  private:
    ReceiveCallback receive_cb_;
};

// RealUdpTransport — production UDP I/O

class RealUdpTransport : public IUdpTransport {
  public:
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
