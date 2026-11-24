// SPDX-License-Identifier: GPL-3.0-or-later
// The checks of frontend/symbols.h (the line parser, the one-pass resolver, the line splitter). A header so that two programs run them: tests/symbols_test.cpp, and
// tests/safe_mode_test.cpp, which the CI workflow already builds (a new test file would not be run there: the workflow cannot be edited from the update zip).
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "symbols.h"

namespace SymbolsChecks {

using CheckFn = void (*)(bool, const char*);

inline Ps4Sym::Entry Want(unsigned long off, unsigned long shown = 0, bool anchor = false) {
    Ps4Sym::Entry e;
    e.off = off;
    e.shown = shown ? shown : off;
    e.anchor = anchor;
    e.bucket = shown != 0 && shown != off;
    return e;
}

/// Runs the table `text` against `wanted` (any order); returns the entries sorted and resolved, the chunk size says how the text is cut when it is fed.
inline std::vector<Ps4Sym::Entry> Resolve(const std::string& text, std::vector<Ps4Sym::Entry> wanted, std::size_t chunk, unsigned long* lines = nullptr) {
    const int n = Ps4Sym::SortUnique(wanted.data(), static_cast<int>(wanted.size()));
    wanted.resize(static_cast<std::size_t>(n));
    Ps4Sym::Resolver resolver{wanted.data(), n};
    Ps4Sym::LineSplitter<Ps4Sym::Resolver> split{resolver};
    for (std::size_t i = 0; i < text.size(); i += chunk) {
        split.Push(text.data() + i, std::min(chunk, text.size() - i));
    }
    split.Flush();
    resolver.Finish();
    if (lines != nullptr) {
        *lines = resolver.Lines();
    }
    return wanted;
}

inline const Ps4Sym::Entry* Find(const std::vector<Ps4Sym::Entry>& v, unsigned long off) {
    for (const auto& e : v) {
        if (e.off == off) {
            return &e;
        }
    }
    return nullptr;
}

inline void Run(CheckFn check) {
    using namespace Ps4Sym;
    unsigned long off = 0;
    unsigned long size = 0;
    const char* name = nullptr;
    std::size_t len = 0;

    // --- ParseLine
    const std::string good = "4fddb9 39 Core::ArmDynarmicCallbacks64::CallSVC";
    check(ParseLine(good.data(), good.size(), off, size, name, len) && off == 0x4fddb9 && size == 0x39 &&
              std::string(name, len) == "Core::ArmDynarmicCallbacks64::CallSVC",
          "symbols: a table line gives offset, size and name");
    const std::string crlf = "A0 10 f\r\n";
    check(ParseLine(crlf.data(), crlf.size(), off, size, name, len) && off == 0xA0 && std::string(name, len) == "f", "symbols: capital hex digits and a CRLF line end");
    const char* bad[] = {"", "# a comment", "zz 10 f", "10 f", "10 10", "10 10 ", "10  f", "12345678901234567 1 f", " 10 10 f"};
    bool all_bad = true;
    for (const char* b : bad) {
        all_bad = all_bad && !ParseLine(b, std::strlen(b), off, size, name, len);
    }
    check(all_bad, "symbols: a comment, a blank line, a missing field or a 17-digit number is not a table line");
    const std::string spaced = "10 20 operator new(unsigned long) [clone .cold]";
    check(ParseLine(spaced.data(), spaced.size(), off, size, name, len) && std::string(name, len) == "operator new(unsigned long) [clone .cold]",
          "symbols: the name keeps its own spaces");

    // --- Resolver: what lies where
    const std::string table =
        "# 5 functions, offsets relative to 0x400000\n"
        "1000 40 A::first\n"
        "1040 20 B::second\n"
        "1100 100 C::third\n"
        "1100 100 C::alias_of_third\n"
        "2000 0 stub_without_size\n"
        "3000 10 D::last\n";
    unsigned long lines = 0;
    auto r = Resolve(table, {Want(0x1000), Want(0x103f), Want(0x1040), Want(0x105f), Want(0x1060), Want(0x10ff), Want(0x1100), Want(0x11ff), Want(0x1200), Want(0x2000),
                             Want(0x20ff), Want(0x2100), Want(0x3000), Want(0x300f), Want(0x3010), Want(0xfff), Want(0x9999999)},
                     4096, &lines);
    check(lines == 6, "symbols: the comment is not counted, the alias line is (6 table lines)");
    auto is = [&](unsigned long o, const char* n) {
        const Entry* e = Find(r, o);
        return e != nullptr && e->found && std::strcmp(e->name, n) == 0;
    };
    auto isnt = [&](unsigned long o) {
        const Entry* e = Find(r, o);
        return e != nullptr && !e->found;
    };
    check(is(0x1000, "A::first") && is(0x103f, "A::first"), "symbols: the first and the last byte of a function belong to it");
    check(is(0x1040, "B::second") && is(0x105f, "B::second"), "symbols: the next function starts where it starts");
    check(isnt(0x1060) && isnt(0x10ff), "symbols: the gap after a function (padding) is not named after the function before it");
    check(is(0x1100, "C::third") && is(0x11ff, "C::third"), "symbols: two names for one address: the first stays");
    check(isnt(0x1200), "symbols: just past the end of a function with a size: nothing");
    check(is(0x2000, "stub_without_size") && is(0x20ff, "stub_without_size") && isnt(0x2100), "symbols: a function with no size covers 0x100 bytes");
    check(is(0x3000, "D::last") && is(0x300f, "D::last") && isnt(0x3010), "symbols: the last function of the table, and the byte after it");
    check(isnt(0xfff), "symbols: before the first function: nothing");
    check(isnt(0x9999999), "symbols: far past the last function: nothing");

    // --- the same answer however the file is cut into reads
    bool same = true;
    for (std::size_t chunk : {1u, 2u, 3u, 7u, 16u, 1000u}) {
        auto q = Resolve(table, {Want(0x1000), Want(0x1050), Want(0x1180), Want(0x2010), Want(0x3005)}, chunk);
        same = same && q.size() == 5 && q[0].found && std::strcmp(q[0].name, "A::first") == 0 && q[1].found && std::strcmp(q[1].name, "B::second") == 0 && q[2].found &&
               std::strcmp(q[2].name, "C::third") == 0 && q[3].found && std::strcmp(q[3].name, "stub_without_size") == 0 && q[4].found &&
               std::strcmp(q[4].name, "D::last") == 0;
    }
    check(same, "symbols: reading the table 1, 2, 3, 7, 16 or 1000 bytes at a time gives the same names");
    {
        std::vector<Entry> w{Want(0x1000)};
        Resolver rr{w.data(), 1};
        LineSplitter<Resolver> sp{rr};
        const std::string t = "# first comment\n# second comment\n1000 40 X\n";
        sp.Push(t.data(), t.size());
        sp.Flush();
        rr.Finish();
        check(std::strcmp(sp.FirstComment(), "# first comment") == 0 && w[0].found, "symbols: the first comment line is kept for the header, the second is not");
    }
    {
        std::vector<Entry> w{Want(0x1000)};
        Resolver rr{w.data(), 1};
        LineSplitter<Resolver> sp{rr};
        const std::string t = "1000 40 X"; // no final newline
        sp.Push(t.data(), t.size());
        sp.Flush();
        rr.Finish();
        check(w[0].found && std::strcmp(w[0].name, "X") == 0, "symbols: a last line with no line end still counts");
    }

    // --- names that are too long are cut, never overrun
    {
        std::string long_name(300, 'n');
        const std::string t = "1000 40 " + long_name + "\n";
        auto q = Resolve(t, {Want(0x1010)}, 64);
        check(q.size() == 1 && q[0].found && std::strlen(q[0].name) == NameChars - 1, "symbols: a name longer than the buffer is cut to fit, with its end mark");
        std::string long_line = "1000 40 " + std::string(2000, 'x') + "\n2000 10 after_the_long_line\n";
        auto z = Resolve(long_line, {Want(0x1000), Want(0x2000)}, 100);
        check(z.size() == 2 && z[0].found && z[1].found && std::strcmp(z[1].name, "after_the_long_line") == 0,
              "symbols: a line longer than the read buffer does not swallow the next one");
    }

    // --- SortUnique: any order in, ascending and distinct out; the bucket and the anchor
    {
        std::vector<Entry> w{Want(30), Want(10), Want(20), Want(10), Want(30), Want(5)};
        const int n = SortUnique(w.data(), static_cast<int>(w.size()));
        check(n == 4 && w[0].off == 5 && w[1].off == 10 && w[2].off == 20 && w[3].off == 30, "symbols: sorted and distinct");
        std::vector<Entry> b{Want(0x1020, 0x1000), Want(0x1020)};
        const int m = SortUnique(b.data(), 2);
        check(m == 1 && b[0].shown == 0x1020, "symbols: a bucket's middle asked for as an address too: printed as the address");
        std::vector<Entry> a{Want(7, 0, true), Want(7)};
        const int k = SortUnique(a.data(), 2);
        check(k == 1 && a[0].anchor, "symbols: the anchor's mark survives a merge");
    }
    // --- a profile bucket is named after the function at its middle
    {
        auto q = Resolve("1000 100 Big::function\n", {Want(0x1000 + 32, 0x1000)}, 64);
        check(q.size() == 1 && q[0].found && q[0].shown == 0x1000 && std::strcmp(q[0].name, "Big::function") == 0, "symbols: a 64-byte bucket gets the function at its middle");
    }
    // --- ParseAnchorHeader: where the table says the build's Ps4SymAnchor is (0.9.7 fix 3)
    {
        unsigned long a = 0;
        const std::string h1 = "# 123 functions, offsets are ELF addresses minus 0x0, anchor=0x60ab0\n1000 40 f\n";
        check(ParseAnchorHeader(h1.data(), h1.size(), a) && a == 0x60ab0, "symbols: the anchor is read from the first line");
        const std::string h2 = "# 9 functions, offsets are ELF addresses minus 0x0, anchor=0x60AB0, the table was over 10 bytes";
        check(ParseAnchorHeader(h2.data(), h2.size(), a) && a == 0x60ab0, "symbols: capital digits, and text after the number, and no line end");
        const std::string h3 = "# 9 functions, offsets are ELF addresses minus 0x400000\n1000 40 anchor=0x77 is a function name\n";
        a = 1;
        check(!ParseAnchorHeader(h3.data(), h3.size(), a) && a == 1, "symbols: an anchor on the second line is not looked at");
        const std::string h4 = "1000 40 f\n# anchor=0x77\n";
        check(!ParseAnchorHeader(h4.data(), h4.size(), a), "symbols: a first line that is not a comment gives no anchor");
        const std::string h5 = "# 9 functions, anchor=0x\n";
        const std::string h6 = "# 9 functions, anchor=0x11111111111111111\n";
        const std::string h7 = "# anchor=0x\n";
        check(!ParseAnchorHeader(h5.data(), h5.size(), a) && !ParseAnchorHeader(h6.data(), h6.size(), a) && !ParseAnchorHeader(h7.data(), h7.size(), a) &&
                  !ParseAnchorHeader("", 0, a) && !ParseAnchorHeader("#", 1, a),
              "symbols: no digits, 17 digits, an empty chunk: no anchor");
        const std::string h8 = "# an old table, offsets relative to 0x400000\n";
        check(!ParseAnchorHeader(h8.data(), h8.size(), a), "symbols: a table without an anchor (made by 0.9.7 fix 2) gives none");
    }
    // --- Shift: the console prints the address minus 0x400000, the table may be in another address space; the difference moves every lookup
    {
        // A table whose addresses are the ELF's own (base 0), and a log whose offsets are 0x400000 higher than that (what a --base 0x400000 mistake looks like from the app).
        const std::string t = "# 3 functions, anchor=0x60ab0\n60ab0 20 Ps4SymAnchor\n70000 100 Other::f\n80000 10 Ps4SymAnchor2\n";
        std::vector<Entry> w{Want(0x60ab0 + 0x400000, 0, true), Want(0x70040 + 0x400000), Want(0x80000 + 0x400000, 0, true), Want(0x70040)};
        int n = SortUnique(w.data(), static_cast<int>(w.size()));
        n = Shift(w.data(), n, static_cast<unsigned long>(0) - 0x400000ul);
        check(n == 4, "symbols: a shift keeps every entry");
        Resolver rr{w.data(), n};
        LineSplitter<Resolver> sp{rr};
        sp.Push(t.data(), t.size());
        sp.Flush();
        rr.Finish();
        // after the shift the offsets are table addresses; what is printed (shown) is still what the log had
        const Entry* a1 = Find(w, 0x60ab0);
        const Entry* f1 = Find(w, 0x70040);
        const Entry* a2 = Find(w, 0x80000);
        check(a1 != nullptr && a1->found && a1->anchor && std::strcmp(a1->name, "Ps4SymAnchor") == 0 && a1->shown == 0x60ab0 + 0x400000,
              "symbols: after a negative shift the anchor is found by name and still printed as the log had it");
        check(f1 != nullptr && f1->found && std::strcmp(f1->name, "Other::f") == 0, "symbols: a function is found at its shifted offset");
        check(a2 != nullptr && a2->found && a2->anchor && std::strcmp(a2->name, "Ps4SymAnchor2") == 0, "symbols: the second anchor too");
        // Shift by 0 is a no-op; a positive one works the other way.
        std::vector<Entry> z{Want(5), Want(9)};
        check(Shift(z.data(), 2, 0) == 2 && z[0].off == 5 && z[1].off == 9, "symbols: a shift of 0 changes nothing");
        std::vector<Entry> y{Want(0x10), Want(0x20)};
        check(Shift(y.data(), 2, 0x400000) == 2 && y[0].off == 0x400010 && y[0].shown == 0x10 && y[1].off == 0x400020, "symbols: a positive shift moves the key and not the printed number");
        // A table of ANOTHER build: the anchors are not where they should be (the distance between them is different), so they do not both come back by name.
        const std::string other = "# 3 functions, anchor=0x60ab0\n60ab0 20 Ps4SymAnchor\n60ad0 100 Unrelated::g\n80000 10 Elsewhere\n";
        std::vector<Entry> w2{Want(0x60ab0, 0, true), Want(0x80000, 0, true)};
        auto q = Resolve(other, w2, 4096);
        const Entry* b2 = Find(q, 0x80000);
        check(b2 != nullptr && b2->found && std::strncmp(b2->name, "Ps4SymAnchor", 12) != 0,
              "symbols: with the second anchor naming something else, the check (name starts with Ps4SymAnchor) fails");
    }
    // --- ScanForCode: candidates for return addresses on a stack whose frame-pointer chain is broken
    {
        const unsigned long stack[] = {0, 0x7ffc02bac, 0x4a3000, 5, 0x400000, 0x10400000, 0x1a8c779 + 0x400000, 0x3fffff, 0x500e8a, 0xdeadbeef, 0x700000};
        unsigned long out[4] = {};
        const int n = ScanForCode(stack, sizeof(stack) / sizeof(stack[0]), 0x400000, 0x10400000, out, 4);
        check(n == 4 && out[0] == 0x4a3000 && out[1] == 0x400000 && out[2] == 0x1a8c779 + 0x400000 && out[3] == 0x500e8a,
              "symbols: the stack scan keeps what lies in the image, in the order found, and stops at the limit (the end of the range is out)");
        unsigned long all[16] = {};
        check(ScanForCode(stack, sizeof(stack) / sizeof(stack[0]), 0x400000, 0x10400000, all, 16) == 5 && ScanForCode(stack, 0, 0, ~0ul, all, 16) == 0 &&
                  ScanForCode(stack, 3, 0x400000, 0x10400000, all, 0) == 0,
              "symbols: the stack scan finds the five in the whole stack, nothing in an empty one, nothing when no room is given");
    }
    // --- a big table with the maximum number of wanted offsets
    {
        std::string big = "# big\n";
        char buf[64];
        for (unsigned long i = 0; i < 120000; ++i) {
            std::snprintf(buf, sizeof(buf), "%lx 40 fn_%lu\n", i * 0x40, i);
            big += buf;
        }
        std::vector<Entry> w;
        for (int i = 0; i < MaxNotes; ++i) {
            w.push_back(Want(static_cast<unsigned long>(i) * 0x40 * 200 + 5));
        }
        auto q = Resolve(big, w, 16384);
        bool ok = q.size() == static_cast<std::size_t>(MaxNotes);
        for (std::size_t i = 0; ok && i < q.size(); ++i) {
            char expect[32];
            std::snprintf(expect, sizeof(expect), "fn_%zu", i * 200);
            ok = q[i].found && std::strcmp(q[i].name, expect) == 0;
        }
        check(ok, "symbols: 512 offsets against a 120000-function table in one pass, every one right");
    }
}

} // namespace SymbolsChecks
