// The shared clock (Core/Clock.h, StageHand's docs/macos.md decision 4): it never goes
// back, it keeps time with the standard library's steady clock, and on macOS
// a host time converts onto it.

#include <chrono>
#include <thread>

#include "Check.h"
#include "uw/base/Clock.h"

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

using namespace uw;

namespace {

void testMonotonic() {
    Nanoseconds last = nowNanoseconds();
    for (int i = 0; i < 100000; ++i) {
        const Nanoseconds now = nowNanoseconds();
        CHECK(now >= last);
        last = now;
    }
}

// 50 ms of sleep, as both clocks see it: within a few milliseconds of each
// other (a busy CI machine can oversleep, but both see the same oversleep).
void testKeepsTime() {
    const auto        steadyBefore = std::chrono::steady_clock::now();
    const Nanoseconds before       = nowNanoseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const Nanoseconds elapsed = nowNanoseconds() - before;
    const auto        steadyElapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - steadyBefore).count();
    CHECK(elapsed >= 50'000'000);
    CHECK(elapsed - steadyElapsed < 5'000'000 && steadyElapsed - elapsed < 5'000'000);
}

#ifdef __APPLE__
void testHostTime() {
    const Nanoseconds before = nowNanoseconds();
    const Nanoseconds host   = hostTimeToNanoseconds(mach_absolute_time());
    const Nanoseconds after  = nowNanoseconds();
    CHECK(before <= host && host <= after);
    CHECK(hostTimeToNanoseconds(0) == 0);
}
#endif

} // namespace

int main() {
    testMonotonic();
    testKeepsTime();
#ifdef __APPLE__
    testHostTime();
#endif
    return test::finish("Clock");
}
