// SPDX-License-Identifier: GPL-3.0-or-later
// Property lists, the message format of usbmuxd and of the phone's lockdown
// services (XML or binary on the wire).  A thin layer over libplist that owns
// the nodes and answers "is there a text / a number under this key".
export module uxplay_property_list;

import std;
import uxplay_status;

export namespace uxplay
{
    // A node of a property list.  Not owning: valid while its property_list lives.
    class property_node
    {
    public:
        property_node() = default;

        [[nodiscard]] bool present() const;
        // of a dictionary; an absent node when there is no such key
        [[nodiscard]] property_node item(std::string_view key) const;
        // the keys of a dictionary, in its order
        [[nodiscard]] std::vector<std::string> keys() const;
        // of an array
        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] property_node element(std::size_t index) const;

        // the value, when the node is of that kind
        [[nodiscard]] std::optional<std::string> text() const;
        [[nodiscard]] std::optional<std::uint64_t> number() const;
        [[nodiscard]] std::optional<bool> flag() const;
        [[nodiscard]] std::optional<std::vector<std::uint8_t>> data() const;

    private:
        friend class property_list;

        explicit property_node(void *wrapped)
            : node{wrapped}
        {
        }

        void *node = nullptr; // plist_t
    };

    class property_list
    {
    public:
        // an empty dictionary
        property_list();
        ~property_list();
        property_list(property_list &&other) noexcept;
        property_list &operator=(property_list &&other) noexcept;
        property_list(property_list const &) = delete;
        property_list &operator=(property_list const &) = delete;

        // XML or binary
        [[nodiscard]] static outcome<property_list> parse(std::span<std::uint8_t const> bytes);
        // A JSON object or array (UTF-8), read into the same nodes: objects
        // are dictionaries, a null is a node without a value.  For the one
        // JSON document the program reads (the release list it asks about
        // updates), with the parser of the library that is linked anyway.
        [[nodiscard]] static outcome<property_list> parse_json(std::string_view text);

        [[nodiscard]] property_node root() const;

        // entries of the top dictionary
        void set_text(std::string_view key, std::string_view value);
        void set_number(std::string_view key, std::uint64_t value);
        void set_flag(std::string_view key, bool value);
        void set_data(std::string_view key, std::span<std::uint8_t const> value);
        // another property list as the value under `key`
        void set_list(std::string_view key, property_list value);

        [[nodiscard]] outcome<std::string> to_xml() const;
        // the binary form ("bplist00")
        [[nodiscard]] outcome<std::vector<std::uint8_t>> to_binary() const;

    private:
        explicit property_list(void *adopted);

        void *top = nullptr; // plist_t, owned
    };
}
