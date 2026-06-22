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

#include <hpactor/net/service_discovery.hpp>

namespace hpactor::net {

/// \brief Static discovery backend — fixed topology from configuration.
///
/// Uses a pre-configured member list with no runtime discovery.
/// \c start(), \c stop(), \c announce(), and \c on_member_change() are
/// no-ops — members never change after construction.
///
/// \note Thread safety: \c discover_all() and \c discover() are safe to
///       call from any thread. The member list is immutable after
///       construction.
class StaticDiscovery : public IServiceDiscovery {
  public:
    /// \brief Construct with a fixed member list.
    ///
    /// \param[in] members Complete topology (does not change after
    ///            construction).
    explicit StaticDiscovery(std::vector<Member> members);

    void start() override {}
    void stop() override {}

    /// \brief Return the fixed member list.
    ///
    /// \return Copy of the configured member list.
    std::vector<Member> discover_all() const override;

    /// \brief Look up a member by endpoint.
    ///
    /// \param[in] ep Endpoint to search for.
    /// \return Pointer to the member, or \c nullptr if not found.
    const Member* discover(EndPoint ep) const override;

    void announce(Member) override {}
    void on_member_change(MemberChangeCallback) override {}
    std::string backend_name() const override {
        return "static";
    }

  private:
    std::vector<Member> members_;
    std::unordered_map<EndPoint, size_t> index_;
};

} // namespace hpactor::net
