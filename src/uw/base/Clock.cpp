#include "uw/base/Clock.h"

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

namespace uw {

#ifdef __APPLE__

namespace {

// Ticks to nanoseconds (125/3 on Apple Silicon, 1/1 on Intel), read once at
// start-up so the audio and MIDI threads never wait on it.
const mach_timebase_info_data_t kTimebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return info;
}();

} // namespace

Nanoseconds hostTimeToNanoseconds(std::uint64_t hostTime) noexcept {
    // In 128 bits: ticks times 125 overflows 64 bits after a few years' uptime.
    const unsigned __int128 ns = static_cast<unsigned __int128>(hostTime) * kTimebase.numer / kTimebase.denom;
    return static_cast<Nanoseconds>(ns);
}

Nanoseconds nowNanoseconds() noexcept {
    return hostTimeToNanoseconds(mach_absolute_time());
}

#else

Nanoseconds nowNanoseconds() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);   // vDSO: no system call
    return static_cast<Nanoseconds>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

#endif

} // namespace uw
