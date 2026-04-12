// ActorProxy implementation - see actor_proxy.hpp

#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/serialization.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/transport.hpp>

namespace hpactor {

ActorProxy::ActorProxy(ActorAddress address, net::Transport* transport)
    : address_(address), transport_(transport) {}

void ActorProxy::send(const ActorAddress& target, MessageVariant msg) {
    // Determine TypeTag using std::visit
    TypeTag tag = std::visit([](const auto& m) -> TypeTag {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, down_msg>) {
            return TypeTag::DownMsg;
        } else if constexpr (std::is_same_v<T, exit_msg>) {
            return TypeTag::ExitMsg;
        } else if constexpr (std::is_same_v<T, link_msg>) {
            return TypeTag::LinkMsg;
        } else if constexpr (std::is_same_v<T, unlink_msg>) {
            return TypeTag::UnlinkMsg;
        } else {
            return TypeTag::User;
        }
    }, msg);

    // Serialize message
    DefaultSerializer serializer;
    bytes payload = serializer.encode(tag, msg);

    // Create frame
    net::Frame frame;
    frame.sender = address_;       // This proxy's address (sender side)
    frame.receiver = target;      // Target actor address
    frame.message_id = MessageId::generate().value();
    frame.payload = std::move(payload);

    // Send via transport
    transport_->send(target, frame.encode());
}

} // namespace hpactor
