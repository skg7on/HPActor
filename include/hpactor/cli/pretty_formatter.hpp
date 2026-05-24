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

#include <hpactor/cli/output_formatter.hpp>

namespace hpactor {
namespace cli {

class PrettyFormatter : public OutputFormatter {
  public:
    void header(const std::string& title) override;
    void table(const std::vector<std::string>& columns,
               const std::vector<std::vector<std::string>>& rows) override;
    void key_value(const std::map<std::string, std::string>& pairs) override;
    void tree(const TreeNode& root) override;
    void raw(const std::string& text) override;
    void error(const std::string& message) override;
    std::string finalize() override;

  private:
    std::string buffer_;
    int columns_ = 80;

    static std::string dim(const std::string& s) {
        return "\033[2m" + s + "\033[0m";
    }
    static std::string bold(const std::string& s) {
        return "\033[1m" + s + "\033[0m";
    }
    static std::string cyan(const std::string& s) {
        return "\033[36m" + s + "\033[0m";
    }
    static std::string green(const std::string& s) {
        return "\033[32m" + s + "\033[0m";
    }
    static std::string red(const std::string& s) {
        return "\033[31m" + s + "\033[0m";
    }

    static std::string pad_right(const std::string& s, size_t width);
    static std::string horizontal_rule(size_t width);
};

} // namespace cli
} // namespace hpactor