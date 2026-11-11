// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.2: the JIT page reuse pack (patches/eden-jit/). The pack is an optional patch to Eden's Dynarmic; when it
// is in the build its strong definitions of the symbols below replace the weak ones here; when it is not (it did not fit the
// Eden commit, or it did not compile and the CI built without it) the weak ones stay, ps4_page_reuse_present is 0, and the
// menu option does nothing except say so in boot.log.
//
// Pure logic around the symbols (Describe, ParseSwitch) is tested on a PC (tests/jit_pack_test.cpp).

#pragma once

#include <cstdio>
#include <string>

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

} // namespace Ps4Jit
