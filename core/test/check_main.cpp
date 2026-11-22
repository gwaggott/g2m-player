#include "check.h"

int main() {
    for (const check::Test& test : check::tests()) {
        int before = check::failures();
        test.run();
        std::fprintf(stderr, "%s %s\n", check::failures() == before ? "ok  " : "FAIL", test.name);
    }
    std::fprintf(stderr, "%zu tests, %d failed checks\n", check::tests().size(), check::failures());
    return check::failures() == 0 ? 0 : 1;
}
