// A minimal test harness: TEST(name) { CHECK(...); }, run by check_main.cpp.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    void (*run)();
};

inline std::vector<Test>& tests() {
    static std::vector<Test> all;
    return all;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, void (*run)()) { tests().push_back({name, run}); }
};

inline bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace check

#define TEST(name)                                         \
    static void name();                                    \
    static check::Register name##_registered(#name, name); \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++check::failures();                                                       \
        }                                                                              \
    } while (0)

// CHECK with a message (printf-style) printed on failure
#define CHECK_MSG(cond, ...)                                                           \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed: ", __FILE__, __LINE__, #cond); \
            std::fprintf(stderr, __VA_ARGS__);                                         \
            std::fprintf(stderr, "\n");                                                \
            ++check::failures();                                                       \
        }                                                                              \
    } while (0)
