// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <google/protobuf/message.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace hpactor {

/// \brief Maximum payload size eligible for inlining (MEM-006).
inline constexpr size_t kMaxInlinePayload = 32;

/// \brief Compile-time trait: true when T can be inlined in the envelope.
template <typename T>
inline constexpr bool kCanInlinePayload =
    sizeof(T) <= kMaxInlinePayload && std::is_trivially_copyable_v<T>;

/// \brief Universal message carrier for all actor communication.
///
/// Carries a \c TypeTag for dispatch routing, serialized protobuf bytes for
/// wire transfer, and an optional \c shared_ptr<Message> for zero-copy local
/// delivery. The \c mpsc_next field provides the intrusive link for lock-free
/// MPSC mailboxes.
///
/// For payloads ≤ \c kMaxInlinePayload bytes, \c StreamBuffer::from_data()
/// provides exact-capacity allocation (no 64KB minimum), significantly reducing
/// allocation overhead for small messages (MEM-006).
///
/// Lifecycle:
/// - Local send: \c TypedMessage(tag, parsed_msg, serialized_bytes)
/// - Remote recv: \c TypedMessage(tag, serialized_bytes)
/// - Lazy parse: \c msg.as<ConcreteProtoMsg>() on first access
///
/// \note Thread safety: Not internally synchronized. A single \c TypedMessage
///       must not be accessed concurrently except that \c as<T>() can be
///       called safely after the message is published to a single-consumer
///       mailbox. The \c mpsc_next atomic is managed exclusively by the
///       mailbox.
class TypedMessage {
  public:
    TypedMessage() = default;

    /// Copy is disabled — the \c mpsc_next atomic member deletes the
    /// implicitly-generated copy constructor.
    TypedMessage(const TypedMessage&) = delete;
    TypedMessage& operator=(const TypedMessage&) = delete;

    /// \brief Move constructor.
    ///
    /// Transfers all fields from \p other. The moved-from object's
    /// \c mpsc_next is left default-initialized.
    TypedMessage(TypedMessage&& other) noexcept
        : tag_(other.tag_), payload_(std::move(other.payload_)),
          inline_size_(other.inline_size_), is_inline_(other.is_inline_),
          parsed_(std::move(other.parsed_)),
          sender_address_(other.sender_address_),
          trace_context_(other.trace_context_),
          has_trace_context_(other.has_trace_context_),
          deadline_ns_(other.deadline_ns_),
          ask_message_id_(other.ask_message_id_),
          ack_requested_(other.ack_requested_), message_id_(other.message_id_),
          delivery_priority_(other.delivery_priority_),
          delivery_flags_(other.delivery_flags_) {
        if (other.is_inline_) {
            std::memcpy(inline_payload_, other.inline_payload_, other.inline_size_);
            other.is_inline_ = false;
            other.inline_size_ = 0;
        }
    }

    /// \brief Move assignment.
    ///
    /// Transfers all fields. \c mpsc_next is intentionally not touched —
    /// ownership of the mailbox link belongs to the mailbox, not the message.
    TypedMessage& operator=(TypedMessage&& other) noexcept {
        tag_ = other.tag_;
        payload_ = std::move(other.payload_);
        is_inline_ = other.is_inline_;
        inline_size_ = other.inline_size_;
        if (other.is_inline_) {
            std::memcpy(inline_payload_, other.inline_payload_, other.inline_size_);
            other.is_inline_ = false;
            other.inline_size_ = 0;
        }
        parsed_ = std::move(other.parsed_);
        sender_address_ = other.sender_address_;
        trace_context_ = other.trace_context_;
        has_trace_context_ = other.has_trace_context_;
        deadline_ns_ = other.deadline_ns_;
        ask_message_id_ = other.ask_message_id_;
        ack_requested_ = other.ack_requested_;
        message_id_ = other.message_id_;
        delivery_priority_ = other.delivery_priority_;
        delivery_flags_ = other.delivery_flags_;
        return *this;
    }

    /// \brief Local-send constructor — carries both parsed and serialized
    ///        forms.
    ///
    /// \param[in] tag Message type tag for dispatch.
    /// \param[in] msg Shared pointer to the parsed protobuf message
    ///                (zero-copy on local fast path).
    /// \param[in] serialized Pre-serialized wire payload.
    TypedMessage(TypeTag tag, std::shared_ptr<google::protobuf::Message> msg,
                 StreamBuffer serialized)
        : tag_(tag), payload_(std::move(serialized)), parsed_(std::move(msg)) {}

