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
// Note: This TU is compiled with -fexceptions for protobuf/JSON parsing.

#include <hpactor/etcd/etcd_serialize.hpp>

#include <sstream>

namespace hpactor::etcd {

namespace {

/// \brief Escape a string for safe JSON embedding.
static std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    return out;
}

} // namespace

std::string serialize_lease(const cluster::singleton::LeadershipLease& lease) {
    std::ostringstream os;
    os << "{";
    os << "\"cluster_id\":\"" << json_escape(lease.cluster_id) << "\",";
    os << "\"singleton_name\":\"" << json_escape(lease.singleton_name) << "\",";
    os << "\"owner_node_id\":\"" << json_escape(lease.owner_node_id) << "\",";
    os << "\"owner_incarnation\":" << lease.owner_incarnation << ",";
    os << "\"owner_process_start_id\":" << lease.owner_process_start_id << ",";
    os << "\"membership_epoch\":" << lease.membership_epoch << ",";
    os << "\"fencing_token\":" << lease.fencing_token << ",";
    os << "\"backend_term\":" << lease.backend_term << ",";
    os << "\"backend_revision\":" << lease.backend_revision;
    // lease_deadline omitted — not persisted (restored from lease TTL on
    // deserialize)
    os << "}";
    return os.str();
}

std::optional<cluster::singleton::LeadershipLease>
deserialize_lease(std::string_view data) {
    // Manual JSON parsing to avoid dependency on a JSON library.
    // The format is simple enough for a hand-rolled parser.
    if (data.empty() || data[0] != '{')
        return std::nullopt;

    cluster::singleton::LeadershipLease lease;

    auto extract_str = [&](const char* key) -> std::string {
        std::string search = std::string("\"") + key + "\":\"";
        auto pos = data.find(search);
        if (pos == std::string_view::npos)
            return {};
        pos += search.size();
        auto end = data.find('"', pos);
        if (end == std::string_view::npos)
            return {};
        return std::string(data.substr(pos, end - pos));
    };

    auto extract_uint = [&](const char* key) -> uint64_t {
        std::string search = std::string("\"") + key + "\":";
        auto pos = data.find(search);
        if (pos == std::string_view::npos)
            return 0;
        pos += search.size();
        auto end = data.find_first_of(",}", pos);
        if (end == std::string_view::npos)
            return 0;
        try {
            return std::stoull(std::string(data.substr(pos, end - pos)));
        } catch (...) {
            return 0;
        }
    };

    lease.cluster_id = extract_str("cluster_id");
    lease.singleton_name = extract_str("singleton_name");
    lease.owner_node_id = extract_str("owner_node_id");
    lease.owner_incarnation = extract_uint("owner_incarnation");
    lease.owner_process_start_id = extract_uint("owner_process_start_id");
    lease.membership_epoch = extract_uint("membership_epoch");
    lease.fencing_token = extract_uint("fencing_token");
    lease.backend_term = extract_uint("backend_term");
    lease.backend_revision = extract_uint("backend_revision");

    // Basic validation: must have the key identifying fields
    if (lease.singleton_name.empty() || lease.owner_node_id.empty()) {
        return std::nullopt;
    }

    return lease;
}

std::string owner_key(std::string_view key_prefix, std::string_view cluster_id,
                      std::string_view singleton_name) {
    std::string key;
    key.reserve(key_prefix.size() + cluster_id.size() + singleton_name.size() + 25);
    key.append(key_prefix);
    key.push_back('/');
    key.append(cluster_id);
    key.append("/singletons/");
    key.append(singleton_name);
    key.append("/owner");
    return key;
}

} // namespace hpactor::etcd
