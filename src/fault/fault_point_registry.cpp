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

#include <hpactor/fault/fault_point.hpp>

namespace hpactor::fault {

FaultPointRegistry& FaultPointRegistry::instance() {
    static FaultPointRegistry reg;
    return reg;
}

void FaultPointRegistry::register_point(std::string path, FaultDomain domain,
                                         std::string description) {
    points_.push_back(FaultPoint{std::move(path), domain, std::move(description)});
}

const FaultPoint* FaultPointRegistry::lookup(std::string_view path) const {
    for (const auto& pt : points_) {
        if (pt.path == path) {
            return &pt;
        }
    }
    return nullptr;
}

bool FaultPointRegistry::matches_prefix(std::string_view path,
                                         std::string_view prefix_pattern) const {
    if (prefix_pattern == "*") return true;
    if (prefix_pattern.size() > path.size()) return false;

    for (size_t i = 0; i < prefix_pattern.size(); ++i) {
        if (prefix_pattern[i] == '*') return true;
        if (prefix_pattern[i] != path[i]) return false;
    }
    return prefix_pattern.size() == path.size();
}

void FaultPointRegistry::collect_prefix(std::string_view prefix_pattern,
                                         std::vector<const FaultPoint*>& out) const {
    for (const auto& pt : points_) {
        if (matches_prefix(pt.path, prefix_pattern)) {
            out.push_back(&pt);
        }
    }
}

} // namespace hpactor::fault