    /// \brief Remote-receive constructor — serialized payload only.
    ///
    /// The parsed form is lazily populated on the first call to \c as<T>().
    ///
    /// \param[in] tag Message type tag for dispatch.
    /// \param[in] payload Serialized protobuf payload from the wire.
    explicit TypedMessage(TypeTag tag, StreamBuffer payload)
        : tag_(tag), payload_(std::move(payload)) {}

    /// \brief Convenience constructor — serializes \p msg eagerly.
    ///
    /// \param[in] tag Message type tag for dispatch.
    /// \param[in] msg Protobuf message to serialize immediately.
    /// \note The serialized bytes are stored in \c payload_. The caller
    ///       retains ownership of \p msg; a copy is not retained unless
    ///       the caller also passes it via the parsed-msg constructor.
    TypedMessage(TypeTag tag, const google::protobuf::Message& msg);

    /// \brief The \c TypeTag used for dispatch routing.
    [[nodiscard]] TypeTag type_id() const noexcept {
        return tag_;
    }

    /// \brief Raw serialized payload for wire transfer.
    [[nodiscard]] const StreamBuffer& payload() const noexcept {
        return payload_;
    }

    /// \brief Pre-parsed protobuf message, when available.
    ///
    /// Non-null on the local fast path or after \c as<T>() has been called.
    /// \return Shared pointer to the parsed message, or nullptr.
    [[nodiscard]] std::shared_ptr<google::protobuf::Message>
    parsed() const noexcept {
        return parsed_;
    }

    /// \brief Lazy deserialize into a concrete protobuf type.
    ///
    /// If \c parsed_ is already populated, returns a static cast without
    /// additional allocation. Otherwise parses \c payload_ into \p T and
    /// caches the result. All subsequent calls return the cached instance.
    ///
    /// \tparam T Concrete protobuf message type.
    /// \return Shared pointer to the parsed message, or \c nullptr if
    ///         \c payload_ is empty or parsing fails.
    template <typename T> std::shared_ptr<T> as() const {
        if (parsed_) {
            return std::static_pointer_cast<T>(parsed_);
        }
        if (payload_.empty()) {
            return nullptr;
        }
        auto msg = mem::allocate_shared<T>(mem::current_actor_id(),
                                           mem::RegionType::kMessage);
        if (!msg->ParseFromArray(payload_.data(),
                                 static_cast<int>(payload_.size()))) {
            return nullptr;
        }
        parsed_ = msg;
        return msg;
    }

    /// \brief Sender address for reply routing.
    ///
    /// Set by \c ActorContext::send() (local) or \c deliver_remote()
    /// (remote). Read by \c EventBasedActor::receive() to populate
    /// \c current_sender_ for \c reply().
    [[nodiscard]] const ActorAddress& sender_address() const noexcept {
        return sender_address_;
    }
    void set_sender_address(const ActorAddress& addr) {
        sender_address_ = addr;
    }

    /// \brief Whether a valid trace context is attached.
    [[nodiscard]] bool has_trace_context() const noexcept {
        return has_trace_context_;
    }

    /// \brief The attached trace context.
    ///
    /// \pre \c has_trace_context() returns \c true.
    [[nodiscard]] const TraceContext& trace_context() const noexcept {
        return trace_context_;
    }

    /// \brief Attach a trace context for distributed tracing propagation.
    void set_trace_context(const TraceContext& ctx) noexcept {
        trace_context_ = ctx;
        has_trace_context_ = ctx.valid();
    }

    /// \brief Remove the attached trace context.
    void clear_trace_context() noexcept {
        trace_context_.clear();
        has_trace_context_ = false;
    }

    /// \brief Delivery deadline in nanoseconds (monotonic clock).
    ///
    /// \c INT64_MAX means no deadline. Set from \c MailboxEnvelopeMeta at
    /// push time.
    [[nodiscard]] int64_t deadline_ns() const noexcept {
        return deadline_ns_;
    }
    void set_deadline_ns(int64_t ns) noexcept {
        deadline_ns_ = ns;
    }

    /// \brief Ask-correlation identifier.
    ///
    /// Set by \c ActorContext::ask() to link a response to its request.
    /// Zero means "not an ask-tracked message."
    [[nodiscard]] uint64_t ask_message_id() const noexcept {
        return ask_message_id_;
    }
    void set_ask_message_id(uint64_t id) noexcept {
        ask_message_id_ = id;
    }

