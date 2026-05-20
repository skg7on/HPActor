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

#include <hpactor/config/binary_format.hpp>
#include <hpactor/config/binary_serializer.hpp>

#include <cstring>
#include <unordered_map>

namespace hpactor::config {

namespace {

// Assign Dst from Src when the types are directly compatible; silently skip
// otherwise.  Catches mismatches like std::string = uint32_t (string is
// assignable from char = int, but that's not what we want for binary offset
// fields).
template <typename Dst, typename Src>
void assign_or_skip(Dst& dst, const Src& src) {
    if constexpr (std::is_assignable_v<Dst&, const Src&> &&
                  !(std::is_arithmetic_v<Src> && std::is_same_v<Dst, std::string>)) {
        dst = src;
    }
}

class BinaryWriter {
  public:
    BinaryWriter() {
        buf_.reserve(4096);
        // Reserve offset 0 as sentinel for "no string"
        string_table_.push_back(0);
    }

    uint32_t add_string(const std::string& s) {
        if (s.empty())
            return 0;
        auto it = string_map_.find(s);
        if (it != string_map_.end())
            return it->second;
        uint32_t offset = static_cast<uint32_t>(string_table_.size());
        string_table_.insert(string_table_.end(), s.begin(), s.end());
        string_table_.push_back('\0');
        string_map_[s] = offset;
        return offset;
    }

    template <typename T> uint32_t write(const T& val) {
        uint32_t offset = static_cast<uint32_t>(buf_.size());
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&val);
        buf_.insert(buf_.end(), ptr, ptr + sizeof(T));
        return offset;
    }

    uint32_t write_bytes(const uint8_t* data, size_t len) {
        uint32_t offset = static_cast<uint32_t>(buf_.size());
        buf_.insert(buf_.end(), data, data + len);
        return offset;
    }

    uint32_t current_offset() const {
        return static_cast<uint32_t>(buf_.size());
    }

    void finalize(std::vector<uint8_t>& out) {
        // Append string table
        uint32_t string_table_offset = static_cast<uint32_t>(buf_.size());
        uint32_t string_table_size = static_cast<uint32_t>(string_table_.size());
        buf_.insert(buf_.end(), string_table_.begin(), string_table_.end());

        // Update header with final offsets
        BinaryHeader* hdr = reinterpret_cast<BinaryHeader*>(buf_.data());
        hdr->string_table_offset = string_table_offset;
        hdr->string_table_size = string_table_size;

        out = std::move(buf_);
    }

  private:
    std::vector<uint8_t> buf_;
    std::vector<uint8_t> string_table_;
    std::unordered_map<std::string, uint32_t> string_map_;
};

} // anonymous namespace

std::vector<uint8_t> serialize_topology(const TopologyModel& model) {
    BinaryWriter w;

    // Reserve header space (filled at end)
    uint32_t header_offset = w.write(BinaryHeader{});
    (void)header_offset;

    // SystemDef
    BinarySystemDef bsys{};

// Shared scalar fields (generated from system_toml_fields.def)
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define HPACTOR_SYSTEM_TOML_FIELD(name, type, toml, def)                       \
    assign_or_skip(bsys.name, model.system.name);
#include <hpactor/config/system_toml_fields.def>
#undef HPACTOR_SYSTEM_TOML_FIELD
    // NOLINTEND(cppcoreguidelines-macro-usage)

    // SystemDef-only fields (not in the shared X-macro table)
    bsys.default_mailbox_size = model.system.default_mailbox_size;
    bsys.reserved_pad = 0;
    bsys.use_coroutines = model.system.use_coroutines ? 1 : 0;

    // String table fields (override X-macro-generated assignments)
    bsys.version_offset = w.add_string(model.system.version);
    bsys.http_bind_host = w.add_string(model.system.http_bind_host);
    // Tracing
    bsys.tracing_enabled = model.system.tracing.enabled ? 1 : 0;
    bsys.tracing_propagate_unsampled =
        model.system.tracing.propagate_unsampled ? 1 : 0;
    bsys.tracing_ring_buffer_capacity = model.system.tracing.ring_buffer_capacity;
    bsys.tracing_sampler = static_cast<uint32_t>(model.system.tracing.sampler);
    bsys.tracing_exporter = static_cast<uint32_t>(model.system.tracing.exporter);
    bsys.tracing_sample_ratio = model.system.tracing.sample_ratio;
    bsys.tracing_export_interval_ms =
        static_cast<uint32_t>(model.system.tracing.export_interval.count());
    bsys.tracing_max_export_batch_size = model.system.tracing.max_export_batch_size;
    bsys.tracing_max_tracestate_len = model.system.tracing.max_tracestate_len;
    bsys.tracing_pad = 0;
    bsys.tracing_flags = 0;
    bsys.tracing_service_name_offset =
        w.add_string(model.system.tracing.service_name);
    bsys.tracing_otlp_endpoint_offset =
        w.add_string(model.system.tracing.otlp_endpoint);
    bsys.tracing_json_file_path_offset =
        w.add_string(model.system.tracing.json_file_path);
    uint32_t system_offset = w.write(bsys);

    // Dispatchers
    uint32_t dispatcher_count = static_cast<uint32_t>(model.dispatchers.size());
    uint32_t dispatchers_offset = w.current_offset();

    for (const auto& d : model.dispatchers) {
        BinaryDispatcherDef bd{};
        bd.name_offset = w.add_string(d.name);
        bd.threads = d.threads;
        bd.cpu_affinity_count = static_cast<uint16_t>(d.cpu_affinity.size());
        if (!d.cpu_affinity.empty()) {
            bd.cpu_affinity_offset =
                w.write_bytes(d.cpu_affinity.data(), d.cpu_affinity.size());
        }
        w.write(bd);
    }

    // Actors
    uint32_t actor_count = static_cast<uint32_t>(model.actors.size());
    uint32_t actors_offset = w.current_offset();

    for (const auto& a : model.actors) {
        BinaryActorDef ba{};
        ba.id_offset = w.add_string(a.id);
        ba.behavior_offset = w.add_string(a.behavior);
        ba.supervisor_offset = w.add_string(a.supervisor);
        ba.dispatcher_offset = w.add_string(a.dispatcher);
        ba.dispatch_policy = static_cast<uint8_t>(a.dispatch_policy);
        ba.mailbox_capacity = a.mailbox_capacity;
        ba.slab_class_bytes = a.resources.slab_class_bytes;
        ba.max_memory_kb = a.resources.max_memory_kb;
        ba.args_count = static_cast<uint16_t>(a.args.size());
        // args table follows immediately after this actor def
        ba.args_offset = a.args.empty()
                             ? 0
                             : w.current_offset() +
                                   static_cast<uint32_t>(sizeof(BinaryActorDef));
        w.write(ba);
        // Write args table right after the actor def
        for (const auto& [k, v] : a.args) {
            BinaryKeyValue bkv{};
            bkv.key_offset = w.add_string(k);
            bkv.value_offset = w.add_string(v);
            w.write(bkv);
        }
    }

    // Finalize: append string table, update header
    std::vector<uint8_t> result;
    w.finalize(result);

    // Patch header
    BinaryHeader* hdr = reinterpret_cast<BinaryHeader*>(result.data());
    hdr->magic = TOPOLOGY_BINARY_MAGIC;
    hdr->version = 1;
    hdr->system_offset = system_offset;
    hdr->dispatcher_count = dispatcher_count;
    hdr->dispatchers_offset = dispatchers_offset;
    hdr->actor_count = actor_count;
    hdr->actors_offset = actors_offset;

    return result;
}

} // namespace hpactor::config
