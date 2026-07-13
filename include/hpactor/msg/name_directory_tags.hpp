// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/msg/type_tag.hpp>

namespace hpactor::cluster::name {

/// \brief Subsystem TypeTags for the distributed name directory protocol.
///
/// Range 0x80–0xFF is the subsystem extension range (see \c TypeTag enum
/// in \c type_tag.hpp). These tags are NOT added to the \c TypeTag enum —
/// they are \c inline \c constexpr variables that implicitly convert to
/// \c TypeTag via \c make_subsystem_tag().
///
/// \note Tags 0x80, 0x82, and 0x84 are request tags (short-circuited by
///       \c InboundFrameRouter before the \c DeliveryPipeline). Tags 0x81
///       and 0x83 are response tags (delivered through normal messaging).

/// \brief NameRegisterRequest (0x80) — sent by the hosting node to the
///        home node to register an actor name.
inline constexpr TypeTag kNameRegisterRequestTag =
    make_subsystem_tag(0x80);
/// \brief NameRegisterResponse (0x81) — ACK or rejection from the home node.
inline constexpr TypeTag kNameRegisterResponseTag =
    make_subsystem_tag(0x81);
/// \brief NameResolveQuery (0x82) — sent by any node to the home node to
///        resolve an actor name.
inline constexpr TypeTag kNameResolveQueryTag =
    make_subsystem_tag(0x82);
/// \brief NameResolveResponse (0x83) — resolution result from the home node.
inline constexpr TypeTag kNameResolveResponseTag =
    make_subsystem_tag(0x83);
/// \brief NameUnregisterRequest (0x84) — sent by the hosting node to the
///        home node when the actor terminates (fire-and-forget).
inline constexpr TypeTag kNameUnregisterRequestTag =
    make_subsystem_tag(0x84);

/// \brief Return true if \p tag is any name-directory protocol tag.
///
/// \param[in] tag TypeTag value to check.
/// \return \c true if \p tag is in the range 0x80–0x84 (inclusive).
inline bool is_name_protocol_tag(TypeTag tag) noexcept {
    uint32_t v = static_cast<uint32_t>(tag);
    return v >= 0x80 && v <= 0x84;
}

} // namespace hpactor::cluster::name
