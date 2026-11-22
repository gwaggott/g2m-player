// A small JSON parser for model file headers (load time only).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace g2m::json {

struct Value {
    enum class Type { null, boolean, number, string, array, object };

    Type type = Type::null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> items;      // array elements, or object values
    std::vector<std::string> keys;  // object keys, parallel to items

    bool is_number() const { return type == Type::number; }
    bool is_string() const { return type == Type::string; }
    bool is_array() const { return type == Type::array; }
    bool is_object() const { return type == Type::object; }

    // An object's member (the last one of that key), or nullptr.
    const Value* get(std::string_view key) const;
    // True with the value in `out` if this is a whole number in int range.
    bool as_int(int& out) const;
};

// Parses a whole document (surrounding whitespace allowed). Never throws.
bool parse(std::string_view text, Value& out, std::string& error);

}  // namespace g2m::json
