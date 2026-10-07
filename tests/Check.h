#pragma once

// Minimal self-contained test support (no third-party framework, per the
// minimize-dependencies principle). Each test file is its own executable:
// CHECK() records failures, and main() returns finish("suite").

#include <cstdio>

namespace uw::test {

inline int failures = 0;

inline int finish(const char* suite) {
    if (failures != 0) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", suite, failures);
        return 1;
    }
    std::printf("%s tests passed\n", suite);
    return 0;
}

} // namespace uw::test

#define CHECK(expr)                                                                \
    do {                                                                           \
        if (!(expr)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,  \
                         #expr);                                                   \
            ++uw::test::failures;                                           \
        }                                                                          \
    } while (0)
