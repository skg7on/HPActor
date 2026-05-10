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

#include <hpactor/config/toml_parser_registry.hpp>

#include <algorithm>
#include <mutex>

namespace hpactor::config {

TomlParserRegistry& TomlParserRegistry::instance() {
    static auto* reg = new TomlParserRegistry();
    return *reg;
}

bool TomlParserRegistry::add_system_parser(std::string_view name, int order,
                                           SystemParserFactory factory) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    for (const auto& entry : system_parsers_) {
        if (entry.name == name)
            return false;
    }

    system_parsers_.push_back({std::string{name}, order, factory});
    return true;
}

bool TomlParserRegistry::add_document_parser(std::string_view name, int order,
                                             DocumentParserFactory factory) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    for (const auto& entry : document_parsers_) {
        if (entry.name == name)
            return false;
    }

    document_parsers_.push_back({std::string{name}, order, factory});
    return true;
}

std::vector<std::unique_ptr<ITomlSystemConfigParser>>
TomlParserRegistry::create_system_parsers() const {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    auto entries = system_parsers_;
    std::sort(entries.begin(), entries.end(),
              [](const SystemEntry& a, const SystemEntry& b) {
                  if (a.order != b.order)
                      return a.order < b.order;
                  return a.name < b.name;
              });

    std::vector<std::unique_ptr<ITomlSystemConfigParser>> parsers;
    parsers.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.factory)
            parsers.push_back(entry.factory());
    }
    return parsers;
}

std::vector<std::unique_ptr<ITomlDocumentConfigParser>>
TomlParserRegistry::create_document_parsers() const {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    auto entries = document_parsers_;
    std::sort(entries.begin(), entries.end(),
              [](const DocumentEntry& a, const DocumentEntry& b) {
                  if (a.order != b.order)
                      return a.order < b.order;
                  return a.name < b.name;
              });

    std::vector<std::unique_ptr<ITomlDocumentConfigParser>> parsers;
    parsers.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.factory)
            parsers.push_back(entry.factory());
    }
    return parsers;
}

} // namespace hpactor::config
