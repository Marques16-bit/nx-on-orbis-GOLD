#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""0.9.7 fix 2: the table of function names the app reads to put names under the "eboot+0x..." offsets of boot.log (frontend/symbols.h).

    nm --demangle --defined-only --print-size --numeric-sort eboot.elf | python3 tools/make-symbols.py [--base 0x400000] [--max-bytes N] > symbols.txt

Output, one function per line, ascending: "<hex offset> <hex size> <name>" where offset = address - base (the number boot.log prints after "eboot+0x").
The first line is a comment ("# ...") with the entry count and the base. The name has its parameter list cut off (it is long and the address is what identifies the
function) and is cut at 110 characters. Only code symbols (nm types T, t, W, w) are kept. A table that would be larger than --max-bytes (default 14 MiB) loses its smallest
functions first, and the header says so.

Exit status 0 even when nothing usable came in: the table is a diagnostic, the build must not stop for it (the output then holds only the comment saying why).
"""
import argparse
import re
import sys

CODE_TYPES = set("TtWw")
NAME_MAX = 110


def cut_params(name: str) -> str:
    """'ns::f<T>(int, char) const' -> 'ns::f<T>'. Keeps 'operator()' (its own parentheses) and names with no parameter list."""
    probe = name
    # 'operator()' must not be taken for a parameter list
    probe = probe.replace("operator()", "operator@@")
    # drop trailing qualifiers after the parameter list
    probe = re.sub(r"\)\s*(const|volatile|&&|&|noexcept|\s)*(\[clone [^\]]*\])?\s*$", lambda m: ")" + (m.group(2) or ""), probe)
    clone = ""
    m = re.search(r"(\[clone [^\]]*\])\s*$", probe)
    if m:
        clone = " " + m.group(1)
        probe = probe[: m.start()].rstrip()
    if probe.endswith(")"):
        depth = 0
        for i in range(len(probe) - 1, -1, -1):
            c = probe[i]
            if c == ")":
                depth += 1
            elif c == "(":
                depth -= 1
                if depth == 0:
                    probe = probe[:i]
                    break
    return (probe + clone).replace("operator@@", "operator()")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="0x400000")
    ap.add_argument("--max-bytes", type=int, default=14 * 1024 * 1024)
    args = ap.parse_args()
    base = int(args.base, 16)

    rows = []  # (offset, size, name)
    seen_start = set()
    for raw in sys.stdin:
        parts = raw.rstrip("\n").split(" ", 3)
        # nm --print-size: "<addr> <size> <type> <name>"; no size: "<addr> <type> <name>"
        if len(parts) < 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if len(parts[1]) == 1:  # no size column
            size = 0
            typ = parts[1]
            name = " ".join(parts[2:])
        else:
            try:
                size = int(parts[1], 16)
            except ValueError:
                continue
            if len(parts) < 4 or len(parts[2]) != 1:
                continue
            typ = parts[2]
            name = parts[3]
        if typ not in CODE_TYPES or addr < base:
            continue
        name = cut_params(name.strip())
        if not name:
            continue
        off = addr - base
        if off in seen_start:  # an alias at the same address: the first name (nm's order) stays
            continue
        seen_start.add(off)
        rows.append((off, size, name[:NAME_MAX]))

    rows.sort(key=lambda r: r[0])
    lines = ["%x %x %s\n" % r for r in rows]
    total = sum(len(l) for l in lines)
    dropped = 0
    floor = 0
    while total > args.max_bytes and rows:
        floor = floor * 2 + 16
        keep = [r for r in rows if r[1] >= floor]
        dropped = len(rows) - len(keep)
        rows = keep
        lines = ["%x %x %s\n" % r for r in rows]
        total = sum(len(l) for l in lines)
    if not rows:
        sys.stdout.write("# no symbols: the ELF had no symbol table (stripped) or nm printed nothing usable; offsets in boot.log stay numbers\n")
        return 0
    head = "# %d functions, offsets relative to 0x%x" % (len(rows), base)
    if dropped:
        head += ", the table was over %d bytes so functions smaller than %d bytes were left out (%d)" % (args.max_bytes, floor, dropped)
    sys.stdout.write(head + "\n")
    sys.stdout.writelines(lines)
    return 0


if __name__ == "__main__":
    sys.exit(main())
