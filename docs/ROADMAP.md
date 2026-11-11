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
| `patches/eden-jit/` (1, optional) | The JIT page reuse pack: one patch to Dynarmic (an IR pass, one new IR instruction, its emitter and the access emitters), `MODE` (`on`/`off`). Applied by `scripts/apply-jit-pack.sh` only if it fits; `scripts/drop-jit-pack.sh` takes it out when the CI build fails inside Dynarmic | Console run, off and on |
| `patches/eden/` (30) | The whole port, as patches on Eden `5f142c79`: memory (0002, 0007, 0027, 0028, 0029, 0030), JIT (0003, 0005, 0008), GPU driver workarounds (0009, 0015, 0025), texture-cache fixes (0016, 0017, 0020), colour self-tests and diagnostics (0010-0014, 0018-0023, 0026), containers (0024) | One patch at a time, each tested on the console |
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
| **0.5.0** | Folders and permissions that FTP can use, options that survive a restart, a LOG tab, quick profiles, the new icon. The version to try Mario Kart 8 Deluxe and Super Mario Maker 2 with. Then: a Homebrew Menu that lists programs (Eden's NRO loader would provide the loader environment) | Yes |
| **0.6.0** | Firmware from a .zip, clean game titles, the colour diagnosis (the BGRA fix forced On), the close request logged. First version after Super Mario Maker 2 opened | Yes |
| **0.7.0** | The video hook: colour swap on the screen, performance HUD, pause menu on L3 + R3, shader cache shown. Next (0.8.0): game covers (menu after the system exists), Eden's extra settings once their names are checked, and the performance work the HUD will point to | Yes |
| **0.7.1** | Games open again: the shader-cache preload is limited and recovers by itself, a Clear shader cache action, an overlay switch. Then 0.8.0 as planned | Yes |
| **0.7.2** | A lighter video hook (draws after the flip, only on linear buffers), the PERFORMANCE tab. Then 0.8.0 as planned | Yes |
| **0.7.4** | Options from Eden's own settings screens in the menu and `settings.txt` (VSync, window adapting filter, anti-aliasing, anisotropic filtering, DMA accuracy, region, speed percentage, ...), each applied only if the pinned Eden has the setting. Then 0.8.0 as planned | Yes (each option on the console) |
| **0.7.6** | The HUD and the L3 + R3 menu stop flickering (overlay kept alive in every video buffer), memory profiles chosen from the direct memory the console reports (first part of 0.10.0), and a memory watch that warns with the speed of the fall and can pause before the crash. From here the focus is performance | Yes (flicker, and the memory classes on a Fat / Slim) |
| **0.7.10** | Menu in colour themes (7) with drawn icons on the tabs, the cover of each game read from inside the game itself (its control data), and the logs of Eden and the driver kept per run like the boot log | Yes (the covers need the keys; the themes on a TV) |
| **0.8.0** | What the Mario Maker 2 logs show (written down with the evidence), a detector of picture freezes that logs the flip it ended on and takes three screenshots after each of the first three, so the start-of-scene defect can be seen to change | Yes (a console run with Mario Maker 2 and the `shots/` pictures) |
| **0.8.1** | The scene-change stall read from the 0.8.0 run (it is the game loading, not the GPU: the 0.8.0 guess is withdrawn), and a profile of every freeze (which threads are busy during it) so the performance work starts from measurements | Yes (a console run with Mario Maker 2) |
| **0.9.2** | The JIT's page lookup reuse (Eden4's effect, written for Eden's flat page table) as an optional pack of one patch to Dynarmic (`patches/eden-jit/`), off by default (`jit_page_reuse`, CPU AND MEMORY tab; the "More FPS (test)" profile turns it on); applied only if it fits the pinned Eden, and taken out by the CI if it does not compile | Yes (never run, and its code generator never compiled: a console run of a CPU-bound game with it off and on; `boot.log` says `JIT page reuse:` and how many groups were formed) |
| **0.9.1** | The system flexible pool is no longer an experiment: on by default (auto, up to 1536 MiB, found by a binary search of the pool), kept to 512 MiB in safe mode; patch 0030 counts what the kernel frees; up to 4 controllers (`players`, SYSTEM tab) | Yes (a console run of Odyssey: the free direct block should stay far above the ~93 MiB of 0.9.0; a second DualShock logged in as a guest) |
| **0.9.0** | Eden4 (another PS4 port of Eden) read in full and credited (`docs/EDEN4.md`): its memory idea taken as `sysflex_mib`, which lets the guest's RAM and Eden's tables take memory from the console's system flexible pool instead of direct memory (off by default; patch 0029); a README that describes the project as it is now, `docs/SETTINGS.md` generated from the menu, and `tests/docs_check.sh` that checks them against the code | Yes (a console run of Odyssey with `sysflex_mib` on: does the pool exist, how big, and does the free direct block stay higher; the `system flexible memory:` lines of `boot.log` answer it) |
| **0.8.5** | The 256 MiB GPU stream buffer found to be Eden's own constant (`MAX_STREAM_BUFFER_SIZE`, which Eden already sets to 128 MiB for FreeBSD) and cut to 128 MiB: 128 MiB of direct memory back for every game | Yes (a console run of Odyssey: the first `status:` line should show about 128 MiB more free; this is the first change to Eden since 0.8.0) |
| **0.8.4** | The renderer shown to be Vulkan (nothing in the build can draw with OpenGL), the Super Mario Odyssey memory run read (direct memory ran out at 58 MiB free after the guest grew 257 to 1687 MiB), the vulkan-ps4 repository assessed (not a replacement for the driver in use), and the memory alert now pausing the game by default instead of only warning | Yes (a console run of Odyssey; the pause panel is what is new) |
| **0.8.3** | The 0.8.2 run read (no CPU error in it; the speed limit is still the emulated CPU), ASTC textures kept as BC3/BC1 instead of RGBA8 (an Eden option, now in the menu), and a profile that adds up the cores busy in every named thread, to find where the 3.1-3.6 cores the driver reports go | Yes (a console run; the new `profile total:` line is the point, send boot.log) |
| **0.8.2** | More options to raise the speed (the emulated CPU clock and custom CPU ticks, asynchronous presentation, a "More FPS" profile) and to use less memory (a "Minimum RAM" profile, the menu's pictures kept small or off), and a safe mode that turns the experimental lines of settings.txt off for the one start after a start that died before drawing | Yes (nothing of it was run on a console: each option has to be tried one at a time against Default) |
| **0.7.9** | The games list shows covers (a big picture of the selected game and thumbnails, a coloured tile with the initials when there is none), and the log gets a frame timeline every 2 s, the logs of the last 8 runs kept in `logs/`, and a start time | Yes (the covers on a TV; the timeline tells a slow game from a stuttering one) |
| **0.7.8** | L3 + TouchPad saves the picture on screen as a BMP (to look at the Mario Maker 2 theme glitch and the HUD as they are), a false stall alarm gone | Yes (the picture tool on the console; the HUD, the themes and the speed still need the pictures and the ELF) |
| **0.7.7-fix** | "Exit game" / "Restart game" that start the app again (a ladder of four ways that learns which one works), the HUD written at every flip with its cache lines flushed, asynchronous shaders off by default (Eden skips draws whose shader is not built) | Yes (the restart on the console, the HUD, the Mario Maker 2 themes) |
| **0.7.7** | A shader cache with no limit that is never deleted and recovers from a death while it loads (one builder thread, then parked), "Restart game" in the L3 + R3 menu, a way out of the game that closes the app when it cannot restart it, a 1152 MiB floor for the arena. From here the focus is performance | Yes (the restart lookup, the cache ladder, and the Mario Maker 2 themes with the new test profile) |
| 0.7.0 | Colours (A7), written against the exact Eden sources from the `eden-sources` artifact and checked with `git apply --check` before it ships; then clean-up of the diagnostics (A5) | Yes |  Also here, once `eden-sources` is read: game covers, the in-game menu on L3+R3 (Continue, Options, Exit; Eden has no save states) and an FPS / memory / temperature HUD, all drawn over Eden's presented frames.
| 0.8.0 | Fastmem and memory headroom (A4): fewer redirects, Eden's 256 MiB stream buffer to 128 MiB, texture-cache budgets, then `ORBIS_VRAM_GARLIC` as a last resort | Yes (memory) |
| later | Speed items: A3, A8, A9, thread placement experiments (a Jaguar module shares an L2 between four cores), and the Eden4 JIT changes one at a time with its PC harness (see `docs/EDEN4.md`) | Yes | Memory for games that never return pages (Odyssey freed nothing in the 0.9.0 run): one idea is to find 64 KiB chunks the game has filled with zeros and give them back, since an uncommitted chunk reads as zero; it needs every guest thread stopped while a chunk is checked and given back (a write racing the decommit would be lost), so it is not started. Black cutscenes in Mario vs. Donkey Kong: needs `vic_frame.raw` / `vic_frame.txt` from `/data/edenps4/` to see whether the decoded frame is real.
| 0.10.0 | Console profiles: pick the memory budget, GPU arena, JIT cache and settings from what the console reports (total direct memory, and whether it is a Pro) instead of constants measured on one Pro; `settings.txt` override | Yes, on each model |
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
