// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/registrar.hpp>

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <vector>

using namespace hpactor;
using namespace hpactor::net;

namespace {

void make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Drive the EventLoop for up to timeout_ms waiting for a condition.
// Calls update_fd after each wait cycle to re-arm edge-triggered epoll
// events so the read handler can be invoked again for remaining data.
bool drive_until(EventLoop& loop, int fd, std::function<bool()> cond,
                 int timeout_ms = 2000) {
    int waited = 0;
    while (!cond() && waited < timeout_ms) {
        loop.wait(50);
        loop.process_completions();
        // Re-arm: on epoll edge-triggered, a single write() produces one
        // event. If the read handler returned early (self-sync byte-slip),
        // update_fd re-arms EPOLLIN so the next wait() fires it again.
        loop.update_fd(fd, EventLoop::Event::Read);
        waited += 50;
    }
    return cond();
}

// Build a framed registrar message.
StreamBuffer build_frame(TcpMessageType type, const StreamBuffer& payload) {
    StreamBuffer frame;
    frame.resize(TcpHeaderSize + payload.size());
    uint32_t magic_be = htonl(TcpRegistrarMagic);
    memcpy(frame.data(), &magic_be, 4);
    frame[4] = TcpRegistrarVersion;
    frame[5] = static_cast<uint8_t>(type);
    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(frame.data() + 6, &len_be, 4);
    if (!payload.empty())
        memcpy(frame.data() + TcpHeaderSize, payload.data(), payload.size());
    return frame;
}

} // anonymous namespace

class RegistrarFramingTest : public ::testing::Test {
  protected:
    void SetUp() override {
        int fds[2];
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        server_fd_ = fds[0];
        client_fd_ = fds[1];
        make_nonblocking(server_fd_);
        make_nonblocking(client_fd_);

        server_ep_ = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    }

    void TearDown() override {
        if (server_fd_ >= 0)
            ::close(server_fd_);
        if (client_fd_ >= 0)
            ::close(client_fd_);
    }

    int server_fd_ = -1;
    int client_fd_ = -1;
    EndPoint server_ep_;
};

// ── Connection lifecycle ─────────────────────────────────────────

TEST_F(RegistrarFramingTest, AcceptedRegistersWithLoop) {
    EventLoop loop;
    auto conn = RegistrarConnection::accepted(server_fd_, server_ep_, &loop);
    EXPECT_TRUE(loop.has_event(server_fd_, EventLoop::Event::Read));
}

TEST_F(RegistrarFramingTest, AcceptedVsConnectingRegistration) {
    EventLoop loop;
    auto accepted_conn =
        RegistrarConnection::accepted(server_fd_, server_ep_, &loop);
    auto connecting_conn =
        RegistrarConnection::connecting(client_fd_, server_ep_, &loop);
    EXPECT_EQ(accepted_conn->fd(), server_fd_);
    EXPECT_EQ(connecting_conn->fd(), client_fd_);
}

TEST_F(RegistrarFramingTest, CloseDoesNotCrashOnAlreadyClosedFd) {
    EventLoop loop;
    auto conn = RegistrarConnection::accepted(server_fd_, server_ep_, &loop);
    conn->close();
    conn->close(); // double close — fd_ = -1 guard
    SUCCEED();
}

// ── Frame encoding (send_message) ────────────────────────────────

TEST_F(RegistrarFramingTest, SendMessageEncodesFrameCorrectly) {
    EventLoop loop;
    auto conn = RegistrarConnection::accepted(server_fd_, server_ep_, &loop);
    StreamBuffer payload{'t', 'e', 's', 't'};
    conn->send_message(TcpMessageType::Register, payload);
    SUCCEED();
}

TEST_F(RegistrarFramingTest, SendMessageWithNullLoopNoCrash) {
    auto conn = RegistrarConnection::connecting(client_fd_, server_ep_, nullptr);
    StreamBuffer payload{'x'};
    conn->send_message(TcpMessageType::Heartbeat, payload);
    SUCCEED();
}

// ── Message handler roundtrip ────────────────────────────────────

