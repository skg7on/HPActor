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

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

struct TreeNode {
    std::string name;
    std::string description;
    std::vector<TreeNode> children;
};

class OutputFormatter {
  public:
    virtual ~OutputFormatter() = default;
    virtual void header(const std::string& title) = 0;
    virtual void table(const std::vector<std::string>& columns,
                       const std::vector<std::vector<std::string>>& rows) = 0;
    virtual void key_value(const std::map<std::string, std::string>& pairs) = 0;
    virtual void tree(const TreeNode& root) = 0;
    virtual void raw(const std::string& text) = 0;
    virtual void error(const std::string& message) = 0;
    virtual std::string finalize() = 0;

    static std::unique_ptr<OutputFormatter> create(const std::string& format);
};

} // namespace cli
} // namespace hpactor