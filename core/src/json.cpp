#include "json.h"

#include <charconv>
#include <climits>
#include <cmath>
// Apple's libc++ has no floating-point from_chars for older macOS targets (10.15)
#if defined(__APPLE__) && !defined(G2M_JSON_STREAM_NUMBERS)
#define G2M_JSON_STREAM_NUMBERS
#endif
#ifdef G2M_JSON_STREAM_NUMBERS
#include <locale>
#include <sstream>
#endif

namespace g2m::json {

const Value* Value::get(std::string_view key) const {
    for (std::size_t i = keys.size(); i-- > 0;)
        if (keys[i] == key) return &items[i];
    return nullptr;
}

bool Value::as_int(int& out) const {
    if (type != Type::number || number != std::floor(number) || number < INT_MIN || number > INT_MAX)
        return false;
    out = static_cast<int>(number);
    return true;
}

namespace {

constexpr int kMaxDepth = 64;

struct Parser {
    std::string_view text;
    std::size_t pos = 0;
    std::string error;

    bool fail(const std::string& message) {
        if (error.empty()) error = message + " at byte " + std::to_string(pos);
        return false;
    }

    bool at_end() const { return pos >= text.size(); }
    char peek() const { return at_end() ? '\0' : text[pos]; }

    void skip_space() {
        while (!at_end() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
            ++pos;
    }

    bool literal(std::string_view word) {
        if (text.substr(pos, word.size()) != word) return fail("invalid literal");
        pos += word.size();
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("nested too deeply");
        skip_space();
        switch (peek()) {
            case '{': return object(out, depth);
            case '[': return array(out, depth);
            case '"': out.type = Value::Type::string; return string(out.string);
            case 't': out.type = Value::Type::boolean; out.boolean = true; return literal("true");
            case 'f': out.type = Value::Type::boolean; out.boolean = false; return literal("false");
            case 'n': out.type = Value::Type::null; return literal("null");
            default: return number(out);
        }
    }

    bool object(Value& out, int depth) {
        out.type = Value::Type::object;
        ++pos;  // '{'
        skip_space();
        if (peek() == '}') {
            ++pos;
            return true;
        }
        for (;;) {
            skip_space();
            if (peek() != '"') return fail("expected a key");
            out.keys.emplace_back();
            if (!string(out.keys.back())) return false;
            skip_space();
            if (peek() != ':') return fail("expected ':'");
            ++pos;
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) return false;
            skip_space();
            if (peek() == ',') {
                ++pos;
                continue;
            }
            if (peek() == '}') {
                ++pos;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value& out, int depth) {
        out.type = Value::Type::array;
        ++pos;  // '['
        skip_space();
        if (peek() == ']') {
            ++pos;
            return true;
        }
        for (;;) {
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) return false;
            skip_space();
            if (peek() == ',') {
                ++pos;
                continue;
            }
            if (peek() == ']') {
                ++pos;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool hex4(unsigned& out) {
        if (text.size() - pos < 4) return fail("short \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text[pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= c - '0';
            else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }

    static void append_utf8(std::string& s, unsigned c) {
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else if (c < 0x800) {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += static_cast<char>(0xE0 | (c >> 12));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (c >> 18));
            s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    bool string(std::string& out) {
        ++pos;  // '"'
        for (;;) {
            if (at_end()) return fail("unterminated string");
            char c = text[pos++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_end()) return fail("unterminated string");
            switch (text[pos++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned c1 = 0;
                    if (!hex4(c1)) return false;
                    if (c1 >= 0xD800 && c1 < 0xDC00) {  // a surrogate pair
                        unsigned c2 = 0;
                        if (text.substr(pos, 2) != "\\u") return fail("lone surrogate");
                        pos += 2;
                        if (!hex4(c2)) return false;
                        if (c2 < 0xDC00 || c2 >= 0xE000) return fail("bad surrogate pair");
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    } else if (c1 >= 0xDC00 && c1 < 0xE000) {
                        return fail("lone surrogate");
                    }
                    append_utf8(out, c1);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }

    // JSON's grammar is checked here; std::from_chars (locale-independent,
    // unlike strtod) converts the span.
    bool number(Value& out) {
        std::size_t start = pos;
        auto digits = [&] {
            std::size_t from = pos;
            while (!at_end() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            return pos - from;
        };
        if (peek() == '-') ++pos;
        if (peek() == '0') ++pos;
        else if (digits() == 0) return fail("invalid value");
        if (peek() == '.') {
            ++pos;
            if (digits() == 0) return fail("invalid number");
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos;
            if (peek() == '+' || peek() == '-') ++pos;
            if (digits() == 0) return fail("invalid number");
        }
        const char* first = text.data() + start;
        const char* last = text.data() + pos;
#ifdef G2M_JSON_STREAM_NUMBERS
        // a stream in the classic locale reads the same (correctly rounded)
        std::istringstream in(std::string(first, last));
        in.imbue(std::locale::classic());
        in >> out.number;
        if (in.fail() || in.peek() != std::char_traits<char>::eof() || !std::isfinite(out.number))
            return fail("number out of range");
#else
        auto [end, ec] = std::from_chars(first, last, out.number);
        if (ec != std::errc() || end != last) return fail("number out of range");
#endif
        out.type = Value::Type::number;
        return true;
    }
};

}  // namespace

bool parse(std::string_view text, Value& out, std::string& error) {
    Parser p;
    p.text = text;
    out = Value();
    bool ok = p.value(out, 0);
    if (ok) {
        p.skip_space();
        if (!p.at_end()) ok = p.fail("trailing characters");
    }
    error = p.error;
    return ok;
}

}  // namespace g2m::json
