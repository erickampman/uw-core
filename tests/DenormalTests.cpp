// Denormals flushed to zero (Core/Denormals.h), on x86 and ARM64 alike: a
// result too small to be normal becomes 0, a denormal input counts as 0, and
// the thread's mode is back as it was afterwards. The engine runs every block
// under it.

#include <cfloat>

#include "Check.h"
#include "uw/base/Denormals.h"

using namespace uw;

namespace {

// volatile, so the compiler can't work these out ahead of time (it would,
// without the flush).
volatile float kTiny   = 1e-38f;   // normal, just above FLT_MIN
volatile float kSmall  = 1e-3f;
volatile float kOne    = 1.0f;
volatile float kDenorm = 1e-41f;   // denormal already

void testFlush() {
    const float before = kTiny * kSmall;   // 1e-41: denormal
    CHECK(before != 0.0f && before < FLT_MIN);
    {
        const ScopedFlushDenormals flush;
        CHECK(kTiny * kSmall == 0.0f);   // a denormal result: 0
        CHECK(kDenorm * kOne == 0.0f);   // a denormal input: 0
        CHECK(kTiny * kOne == kTiny);    // normal values untouched
    }
    const float after = kTiny * kSmall;   // the thread's mode restored
    CHECK(after != 0.0f && after == before);
}

void testNested() {
    const ScopedFlushDenormals outer;
    {
        const ScopedFlushDenormals inner;
    }
    CHECK(kTiny * kSmall == 0.0f);   // the inner one restores the outer one's mode, still flushing
}

} // namespace

int main() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE__) || defined(__aarch64__)
    testFlush();
    testNested();
#endif
    return test::finish("Denormals");
}
