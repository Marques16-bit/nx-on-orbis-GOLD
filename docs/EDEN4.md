# Eden4: what it is, what this project took from it, and what it did not

**Eden4** (release `Eden4-1.0.0-experimental`) is another unofficial PS4 port of the Eden Switch emulator, GPL-3.0-or-later,
also on Mesa RADV for the PS4 GPU. Its package does not name its author, so it is credited by project name (see the
README's Credits and `NOTICE.md`). It builds on **DolphinPS4** by **ShiroKlein** and contributors (the PS4 runtime and the GNM
winsys) and lists its own references in its `CREDITS.md`.

This page records an analysis of its sources (README, `CREDITS.md`, `docs/`, `runtime/port/ps4_runtime.cpp`, `eden/ps4`,
`eden/src/yuzu_ps4`, `tools/jit-harness`) against this repository, and the decision for each part. Nothing here was run on a
console: it is a reading of text, and the one thing that was taken was tested on a PC only.

## Taken (0.9.0)

| What | Where it is here | Notes |
|---|---|---|
| **Memory outside direct memory.** Eden4 keeps its C heap in the console's *system flexible memory* (512 MiB, then 32 MiB overflow steps from system flexible, flexible and finally direct memory), so what the heap holds does not count against the ~4.5 GiB of direct memory that big games run out of. | Option `sysflex_mib` (`off` by default; `128`, `256`, `384`, `512`), patch `0029`, `frontend/ps4_platform.cpp`, `frontend/memory_guard.{h,cpp}` | The **code is new**: here the pool feeds the 64 KiB lazy pieces of the guest's RAM and Eden's big tables (`orbis_lazy_memory`), not the C heap, because the C heap belongs to `orbis-compat`, which is not in this repository. The 32 MiB step follows Eden4's. The 128 MiB that stays free in the pool for the system is this project's own choice (Eden4 keeps 64 MiB of *regular* flexible memory and a reserve of direct memory instead). Eden4 also commits its lazily backed GPU page table from this pool first (`ps4_commit_lazy`): the same idea as patch 0029. |
| **Where to map the probe block.** Eden4 found that the kernel library's own objects land at `0x200000000`, so blocks have to be placed elsewhere. | `ProbeSystemFlexibleMiB()` tries hints `0x1000000000`, `0x800000000`, `0x600000000` and accepts only an address at or above `0x400000000` | A detail, not code. |
| **Safe use of an unproven pool.** Eden4 does not say what happens on a firmware without the call. | Here: the call is looked up at start (the SDK's stub library, else `sceKernelDlsym`), probed in steps, and any failure makes the program fall back to direct memory and count the refusal | Tested against a model of the kernel in `tests/lazy_memory_test.sh`. |

**What it did (0.9.0 console run, read in 0.9.1).** The pool exists on this console (firmware 12.02) and accepted a 640 MiB probe; the
whole 512 MiB budget was used and the system kept working; Super Mario Odyssey, which ran out of direct memory at 1982 MiB of guest
memory in 0.8.5, ran on to 2152 MiB. It still left only about 93 MiB of the largest free direct block, so 0.9.1 makes the pool
**on by default and larger** (automatic, up to 1536 MiB, limited by what the pool has; safe mode keeps it to 512 MiB). Its real size is
not known yet: the 0.9.0 probe stopped at 640 MiB, and the 0.9.1 probe is a binary search up to 2176 MiB that logs the result.

## Already here, or done differently

| Eden4 | This project |
|---|---|
| GPU cache budget from the largest free direct block, collection at 30-40% of it | The memory watch has used the largest free block since 0.7.6; the driver arena sets the cache budget (patch 0025). |
| Guest 4 GiB committed in direct memory up front, leaving about 480 MiB for the GPU | Here guest RAM and the tables are committed lazily in 64 KiB pieces (patches 0002, 0007, 0027), which is why a game that does not use its RAM does not pay for it. |
| Stream buffer 16 MiB | 128 MiB since 0.8.5 (patch 0028), the value Eden itself uses for FreeBSD; it came from Eden's source, not from Eden4. A smaller value is a possible option later: a smaller buffer makes a frame wait sooner for the GPU. |
| Disk shader cache off | On, with no limit and a recovery ladder (0.7.7). |
| Sampling profiler, crash log | Present (the profiler adds up the cores per thread since 0.8.3). |

## Not taken, and why

| Eden4 | Why not |
|---|---|
| **Dynarmic JIT changes**: a two-level page-table walk inline in the generated code (`SparseLargeVector`, a "direct" mirror holding `host - guest` offsets, a page-reuse pass, spare host registers R13/R14, code cache writable only in a window), plus a PC harness that runs them against AArch64 test programs. | The most promising lever for speed, because the emulated CPU is what limits every game here. **0.9.2 carries one part of it, the page reuse, and 0.9.3 a second one, the direct table (a decoded copy beside Eden's table, `jit_direct_table`)**, rewritten for this port's flat page table as an optional pack (`patches/eden-jit/`, `jit_page_reuse`, off by default; never run on a console, its code generator never compiled). The rest is not ported: the two-level table and direct mirror (a different memory layout, and the page reuse does not need them), the register allocator changes (a separate and riskier change), the code-cache windows (this port's cache is already writable and executable, patch 0005). **Next step:** a console run of the pack off and on; and Eden4's harness (`tools/jit-harness/`) on a PC, with Xbyak, fmt and Boost, run against the pack. |
| **Staging cap of 64 MiB with reclaim and retry** on low GPU memory (waits for the GPU, frees idle staging buffers, retries). | A patch across several Vulkan files that could not be compiled here against the pinned Eden. The 0.8.4 pause-before-crash and the 128 MiB stream buffer address the same problem from the outside. |
| **VMA block size 16 MiB.** | The driver arena is one block of 1280 MiB here; smaller blocks could fragment the GARLIC part. Needs a console test of its own. |
| **GPU page table committed on use.** | Already lazy here (the lazy regions); what is new is where the pieces come from, above. |
| **HOME menu** (a Switch-style launcher, after SwitchLauncher by kasobos67) | A different interface; this project has its own tab menu, with covers read from the games, in two languages. |
| **Seventh CPU core variant** (`PARAM.SFO` attribute `0x00010000`). | Another candidate for the speed problem: one more core. It changes the package's `param.sfo`, which is on this project's "hands off" list until a console test proves a variant starts and keeps the 4.4 GiB. It would be a second package, built and tested on its own. |
| **PS4 Pro mode.** | Eden4 reports a GPU hang and a console crash; this project stays in base mode, like Eden4. |

## Things Eden4 says that are worth knowing

- Its first-start time is 20-35 s (JIT compilation at boot), only two games were tried, and "most games run below full speed":
  the same limit as here.
- It reports that when RADV cannot allocate a command buffer, Mesa writes out of bounds (`radv_amdgpu_cs_grow`) and the app crashes
  instead of failing cleanly. Worth remembering when reading a crash in the driver here.
- Its README says it was tested on firmware 13.02, in base PS4 mode; this project's console is on 12.02.

## How the list was made

Eden4's tree was unpacked and read: the memory sections of its `docs/ARCHITECTURE.md`, `docs/KNOWN_ISSUES.md`, its changelog of
test builds 00.01-00.30, `runtime/port/ps4_runtime.cpp` (heap and pools), and the file list of `eden/` and `tools/jit-harness`. The
JIT sources were listed and compared by topic, not compiled. The Eden4 source is not copied into this repository.
