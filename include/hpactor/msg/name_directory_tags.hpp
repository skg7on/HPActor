// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/msg/type_tag.hpp>

namespace hpactor::cluster::name {

/// \brief Subsystem TypeTags for the distributed name directory protocol.
///
/// Range 0x80–0xFF is the subsystem extension range. These tags are NOT
/// added to the TypeTag enum — they are constexpr variables that implicitly
/// convert to TypeTag.

inline constexpr TypeTag kNameRegisterRequestTag =
    make_subsystem_tag(0x80);
inline constexpr TypeTag kNameRegisterResponseTag =
    make_subsystem_tag(0x81);
inline constexpr TypeTag kNameResolveQueryTag =
    make_subsystem_tag(0x82);
inline constexpr TypeTag kNameResolveResponseTag =
    make_subsystem_tag(0x83);
inline constexpr TypeTag kNameUnregisterRequestTag =
    make_subsystem_tag(0x84);

/// \brief Return true if \p tag is any name-directory protocol tag.
inline bool is_name_protocol_tag(TypeTag tag) noexcept {
    uint32_t v = static_cast<uint32_t>(tag);
    return v >= 0x80 && v <= 0x84;
}

} // namespace hpactor::cluster::name
