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

#include <hpactor/cli/cli_types.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace hpactor::routing {

/// \brief Abstract strategy for selecting a routee from a pool.
///
/// Called by \c PoolRouter and \c GroupRouter for each incoming message.
/// All implementations are thread-compatible (routers execute on a single
/// scheduler thread per actor).
class IRoutingLogic {
  public:
    virtual ~IRoutingLogic() = default;

    /// \brief Select a routee index from the available routees.
    ///
    /// \param[in] routees The current list of routee \c ActorRefs.
    /// \param[in] msg The incoming typed message being routed.
    /// \param[in] routee_states Mailbox snapshots for each routee
    ///            (may be empty or contain fewer entries than routees;
    ///            missing entries are treated as depth=0).
    /// \return Index into \p routees (0 <= result < routees.size()),
    ///         or 0 when \p routees is empty.
    [[nodiscard]] virtual size_t
    select_routee(const std::vector<ActorRef>& routees, const TypedMessage& msg,
                  const std::vector<cli::MboxSnapshot>& routee_states) = 0;

    /// \brief Human-readable name for logging and CLI introspection.
    [[nodiscard]] virtual const char* name() const = 0;

    /// \brief Called when the routee set changes (add, remove, resize).
    ///
    /// Default no-op. Override to rebuild internal state (e.g., hash ring).
    /// \param[in] routees The updated full list of routees.
    virtual void on_routees_changed(const std::vector<ActorRef>& /*routees*/) {}
};

// ────────────────────────────────────────────────────────────────────────────
// RoundRobinLogic — atomic counter, sequential selection
// ────────────────────────────────────────────────────────────────────────────

/// \brief Selects routees sequentially using an atomic counter.
///
/// Each call to \c select_routee() increments an internal counter and
/// returns the counter modulo the number of routees.
class RoundRobinLogic final : public IRoutingLogic {
  public:
    [[nodiscard]] size_t
    select_routee(const std::vector<ActorRef>& routees, const TypedMessage& msg,
                  const std::vector<cli::MboxSnapshot>& routee_states) override;

    [[nodiscard]] const char* name() const override {
        return "round-robin";
    }

  private:
    std::atomic<uint64_t> counter_{0};
};

// ────────────────────────────────────────────────────────────────────────────
// RandomLogic — pseudo-random selection with xorshift64
// ────────────────────────────────────────────────────────────────────────────

/// \brief Selects routees randomly using an xorshift64 PRNG.
///
/// Thread-safe via CAS on the internal state. Seedable for reproducibility.
class RandomLogic final : public IRoutingLogic {
  public:
    /// \brief Construct with an optional seed.
    ///
    /// \param[in] seed PRNG seed. A seed of 0 uses a non-deterministic
    ///                 fallback (hash of the address of the state variable).
    explicit RandomLogic(uint64_t seed = 0);

    [[nodiscard]] size_t
    select_routee(const std::vector<ActorRef>& routees, const TypedMessage& msg,
                  const std::vector<cli::MboxSnapshot>& routee_states) override;

    [[nodiscard]] const char* name() const override {
        return "random";
    }

  private:
    std::atomic<uint64_t> state_;
};

// ────────────────────────────────────────────────────────────────────────────
// ConsistentHashingLogic — hash-ring-based selection
// ────────────────────────────────────────────────────────────────────────────

/// \brief Selects routees using consistent hashing with virtual nodes.
///
/// Messages with the same key (by default derived from \c TypeTag) always
/// map to the same routee, modulo ring changes from routee set modifications.
/// Uses a sorted ring of virtual nodes with binary search for O(log n)
/// lookup.
class ConsistentHashingLogic final : public IRoutingLogic {
  public:
    /// \brief Key extractor function type.
    ///
    /// Given a message, returns a 64-bit hash key. The default extractor
    /// uses the \c TypeTag value. Custom extractors can hash on a specific
    /// message field (e.g., a user ID) for domain-based consistent hashing.
    using KeyExtractor = std::function<uint64_t(const TypedMessage&)>;

    /// \brief Construct with configurable virtual node count and key
    ///        extraction.
    ///
    /// \param[in] virtual_nodes_per_routee Number of virtual nodes per
    ///            routee (default 128). More nodes = better distribution,
    ///            higher rebuild cost.
    /// \param[in] key_extractor Function to extract a hash key from a
    ///            message. When \c nullptr, uses the default extractor
    ///            (hashes \c TypeTag).
    explicit ConsistentHashingLogic(uint32_t virtual_nodes_per_routee = 128,
                                    KeyExtractor key_extractor = nullptr);

    [[nodiscard]] size_t
    select_routee(const std::vector<ActorRef>& routees, const TypedMessage& msg,
                  const std::vector<cli::MboxSnapshot>& routee_states) override;

    [[nodiscard]] const char* name() const override {
        return "consistent-hashing";
    }

    /// \brief Rebuild the hash ring (called when the routee set changes).
    /// \param[in] routees The new set of routees.
    void rebuild_ring(const std::vector<ActorRef>& routees);

    /// \brief React to routee set changes by rebuilding the hash ring.
    void on_routees_changed(const std::vector<ActorRef>& routees) override {
        rebuild_ring(routees);
    }

  private:
    uint32_t vnodes_per_routee_;
    KeyExtractor key_extractor_;
    /// Sorted ring: (hash_value, routee_index). Look up via
    /// std::upper_bound.
    std::vector<std::pair<uint64_t, size_t>> ring_;
};

// ────────────────────────────────────────────────────────────────────────────
// SmallestMailboxLogic — load-aware selection by mailbox depth
// ────────────────────────────────────────────────────────────────────────────

/// \brief Selects the routee with the smallest mailbox depth.
///
/// Uses \c MboxSnapshot::depth from each routee's mailbox snapshot.
/// Routees with no snapshot available are treated as having depth=0.
class SmallestMailboxLogic final : public IRoutingLogic {
  public:
    [[nodiscard]] size_t
    select_routee(const std::vector<ActorRef>& routees, const TypedMessage& msg,
                  const std::vector<cli::MboxSnapshot>& routee_states) override;

    [[nodiscard]] const char* name() const override {
        return "smallest-mailbox";
    }
};

} // namespace hpactor::routing
