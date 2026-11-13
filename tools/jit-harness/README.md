# JIT harness (0.9.4)

Runs random AArch64 programs (loads and stores of every size, pairs, vectors, exclusives, accesses that hug the end of a page, with pages that are
fast, rasterizer-cached, debug or unmapped, re-drawn between "epochs" while the compiled blocks stay) through **Dynarmic's x64 backend as the build
pins it (Eden `5f142c79` + patches/eden/ + the JIT pack)**, on an ordinary x86-64 Linux PC, in four configurations (`base`, `reuse`, `direct`, `both`:
the pack's two parts off and on), and compares everything observable: registers, vector registers, guest memory and the exact sequence of memory
callbacks. All four must give the same lines. This is the check the JIT pack's emitters had never had before 0.9.4.

Result of 0.9.4 (3 generator seeds, 200 and 300 programs, 3800 runs per configuration): all four identical; with the direct table's slots zeroed
by hand (`PROBE_ZERO=1`) from the second epoch on, the `direct` runs differ at once, which shows the JIT really reads the direct table.

It is not part of the CI: it needs clang, Xbyak, fmt, Boost and about 100 Dynarmic objects, which the console build does not.

## How it was built (on a plain Linux x86-64 box with clang 18 or newer)

```
git clone https://github.com/eden-emulator/mirror eden && cd eden && git checkout 5f142c7926d0c7fcbbd0ce30794d72f638a43b2a
# apply patches/eden/*.patch with git am --keep-cr, then the JIT pack: scripts/apply-jit-pack.sh <this checkout>
# dependencies (headers only, except Boost's own headers; all come from GitHub): herumi/xbyak, fmtlib/fmt, boostorg/boost (--recurse-submodules --shallow-submodules)
# Dynarmic's own mcl is in src/dynarmic/src/mcl
clang++ -std=c++23 -fbracket-depth=1024 -DXBYAK_STRICT_CHECK_MEM_REG_SIZE=0 -DMCL_IGNORE_ASSERTS=1 -DFMT_USE_USER_DEFINED_LITERALS=1 \
        -DDYNARMIC_ENABLE_CPU_FEATURE_DETECTION=1 -DARCHITECTURE_x86_64=1 -DMCL_ARCHITECTURE_X86_64=1 -DFMT_HEADER_ONLY=1 \
        -I src/dynarmic/src -I src/dynarmic/src/mcl/include -I src -isystem <xbyak> -isystem <fmt>/include -isystem <every boost/libs/*/include> \
        -c <each file of dynarmic-sources.txt, relative to src/dynarmic/src>
# a64_translate.cpp includes the files of frontend/A64/translate/impl/*.cpp itself: do not build or link those separately
clang++ -c harness.cpp -D__ORBIS__ (same flags, plus -I tests/jit_pack/direct_shim FIRST so the PC stand-in of the lazy vector wins); stubs.cpp
clang++ -c src/common/page_table.cpp (same flags)
clang++ -o harness harness.o page_table.o stubs.o <all the objects> -pthread

python3 gen.py 1 200 > tests.s
clang --target=aarch64-linux-gnu -c -x assembler tests.s -o t.o && llvm-objcopy -O binary -j .text t.o tests.bin
for m in base reuse direct both; do ./harness $m tests.bin 200 4 7 | sed '1d;$d' | sed '$d' > out_$m.txt; done   # the first and the last two lines name the mode and count
cmp out_base.txt out_reuse.txt && cmp out_base.txt out_direct.txt && cmp out_base.txt out_both.txt
```

`stubs.cpp` is what Dynarmic needs from the rest of Eden to link (assert hooks, log sink, disassembler). The floating-point and A32 files that Eden's CMake
builds are in `dynarmic-sources.txt` too.
