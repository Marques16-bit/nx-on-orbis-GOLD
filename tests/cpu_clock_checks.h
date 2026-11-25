// SPDX-License-Identifier: GPL-3.0-or-later
// Checks of frontend/cpu_clock.h. Included by tests/safe_mode_test.cpp (a host test the CI workflow already builds: a new test file would not be run there, the workflow
// cannot be edited from the update zip), like symbols_checks.h.

#pragma once

#include <cstdint>
#include <cstdio>

#include "cpu_clock.h"

namespace CpuClockChecks {

using CheckFn = void (*)(bool, const char*);

inline void Run(CheckFn Check) {
    std::printf("cpu clock (0.9.7 fix 4)\n");
    using namespace Ps4Clock;
    // The arithmetic, with a clock and a counter made up: the counter moves 2130 ticks per microsecond, the clock 37 us per read.
    {
        std::uint64_t now = 1000, ticks = 0;
        const auto now_us = [&] { now += 37; ticks += 37 * 2130; return now; };
        const auto tsc = [&] { return ticks; };
        const double mhz = MeasureMHz(now_us, tsc, 50000);
        Check(mhz > 2129.5 && mhz < 2130.5, "a counter of 2130 ticks per microsecond measures 2130 MHz");
    }
    {
        std::uint64_t now = 5, ticks = 100;
        const auto now_us = [&] { now += 1000; ticks += 1600 * 1000; return now; };
        const auto tsc = [&] { return ticks; };
        const double mhz = MeasureMHz(now_us, tsc, 50000);
        Check(mhz > 1599.5 && mhz < 1600.5, "and 1600 ticks per microsecond measures 1600 MHz");
    }
    {
        // A clock that never moves ends by the spin limit, and says 0 instead of dividing by zero.
        const auto stuck = [] { return std::uint64_t(777); };
        std::uint64_t t = 0;
        const auto counter = [&] { return t += 5; };
        Check(MeasureMHz(stuck, counter, 50000, 1000) == 0.0, "a clock that stands still gives 0, not a number");
        // A counter that does not move (a build that is not x86-64) gives 0 too.
        std::uint64_t now = 0;
        const auto moving = [&] { return now += 100; };
        const auto frozen = [] { return std::uint64_t(42); };
        Check(MeasureMHz(moving, frozen, 50000) == 0.0, "a counter that stands still gives 0");
    }
    Check(Classify(1600.0) == Known::Base && Classify(1593.0) == Known::Base && Classify(1650.0) == Known::Base, "1.6 GHz (and what is near it) is the base clock");
    Check(Classify(2130.0) == Known::Pro && Classify(2133.0) == Known::Pro && Classify(2100.0) == Known::Pro, "2.13 GHz (and what is near it) is the Pro's");
    Check(Classify(0.0) == Known::Unknown && Classify(1900.0) == Known::Unknown && Classify(3500.0) == Known::Unknown && Classify(-5.0) == Known::Unknown,
          "anything else, 0 and negatives included, is unknown");
    Check(Describe(Known::Base)[0] != 0 && Describe(Known::Pro)[0] != 0 && Describe(Known::Unknown)[0] != 0, "every answer has a sentence");
    // The real counter of this machine: only that it moves and is not absurd (a CI runner may be a virtual machine, so nothing tighter than this).
    {
        const std::uint64_t first = Tsc();
        const std::uint64_t second = Tsc();
#if defined(__x86_64__) || defined(__i386__)
        Check(second >= first, "the time-stamp counter does not go backwards");
#else
        Check(first == 0 && second == 0, "off x86 the counter reads 0");
#endif
    }
}

} // namespace CpuClockChecks
