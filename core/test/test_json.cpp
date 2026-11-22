#include "json.h"

#include "check.h"

using g2m::json::Value;

static Value parse_ok(const std::string& text) {
    Value v;
    std::string error;
    bool ok = g2m::json::parse(text, v, error);
    CHECK_MSG(ok, "%s: %s", text.c_str(), error.c_str());
    return v;
}

static std::string parse_error(const std::string& text) {
    Value v;
    std::string error;
    bool ok = g2m::json::parse(text, v, error);
    CHECK_MSG(!ok, "%s parsed", text.c_str());
    CHECK(!error.empty());
    return error;
}

TEST(objects_and_arrays) {
    Value v = parse_ok(" {\"a\": [1, 2.5, -3e2, 0], \"b\": {\"c\": true, \"d\": null}, \"e\": false} \n");
    CHECK(v.is_object());
    const Value* a = v.get("a");
    CHECK(a && a->is_array() && a->items.size() == 4);
    CHECK(a->items[0].number == 1 && a->items[1].number == 2.5 && a->items[2].number == -300 &&
          a->items[3].number == 0);
    const Value* b = v.get("b");
    CHECK(b && b->get("c") && b->get("c")->boolean);
    CHECK(b->get("d")->type == Value::Type::null);
    CHECK(v.get("e")->type == Value::Type::boolean && !v.get("e")->boolean);
    CHECK(v.get("missing") == nullptr);
    CHECK(parse_ok("[]").items.empty() && parse_ok("{}").is_object());
}

TEST(numbers) {
    CHECK(parse_ok("0.45").number == 0.45);
    CHECK(parse_ok("4025189449").number == 4025189449.0);
    CHECK(parse_ok("-0.5E-1").number == -0.05);
    int n;
    CHECK(parse_ok("48000").as_int(n) && n == 48000);
    CHECK(!parse_ok("1.5").as_int(n));
    CHECK(!parse_ok("1e12").as_int(n));
    for (const char* bad : {"01", "1.", ".5", "+1", "-", "1e", "0x10", "NaN", "Infinity", "1e999"})
        parse_error(bad);
}

TEST(strings) {
    CHECK(parse_ok("\"a\\\"b\\\\c\\/d\\n\\t\"").string == "a\"b\\c/d\n\t");
    CHECK(parse_ok("\"\\u00e9\"").string == "\xc3\xa9");
    CHECK(parse_ok("\"\\ud83c\\udfb8\"").string == "\xf0\x9f\x8e\xb8");  // a guitar
    CHECK(parse_ok("\"caf\xc3\xa9\"").string == "caf\xc3\xa9");
    for (const char* bad : {"\"abc", "\"\\x\"", "\"\\u12\"", "\"\\ud83c\"", "\"\\udfb8\"", "\"a\nb\""})
        parse_error(bad);
}

TEST(last_duplicate_key_wins) {
    CHECK(parse_ok("{\"a\": 1, \"a\": 2}").get("a")->number == 2);
}

TEST(errors) {
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "{\"a\": 1,}", "{1: 2}", "[1 2]", "tru", "nul",
                            "{} x", "[1]]"})
        parse_error(bad);
    CHECK(check::contains(parse_error("[1, x]"), "byte 4"));
    std::string deep(100, '[');
    CHECK(check::contains(parse_error(deep + std::string(100, ']')), "nested too deeply"));
    parse_ok(std::string(60, '[') + std::string(60, ']'));
}
