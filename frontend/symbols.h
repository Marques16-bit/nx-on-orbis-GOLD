// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.7 fix 2: names for the "eboot+0x..." offsets that boot.log is full of (stall stacks, crash backtraces, the CPU profile, the heap census).
//
// Why: the logs of the .nro and SpongeBob stalls say "CPUCore_0 is busy in eboot+0xa3630 <- 0x500e8a <- 0x500cb8 <- 0x4fddb9" and nobody can say what that is without
// the build's ELF. The build now writes the ELF's function table next to the app (assets/misc/symbols.txt, made by tools/make-symbols.py from `nm`), and the app
// itself puts the names under each dump. This file is the pure part (a line parser and a one-pass resolver), so a PC can test it; symbols.cpp reads the file on the
// console and writes to boot.log.
//
// File format, one function per line, sorted by offset ascending (offset = address - 0x400000, the same number the log prints):
//   <hex offset> <hex size> <name>
// A line that starts with '#' is a comment (the first one is shown in boot.log: how many entries, which build).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Ps4Sym {

/// The address the console loads eboot.bin at; the logs print "eboot+0x..." = address - ImageBase (ps4_platform.cpp uses the same number).
constexpr unsigned long ImageBase = 0x400000;
/// How many distinct offsets one dump can ask names for; more are counted and left out.
constexpr int MaxNotes = 512;
constexpr std::size_t NameChars = 100;
/// A symbol that has no size in the table (assembler stubs) is taken to cover this much.
constexpr unsigned long UnsizedReach = 0x100;

struct Entry {
    unsigned long off = 0;   ///< what was asked for (what the resolver looks up)
    unsigned long shown = 0; ///< what the log prints for it (for a profile bucket: where the bucket starts)
    unsigned long start = 0; ///< where the function found begins
    unsigned long size = 0;
    bool found = false;
    bool bucket = false;     ///< a point inside a 64-byte profile bucket, not a code address from a stack
    bool anchor = false;     ///< the app's own check that the table belongs to this build
    char name[NameChars] = {};
};

inline int HexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// "<hex> <hex> <name>" -> the three parts. False for a comment, a blank line or anything that is not that.
inline bool ParseLine(const char* s, std::size_t n, unsigned long& off, unsigned long& size, const char*& name, std::size_t& name_len) {
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        --n;
    }
    std::size_t i = 0;
    unsigned long values[2] = {0, 0};
    for (int part = 0; part < 2; ++part) {
        std::size_t digits = 0;
        while (i < n && HexDigit(s[i]) >= 0) {
            if (digits >= 16) {
                return false;
            }
            values[part] = values[part] * 16 + static_cast<unsigned long>(HexDigit(s[i]));
            ++i;
            ++digits;
        }
        if (digits == 0 || i >= n || s[i] != ' ') {
            return false;
        }
        ++i;
    }
    if (i >= n) {
        return false;
    }
    off = values[0];
    size = values[1];
    name = s + i;
    name_len = n - i;
    return true;
}

/// Sorts by `off` (insertion sort: at most MaxNotes entries) and merges equal ones. Returns the new count.
inline int SortUnique(Entry* e, int n) {
    for (int i = 1; i < n; ++i) {
        const Entry key = e[i];
        int j = i - 1;
        while (j >= 0 && e[j].off > key.off) {
            e[j + 1] = e[j];
            --j;
        }
        e[j + 1] = key;
    }
    int out = 0;
    for (int i = 0; i < n; ++i) {
        if (out > 0 && e[out - 1].off == e[i].off) {
            // The same address asked for twice (a bucket and a frame): keep the one that is not a bucket, and the anchor mark.
            e[out - 1].anchor = e[out - 1].anchor || e[i].anchor;
            if (e[out - 1].bucket && !e[i].bucket) {
                e[out - 1].bucket = false;
                e[out - 1].shown = e[i].shown;
            }
            continue;
        }
        e[out++] = e[i];
    }
    return out;
}

/// Feed the table's lines in file order (ascending); the entries given (sorted, unique) get the function that contains their offset.
/// One pass over the file, nothing allocated: the whole table never has to be in memory.
class Resolver {
public:
    Resolver(Entry* entries, int count) : e_(entries), n_(count) {}

