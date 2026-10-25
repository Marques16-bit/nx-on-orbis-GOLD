# Roadmap and audit notes

Audit of 2026-10-21 over the sources of branch `main` (v0.1.0).

Read line by line: `frontend/` (`main.cpp`, `ps4_platform.*`, `heap_census.cpp`,
`libc_wide_fixes.cpp`, the CMake files), `toolchain/eden-ps4.cmake`, `tools/*.sh`, `scripts/`,
`package-eden.sh`, `configure-eden.sh`, all docs, and the patches that touch memory, the JIT, GPU
submission, cache budgets and containers (0002-0005, 0007-0009, 0015-0017, 0024-0027). The colour
self-test and trace patches (0010-0014, 0018-0023) and patch 0006 were reviewed by subject and touched
files only. `probe/`, `frontend/emutls/` (LLVM's compiler-rt), `frontend/menu/` and
`toolchain/include-overlay/` were listed and skimmed, not reviewed. The `experimental-fastmem`
branch (tests 25-29) was **not** in the audited archive; items that depend on it say so.

Everything here is a plan, not a result: only a build installed and played on a console proves
anything (see "How a version is tested").

## 1. Map of the repository

| Path | What it is | Change it? |
|---|---|---|
| `patches/eden/` (27) | The whole port, as patches on Eden `5f142c79`: memory (0002, 0007, 0027), JIT (0003, 0005, 0008), GPU driver workarounds (0009, 0015, 0025), texture-cache fixes (0016, 0017, 0020), colour self-tests and diagnostics (0010-0014, 0018-0023, 0026), containers (0024) | One patch at a time, each tested on the console |
| `frontend/` | The PS4 executable: boot, settings, pad, logs, crash reports, profiler, game picker | Yes |
| `frontend/libc_wide_fixes.cpp` | 4-byte `wmemchr` & co. | **No** (see section 2) |
| `toolchain/`, `tools/`, `configure-eden.sh`, `package-eden.sh` | Windows-only build with the original machine's paths | Yes: make it reproducible (v0.3.0) |
| `tests/` | Host tests and the startup checks | Add to it |
| `probe/` | eden-probe, the small program used to measure the console before porting | No (history) |
| `docs/` | Documentation; `dev-log-es.md` is the full test-by-test log | Keep up to date |

## 2. Hands off

Each of these was found by hunting crashes on a real console; changing one without a reason and a
console test will bring a crash back.

- `frontend/libc_wide_fixes.cpp`: the SDK's `wmemchr` compares 16-bit units while the compiler's
  `wchar_t` is 32-bit; this file is what fixed the texture-cache corruption.
- The package layout and signing in `package-eden.sh` (paid `0x3800000000000011`, default authinfo,
  `sce_module/*.prx`, `sce_sys/about/right.sprx`, category `gd`): the only layout that starts and
  gets ~4.4 GiB of direct memory.
- Flushing the GPU command buffer every 256 draws / 96 uploads (patch 0015): the driver loses the
  device above 2 MiB per submission.
- Removing `VK_EXT_conditional_rendering` and `VK_KHR_draw_indirect_count` (patch 0009): GPU hangs.
- `-femulated-tls`, 512 KiB fiber stacks, ASTC decoded synchronously on the CPU.
- Never log, or take a lock, from a signal handler.

## 3. Audit findings

| # | Finding | Status |
|---|---|---|
| A1 | `frontend/` on `main` is the **test 22** state, while the patches are test 24 and the docs call v0.1.0 "test 24". Missing: `cpu_accuracy=unsafe` as default and the `cpu_accuracy` key of `settings.txt` (INSTALL.md documented a key that did nothing), the profiler-hang fix of test 23, the "largest free direct block" wording of the status line, and the build id still said `test22` | Fixed in v0.2.0 |
| A2 | `frontend/heap_census.cpp` wraps `malloc`/`free`; every `free()` and `realloc()` of every thread took one global spin lock and scanned the table, although nearly all frees are small pointers that were never recorded | Fixed in v0.2.0 (lock-free filter, host-tested: same census output, about 3x faster in a 6-thread stress test on a 1-core machine; the gain on the console is not measured) |
| A3 | Patch 0024 swapped Eden's `boost::unordered_flat_map/set` for `std::unordered_*` in the whole PS4 build. The real cause of the texture-cache bug was `wmemchr` (patch/test 20), and boost's map passes the startup check on the console. Node-based `std` maps are slower on the texture cache's hot lookups | Candidate for v0.6.0 (revert 0024, needs a console test) |
| A4 | Fastmem, the direct-memory map and the environment-out-of-heap fix live only in `experimental-fastmem`; the redirect storm (~370,000 slow-path redirects per session) is the main known speed lever | v0.5.0 |
| A5 | Patches 0010-0014, 0018-0023 and 0026 are colour and cache diagnostics, now off by default but still compiled into the texture-cache path (for example `Ps4History::Record` is called and returns early, and a 16384-entry history ring is linked in). Patches 0016 and 0017 mix real fixes (page ownership and fenced retirement of images) with crash breadcrumbs written on every render pass (ten relaxed stores, about 150 render passes per frame: negligible, and they make crash reports useful). No speed gain is claimed from removing any of this | Tidy after the colours are solved (v0.4.0); keep the fixes and the breadcrumbs |
| A6 | The build was Windows-only, hard-coded `C:/Users/alejo/...` in six files, needed an SDK patched by hand and a libc++ 18, OpenSSL and FFmpeg built beforehand. There was no CI | v0.2.0 added checks; v0.3.0 builds the package on GitHub (paths now come from `NXO_ROOT`); not yet run |
| A7 | Colours: the presentation path (Eden's swapchain format against the format `wsi/orbis` registers for video-out) was never tested. The repository has no code on that path: the headless surface is created in patch 0001 and the swapchain format is Eden's own choice (`vk_swapchain.cpp`, unpatched). The game picker, which writes `0xAARRGGBB` pixels straight to video-out, has the right colours, so the display side is sound | v0.4.0: the patch has to be written against Eden's real code, which is not in this repository; the `Check patches` workflow now keeps it as the `eden-sources` artifact |
| A8 | The lazy-memory fault handler (`orbis_lazy_memory.cpp`) holds a global spin lock across two system calls and a 64 KiB `memset` per first touch; during a loading burst other cores spin on it | Candidate for v0.6.0 (commit several chunks per fault) |
| A9 | JIT caches are 32 MiB per emulated core and there is no counter of cache flushes, so it is unknown whether flushes cost time in a race | Add a counter first (v0.6.0), then decide |
| A10 | Checked and fine: `Ps4::Log` syncs every line but is only used for sparse events; the sampling profiler (one signal per 4 ms, round-robin over six threads) is a negligible cost, and it is the project's main diagnostic | No action |

## 4. Versions

| Version | Content | Needs a console test |
|---|---|---|
| 0.2.0 | A1 and A2, this roadmap and the changelog, the update-zip workflow, the patch/host-test checks. Not built on its own: it is part of 0.3.0 | (with 0.3.0) |
| **0.3.0** | The `.pkg` and ELF built by GitHub Actions: scripts take their paths from `NXO_ROOT`, the SDK and tools are fetched in the workflow, a release is published with the changelog as its notes. Expect a few rounds of fixing from the CI logs, then one console test of the first package | Yes (boots, MK8D race, `cpu_accuracy` key, `boot.log` banner) |
| **0.4.0** | The menu (games and settings in tabs, Portuguese and English), the new icon, a workflow that keeps only the `.pkg`. Needs no new engine work | Yes (the menu on the console, a game starts from it, settings reach the game) |
| 0.5.0 | Colours (A7), written against the exact Eden sources from the `eden-sources` artifact and checked with `git apply --check` before it ships; then clean-up of the diagnostics (A5) | Yes |
| 0.6.0 | Fastmem and memory headroom (A4): fewer redirects, Eden's 256 MiB stream buffer to 128 MiB, texture-cache budgets, then `ORBIS_VRAM_GARLIC` as a last resort | Yes (memory) |
| 0.7.0 | Speed items: A3, A8, A9, thread placement experiments (a Jaguar module shares an L2 between four cores) | Yes |
| 0.8.0 | Console profiles: pick the memory budget, GPU arena, JIT cache and settings from what the console reports (total direct memory, and whether it is a Pro) instead of constants measured on one Pro; `settings.txt` override | Yes, on each model |
| **1.0.0** | See below | Yes, on each model |

Rule for every version: change one area, keep a changelog entry, never mix a colour change with a
memory change in the same package, so a regression has one suspect.

## 5. What 1.0.0 means

1.0.0 is declared only when **all** of these hold:

1. The `.pkg` and the ELF are built by the workflow, from a tag, with no hand steps.
2. Red and blue are correct in the whole image.
3. A session of 30 minutes (MK8D menus plus races, and one lighter game) ends without a crash or a
   hang on a PS4 Pro.
4. Someone who owns a PS4 Fat and someone who owns a Slim have installed it, run the same session
   and sent a `boot.log`.
5. The README, INSTALL, STATUS and the release notes match what the release does.

It does **not** promise Mario Kart 8 Deluxe at 30 or 60 fps. The measured limit is the emulated CPU
(core 0 at 76-84%, almost all in translated code, with the GPU never waited on); the earlier
estimate for MK8D on a Pro is 20-25 fps at best, roughly a quarter less on a base model. Lighter
2D and indie games are where full speed is realistic.

## 6. How a version is tested

1. Install the package; keep the matching ELF (crash offsets only symbolize against that exact
   file).
2. Run the Homebrew Menu, then MK8D: menu, then a race (the same 2-lap session each time).
3. Send back `boot.log`, `mesa.log` and `log/eden_log.txt`. The `status:` lines give the fps; the
   `profile` lines show where the time goes.
4. Compare with the previous version's `status:` lines before calling anything faster.
