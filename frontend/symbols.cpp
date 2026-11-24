// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.7 fix 2: the console half of symbols.h. A dump (stall stacks, crash, profile, heap census) calls Note() for every offset it prints and Legend() at its end;
// Legend() reads /app0/assets/misc/symbols.txt once, front to back, and writes one line per distinct offset: the function it lies in.
//
// Written to be callable from a fault handler: no allocation (static tables, a static read buffer, open/read/close and not stdio), and a try-lock, so a thread that
// is interrupted while it holds the lock makes the handler skip its legend rather than wait for itself. Log() is what the crash handler already uses.

#include <atomic>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "ps4_platform.h"
#include "symbols.h"

// The app's check that the table belongs to this build. Two functions that are only there to be found (Ps4SymAnchor here, Ps4SymAnchor2 in main.cpp, another object file):
// the table's first line says where Ps4SymAnchor is in the table's address space, the app moves every lookup by the difference from where it really is, and then BOTH
// have to come back by name from the table; the distance between two functions of different object files is the build's own, so a table of another build fails there.
// (0.9.7 fix 2 had the single check and the wrong base: it refused the table, which is what it is for. If it had printed names, they would have been wrong.)
// Not inlined, not merged, never optimised away.
extern "C" __attribute__((noinline, used)) int Ps4SymAnchor(int x) {
    static volatile int sink = 0;
    sink = sink + x;
    return sink;
}
extern "C" int Ps4SymAnchor2(int x); // main.cpp

namespace {

constexpr const char* TablePath = "/app0/assets/misc/symbols.txt";
constexpr int MaxDone = 1024;
constexpr int MaxPrinted = 200;

std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
Ps4Sym::Entry g_notes[Ps4Sym::MaxNotes + 2]; // +2: the two anchors always have room
int g_count = 0;
int g_lost = 0;
unsigned long g_done[MaxDone]; // offsets already described in this boot.log: a later dump does not repeat them
int g_done_count = 0;
bool g_no_table_said = false;
int g_refused_said = 0;
char g_chunk[16384];

struct TryLock {
    TryLock() : held(!g_lock.test_and_set(std::memory_order_acquire)) {}
    ~TryLock() {
        if (held) {
            g_lock.clear(std::memory_order_release);
        }
    }
    bool held;
};

bool AlreadyKnown(unsigned long off, unsigned long shown) {
    for (int i = 0; i < g_count; ++i) {
        if (g_notes[i].off == off && g_notes[i].shown == shown) {
            return true;
        }
    }
    for (int i = 0; i < g_done_count; ++i) {
        if (g_done[i] == shown) {
            return true;
        }
    }
    return false;
}

void AddLocked(unsigned long off, unsigned long shown, bool bucket) {
    if (AlreadyKnown(off, shown)) {
        return;
    }
    if (g_count >= Ps4Sym::MaxNotes) {
        ++g_lost;
        return;
    }
    Ps4Sym::Entry& e = g_notes[g_count++];
    e = Ps4Sym::Entry{};
    e.off = off;
    e.shown = shown;
    e.bucket = bucket;
}

} // namespace