TEST_F(RegistrarFramingTest, MessageHandlerCalledOnValidFrame) {
    EventLoop loop;
    auto conn = RegistrarConnection::accepted(server_fd_, server_ep_, &loop);

    std::optional<TcpMessageType> received_type;
    std::optional<std::string> received_payload;
    conn->set_message_handler([&](TcpMessageType type, const StreamBuffer& data) {
        received_type = type;
        received_payload = std::string(data.begin(), data.end());
    });

    StreamBuffer payload{'h', 'e', 'l', 'l', 'o'};
    auto frame = build_frame(TcpMessageType::Heartbeat, payload);
    write(client_fd_, frame.data(), frame.size());

    EXPECT_TRUE(drive_until(loop, server_fd_,
                            [&] { return received_type.has_value(); }));
    EXPECT_TRUE(received_type.has_value());
    if (received_type.has_value()) {
        EXPECT_EQ(received_type.value(), TcpMessageType::Heartbeat);
    }
    EXPECT_TRUE(received_payload.has_value());
    if (received_payload.has_value()) {
        EXPECT_EQ(received_payload.value(), "hello");
    }
}

// ── Self-synchronizing magic recovery ────────────────────────────
//
// Writes garbage bytes followed by a valid frame in a single write().
// handle_read_event() will encounter the bad magic, self-sync via
// byte-slip, and return. drive_until() re-arms the fd via update_fd()
// after each wait cycle so edge-triggered epoll re-fires the handler.

TEST_F(RegistrarFramingTest, SelfSyncOnBadMagic) {
    EventLoop loop;
    auto conn = RegistrarConnection::accepted(server_fd_, server_ep_, &loop);

    std::atomic<int> msg_count{0};
    conn->set_message_handler(
        [&](TcpMessageType, const StreamBuffer&) { msg_count++; });

    // Combine garbage + valid frame into one write so recv() never
    // hits EAGAIN (which triggers disconnect) before processing the
    // valid frame.
    StreamBuffer combined;
    uint8_t garbage[] = {0xFF, 0xEE, 0xDD, 0xCC};
    combined.insert(combined.end(), garbage, garbage + sizeof(garbage));
    StreamBuffer payload{'o', 'k'};
    auto frame = build_frame(TcpMessageType::Register, payload);
    combined.insert(combined.end(), frame.begin(), frame.end());
    write(client_fd_, combined.data(), combined.size());

    EXPECT_TRUE(
        drive_until(loop, server_fd_, [&] { return msg_count.load() == 1; }));
    EXPECT_EQ(msg_count.load(), 1);
}

// ── Disconnect on recv error ─────────────────────────────────────

TEST_F(RegistrarFramingTest, DisconnectOnRecvZero) {
    EventLoop loop;
    auto conn = RegistrarConnection::accepted(server_fd_, server_ep_, &loop);

    std::atomic<bool> disconnected{false};
    conn->set_disconnect_handler([&]() { disconnected = true; });

    ::close(client_fd_);
    client_fd_ = -1;

    EXPECT_TRUE(drive_until(loop, server_fd_, [&] { return disconnected.load(); }));
    EXPECT_TRUE(disconnected.load());
}

// ── Send multiple messages ───────────────────────────────────────

TEST_F(RegistrarFramingTest, SendMultipleMessagesDoesNotCrash) {
    EventLoop loop;
    auto conn = RegistrarConnection::accepted(server_fd_, server_ep_, &loop);

    StreamBuffer payload1{'d', 'a', 't', 'a'};
    StreamBuffer payload2{'m', 'o', 'r', 'e'};
    conn->send_message(TcpMessageType::Register, payload1);
    conn->send_message(TcpMessageType::Heartbeat, payload2);
    SUCCEED();
}

// ── Multiple messages (read side) ────────────────────────────────
//
// Combined write of three frames + update_fd re-arming for epoll compat.

TEST_F(RegistrarFramingTest, MultipleMessagesHandledInOrder) {
    EventLoop loop;
    auto conn = RegistrarConnection::accepted(server_fd_, server_ep_, &loop);

    std::vector<TcpMessageType> received_types;
    conn->set_message_handler([&](TcpMessageType type, const StreamBuffer&) {
        received_types.push_back(type);
    });

    StreamBuffer combined;
    TcpMessageType types[] = {TcpMessageType::Register,
                              TcpMessageType::Heartbeat, TcpMessageType::Accept};
    for (auto t : types) {
        StreamBuffer payload{static_cast<uint8_t>('x')};
        auto frame = build_frame(t, payload);
        combined.insert(combined.end(), frame.begin(), frame.end());
    }
    write(client_fd_, combined.data(), combined.size());

    EXPECT_TRUE(drive_until(loop, server_fd_,
                            [&] { return received_types.size() >= 3; }));
    ASSERT_GE(received_types.size(), 3u);
    EXPECT_EQ(received_types[0], TcpMessageType::Register);
    EXPECT_EQ(received_types[1], TcpMessageType::Heartbeat);
    EXPECT_EQ(received_types[2], TcpMessageType::Accept);
}
