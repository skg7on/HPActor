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
#include <functional>
#include <string>
#include <string_view>

namespace hpactor::config {

class TomlValueView {
  public:
    enum class Kind : uint8_t {
        Missing = 0,
        String,
        Integer,
        FloatingPoint,
        Boolean,
        Array,
        Table,
    };

    TomlValueView() noexcept = default;

    Kind kind() const noexcept;
    bool is_string() const noexcept;
    bool is_integer() const noexcept;
    bool is_floating_point() const noexcept;
    bool is_boolean() const noexcept;

    std::string as_string(std::string_view fallback) const;
    int64_t as_int64(int64_t fallback) const noexcept;
    double as_double(double fallback) const noexcept;
    bool as_bool(bool fallback) const noexcept;

  private:
    friend class TomlTableView;
    explicit TomlValueView(const void* node) noexcept;

    const void* node_{nullptr};
};

class TomlTableView {
  public:
    using StringArrayVisitor = std::function<void(std::string_view)>;
    using TableArrayVisitor = std::function<void(TomlTableView)>;
    using NamedTableVisitor = std::function<void(std::string_view, TomlTableView)>;
    using KeyValueVisitor = std::function<void(std::string_view, TomlValueView)>;
    using IntegerArrayVisitor = std::function<void(int64_t)>;

    TomlTableView() noexcept = default;

    bool valid() const noexcept;
    bool contains(std::string_view key) const;
    TomlValueView value(std::string_view key) const;
    TomlTableView table(std::string_view key) const;

    std::string
    read_string(std::string_view key, std::string_view fallback = "") const;
    uint32_t read_uint32(std::string_view key, uint32_t fallback = 0) const noexcept;
    bool read_bool(std::string_view key, bool fallback = false) const noexcept;
    double read_double(std::string_view key, double fallback = 0.0) const noexcept;

    void for_each_string_array(std::string_view key,
                               const StringArrayVisitor& visitor) const;
    void for_each_table_array(std::string_view key,
                              const TableArrayVisitor& visitor) const;
    void for_each_subtable(std::string_view key,
                           const NamedTableVisitor& visitor) const;
    void for_each_key_value(std::string_view key,
                            const KeyValueVisitor& visitor) const;
    void for_each_integer_array(std::string_view key,
                                const IntegerArrayVisitor& visitor) const;
    void for_each_entry(const KeyValueVisitor& visitor) const;

  private:
    friend TomlTableView make_toml_table_view(const void* table) noexcept;
    explicit TomlTableView(const void* table) noexcept;

    const void* table_{nullptr};
};

TomlTableView make_toml_table_view(const void* table) noexcept;

} // namespace hpactor::config
