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

namespace hpactor {
namespace cli {

struct CommandNode;
class ICommand;

/// \brief Mount a single registered command into the tree, creating
///        intermediate nodes as needed.
///
/// \param[in,out] root  Root of the command tree.
/// \param[in]     cmd   The command to mount.
void mount_command(CommandNode* root, const ICommand& cmd);

/// \brief Populate a command tree root from the global CommandRegistry.
///
/// Iterates all registered commands, sorts them by order/path for deterministic
/// assembly, mounts each into the tree (creating intermediate nodes as needed),
/// and registers forward-looking ask commands.
///
/// \param[in,out] root  The root CommandNode to populate.  Must be an
///                      already-constructed node (e.g., the "/" root).
void build_command_tree_from_registry(CommandNode& root);

} // namespace cli
} // namespace hpactor
