// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <hpactor/types/types.hpp>

#include <charconv>
#include <string>

namespace hpactor {
namespace cli {

inline ActorId parse_actor_id(const std::string& s) {
    uint64_t raw = 0;
    int base = 10;
    const char* start = s.data();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        start = s.data() + 2;
    }
    auto [ptr, ec] = std::from_chars(start, s.data() + s.size(), raw, base);
    if (ec != std::errc{})
        return ActorId{0};
    return ActorId{raw};
}

} // namespace cli
} // namespace hpactor