    /// \brief Whether the sender requested reliable ACK/NACK for this message.
    ///
    /// Set from the WireFrame \c AckRequested flag in \c deliver_remote().
    /// Best-effort messages (no flag) return \c false.
    [[nodiscard]] bool ack_requested() const noexcept {
        return ack_requested_;
    }
    void set_ack_requested(bool v) noexcept {
        ack_requested_ = v;
    }

    /// \brief Delivery priority assigned by the pipeline at admission time.
    ///
    /// Stamped from \c MailboxEnvelopeMeta::priority before mailbox push.
    /// 0 is the highest priority; higher values are lower priority.
    [[nodiscard]] uint8_t delivery_priority() const noexcept {
        return delivery_priority_;
    }
    void set_delivery_priority(uint8_t value) noexcept {
        delivery_priority_ = value;
    }

    /// \brief Delivery flags stamped by the pipeline at admission time.
    ///
    /// Stamped from \c MailboxEnvelopeMeta::flags before mailbox push.
    /// Carries frame-level flags such as NoDrop for downstream consumers
    /// (e.g. PythonBridgeActor).
    [[nodiscard]] uint32_t delivery_flags() const noexcept {
        return delivery_flags_;
    }
    void set_delivery_flags(uint32_t value) noexcept {
        delivery_flags_ = value;
    }

    /// \brief Sender-assigned message identifier for reliable messaging.
    ///
    /// Set from \c ActorMsgFrame.message_id in \c deliver_remote(). Zero
    /// means not a tracked message.
    [[nodiscard]] uint64_t message_id() const noexcept {
        return message_id_;
    }
    void set_message_id(uint64_t id) noexcept {
        message_id_ = id;
    }

    // ── MEM-006: Inline payload support ──────────────────────────

    /// \brief Whether this message's payload is stored inline rather than in
    ///        the heap-allocated StreamBuffer.
    [[nodiscard]] bool is_inline() const noexcept {
        return is_inline_;
    }

    /// \brief Pointer to the inline payload data.
    /// \pre \c is_inline() returns true.
    [[nodiscard]] const uint8_t* inline_data() const noexcept {
        return inline_payload_;
    }

    /// \brief Size of the inline payload in bytes.
    /// \pre \c is_inline() returns true.
    [[nodiscard]] uint8_t inline_size() const noexcept {
        return inline_size_;
    }

    /// \brief Create a TypedMessage with inline storage for small payloads
    ///        (MEM-006 §3.1).
    ///
    /// When \p data_len ≤ \c kMaxInlinePayload, the serialized bytes are copied
    /// into inline storage, eliminating a separate heap allocation. For larger
    /// payloads, falls back to the standard StreamBuffer constructor.
    ///
    /// \param[in] tag Message type tag for dispatch.
    /// \param[in] data Serialized payload bytes.
    /// \param[in] data_len Number of bytes in \p data.
    /// \return A TypedMessage with inline or external storage as appropriate.
    [[nodiscard]] static TypedMessage
    create_inline(TypeTag tag, const uint8_t* data, size_t data_len) noexcept;

    /// \brief MPSC mailbox intrusive link pointer.
    ///
    /// Must be named \c mpsc_next for \c MPSCMailbox<T> compatibility.
    /// Managed exclusively by the mailbox; message producers and consumers
    /// must not read or write this field directly.
    ///
    /// \note Lock-free: Accessed via atomic CAS operations by the mailbox
    ///       enqueue/dequeue paths.
    std::atomic<TypedMessage*> mpsc_next{nullptr};

  private:
    TypeTag tag_ = TypeTag::Invalid;
    StreamBuffer payload_;
    uint8_t inline_payload_[kMaxInlinePayload]{};
    uint8_t inline_size_{0};
    bool is_inline_{false};
    mutable std::shared_ptr<google::protobuf::Message> parsed_;
    ActorAddress sender_address_;
    TraceContext trace_context_;
    bool has_trace_context_ = false;
    int64_t deadline_ns_ = INT64_MAX;
    uint64_t ask_message_id_ = 0;
    bool ack_requested_ = false;
    uint64_t message_id_ = 0;
    uint8_t delivery_priority_{0};
    uint32_t delivery_flags_{0};
};

} // namespace hpactor