namespace Ps4Sym {

void Note(unsigned long off) {
    TryLock lock;
    if (lock.held) {
        AddLocked(off, off, false);
    }
}

void NoteBucket(unsigned long bucket_start, unsigned long bucket_bytes) {
    TryLock lock;
    if (lock.held && bucket_bytes != 0) {
        // Three points: a 64-byte bucket can hold the end of one function and the start of the next. Same `shown` for all three: one bucket in the log.
        AddLocked(bucket_start, bucket_start, true);
        AddLocked(bucket_start + bucket_bytes / 2, bucket_start, true);
        AddLocked(bucket_start + bucket_bytes - 1, bucket_start, true);
    }
}

void Legend(const char* why) {
    TryLock lock;
    if (!lock.held || g_count == 0) {
        return;
    }
    const unsigned long anchor_off = reinterpret_cast<unsigned long>(&Ps4SymAnchor) - ImageBase;
    const unsigned long anchor2_off = reinterpret_cast<unsigned long>(&Ps4SymAnchor2) - ImageBase;
    for (int k = 0; k < 2; ++k) {
        Entry& anchor = g_notes[g_count + k];
        anchor = Entry{};
        anchor.off = k == 0 ? anchor_off : anchor2_off;
        anchor.shown = anchor.off;
        anchor.anchor = true;
    }
    int total = SortUnique(g_notes, g_count + 2);
    const int lost = g_lost;
    g_count = 0; // whatever happens below, this round of notes is spent
    g_lost = 0;

    const int fd = open(TablePath, O_RDONLY);
    if (fd < 0) {
        if (!g_no_table_said) {
            g_no_table_said = true;
            Ps4::Log("symbols: %s is not in this build, so the offsets stay numbers (a build made before 0.9.7 fix 2, or the build step that makes the table did not run)",
                     TablePath);
        }
        return;
    }
    // The first read holds the header line: where the table puts Ps4SymAnchor. The difference to where it really is moves every lookup.
    const ssize_t first = read(fd, g_chunk, sizeof(g_chunk));
    unsigned long table_anchor = 0;
    const bool has_anchor = first > 0 && ParseAnchorHeader(g_chunk, static_cast<std::size_t>(first), table_anchor);
    const unsigned long shift = has_anchor ? table_anchor - anchor_off : 0;
    if (shift != 0) {
        total = Shift(g_notes, total, shift);
    }
    Resolver resolver{g_notes, total};
    LineSplitter<Resolver> lines{resolver};
    if (first > 0) {
        lines.Push(g_chunk, static_cast<std::size_t>(first));
        for (;;) {
            const ssize_t got = read(fd, g_chunk, sizeof(g_chunk));
            if (got <= 0) {
                break;
            }
            lines.Push(g_chunk, static_cast<std::size_t>(got));
        }
    }
    lines.Flush();
    resolver.Finish();
    close(fd);

    bool anchors_ok = true;
    const char* anchor_got[2] = {"(nothing)", "(nothing)"};
    int seen = 0;
    for (int i = 0; i < total; ++i) {
        if (g_notes[i].anchor) {
            const char* got = g_notes[i].found ? g_notes[i].name : "(nothing)";
            anchor_got[g_notes[i].shown == anchor_off ? 0 : 1] = got; // in the order the message names them, not in address order
            ++seen;
            anchors_ok = anchors_ok && g_notes[i].found && std::strncmp(got, "Ps4SymAnchor", 12) == 0;
        }
    }
    anchors_ok = anchors_ok && seen == 2;
    if (resolver.Lines() == 0) {
        Ps4::Log("symbols: %s has no usable line (%s)", TablePath, lines.FirstComment()[0] ? lines.FirstComment() : "no header");
        return;
    }
    if (!anchors_ok) {
        // Said at most 3 times in a boot.log: the same words under every dump are noise. The numbers are what the next fix needs.
        if (g_refused_said < 3) {
            ++g_refused_said;
            Ps4::Log("symbols: the table does NOT belong to this build; names left out. This build's Ps4SymAnchor is at eboot+0x%lx and Ps4SymAnchor2 at eboot+0x%lx; the "
                     "table's first line %s anchor=0x%lx, so every lookup was moved by 0x%lx; the table has '%s' and '%s' at those two places (%s)",
                     anchor_off, anchor2_off, has_anchor ? "says" : "does not say (read as 0)", table_anchor, shift, anchor_got[0], anchor_got[1], lines.FirstComment());
        }
        return;
    }
    const char* header = lines.FirstComment();
    header = header[0] == '#' ? (header[1] == ' ' ? header + 2 : header + 1) : "table";
    Ps4::Log("symbols for %s (%s):", why, header);
    int printed = 0;
    unsigned long last_bucket = ~0ul; // the bucket the last bucket line was about, and the name it gave: the same function twice is said once
    char last_name[NameChars] = {};
    for (int i = 0; i < total; ++i) {
        const Entry& e = g_notes[i];
        if (e.anchor) {
            continue;
        }
        if (e.bucket) {
            if (e.shown == last_bucket && (!e.found || std::strcmp(e.name, last_name) == 0)) {
                continue;
            }
            last_bucket = e.shown;
            if (e.found) {
                std::memcpy(last_name, e.name, NameChars);
            } else {
                last_name[0] = 0;
            }
        }
        if (printed >= MaxPrinted) {
            Ps4::Log("   ... and %d more offsets (the list is cut at %d lines)", total - i, MaxPrinted);
            break;
        }
        ++printed;
        bool said = false;
        for (int d = 0; d < g_done_count && !said; ++d) {
            said = g_done[d] == e.shown;
        }
        if (!said && g_done_count < MaxDone) {
            g_done[g_done_count++] = e.shown;
        }
        if (!e.found) {
            Ps4::Log(e.bucket ? "   0x%lx..+0x3f  (not in the table: padding, or code the table does not list)" : "   0x%lx  (not in the table: padding, or code the table does not list)",
                     e.shown);
        } else if (e.bucket) {
            Ps4::Log("   0x%lx..+0x3f  %s", e.shown, e.name); // a profile bucket: 64 bytes, named after the functions at its start, middle and end
        } else {
            Ps4::Log("   0x%lx  %s+0x%lx", e.shown, e.name, e.off - e.start);
        }
    }
    if (lost != 0) {
        Ps4::Log("   (%d more offsets were left out: a dump names at most %d at a time)", lost, MaxNotes);
    }
}

} // namespace Ps4Sym

extern "C" void Ps4SymNote(unsigned long off) {
    Ps4Sym::Note(off);
}

extern "C" void Ps4SymLegend(const char* why) {
    Ps4Sym::Legend(why);
}
