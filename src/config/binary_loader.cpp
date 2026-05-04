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

#include <hpactor/config/binary_loader.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hpactor::config {

// -----------------------------------------------------------------------------
// Binary format types (must match binary_format.hpp)
// -----------------------------------------------------------------------------
namespace {

constexpr uint32_t MAGIC = 0x48504154; // "HPAT"

struct RawHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t system_offset;
    uint32_t dispatcher_count;
    uint32_t dispatchers_offset;
    uint32_t actor_count;
    uint32_t actors_offset;
    uint32_t string_table_offset;
    uint32_t string_table_size;
};

struct RawSystemDef {
    uint32_t scheduler_threads;
    uint32_t max_queue_depth;
    uint32_t default_mailbox_size;
    uint32_t enable_network;
    uint16_t tcp_port;
    uint16_t udp_port;
    uint32_t spawn_timeout_ms;
    uint32_t enable_http_gateway;
    uint16_t http_port;
    uint32_t http_max_connections;
    uint32_t http_max_request_size;
    uint32_t http_reply_timeout_ms;
    uint32_t use_coroutines;
    uint32_t version_offset;
    uint32_t http_bind_host_offset;
};

struct RawDispatcher {
    uint32_t name_offset;
    uint16_t threads;
    uint16_t cpu_affinity_count;
    uint32_t cpu_affinity_offset;
};

struct RawActor {
    uint32_t id_offset;
    uint32_t behavior_offset;
    uint32_t supervisor_offset;
    uint32_t dispatcher_offset;
    uint8_t  dispatch_policy;
    uint32_t mailbox_capacity;
    uint32_t slab_class_bytes;
    uint32_t max_memory_kb;
    uint16_t args_count;
    uint32_t args_offset;
};

struct RawKV {
    uint32_t key_offset;
    uint32_t value_offset;
};

inline const char* str_at(const uint8_t* str_table, uint32_t offset) {
    return offset ? reinterpret_cast<const char*>(str_table + offset) : "";
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// load_binary_topology — mmap a .bin file into a TopologyModel
// -----------------------------------------------------------------------------
result<TopologyModel> load_binary_topology(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return result<TopologyModel>::make(error(errors::unknown));
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return result<TopologyModel>::make(error(errors::unknown));
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (mapped == MAP_FAILED) {
        return result<TopologyModel>::make(error(errors::unknown));
    }

    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    const RawHeader* hdr = reinterpret_cast<const RawHeader*>(base);

    if (hdr->magic != MAGIC) {
        munmap(mapped, file_size);
        return result<TopologyModel>::make(error(errors::unknown));
    }

    const uint8_t* str_table = base + hdr->string_table_offset;
    TopologyModel model;

    // System config
    const RawSystemDef* rsys =
        reinterpret_cast<const RawSystemDef*>(base + hdr->system_offset);
    model.system.version = str_at(str_table, rsys->version_offset);
    model.system.scheduler_threads = rsys->scheduler_threads;
    model.system.max_queue_depth = rsys->max_queue_depth;
    model.system.default_mailbox_size = rsys->default_mailbox_size;
    model.system.enable_network = rsys->enable_network != 0;
    model.system.tcp_port = rsys->tcp_port;
    model.system.udp_port = rsys->udp_port;
    model.system.spawn_timeout_ms = rsys->spawn_timeout_ms;
    model.system.enable_http_gateway = rsys->enable_http_gateway != 0;
    model.system.http_bind_host = str_at(str_table, rsys->http_bind_host_offset);
    model.system.http_port = rsys->http_port;
    model.system.http_max_connections = rsys->http_max_connections;
    model.system.http_max_request_size = rsys->http_max_request_size;
    model.system.http_reply_timeout_ms = rsys->http_reply_timeout_ms;
    model.system.use_coroutines = rsys->use_coroutines != 0;

    // Dispatchers
    const RawDispatcher* dispatchers =
        reinterpret_cast<const RawDispatcher*>(base + hdr->dispatchers_offset);
    for (uint32_t i = 0; i < hdr->dispatcher_count; ++i) {
        DispatcherDef def;
        def.name = str_at(str_table, dispatchers[i].name_offset);
        def.threads = dispatchers[i].threads;
        if (dispatchers[i].cpu_affinity_count > 0 && dispatchers[i].cpu_affinity_offset) {
            const uint8_t* aff = base + dispatchers[i].cpu_affinity_offset;
            def.cpu_affinity.assign(aff, aff + dispatchers[i].cpu_affinity_count);
        }
        model.dispatchers.push_back(std::move(def));
    }

    // Actors
    const RawActor* actors =
        reinterpret_cast<const RawActor*>(base + hdr->actors_offset);
    for (uint32_t i = 0; i < hdr->actor_count; ++i) {
        const RawActor& ra = actors[i];
        ActorDef def;
        def.id = str_at(str_table, ra.id_offset);
        def.behavior = str_at(str_table, ra.behavior_offset);
        def.supervisor = str_at(str_table, ra.supervisor_offset);
        def.dispatcher = str_at(str_table, ra.dispatcher_offset);
        def.dispatch_policy = static_cast<DispatchPolicy>(ra.dispatch_policy);
        def.mailbox_capacity = ra.mailbox_capacity;
        def.resources.slab_class_bytes = ra.slab_class_bytes;
        def.resources.max_memory_kb = ra.max_memory_kb;

        if (ra.args_count > 0 && ra.args_offset) {
            const RawKV* kvs = reinterpret_cast<const RawKV*>(base + ra.args_offset);
            for (uint16_t j = 0; j < ra.args_count; ++j) {
                def.args[str_at(str_table, kvs[j].key_offset)] =
                    str_at(str_table, kvs[j].value_offset);
            }
        }

        model.actors.push_back(std::move(def));
    }

    munmap(mapped, file_size);
    return result<TopologyModel>::make(std::move(model));
}

} // namespace hpactor::config
