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

/// \brief Type-safe opaque view of a single TOML value.
///
/// Wraps a toml++ node pointer without exposing the toml++ type in public
/// headers. All accessors return fallback values when the value is of the
/// wrong kind or missing.
class TomlValueView {
  public:
    /// \brief TOML value type classification.
    enum class Kind : uint8_t {
        Missing = 0,   ///< Key not present or node is null.
        String,        ///< String value.
        Integer,       ///< 64-bit signed integer.
        FloatingPoint, ///< Double-precision float.
        Boolean,       ///< Boolean value.
        Array,         ///< Array of values.
        Table,         ///< Nested table.
    };

    TomlValueView() noexcept = default;

    /// \brief The TOML type of this value.
    ///
    /// \return The value kind.
    Kind kind() const noexcept;

    /// \brief Check whether this value is a string.
    bool is_string() const noexcept;
    /// \brief Check whether this value is an integer.
    bool is_integer() const noexcept;
    /// \brief Check whether this value is a floating-point number.
    bool is_floating_point() const noexcept;
    /// \brief Check whether this value is a boolean.
    bool is_boolean() const noexcept;

    /// \brief Read as string.
    ///
    /// \param[in] fallback Returned if the value is not a string.
    /// \return The string value or \p fallback.
    std::string as_string(std::string_view fallback) const;

    /// \brief Read as int64_t.
    ///
    /// \param[in] fallback Returned if the value is not an integer.
    /// \return The integer value or \p fallback.
    int64_t as_int64(int64_t fallback) const noexcept;

    /// \brief Read as double.
    ///
    /// \param[in] fallback Returned if the value is not a float.
    /// \return The double value or \p fallback.
    double as_double(double fallback) const noexcept;

    /// \brief Read as bool.
    ///
    /// \param[in] fallback Returned if the value is not a boolean.
    /// \return The boolean value or \p fallback.
    bool as_bool(bool fallback) const noexcept;

  private:
    friend class TomlTableView;
    explicit TomlValueView(const void* node) noexcept;

    const void* node_{nullptr};
};

/// \brief Type-safe opaque view of a TOML table.
///
/// Wraps a toml++ table pointer without exposing toml++ in public headers.
/// Provides typed accessors, iteration, and visitor-based traversal.
/// Keeps the toml.hpp include confined to translation units that need it.
class TomlTableView {
  public:
    /// \brief Visitor for iterating over a string array.
    using StringArrayVisitor = std::function<void(std::string_view)>;
    /// \brief Visitor for iterating over a table array.
    using TableArrayVisitor = std::function<void(TomlTableView)>;
    /// \brief Visitor for iterating over named sub-tables.
    using NamedTableVisitor = std::function<void(std::string_view, TomlTableView)>;
    /// \brief Visitor for iterating over key-value pairs.
    using KeyValueVisitor = std::function<void(std::string_view, TomlValueView)>;
    /// \brief Visitor for iterating over an integer array.
    using IntegerArrayVisitor = std::function<void(int64_t)>;

    TomlTableView() noexcept = default;

    /// \brief Whether this view wraps a valid non-null table pointer.
    ///
    /// \return true if the underlying table pointer is non-null.
    bool valid() const noexcept;

    /// \brief Check whether a key exists in the table.
    ///
    /// \param[in] key Key to look up.
    /// \return true if the key is present.
    bool contains(std::string_view key) const;

    /// \brief Get the value at a key.
    ///
    /// \param[in] key Key to look up.
    /// \return A TomlValueView for the key, or a Missing-kind view if absent.
    TomlValueView value(std::string_view key) const;

    /// \brief Get a sub-table at a key.
    ///
    /// \param[in] key Key to look up.
    /// \return A TomlTableView for the sub-table, or an invalid view if absent
    ///         or not a table.
    TomlTableView table(std::string_view key) const;

    /// \brief Read a string value with fallback.
    ///
    /// \param[in] key Key to read.
    /// \param[in] fallback Returned if the key is missing or not a string.
    /// \return The string value or \p fallback.
    std::string
    read_string(std::string_view key, std::string_view fallback = "") const;

    /// \brief Read a uint32_t value with fallback.
    ///
    /// \param[in] key Key to read.
    /// \param[in] fallback Returned if the key is missing or not an integer.
    /// \return The integer value or \p fallback.
    uint32_t read_uint32(std::string_view key, uint32_t fallback = 0) const noexcept;

    /// \brief Read a boolean value with fallback.
    ///
    /// \param[in] key Key to read.
    /// \param[in] fallback Returned if the key is missing or not a bool.
    /// \return The boolean value or \p fallback.
    bool read_bool(std::string_view key, bool fallback = false) const noexcept;

    /// \brief Read a double value with fallback.
    ///
    /// \param[in] key Key to read.
    /// \param[in] fallback Returned if the key is missing or not a float.
    /// \return The double value or \p fallback.
    double read_double(std::string_view key, double fallback = 0.0) const noexcept;

    /// \brief Iterate over a string array with a visitor.
    ///
    /// \param[in] key Key whose value should be an array of strings.
    /// \param[in] visitor Called for each string element.
    void for_each_string_array(std::string_view key,
                               const StringArrayVisitor& visitor) const;

    /// \brief Iterate over a table array with a visitor.
    ///
    /// \param[in] key Key whose value should be an array of tables.
    /// \param[in] visitor Called for each table element.
    void for_each_table_array(std::string_view key,
                              const TableArrayVisitor& visitor) const;

    /// \brief Iterate over named sub-tables of a table.
    ///
    /// \param[in] key Key whose value should be a table of sub-tables.
    /// \param[in] visitor Called with each sub-table name and view.
    void for_each_subtable(std::string_view key,
                           const NamedTableVisitor& visitor) const;

    /// \brief Iterate over key-value pairs within a sub-table.
    ///
    /// \param[in] key Key whose value should be a table.
    /// \param[in] visitor Called with each key and value view.
    void for_each_key_value(std::string_view key,
                            const KeyValueVisitor& visitor) const;

    /// \brief Iterate over an integer array with a visitor.
    ///
    /// \param[in] key Key whose value should be an array of integers.
    /// \param[in] visitor Called for each integer element.
    void for_each_integer_array(std::string_view key,
                                const IntegerArrayVisitor& visitor) const;

    /// \brief Iterate over all key-value pairs at the root of this table.
    ///
    /// \param[in] visitor Called with each key and value view.
    void for_each_entry(const KeyValueVisitor& visitor) const;

  private:
    friend TomlTableView make_toml_table_view(const void* table) noexcept;
    explicit TomlTableView(const void* table) noexcept;

    const void* table_{nullptr};
};

/// \brief Create a TomlTableView from a raw toml++ table pointer.
///
/// This is the sole escape hatch that admits a toml++ pointer into the
/// opaque view system. Called only from toml_parser.cpp.
///
/// \param[in] table Opaque pointer to a toml::table.
/// \return A TomlTableView wrapping the pointer.
TomlTableView make_toml_table_view(const void* table) noexcept;

} // namespace hpactor::config
