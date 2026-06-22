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

#include <hpactor/adt/node_identity.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/udp_transport.hpp>

// FRIEND_TEST macro: self-contained definition compatible with gtest.
// This avoids a dependency on gtest/gtest_prod.h which may not be in the
// include path during library builds.  The class-name convention here matches
// what GTest's TEST_F() macro generates internally.
#ifndef FRIEND_TEST
#    define FRIEND_TEST(test_case_name, test_name)                             \
        friend class test_case_name##_##test_name##_Test
#endif

#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hpactor::net {

/// \brief SWIM gossip protocol message types.
enum class GossipMessageType : uint8_t {
    Ping = 0x01,        ///< Direct ping probe.
    Ack = 0x02,         ///< Direct ack response.
    PingReq = 0x03,     ///< Indirect ping request via a proxy.
    IndirectAck = 0x04, ///< Indirect ack response via a proxy.
    Join = 0x05,        ///< Join request to a seed node.
    SyncRsp = 0x06,     ///< Full membership sync response.
    Leave = 0x07,       ///< Graceful leave announcement.
};

/// \brief Piggyback entry types for membership dissemination.
enum class PiggybackType : uint8_t {
    Alive = 0x01,      ///< Member is alive (or suspected resolved).
    Suspicious = 0x02, ///< Member is suspected of failure.
    Dead = 0x03,       ///< Member is confirmed dead.
    Metadata = 0x04,   ///< Member metadata update (actor types, load).
};

/// \brief A piggyback entry carried in gossip messages.
///
/// Attached to Ping/Ack messages to disseminate membership state
/// changes without additional round-trips.
struct PiggybackEntry {
    /// \brief Type of piggyback entry.
    PiggybackType type;
    /// \brief Member identity.
    NodeIdentity identity;
    /// \brief Incarnation number (higher wins).
    uint64_t incarnation;
    /// \brief Actor types this member hosts (\c Metadata only).
    std::vector<std::string> actor_types;
    /// \brief Current load metric (\c Metadata only).
    uint32_t load = 0;
};

/// \brief SWIM gossip protocol configuration.
struct GossipConfig {
    /// \brief UDP port for gossip messages (default 5354).
    uint16_t gossip_port = 5354;
    /// \brief Interval between protocol rounds (default 1 s).
    std::chrono::milliseconds protocol_period{1000};
    /// \brief Timeout for direct ping responses (default 200 ms).
    std::chrono::milliseconds ping_timeout{200};
    /// \brief Suspicion timeout before declaring dead (default 3 s).
    std::chrono::milliseconds suspicion_timeout{3000};
    /// \brief Time before purging dead member tombstones (default 30 s).
    std::chrono::milliseconds dead_timeout{30000};
    /// \brief Number of peers to ping per round (default 3).
    uint32_t fanout = 3;
    /// \brief Number of indirect probes to request (default 3).
    uint32_t indirect_probes = 3;
    /// \brief Seed endpoints for initial cluster join.
    std::vector<EndPoint> seeds;
    /// \brief Local member state.
    ///
    /// Only \c endpoint, \c host, \c tcp_port, \c uds_path,
    /// \c acceptors, and \c actor_types are config. \c incarnation,
    /// \c status, and \c last_seen are set at startup.
    Member local_state;
};

/// \brief Magic number for gossip wire protocol ("HPGC").
constexpr uint32_t GossipMagic = 0x48504743;
/// \brief Wire protocol version.
constexpr uint8_t GossipVersion = 0x01;
/// \brief Maximum UDP gossip message size (1400 bytes to avoid
/// fragmentation).
constexpr size_t kGossipMaxMsgSize = 1400;

/// \brief Tracks an in-flight ping to a peer.
struct PendingPing {
    /// \brief When the direct ping expires.
    std::chrono::steady_clock::time_point expires_at;
    /// \brief Whether an indirect ping was also requested.
    bool indirect_requested = false;
    /// \brief When the indirect ping expires.
    ///
    /// Indirect timeout = direct timeout + ping_timeout (same timeout).
    std::chrono::steady_clock::time_point indirect_expires_at;
};

