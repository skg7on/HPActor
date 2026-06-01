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

/// \brief Magic number for binary topology files ("HPAT").
constexpr uint32_t TOPOLOGY_BINARY_MAGIC = 0x48504154;

/// \brief File header for the mmap-friendly binary topology format.
///
/// All strings are stored as offsets into a string table. After mmap,
/// string pointers point directly into the mapped region (zero-copy).
struct BinaryHeader {
    /// \brief Magic number; must equal TOPOLOGY_BINARY_MAGIC.
    uint32_t magic;
    /// \brief Format version for forward compatibility.
    uint32_t version;
    /// \brief Byte offset to the BinarySystemDef record.
    uint32_t system_offset;
    /// \brief Number of dispatcher definitions.
    uint32_t dispatcher_count;
    /// \brief Byte offset to the BinaryDispatcherDef array.
    uint32_t dispatchers_offset;
    /// \brief Number of actor definitions.
    uint32_t actor_count;
    /// \brief Byte offset to the BinaryActorDef array.
    uint32_t actors_offset;
    /// \brief Byte offset to the string table.
    uint32_t string_table_offset;
    /// \brief Size of the string table in bytes.
    uint32_t string_table_size;
};

/// \brief Fixed-size system config record in binary format.
///
/// Aligned for direct memcpy. String fields are stored as offsets into
/// the string table.
struct BinarySystemDef {
    uint32_t scheduler_threads;
    uint32_t max_queue_depth;
    uint32_t default_mailbox_size;
    uint32_t enable_network; ///< bool stored as uint32_t.
    uint16_t tcp_port;
    uint16_t reserved_pad; ///< Was udp_port; now in RegistrarConfig.
    uint32_t spawn_timeout_ms;
    uint32_t enable_http_gateway;
    uint16_t http_port;
    uint32_t http_max_connections;
    uint32_t http_max_request_size;
    uint32_t http_reply_timeout_ms;
    uint32_t use_coroutines;
    uint32_t version_offset; ///< String table offset to version string.
    union {
        uint32_t http_bind_host;        ///< X-macro compatible name.
        uint32_t http_bind_host_offset; ///< String table offset (canonical).
    };
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
    uint32_t tracing_service_name_offset;   ///< String table offset.
    uint32_t tracing_otlp_endpoint_offset;  ///< String table offset.
    uint32_t tracing_json_file_path_offset; ///< String table offset.
};

/// \brief Dispatcher definition in binary format.
struct BinaryDispatcherDef {
    /// \brief String table offset to the dispatcher name.
    uint32_t name_offset;
    /// \brief Number of worker threads.
    uint16_t threads;
    /// \brief Number of CPU affinity entries.
    uint16_t cpu_affinity_count;
    /// \brief Offset to uint8_t CPU affinity array. 0 if count=0.
    uint32_t cpu_affinity_offset;
};

/// \brief Actor definition in binary format.
struct BinaryActorDef {
    /// \brief String table offset to actor id.
    uint32_t id_offset;
    /// \brief String table offset to behavior name.
    uint32_t behavior_offset;
    /// \brief String table offset to supervisor id. 0 = unsupervised.
    uint32_t supervisor_offset;
    /// \brief String table offset to dispatcher name. 0 = default pool.
    uint32_t dispatcher_offset;
    /// \brief DispatchPolicy enum value.
    uint8_t dispatch_policy;
    uint32_t mailbox_capacity;
    uint32_t slab_class_bytes;
    uint32_t max_memory_kb;
    /// \brief Number of key-value argument pairs.
    uint16_t args_count;
    /// \brief Offset to BinaryKeyValue array. 0 if count=0.
    uint32_t args_offset;
};

/// \brief A key-value pair in the binary format.
struct BinaryKeyValue {
    /// \brief String table offset to the key.
    uint32_t key_offset;
    /// \brief String table offset to the value.
    uint32_t value_offset;
};

} // namespace hpactor::config
