// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.2: the JIT pack (patches/eden-jit/), part 1 (page reuse); 0.9.3 adds part 2 (the direct table, below). The pack is an optional patch to Eden's Dynarmic; when it
// is in the build its strong definitions of the symbols below replace the weak ones here; when it is not (it did not fit the
// Eden commit, or it did not compile and the CI built without it) the weak ones stay, ps4_page_reuse_present is 0, and the
// menu option does nothing except say so in boot.log.
//
// Pure logic around the symbols (Describe, ParseSwitch) is tested on a PC (tests/jit_pack_test.cpp).

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
// The pack (Dynarmic, ir/opt_passes.cpp) defines these too, with ps4_page_reuse_present = 1, and its definitions win over the
// weak ones here. Without the pack these are the ones in use: nothing reads them, and ps4_page_reuse_present says 0. (Weak
// definitions rather than weak references: an undefined weak symbol of an ELF that is loaded by the console's own dynamic
// linker is a risk that a definition is not.)
__attribute__((weak)) bool ps4_page_reuse = false;
__attribute__((weak)) int ps4_page_reuse_present = 0;
__attribute__((weak)) int ps4_page_reuse_status = 0;
__attribute__((weak)) unsigned ps4_page_reuse_why = 0;
__attribute__((weak)) unsigned long long ps4_page_reuse_groups = 0;
__attribute__((weak)) unsigned long long ps4_page_reuse_accesses = 0;

// 0.9.3, part 2 (patches/eden-jit/0002-*.py): the direct table. The pack's page table code (Eden's common/page_table.h/.cpp) and the JIT read
// and write these; this file defines them (weak: only the frontend's own definitions exist, none of them is in the pack), except
// ps4_direct_present and ps4_direct_emitted, which the pack defines too (strong) when it is in the build.
__attribute__((weak)) bool ps4_direct_table = false;                    // asked for in settings.txt; set before the first page table is sized
__attribute__((weak)) bool ps4_direct_live = false;                     // a page table has a direct copy
__attribute__((weak)) std::uintptr_t ps4_direct_entries_base = 0;
__attribute__((weak)) std::uintptr_t ps4_direct_entries_bytes = 0;
__attribute__((weak)) std::uintptr_t ps4_direct_mirror_base = 0;
__attribute__((weak)) int ps4_direct_present = 0;
__attribute__((weak)) unsigned long long ps4_direct_emitted = 0;
}

namespace Ps4Jit {

/// ps4_page_reuse_status: 0 = no block compiled yet, 1 = active, 2 = switched off, 3 = this page table setup is not handled.
struct State {
    bool in_build = false;
    bool wanted = false;
    int status = 0;
    unsigned why = 0;
    unsigned long long groups = 0;
    unsigned long long accesses = 0;
};

/// One line for boot.log. Wording matters: "not in this build" is how the person finds out the pack was left out.
inline std::string Describe(const State& s) {
    char line[256];
    if (!s.in_build) {
        std::snprintf(line, sizeof line, "JIT page reuse: %s, but this build does not have the pack (it did not fit this Eden or did not compile)",
                      s.wanted ? "asked for" : "off");
        return line;
    }
    if (!s.wanted) {
        return "JIT page reuse: off (the pack is in this build; settings.txt jit_page_reuse=on switches it on)";
    }
    switch (s.status) {
    case 0:
        return "JIT page reuse: on, no code compiled yet";
    case 1:
        std::snprintf(line, sizeof line, "JIT page reuse: on; %llu groups of nearby accesses share a page lookup (%llu accesses)", s.groups,
                      s.accesses);
        return line;
    case 2:
        return "JIT page reuse: on in settings but the JIT reports it as off";
    default:
        std::snprintf(line, sizeof line, "JIT page reuse: on, but this page table setup is not handled (reason bits 0x%x): no effect", s.why);
        return line;
    }
}

inline State Read(bool wanted) {
    State s;
    s.wanted = wanted;
    s.in_build = ps4_page_reuse_present != 0;
    if (s.in_build) {
        s.status = ps4_page_reuse_status;
        s.why = ps4_page_reuse_why;
        s.groups = ps4_page_reuse_groups;
        s.accesses = ps4_page_reuse_accesses;
    }
    return s;
}

/// Sets the switch the JIT reads when it compiles a block. False when the pack is not in the build.
inline bool Apply(bool on) {
    ps4_page_reuse = on;
    return ps4_page_reuse_present != 0;
}

/// settings.txt: jit_page_reuse=on|off.
inline bool ParseSwitch(const std::string& value, bool* out) {
    if (value == "on") {
        *out = true;
        return true;
    }
    if (value == "off") {
        *out = false;
        return true;
    }
    return false;
}

/// 0.9.3, part 2: the direct table (a decoded copy of the page table the JIT reads with fewer instructions).
struct DirectState {
    bool in_build = false;
    bool wanted = false;
    bool live = false;
    unsigned long long emitted = 0;
};

inline std::string DescribeDirect(const DirectState& s) {
    char line[256];
    if (!s.in_build) {
        std::snprintf(line, sizeof line, "JIT direct table: %s, but this build does not have it (it did not fit this Eden or did not compile)",
                      s.wanted ? "asked for" : "off");
        return line;
    }
    if (!s.wanted) {
        return "JIT direct table: off (this build has it; settings.txt jit_direct_table=on switches it on)";
    }
    if (!s.live) {
        return "JIT direct table: on, but no page table got a direct copy: the JIT reads the entries as before";
    }
    if (s.emitted == 0) {
        return "JIT direct table: on; the page table has its direct copy; no memory access compiled yet";
    }
    std::snprintf(line, sizeof line, "JIT direct table: on; %llu memory lookups read the direct copy", s.emitted);
    return line;
}

inline DirectState ReadDirect(bool wanted) {
    DirectState s;
    s.wanted = wanted;
    s.in_build = ps4_direct_present != 0;
    if (s.in_build) {
        s.live = ps4_direct_live;
        s.emitted = ps4_direct_emitted;
    }
    return s;
}

/// Sets the switch Eden's page table code reads when it sizes the first page table. False when this build does not have part 2.
inline bool ApplyDirect(bool on) {
    ps4_direct_table = on;
    return ps4_direct_present != 0;
}

/// 0.9.4: the build's own report on the pack (jit-pack-report.txt, written by scripts/apply-jit-pack.sh and ci/stage-eden.sh, packaged under
/// /app0/assets/misc/). Returns the lines for boot.log: each cut at 200 characters, at most 24 of them (the rest is counted), carriage returns
/// and tabs made harmless. An empty text gives no lines.
inline std::vector<std::string> ReportLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0, dropped = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        for (char& c : line) {
            if (c == '\r' || c == '\t') {
                c = ' ';
            }
        }
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (out.size() >= 24) {
            ++dropped;
            continue;
        }
        if (line.size() > 200) {
            line.resize(200);
            line += "...";
        }
        out.push_back("JIT pack, as the build made it: " + line);
    }
    if (dropped > 0) {
        out.push_back("JIT pack, as the build made it: (" + std::to_string(dropped) + " more line(s) in the jit-pack-errors artifact of the build)");
    }
    return out;
}

} // namespace Ps4Jit
