#pragma once

#include <cstddef>

#ifdef __linux__
    #define HPACTOR_PLATFORM_LINUX 1
#elif defined(__APPLE__)
    #define HPACTOR_PLATFORM_MACOS 1
#else
    #define HPACTOR_PLATFORM_UNKNOWN 1
#endif

namespace hpactor {
using byte_t = unsigned char;

inline constexpr size_t default_mailbox_capacity = 1024;
}