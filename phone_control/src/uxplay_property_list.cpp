// SPDX-License-Identifier: GPL-3.0-or-later
module;

#include <plist/plist.h>

module uxplay_property_list;

import std;
import uxplay_status;

namespace uxplay
{
    namespace
    {
        [[nodiscard]] plist_t as_plist(void *node)
        {
            return static_cast<plist_t>(node);
        }
    }

    bool property_node::present() const
    {
        return node != nullptr;
    }

    property_node property_node::item(std::string_view key) const
    {
        if (node == nullptr || plist_get_node_type(as_plist(node)) != PLIST_DICT)
        {
            return property_node{};
        }
        std::string const terminated{key};
        return property_node{plist_dict_get_item(as_plist(node), terminated.c_str())};
    }

    std::size_t property_node::size() const
    {
        if (node == nullptr || plist_get_node_type(as_plist(node)) != PLIST_ARRAY)
        {
            return 0;
        }
        return plist_array_get_size(as_plist(node));
    }

    property_node property_node::element(std::size_t index) const
    {
        if (index >= size())
        {
            return property_node{};
        }
        return property_node{plist_array_get_item(as_plist(node), static_cast<std::uint32_t>(index))};
    }

    std::vector<std::string> property_node::keys() const
    {
        std::vector<std::string> names;
        if (node == nullptr || plist_get_node_type(as_plist(node)) != PLIST_DICT)
        {
            return names;
        }
        plist_dict_iter position = nullptr;
        plist_dict_new_iter(as_plist(node), &position);
        if (position == nullptr)
        {
            return names;
        }
        for (;;)
        {
            char *key = nullptr;
            plist_t value = nullptr;
            plist_dict_next_item(as_plist(node), position, &key, &value);
            if (key == nullptr)
            {
                break;
            }
            names.emplace_back(key);
            plist_mem_free(key);
        }
        plist_mem_free(position);
        return names;
    }

    std::optional<std::string> property_node::text() const
    {
        if (node == nullptr || plist_get_node_type(as_plist(node)) != PLIST_STRING)
        {
            return std::nullopt;
        }
        std::uint64_t length = 0;
        char const *const value = plist_get_string_ptr(as_plist(node), &length);
        if (value == nullptr)
        {
            return std::nullopt;
        }
        return std::string{value, static_cast<std::size_t>(length)};
    }

    std::optional<std::uint64_t> property_node::number() const
    {
        if (node == nullptr || plist_get_node_type(as_plist(node)) != PLIST_INT)
        {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        plist_get_uint_val(as_plist(node), &value);
        return value;
    }

    std::optional<bool> property_node::flag() const
    {
        if (node == nullptr || plist_get_node_type(as_plist(node)) != PLIST_BOOLEAN)
        {
            return std::nullopt;
        }
        std::uint8_t value = 0;
        plist_get_bool_val(as_plist(node), &value);
        return value != 0;
    }

    std::optional<std::vector<std::uint8_t>> property_node::data() const
    {
        if (node == nullptr || plist_get_node_type(as_plist(node)) != PLIST_DATA)
        {
            return std::nullopt;
        }
        std::uint64_t length = 0;
        char const *const value = plist_get_data_ptr(as_plist(node), &length);
        if (value == nullptr)
        {
            return std::vector<std::uint8_t>{};
        }
        auto const *const bytes = reinterpret_cast<std::uint8_t const *>(value);
        return std::vector<std::uint8_t>(bytes, bytes + length);
    }

    property_list::property_list()
        : top{plist_new_dict()}
    {
    }

    property_list::property_list(void *adopted)
        : top{adopted}
    {
    }

    property_list::~property_list()
    {
        if (top != nullptr)
        {
            plist_free(as_plist(top));
        }
    }

    property_list::property_list(property_list &&other) noexcept
        : top{std::exchange(other.top, nullptr)}
    {
    }

    property_list &property_list::operator=(property_list &&other) noexcept
    {
        if (this != &other)
        {
            if (top != nullptr)
            {
                plist_free(as_plist(top));
            }
            top = std::exchange(other.top, nullptr);
        }
        return *this;
    }

    outcome<property_list> property_list::parse(std::span<std::uint8_t const> bytes)
    {
        plist_t parsed = nullptr;
        plist_format_t format = PLIST_FORMAT_NONE;
        plist_err_t const result = plist_from_memory(reinterpret_cast<char const *>(bytes.data()),
                                                     static_cast<std::uint32_t>(bytes.size()), &parsed, &format);
        if (result != PLIST_ERR_SUCCESS || parsed == nullptr)
        {
            return std::unexpected(make_failure("plist_from_memory", result, "the data is not a property list"));
        }
        return property_list{parsed};
    }

    property_node property_list::root() const
    {
        return property_node{top};
    }

    void property_list::set_text(std::string_view key, std::string_view value)
    {
        std::string const terminated_key{key};
        std::string const terminated_value{value};
        plist_dict_set_item(as_plist(top), terminated_key.c_str(), plist_new_string(terminated_value.c_str()));
    }

    void property_list::set_number(std::string_view key, std::uint64_t value)
    {
        std::string const terminated_key{key};
        plist_dict_set_item(as_plist(top), terminated_key.c_str(), plist_new_uint(value));
    }

    void property_list::set_flag(std::string_view key, bool value)
    {
        std::string const terminated_key{key};
        plist_dict_set_item(as_plist(top), terminated_key.c_str(), plist_new_bool(value ? 1 : 0));
    }

    void property_list::set_data(std::string_view key, std::span<std::uint8_t const> value)
    {
        std::string const terminated_key{key};
        // (an empty span may have no address at all)
        plist_dict_set_item(as_plist(top), terminated_key.c_str(),
                            plist_new_data(value.empty() ? "" : reinterpret_cast<char const *>(value.data()),
                                           value.size()));
    }

    void property_list::set_list(std::string_view key, property_list value)
    {
        std::string const terminated_key{key};
        // (the dictionary owns the nodes from here on)
        plist_dict_set_item(as_plist(top), terminated_key.c_str(), as_plist(std::exchange(value.top, nullptr)));
    }

    outcome<std::vector<std::uint8_t>> property_list::to_binary() const
    {
        char *bytes = nullptr;
        std::uint32_t length = 0;
        plist_err_t const result = plist_to_bin(as_plist(top), &bytes, &length);
        if (result != PLIST_ERR_SUCCESS || bytes == nullptr)
        {
            return std::unexpected(make_failure("plist_to_bin", result, "the property list cannot be written"));
        }
        auto const *const first = reinterpret_cast<std::uint8_t const *>(bytes);
        std::vector<std::uint8_t> binary(first, first + length);
        plist_mem_free(bytes);
        return binary;
    }

    outcome<std::string> property_list::to_xml() const
    {
        char *text = nullptr;
        std::uint32_t length = 0;
        plist_err_t const result = plist_to_xml(as_plist(top), &text, &length);
        if (result != PLIST_ERR_SUCCESS || text == nullptr)
        {
            return std::unexpected(make_failure("plist_to_xml", result, "the property list cannot be written"));
        }
        std::string xml{text, length};
        plist_mem_free(text);
        return xml;
    }
}
