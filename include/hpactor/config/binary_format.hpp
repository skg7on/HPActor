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

#include <cstdint>

namespace hpactor::config {

// -----------------------------------------------------------------------------
// Binary topology format — designed for zero-copy mmap access
//
// All strings are stored as offsets into a string table. After mmap, string
// pointers point directly into the mapped region (no allocation).
// -----------------------------------------------------------------------------

constexpr uint32_t TOPOLOGY_BINARY_MAGIC = 0x48504154; // "HPAT"

struct BinaryHeader {
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

// Fixed-size system config record (aligned for direct memcpy)
struct BinarySystemDef {
    uint32_t scheduler_threads;
    uint32_t max_queue_depth;
    uint32_t default_mailbox_size;
    uint32_t enable_network; // bool as uint32_t
    uint16_t tcp_port;
    uint16_t reserved_pad; // was udp_port (now in RegistrarConfig)
    uint32_t spawn_timeout_ms;
    uint32_t enable_http_gateway;
    uint16_t http_port;
    uint32_t http_max_connections;
    uint32_t http_max_request_size;
    uint32_t http_reply_timeout_ms;
    uint32_t use_coroutines;
    uint32_t version_offset; // string table offset
    uint32_t http_bind_host_offset;
    uint32_t tracing_enabled;
    uint32_t tracing_propagate_unsampled;
    uint32_t tracing_ring_buffer_capacity;
    uint32_t tracing_sampler;
    uint32_t tracing_exporter;
    double tracing_sample_ratio;
    uint32_t tracing_export_interval_ms;
    uint32_t tracing_max_export_batch_size;
    uint16_t tracing_max_tracestate_len;
    uint16_t tracing_pad;
    uint32_t tracing_flags;
    uint32_t tracing_service_name_offset;
    uint32_t tracing_otlp_endpoint_offset;
    uint32_t tracing_json_file_path_offset;
};

struct BinaryDispatcherDef {
    uint32_t name_offset;
    uint16_t threads;
    uint16_t cpu_affinity_count;
    uint32_t cpu_affinity_offset; // offset to uint8_t array, 0 if count=0
};

struct BinaryActorDef {
    uint32_t id_offset;
    uint32_t behavior_offset;
    uint32_t supervisor_offset; // 0 = no supervisor
    uint32_t dispatcher_offset; // 0 = default pool
    uint8_t dispatch_policy;
    uint32_t mailbox_capacity;
    uint32_t slab_class_bytes;
    uint32_t max_memory_kb;
    uint16_t args_count;
    uint32_t args_offset; // offset to BinaryKeyValue array, 0 if count=0
};

struct BinaryKeyValue {
    uint32_t key_offset;
    uint32_t value_offset;
};

} // namespace hpactor::config
