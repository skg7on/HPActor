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

#include <hpactor/fault/fault_types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace hpactor::fault {

struct FaultPoint {
    std::string path;
    FaultDomain domain;
    std::string description;
};

class FaultPointRegistry {
  public:
    static FaultPointRegistry& instance();

    void register_point(std::string path, FaultDomain domain,
                        std::string description);

    const FaultPoint* lookup(std::string_view path) const;

    bool matches_prefix(std::string_view path,
                        std::string_view prefix_pattern) const;

    void collect_prefix(std::string_view prefix_pattern,
                        std::vector<const FaultPoint*>& out) const;

    const std::vector<FaultPoint>& points() const noexcept {
        return points_;
    }

  private:
    FaultPointRegistry() = default;
    std::vector<FaultPoint> points_;
};

struct FaultPointRegistrar {
    FaultPointRegistrar(std::string_view path, FaultDomain domain,
                        std::string_view description) {
        FaultPointRegistry::instance().register_point(
            std::string(path), domain, std::string(description));
    }
};

} // namespace hpactor::fault
