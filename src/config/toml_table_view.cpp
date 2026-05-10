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

#include <hpactor/config/toml_table_view.hpp>

#include <toml.hpp>

namespace hpactor::config {

// -----------------------------------------------------------------------------
// TomlValueView
// -----------------------------------------------------------------------------
TomlValueView::TomlValueView(const void* node) noexcept : node_(node) {}

TomlValueView::Kind TomlValueView::kind() const noexcept {
    if (!node_)
        return Kind::Missing;
    const auto* n = static_cast<const toml::node*>(node_);
    if (n->is_string())
        return Kind::String;
    if (n->is_integer())
        return Kind::Integer;
    if (n->is_floating_point())
        return Kind::FloatingPoint;
    if (n->is_boolean())
        return Kind::Boolean;
    if (n->is_array())
        return Kind::Array;
    if (n->is_table())
        return Kind::Table;
    return Kind::Missing;
}

bool TomlValueView::is_string() const noexcept {
    return node_ && static_cast<const toml::node*>(node_)->is_string();
}

bool TomlValueView::is_integer() const noexcept {
    return node_ && static_cast<const toml::node*>(node_)->is_integer();
}

bool TomlValueView::is_floating_point() const noexcept {
    return node_ && static_cast<const toml::node*>(node_)->is_floating_point();
}

bool TomlValueView::is_boolean() const noexcept {
    return node_ && static_cast<const toml::node*>(node_)->is_boolean();
}

std::string TomlValueView::as_string(std::string_view fallback) const {
    if (!node_)
        return std::string{fallback};
    const auto* n = static_cast<const toml::node*>(node_);
    if (!n->is_string())
        return std::string{fallback};
    return std::string{n->value<std::string>().value_or(std::string{fallback})};
}

int64_t TomlValueView::as_int64(int64_t fallback) const noexcept {
    if (!node_)
        return fallback;
    const auto* n = static_cast<const toml::node*>(node_);
    if (!n->is_integer())
        return fallback;
    return n->value<int64_t>().value_or(fallback);
}

double TomlValueView::as_double(double fallback) const noexcept {
    if (!node_)
        return fallback;
    const auto* n = static_cast<const toml::node*>(node_);
    if (n->is_floating_point())
        return n->value<double>().value_or(fallback);
    if (n->is_integer()) {
        auto val = n->value<int64_t>();
        if (val)
            return static_cast<double>(*val);
    }
    return fallback;
}

bool TomlValueView::as_bool(bool fallback) const noexcept {
    if (!node_)
        return fallback;
    const auto* n = static_cast<const toml::node*>(node_);
    if (!n->is_boolean())
        return fallback;
    return n->value<bool>().value_or(fallback);
}

// -----------------------------------------------------------------------------
// TomlTableView
// -----------------------------------------------------------------------------
TomlTableView::TomlTableView(const void* table) noexcept : table_(table) {}

TomlTableView make_toml_table_view(const void* table) noexcept {
    return TomlTableView{table};
}

bool TomlTableView::valid() const noexcept {
    return table_ != nullptr;
}

bool TomlTableView::contains(std::string_view key) const {
    if (!table_)
        return false;
    return static_cast<const toml::table*>(table_)->contains(key);
}

TomlValueView TomlTableView::value(std::string_view key) const {
    if (!table_)
        return TomlValueView{};
    auto* node = static_cast<const toml::table*>(table_)->get(key);
    return TomlValueView{node};
}

TomlTableView TomlTableView::table(std::string_view key) const {
    if (!table_)
        return TomlTableView{};
    auto node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_table())
        return TomlTableView{};
    return TomlTableView{node->as_table()};
}

std::string
TomlTableView::read_string(std::string_view key, std::string_view fallback) const {
    if (!table_)
        return std::string{fallback};
    auto node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_string())
        return std::string{fallback};
    return std::string{node->value<std::string>().value_or(std::string{fallback})};
}

uint32_t TomlTableView::read_uint32(std::string_view key,
                                    uint32_t fallback) const noexcept {
    if (!table_)
        return fallback;
    auto node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_integer())
        return fallback;
    auto val = node->value<int64_t>();
    if (!val || *val < 0)
        return fallback;
    return static_cast<uint32_t>(*val);
}

bool TomlTableView::read_bool(std::string_view key, bool fallback) const noexcept {
    if (!table_)
        return fallback;
    auto* node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_boolean())
        return fallback;
    return node->value<bool>().value_or(fallback);
}

double
TomlTableView::read_double(std::string_view key, double fallback) const noexcept {
    if (!table_)
        return fallback;
    auto* node = static_cast<const toml::table*>(table_)->get(key);
    if (!node)
        return fallback;
    if (node->is_floating_point())
        return node->value<double>().value_or(fallback);
    if (node->is_integer()) {
        auto val = node->value<int64_t>();
        if (val)
            return static_cast<double>(*val);
    }
    return fallback;
}

void TomlTableView::for_each_string_array(std::string_view key,
                                          const StringArrayVisitor& visitor) const {
    if (!table_)
        return;
    auto node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_array())
        return;
    for (const auto& v : *node->as_array()) {
        if (v.is_string())
            visitor(std::string_view{v.value<std::string>().value_or("")});
    }
}

void TomlTableView::for_each_table_array(std::string_view key,
                                         const TableArrayVisitor& visitor) const {
    if (!table_)
        return;
    auto node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_array())
        return;
    for (const auto& v : *node->as_array()) {
        if (v.is_table())
            visitor(TomlTableView{v.as_table()});
    }
}

void TomlTableView::for_each_subtable(std::string_view key,
                                      const NamedTableVisitor& visitor) const {
    if (!table_)
        return;
    auto node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_table())
        return;
    for (const auto& [k, v] : *node->as_table()) {
        if (v.is_table())
            visitor(std::string_view{k.str()}, TomlTableView{v.as_table()});
    }
}

void TomlTableView::for_each_key_value(std::string_view key,
                                       const KeyValueVisitor& visitor) const {
    if (!table_)
        return;
    auto* node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_table())
        return;
    for (const auto& [k, v] : *node->as_table()) {
        visitor(std::string_view{k.str()}, TomlValueView{&v});
    }
}

void TomlTableView::for_each_integer_array(std::string_view key,
                                           const IntegerArrayVisitor& visitor) const {
    if (!table_)
        return;
    auto* node = static_cast<const toml::table*>(table_)->get(key);
    if (!node || !node->is_array())
        return;
    for (const auto& v : *node->as_array()) {
        if (v.is_integer()) {
            auto val = v.value<int64_t>();
            if (val)
                visitor(*val);
        }
    }
}

void TomlTableView::for_each_entry(const KeyValueVisitor& visitor) const {
    if (!table_)
        return;
    const auto* tbl = static_cast<const toml::table*>(table_);
    for (const auto& [k, v] : *tbl) {
        visitor(std::string_view{k.str()}, TomlValueView{&v});
    }
}

} // namespace hpactor::config