    void Feed(const char* line, std::size_t length) {
        unsigned long off = 0;
        unsigned long size = 0;
        const char* name = nullptr;
        std::size_t name_len = 0;
        if (!ParseLine(line, length, off, size, name, name_len)) {
            return;
        }
        ++lines_;
        while (next_ < n_ && e_[next_].off < off) {
            Settle(e_[next_]);
            ++next_;
        }
        if (have_prev_ && prev_start_ == off) {
            return; // a second name for the same address: the first stays
        }
        have_prev_ = true;
        prev_start_ = off;
        prev_size_ = size;
        const std::size_t keep = name_len < NameChars - 1 ? name_len : NameChars - 1;
        std::memcpy(prev_name_, name, keep);
        prev_name_[keep] = 0;
    }

    /// After the last line: whatever is still waiting lies at or after the last function.
    void Finish() {
        while (next_ < n_) {
            Settle(e_[next_]);
            ++next_;
        }
    }

    /// How many table lines were understood (a table that gave none is not a table).
    unsigned long Lines() const {
        return lines_;
    }

private:
    void Settle(Entry& e) {
        if (!have_prev_ || e.off < prev_start_) {
            return;
        }
        const unsigned long reach = prev_size_ != 0 ? prev_size_ : UnsizedReach;
        if (e.off - prev_start_ >= reach) {
            return; // between two functions (padding, or a function the table left out)
        }
        e.found = true;
        e.start = prev_start_;
        e.size = prev_size_;
        std::memcpy(e.name, prev_name_, NameChars);
    }

    Entry* e_;
    int n_;
    int next_ = 0;
    bool have_prev_ = false;
    unsigned long prev_start_ = 0;
    unsigned long prev_size_ = 0;
    char prev_name_[NameChars] = {};
    unsigned long lines_ = 0;
};

/// Splits what read() returned into lines for a Resolver without allocating: `carry` holds the unfinished last line of the previous chunk.
/// Usage: LineSplitter s{resolver}; for each chunk s.Push(data, n); at the end s.Flush().
template <class Sink>
class LineSplitter {
public:
    explicit LineSplitter(Sink& sink) : sink_(sink) {}

    void Push(const char* data, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const char c = data[i];
            if (c == '\n') {
                EndLine();
            } else if (used_ < sizeof(line_)) {
                line_[used_++] = c;
            } else {
                too_long_ = true; // a line longer than the buffer: the rest is dropped, the start still parses
            }
        }
    }

    void Flush() {
        if (used_ != 0) {
            EndLine();
        }
    }

    /// The first comment line of the table ("# ..."), for the header the app prints.
    const char* FirstComment() const {
        return comment_;
    }

private:
    void EndLine() {
        if (used_ > 0 && line_[0] == '#') {
            if (comment_[0] == 0) {
                const std::size_t keep = used_ < sizeof(comment_) - 1 ? used_ : sizeof(comment_) - 1;
                std::memcpy(comment_, line_, keep);
                comment_[keep] = 0;
            }
        } else {
            sink_.Feed(line_, used_);
        }
        used_ = 0;
        too_long_ = false;
    }

    Sink& sink_;
    char line_[512];
    std::size_t used_ = 0;
    bool too_long_ = false;
    char comment_[200] = {};
};

// The console half (symbols.cpp). A dump calls Note() for every offset it prints, and Legend() once at its end. Both are safe from a fault handler (no allocation,
// a try-lock), and both do nothing useful on a build without the table, which is why the files that a host test links alone declare them weak (see heap_census.cpp).
/// An "eboot+0x..." offset a dump printed.
void Note(unsigned long off);
/// A profile bucket: [bucket_start, bucket_start + bucket_bytes). The functions at its first byte, its middle and its last byte name it (a 64-byte bucket can hold
/// two or three small functions; the legend prints each distinct name).
void NoteBucket(unsigned long bucket_start, unsigned long bucket_bytes);
/// Writes the names of everything noted since the last call (each offset once per boot.log) under a heading that says `why`.
void Legend(const char* why);

} // namespace Ps4Sym