/// \brief SWIM gossip-based cluster membership implementation.
///
/// Implements \c IServiceDiscovery using the SWIM (Scalable Weakly-consistent
/// Infection-style process group Membership) protocol over UDP. Provides
/// decentralized failure detection with configurable suspicion/dead
/// timeouts, incarnation-based conflict resolution, piggyback
/// dissemination, and tombstone purging.
///
/// \note Thread safety: Member access is protected by a shared mutex.
///       \c discover_all() and \c discover() take a read lock.
///       Modifications take a write lock.
class GossipMembership : public IServiceDiscovery {
  public:
    /// \brief Construct with an event loop (creates a \c RealUdpTransport).
    ///
    /// \param[in] cfg Gossip configuration.
    /// \param[in] loop Event loop for async UDP I/O.
    GossipMembership(const GossipConfig& cfg, EventLoop* loop);

    /// \brief Construct with a custom UDP transport (for testing).
    ///
    /// \param[in] cfg Gossip configuration.
    /// \param[in] transport Custom transport (e.g., \c FakeUdpTransport).
    GossipMembership(const GossipConfig& cfg,
                     std::unique_ptr<IUdpTransport> transport);
    ~GossipMembership() override;

    /// \brief Start the protocol timer and join the cluster.
    void start() override;

    /// \brief Stop the protocol timer and leave the cluster.
    void stop() override;

    /// \brief Return a snapshot of all known members.
    ///
    /// \return Copy of the current member list (read-locked).
    std::vector<Member> discover_all() const override;

    /// \brief Look up a member by endpoint.
    ///
    /// \param[in] ep Endpoint to search for.
    /// \return Pointer to the member, or \c nullptr if not found.
    const Member* discover(EndPoint ep) const override;

    /// \brief Announce this node's presence (bumps incarnation).
    ///
    /// \param[in] m Local member state.
    void announce(Member m) override;

    /// \brief Register a membership change callback.
    ///
    /// \param[in] cb Callback invoked on join/leave.
    void on_member_change(MemberChangeCallback cb) override;

    std::string backend_name() const override {
        return "gossip";
    }
    const std::unordered_map<EndPoint, Member>* raw_members() const override {
        return &members_;
    }

    // GTest FRIEND_TEST declarations to replace #define private public hack
#ifdef FRIEND_TEST
    FRIEND_TEST(GossipMembershipTest, ConstructionDefaults);
    FRIEND_TEST(GossipMembershipTest, BootstrapSoloCluster);
    FRIEND_TEST(GossipMembershipTest, AnnounceBumpsIncarnation);
    FRIEND_TEST(GossipMembershipTest, DiscoverAllReturnsCopy);
    FRIEND_TEST(GossipMembershipTest, MergeMemberNoneToAlive);
    FRIEND_TEST(GossipMembershipTest, MergeMemberUpdateHigher);
    FRIEND_TEST(GossipMembershipTest, MergeMemberIgnoreLower);
    FRIEND_TEST(GossipMembershipTest, MergeMemberDeadToAlive);
    FRIEND_TEST(GossipMembershipTest, MarkSuspiciousDeadTransitions);
    FRIEND_TEST(GossipMembershipTest, PickRandomPeersAllAvailable);
    FRIEND_TEST(GossipMembershipTest, PickRandomPeersEmptySolo);
    FRIEND_TEST(GossipMembershipTest, PurgeDeadTombstones);
    FRIEND_TEST(GossipMembershipTest, WireEncodeDecodePing);
    FRIEND_TEST(GossipMembershipTest, WireEncodeDecodeMetadata);

