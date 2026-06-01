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

#include <hpactor/config/topology_model.hpp>
#include <hpactor/types/types.hpp>

#include <string>

namespace hpactor::config {

/// \brief Load a pre-compiled binary topology file via mmap.
///
/// Maps the file read-only and reconstructs a TopologyModel with string
/// pointers pointing directly into the mapped region (zero-copy). Validates
/// the magic number and version before reconstructing.
///
/// \param[in] path Path to the binary topology file.
/// \return A fully populated TopologyModel on success, or an error result
///         on file-not-found, invalid magic, or version mismatch.
result<TopologyModel> load_binary_topology(const std::string& path);

} // namespace hpactor::config
