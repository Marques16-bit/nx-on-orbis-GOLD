// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.7 fix 4: how fast does this console's CPU time-stamp counter tick?
//
// Why: the other PS4 Switch port in the same hands (ps4-switch-emu) says that a PS4 Pro runs an app that does not declare Pro support at the BASE PS4's clock
// (CPU 1.6 GHz instead of 2.13 GHz) unless "Boost Mode" is on in the console's system settings. That is a third of the CPU speed, and this emulator is limited by the CPU.
// Nothing in this repository ever measured it, and docs/STATUS.md reasons with 2.13 GHz. This reads the counter against the monotonic clock once at start and boot.log says
// what it found, so the question has an answer the next time a log comes back (with Boost Mode off, then on).
//
// What the number is: ticks of the TSC per microsecond. On AMD's Jaguar the TSC runs at the nominal core clock, so it is the number to look at; whether the kernel moves it
// together with Boost Mode is exactly what the log will show. The text says "looks like", not "is".
//
// Pure (the clock and the counter are parameters), so a PC tests the arithmetic; the console reads the real ones in main.cpp.

#pragma once

#include <cstdint>

namespace Ps4Clock {

/// The time-stamp counter (0 on a build that is not x86-64, which is only the host tests on another machine).
inline std::uint64_t Tsc() {
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

/// Ticks per microsecond (= MHz) over about `window_us` of `now_us`, measured by spinning (a sleeping core may be clocked down, which is not what a game sees).
/// 0 when the counter did not move or the clock did not either. `max_spins` ends a clock that stands still.
template <class NowUs, class TscFn>
double MeasureMHz(NowUs now_us, TscFn tsc, std::uint64_t window_us, std::uint64_t max_spins = 400000000ull) {
    const std::uint64_t t0 = now_us();
    const std::uint64_t c0 = tsc();
    std::uint64_t t1 = t0;
    std::uint64_t spins = 0;
    while (t1 - t0 < window_us && spins < max_spins) {
        t1 = now_us();
        ++spins;
    }
    const std::uint64_t c1 = tsc();
    if (t1 <= t0 || c1 <= c0) {
        return 0.0;
    }
    return double(c1 - c0) / double(t1 - t0);
}

enum class Known { Unknown, Base, Pro };

/// Which PS4 CPU clock a measured value looks like: 1.6 GHz (every PS4 in base mode) or 2.13 GHz (a PS4 Pro in Pro mode or with Boost Mode). 4% either side.
inline Known Classify(double mhz) {
    const auto near = [&](double target) { return mhz >= target * 0.96 && mhz <= target * 1.04; };
    if (near(1600.0)) {
        return Known::Base;
    }
    if (near(2130.0)) {
        return Known::Pro;
    }
    return Known::Unknown;
}

inline const char* Describe(Known k) {
    switch (k) {
    case Known::Base:
        return "looks like the base clock of a PS4 (1.6 GHz); a PS4 Pro can run its CPU at 2.13 GHz with Boost Mode on in the system settings";
    case Known::Pro:
        return "looks like the PS4 Pro's 2.13 GHz";
    default:
        return "not one of the two PS4 clocks (1.6 and 2.13 GHz); only the number is of use";
    }
}

} // namespace Ps4Clock