    // Integration tests (protocol flow)
    FRIEND_TEST(GossipProtocolIntegrationTest, JoinFlow);
    FRIEND_TEST(GossipProtocolIntegrationTest, ProtocolRoundPingAck);
    FRIEND_TEST(GossipProtocolIntegrationTest, IndirectProbePingReq);
    FRIEND_TEST(GossipProtocolIntegrationTest, FailureDetectionSuspicious);
    FRIEND_TEST(GossipProtocolIntegrationTest, FailureDetectionDead);
    FRIEND_TEST(GossipProtocolIntegrationTest, LeavePropagation);
    FRIEND_TEST(GossipProtocolIntegrationTest, PiggybackDissemination);
    FRIEND_TEST(GossipProtocolIntegrationTest, IncarnationConflictResolution);
    FRIEND_TEST(GossipProtocolIntegrationTest, MemberChangeCallback);
    FRIEND_TEST(GossipProtocolIntegrationTest, TombstonePurging);
    FRIEND_TEST(GossipProtocolIntegrationTest, FaultInjectionPacketLoss);
    FRIEND_TEST(GossipProtocolIntegrationTest, FailureDetectionEndToEnd);
#endif

  private:
    void protocol_round();
    void handle_packet(const StreamBuffer& data, const std::string& from_host,
                       uint16_t from_port);

    // Message handlers
    void handle_ping(EndPoint sender, uint64_t inc, uint32_t seq,
                     std::vector<PiggybackEntry> pb, const std::string& host,
                     uint16_t port);
    void handle_ack(EndPoint sender, uint64_t inc, std::vector<PiggybackEntry> pb);
    void handle_ping_req(EndPoint sender, EndPoint target);
    void handle_indirect_ack(EndPoint sender, EndPoint target);
    void handle_join(EndPoint sender, uint64_t inc, std::vector<PiggybackEntry> pb);
    void handle_sync_rsp(std::vector<Member> members);
    void handle_leave(EndPoint sender, uint64_t inc);

    // Message sending
    void send_ping(EndPoint target);
    void send_ack(EndPoint target, std::vector<PiggybackEntry> pb);
    void send_ping_req(EndPoint proxy, EndPoint target);
    void send_indirect_ack(EndPoint target, EndPoint orig_target);
    void send_join(EndPoint seed);
    void send_sync_rsp(EndPoint target);
    void send_leave(EndPoint target);

    // Wire protocol encode/decode
    StreamBuffer encode_message(GossipMessageType type, uint64_t inc,
                                uint32_t seq, EndPoint ping_target,
                                const std::vector<PiggybackEntry>& pb) const;
    bool
    decode_message(const StreamBuffer& data, GossipMessageType& type,
                   EndPoint& sender, uint64_t& inc, uint32_t& seq,
                   EndPoint& ping_target, std::vector<PiggybackEntry>& pb) const;
    StreamBuffer encode_sync_rsp(const std::vector<Member>& members) const;
    bool
    decode_sync_rsp(const StreamBuffer& data, std::vector<Member>& members) const;

    // State mutations
    void mark_suspicious(EndPoint ep);
    void mark_dead(EndPoint ep);
    void merge_member(const Member& remote);
    void apply_piggyback(const std::vector<PiggybackEntry>& entries);
    void purge_dead_tombstones();
    std::vector<EndPoint>
    pick_random_peers(size_t count, std::unordered_set<EndPoint> exclude = {});

    GossipConfig config_;
    EventLoop* loop_ = nullptr;
    std::unique_ptr<IUdpTransport> transport_;
    uint64_t incarnation_ = 0;
    uint32_t seq_no_ = 0;

    std::unordered_map<EndPoint, Member> members_;
    std::unordered_map<EndPoint, PendingPing> pending_pings_;
    MemberChangeCallback member_change_cb_;
    uint64_t protocol_timer_ = 0;
    uint64_t join_retry_timer_ = 0;
    size_t join_seed_index_ = 0;
    bool needs_dissemination_ = false;
    mutable std::shared_mutex members_mutex_;
    mutable std::mt19937 rng_;
    bool rng_seeded_ = false;
    std::unordered_map<EndPoint, EndPoint> forwarded_pings_;
};

} // namespace hpactor::net
