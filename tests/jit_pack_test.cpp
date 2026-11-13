// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of frontend/jit_pack.h: a build without the pack keeps the weak defaults (present = 0), and the boot.log line says what happens.
//   g++ -std=c++20 -Wall -Wextra -Ifrontend tests/jit_pack_test.cpp -o jit_pack_test
//   ./jit_pack_test
#include <cstdio>
#include <string>

#include "jit_pack.h"

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}
bool Has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

} // namespace

int main() {
    // Nothing here defines the pack's strong symbols, as in a build without the pack.
    const Ps4Jit::State none = Ps4Jit::Read(true);
    Check(!none.in_build && none.wanted, "no pack: not in the build, and the switch was asked for");
    Check(!Ps4Jit::Apply(true), "no pack: Apply says the pack is not there");
    Check(Has(Ps4Jit::Describe(none), "does not have the pack"), "no pack: the log line says the build does not have it");
    Check(Has(Ps4Jit::Describe(Ps4Jit::Read(false)), "off") && Has(Ps4Jit::Describe(Ps4Jit::Read(false)), "does not have the pack"),
          "no pack and off: still says it is not there");

    Ps4Jit::State s;
    s.in_build = true;
    Check(Has(Ps4Jit::Describe(s), "off (the pack is in this build"), "pack, off: says how to switch it on");
    s.wanted = true;
    Check(Has(Ps4Jit::Describe(s), "no code compiled yet"), "pack, on, status 0: nothing compiled yet");
    s.status = 1;
    s.groups = 12;
    s.accesses = 31;
    const std::string active = Ps4Jit::Describe(s);
    Check(Has(active, "12 groups") && Has(active, "31 accesses"), "pack, on, active: counts the groups and accesses");
    s.status = 3;
    s.why = 0x48;
    Check(Has(Ps4Jit::Describe(s), "not handled") && Has(Ps4Jit::Describe(s), "0x48"), "pack, on, status 3: says why the pass did nothing");
    s.status = 2;
    Check(Has(Ps4Jit::Describe(s), "reports it as off"), "pack, on, status 2: the JIT thinks it is off");

    bool v = false;
    Check(Ps4Jit::ParseSwitch("on", &v) && v, "ParseSwitch: on");
    Check(Ps4Jit::ParseSwitch("off", &v) && !v, "ParseSwitch: off");
    Check(!Ps4Jit::ParseSwitch("yes", &v) && !v, "ParseSwitch: anything else is refused and leaves the value");

    // 0.9.3, part 2: the direct table.
    const Ps4Jit::DirectState dnone = Ps4Jit::ReadDirect(true);
    Check(!dnone.in_build && dnone.wanted && !dnone.live, "direct, no pack: not in the build, and it was asked for");
    Check(!Ps4Jit::ApplyDirect(true) && ps4_direct_table, "direct, no pack: Apply says it is not there, and sets the switch the page table code reads");
    Check(!Ps4Jit::ApplyDirect(false) && !ps4_direct_table, "direct: Apply(false) clears the switch");
    Check(Has(Ps4Jit::DescribeDirect(dnone), "does not have it"), "direct, no pack: the log line says the build does not have it");
    Check(Has(Ps4Jit::DescribeDirect(Ps4Jit::ReadDirect(false)), "off") && Has(Ps4Jit::DescribeDirect(Ps4Jit::ReadDirect(false)), "does not have it"),
          "direct, no pack and off: still says it is not there");
    Ps4Jit::DirectState d;
    d.in_build = true;
    Check(Has(Ps4Jit::DescribeDirect(d), "off (this build has it") && Has(Ps4Jit::DescribeDirect(d), "jit_direct_table=on"), "direct, off: says how to switch it on");
    d.wanted = true;
    Check(Has(Ps4Jit::DescribeDirect(d), "no page table got a direct copy"), "direct, on, not live: says the JIT goes on as before");
    d.live = true;
    Check(Has(Ps4Jit::DescribeDirect(d), "no memory access compiled yet"), "direct, live, nothing compiled");
    d.emitted = 4096;
    Check(Has(Ps4Jit::DescribeDirect(d), "4096 memory lookups read the direct copy"), "direct, live: counts the lookups");

    // 0.9.4: the report of the build, as boot.log quotes it.
    Check(Ps4Jit::ReportLines("").empty() && Ps4Jit::ReportLines("\n\n  \n").empty(), "report: nothing to say gives no lines");
    const auto two = Ps4Jit::ReportLines("apply: 0001.patch: applied\r\napply: 0002.py: applied\n");
    Check(two.size() == 2 && two[0] == "JIT pack, as the build made it: apply: 0001.patch: applied" && two[1].find('\r') == std::string::npos,
          "report: one log line per report line, prefixed, carriage returns gone");
    const auto tab = Ps4Jit::ReportLines("build:\tfailed   \n");
    Check(tab.size() == 1 && tab[0] == "JIT pack, as the build made it: build: failed", "report: tabs become spaces and trailing blanks go");
    const auto longline = Ps4Jit::ReportLines(std::string(500, 'x') + "\n");
    Check(longline.size() == 1 && longline[0].size() == std::string("JIT pack, as the build made it: ").size() + 203 && longline[0].substr(longline[0].size() - 3) == "...",
          "report: a long line is cut at 200 characters");
    std::string many;
    for (int i = 0; i < 40; ++i) {
        many += "line " + std::to_string(i) + "\n";
    }
    const auto capped = Ps4Jit::ReportLines(many);
    Check(capped.size() == 25 && Has(capped[0], "line 0") && Has(capped[23], "line 23") && Has(capped[24], "16 more line(s)"), "report: 24 lines at most, the rest counted");

    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
