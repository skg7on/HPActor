// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "command_utils.hpp"
#include <cstdio>

namespace hpactor::cli {

std::string format_bytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f GB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

} // namespace hpactor::cli
