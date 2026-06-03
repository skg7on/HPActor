// Smoke test: verify udp_transport.hpp compiles and FakeUdpTransport works
#include <hpactor/net/udp_transport.hpp>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <type_traits>
#include <utility>

namespace {

hpactor::Ipv4Endpoint make_local_endpoint(uint16_t port) {
    return hpactor::Ipv4Endpoint{0x7F000001, htons(port)};
}

TEST(FakeUdpTransportSmoke, IsDefaultConstructible) {
    hpactor::net::FakeUdpTransport fake;
    EXPECT_TRUE(fake.bind(9999));
}

TEST(FakeUdpTransportSmoke, SendCapturesPacket) {
    hpactor::net::FakeUdpTransport fake;
    hpactor::StreamBuffer data;
    const uint8_t ping[] = {'p', 'i', 'n', 'g'};
    data.append(ping, 4);
    hpactor::EndPoint dest = make_local_endpoint(4567);
    fake.send(data, dest);
    ASSERT_EQ(fake.sent_packets.size(), 1u);
    EXPECT_EQ(fake.sent_packets[0].dest, dest);
}

TEST(FakeUdpTransportSmoke, CloseClearsSentPackets) {
    hpactor::net::FakeUdpTransport fake;
    hpactor::StreamBuffer data;
    const uint8_t x = 'x';
    data.append(&x, 1);
    fake.send(data, make_local_endpoint(1));
    EXPECT_EQ(fake.sent_packets.size(), 1u);
    fake.close();
    EXPECT_TRUE(fake.sent_packets.empty());
}

TEST(FakeUdpTransportSmoke, InjectPacketCallsReceiveCallback) {
    hpactor::net::FakeUdpTransport fake;
    int count = 0;
    fake.set_receive_callback([&count](const hpactor::StreamBuffer&,
                                       const std::string&, uint16_t) { ++count; });
    hpactor::StreamBuffer data;
    const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
    data.append(hello, 5);
    fake.inject_packet(data, "127.0.0.1", 9000);
    EXPECT_EQ(count, 1);
}

TEST(FakeUdpTransportSmoke, IuDpTransportIsAbstract) {
    EXPECT_TRUE(std::is_abstract_v<hpactor::net::IUdpTransport>);
}

} // namespace
