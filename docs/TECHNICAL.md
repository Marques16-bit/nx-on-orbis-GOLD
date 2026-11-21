# Technical findings

What was learned porting a large C++20 emulator to the PS4 with OpenOrbis and the orbis-ports
SDK. Measured on a PS4 Pro, firmware 12.02, GoldHEN. Several items apply to any PS4 homebrew.

## Package layout and memory

- **Signing/layout decides how much memory the process gets.** Same ELF, three layouts:
  - orbis-ports' default (paid `0x3800000000000011`, default authinfo, no `sce_module/`): does not
    start on this console.
  - Piglet-style (paid `0x3800000000000035` + Piglet authinfo): starts, but only **768 MiB** of
    direct memory, not enough for the GPU driver.
  - **OpenOrbis samples' layout** (paid `0x3800000000000011`, default authinfo, **with**
    `sce_module/libc.prx` + `libSceFios2.prx` and `sce_sys/about/right.sprx`, SFO category `gd`):
    starts with **~4.4 GiB** of direct memory and the CPU at ~2.1 GHz. `package-eden.sh` uses it.
- Flexible memory (anonymous `mmap`) is only ~384 MiB. Large things must come from direct memory.
- **Guest RAM is committed on demand.** `src/common/orbis_lazy_memory.*` reserves address ranges;
  the first touch of each 64 KiB chunk faults, a handler installed with
  `sceKernelInstallExceptionHandler` maps a zeroed chunk of direct memory there and the access
  retries (~5 µs per fault). Used for the 4 GiB guest RAM and Eden's big sparse tables.
- 64 KiB chunks taken first-fit from the bottom scatter over all of physical memory and break up
  the large blocks other allocators need (the C heap takes 128 MiB carve-outs). v0.1.0 takes them
  from the top 2 GiB first.
- `sceKernelAvailableDirectMemorySize` returns the **largest free block**, not the total.
  `sceKernelGetDirectMemoryType` can be walked over the whole range to get a real map (experimental
  branch, `LogDirectMemoryMap`).
- **Address space is limited.** eden-probe: the largest single `sceKernelReserveVirtualRange` was
  16 GiB (64 GiB fails with `0x8002000c`). In the emulator, 512 GiB was refused; a view that did fit
  left no room for the guest page table reserved after it. 8 GiB is what fits next to everything else.

## C/C++ runtime

- **The SDK's wide-character functions are 16-bit, the compiler's `wchar_t` is 32-bit.** The SDK's
  static `wmemchr` compares 2-byte units (`cmpw`, step 2). libc++ lowers `std::find` /
  `std::ranges::find` over any 4-byte trivially comparable type (`u32`, ids...) to
  `__builtin_wmemchr`, i.e. that `wmemchr`. Result: searches that only find a value if it is the
  first element. In Eden it corrupted the texture cache (dozens of crashes over tests 7-19).
  `frontend/libc_wide_fixes.cpp` defines 4-byte `wmemchr`, `wmemcmp`, `wmemcpy`, `wmemmove`,
  `wmemset`, `wcslen`, `wcscmp`, `wcsncmp`, `wcschr` in the executable (built with `-fno-builtin`).
  **Any PS4 homebrew using a newer libc++ with this SDK is likely affected.**
- Eden's static TLS block (9 KiB) ended up laid out differently from what lld computed (reads of a
  neighbouring field); `-femulated-tls` for the whole program plus compiler-rt's `emutls.c` fixed it.
- libc++ 18 had to be built for the PS4 (see `BUILDING.md`).
- Never log (or take any lock) from a signal handler. The sampling profiler's late signals once
  logged from a thread that already held the log mutex and froze the whole emulator.
- One unexplained heap overwrite was seen (test 28): the small heap block musl keeps the environment
  in held log text, and the GPU driver crashed in `getenv`. Not found; the experimental branch moves
  the environment to static storage and watches the old block.

## The GPU driver (Mesa RADV for "Liverpool", from orbis-ports)

- Vulkan 1.3, 197 extensions; of what Eden uses only `VK_EXT_index_type_uint8` is missing. No
  float16, no ASTC/ETC2 (Eden decodes ASTC; on the CPU here, the GPU compute path and the async CPU
  path both crashed).
- `VK_EXT_conditional_rendering` and `VK_KHR_draw_indirect_count` caused GPU hangs on GFX7; v0.1.0
  removes them before creating the device and uses the macro fallback for indirect-count draws.
- **Each submission is flattened into a 524288-dword (2 MiB) staging buffer**; a bigger one hangs
  the GPU (`mesa.log`: "submission does not fit"). v0.1.0 submits every 256 draws and after 96
  uploads.
- **Memory**: the driver takes one ONION arena (`ORBIS_ARENA_MIB`, default 1024; v0.1.0 sets 1280,
  1024 ran out loading a race). But images and other device-local buffers go to **GARLIC memory
  allocated separately, outside the arena**, and mapped into the arena's address window, so they
  cost direct memory on top of it (hundreds of MiB while a race loads). `ORBIS_VRAM_GARLIC=0` keeps
  everything in the arena; the driver warns it costs GPU speed. Not tried.
- Its log: set `MESA_LOG_FILE`, `MESA_LOG_LEVEL=info`, `RADV_DEBUG=info` (the frontend does). Its
  BUDGET lines say how many cores are busy and how long the CPU waited for the GPU.
- Self-tests that all came out correct on the console (so not causes of the color bug): B8G8R8A8
  sampling and blits, all 24 component mappings (compute), R8G8, A2B10G10R10 sampling and rendering.
  The color bug affects the whole image (3D, videos, UI); the presentation path (swapchain format
  vs the format `wsi/orbis` registers for the video output, zero-copy scan-out) was never tested.

## JIT (dynarmic)

- Code buffers: direct memory mapped RW, then `sceKernelMprotect` to RWX (the only order allowed).
  32 MiB per emulated core.
- Faults are delivered through the kernel exception handler, not `sigaction`. The handler gets a
  ucontext whose FreeBSD mcontext starts at offset 64 (`rip` +160, `rsp` +184, fault address +136);
  modifying `rip`/`rsp` and returning resumes there (verified by the experimental branch's fastmem
  self-test, which performs dynarmic's "fake call").
- The same direct memory can be mapped at two addresses (aliasing works), and `MAP_FIXED` mappings
  inside a reservation work: what fastmem needs.
- Fastmem (experimental branch): 16 KiB host pages against 4 KiB guest pages means a host page can
  only alias the backing when its four guest pages map consecutive backing with the same rights;
  everything else is left unmapped and the faulting instruction is moved to the slow path.

## Threads, timing, input, audio

- `pthread` mutexes behave (startup check: 3 threads x 400000 increments, exact).
- Threads get 2 MiB stacks and 64 KiB alternate signal stacks so a stack overflow can still report.
- Eden's fibers: 512 KiB stacks (128 KiB rewind) instead of 4 MiB; 94 fibers at 4 MiB took 372 MiB.
- Audio: `sceAudioOut` sink (`src/audio_core/sink/ps4_sink.*`), main port, 256 frames, 48 kHz stereo.
- Video out: the game picker draws directly with `sceVideoOut` and keeps its 16 MiB of framebuffers
  after closing the display (freeing them rebooted the console when a game started).

## Diagnostics built in (`frontend/ps4_platform.cpp`)

- `boot.log` written unbuffered from the first static constructor; crash handler with registers and
  a frame-pointer backtrace as `eboot+0x...` offsets; hang watchdog that dumps every thread's stack
  (signals raised with `sceKernelRaiseException`); a sampling profiler (per-thread busy share, JIT
  share and hottest code buckets every 30 s); heap census of allocations of 256 KiB or more (1 MiB until the 0.9.7 fix).
- Startup checks: TLS, mutexes, `std::find` over 4-byte types, boost `flat_map`.

## The overlay and the memory watch (0.7.6; not yet seen on a console)

- **Overlay** (`frontend/video_hook.cpp`). RADV's WSI registers its swapchain images as the video-out
  buffers; the linker's `--wrap` sends `sceVideoOutRegisterBuffers` and `sceVideoOutSubmitFlip` through
  the hook. The game's GPU rewrites a whole buffer per frame, so an overlay written once around the flip
  was missing from the next frame (a flicker at the game's frame rate). The keep-alive mode builds each
  element (HUD, notice, panel) as a bitmap in normal memory and keeps it in every buffer: the worker looks
  at 9 probe pixels per buffer (reads of the write-combined buffers are slow, so nothing else is read),
  copies the bitmap again when a probe no longer matches, and the wrapped flip stamps the frame about to
  be shown. `overlay_mode=classic` keeps the 0.7.3 drawing. The counters in `Describe()` (boot.log) say how
  often each path was needed.
- **Memory** (`frontend/memory_guard.{h,cpp}`, pure logic, host-tested). The process gets ~4608 MiB of direct
  memory on the PS4 Pro measured; what runs out is the largest free block (`sceKernelAvailableDirectMemorySize`),
  because the C heap takes 128 MiB at a time. At start the memory class (roomy / standard / tight) sets the
  driver arena, the shader preload and the driver cache before `settings.txt`; while playing, a watch
  samples the block once a second and reports a stage (ok / low under 160 MiB / critical under 64 MiB, with
  hysteresis) and the speed of the fall. `memory_action=pause` stops the game at critical and asks. It
  reclaims no memory held by Eden: that needs Eden-side patches (roadmap 0.8.0).

## The shader cache, the way out and the restart (0.7.7; not yet seen on a console)

- **Cache** (`frontend/shader_policy.{h,cpp}`, host-tested). Eden's `PipelineCache::LoadDiskResources` builds
  every pipeline of `vulkan.bin` in a worker pool before the game starts, and a stopped token kills the
  pool for good, so the size is limited before Eden reads the file (`shader_preload`, default all) and the
  load is never cut short. A cache over a chosen limit is parked (`vulkan.bin.bak`, `.bak2`...), never
  deleted. While the load runs `cache/shader/loading.marker` holds `attempt N <title id>`; a start that finds
  it loads the same cache with one builder thread (`std::thread::hardware_concurrency()` is wrapped with
  `-Wl,--wrap=_ZNSt3__16thread20hardware_concurrencyEv`, see `ps4_platform.cpp`), and the one after that
  parks the cache and starts empty. The cap is set before `System::Load` because Eden sizes the pool when
  it creates the pipeline cache there.
- **Leaving the game.** Eden cannot be torn down and rebuilt in one process, so leaving starts the app again
  (`sceSystemServiceLoadExec`, found with `sceKernelDlsym` in the console's libSceSystemService because the
  SDK stub lacks it) and, when that fails, ends the process. "Restart game" leaves `restart_game.txt` for the
  new process (read and deleted first thing in `FindGame`).

## The restart ladder and the HUD at the flip (0.7.7-fix; not yet seen on a console)

- `RestartApp()` tries `sceSystemServiceLoadExec` with four (path, argv) pairs, the one known to work first.
  An attempt is written to `restart_state.txt` before the call; `NoteRestartOutcome()` at the next start reads
  the wall-clock time since it and records `good` or marks the way in `bad`. A call that works never returns,
  so the log cannot say so itself.
- `FlipStamp()` (inside the wrapped `sceVideoOutSubmitFlip`) writes the HUD and notice again whatever the
  probes say; `RestampShown()` repeats it every 4 ms for 40 ms; `Blit()` flushes the cache lines of each row.
- Asynchronous shaders: `PipelineCache::BuiltPipeline` returns null for an unbuilt pipeline when they are on,
  and `RasterizerVulkan::PrepareDraw` skips the draw; the port's default is now off, as Eden's.

## The picture of the screen (0.7.8; not yet seen on a console)

- `Ps4Hook::SaveScreenshot()` copies the buffer flipped last (`last_index`), row by row and in order, under the
  hook's lock, and writes it as a 24-bit BMP after the lock is released (`screenshot.h`, header only so the
  workflow's fixed test command line still links). The words are read the way the console reads them
  (`format_used`), as the HUD's `Pixel()` writes them.
- `main.cpp` calls it on L3 + TouchPad (edge) with the game paused (`system.Pause()`, 300 ms, copy,
  `system.Run()`), into `/data/edenps4/shots/`.
- The hang watch counts a stall from `max(last frame, when the watch was armed)`.

## Covers and the extra log (0.7.9; not yet seen on a console)

- `menu/image_decode.h` (header only) decodes PNG (every colour type and bit depth, interlaced too, a
  hand-written inflate), baseline and progressive JPEG (4:4:4, 4:2:2, 4:2:0, grey; bilinear chroma) and BMP
  (24 and 32 bit, palettes). Limits: 4096 px a side, 16 Mpixels; damaged files return false. `tests/image_decode_test.cpp`
  compares the pixels with Pillow's (`tests/data/*.raw`) and feeds it truncated and scrambled copies, under
  ASAN and UBSAN. That test is not in the GitHub workflow (a workflow cannot be changed by an update zip): run it by hand.
- `menu/cover_loader.h` (header only) matches `covers/*.png|jpg|jpeg|bmp` to the games by title id, by file
  name or by the title without its tags (case ignored), decodes with limits (12 MiB a file, 64 MiB of big covers,
  an 8 s budget) and keeps a 400-pixel cover and a 44-pixel thumbnail per game. `GameEntry` carries them; with
  none the menu draws a gradient tile whose colours come from a hash of the title id.
- `Ps4Hook::TakeTimeline()`: the wrapped flip counts flips and the intervals between them (sum, longest, and how
  many were over 34, 50, 100 and 250 ms) under the hook's lock; `main.cpp` writes `tl: ...` every 2 s.
- `ArchiveOldLog()` (`ps4_platform.cpp`) moves the old boot log to `logs/boot_YYYYMMDD-HHMMSS.log` (the time of its
  last line) before the usual rename, and keeps the newest 8.

## JIT page reuse (0.9.2; never run on a console, code generator never compiled)

The optional pack `patches/eden-jit/0001-...` (four Dynarmic files of the pinned Eden; made against Eden's current Dynarmic sources) gives
the JIT Eden4's "page reuse" for this port's flat page table.

- **Pass** (`ir/opt_passes.cpp`, `PS4PageReusePass`, run after the usual passes by a wrapper around `Optimize` for A64): walks the block,
  decomposes each address into an IR root plus constant offsets (through `Add64`/`Sub64` with an immediate, no carry-in, at most 8 deep),
  and puts accesses with the same root into one group while the group's bytes stay within 256. Exclusive accesses and monitor changes,
  barriers, supervisor calls, exceptions, cache operations and any access type other than NORMAL, VEC and UNPRIV end the open groups.
  A group of two or more gets `A64EdenPageLookup(root + lowest offset, span)` placed before its first access, and each member's arg 0 (the
  location descriptor immediate, unused by the emitters) is set to that instruction's value. Runs only if `ps4_page_reuse` is set and the
  `UserConfig` is the one Eden builds (`PS4PageReuseConfigWhy`; the reason bits go to `boot.log`). The tested part: `tests/jit_pack_test.sh`.
- **Producer** (`a64_emit_x64_memory.cpp`, `EmitA64EdenPageLookup`): span inside one page (`offset <= 0x1000 - span`), index inside the
  range-checked address space, load the entry through r14, marked bit, mask, sign extension; any miss makes the result 0. All scratch
  registers are allocated before the first branch.
- **Consumers** (`emit_x64_memory.cpp.inc`, the top of `EmitMemoryRead` and `EmitMemoryWrite`): `test value, value; jz slow`, then the access
  at `[value + vaddr]`; the out-of-line `slow` path is the usual lookup for that one address (`PS4ReuseSlowLookup`, templated on the config
  so the A32 build compiles without the A64 members), then the usual fallback call.
- **Why not the Eden4 code:** its producer and slow path read a two-level table (`page_table_two_level_*`) that Eden does not have; the
  consumer is table-independent and is kept in the same shape.
- **What was not checked:** that Xbyak accepts every emitted form, that register allocation holds across the longer-lived value (Eden4 had a
  bug of this kind, fixed in its build 00.12 with a PC harness that runs Dynarmic against AArch64 programs; it is in Eden4's source at
  `tools/jit-harness/`, needs Xbyak/fmt/Boost, and is the right next step if the option is to stay), and the pinned Eden's differences from
  the Dynarmic the patch was made against (`git apply --check` in `scripts/apply-jit-pack.sh` catches changed context, not changed meaning).

## JIT direct table (0.9.3, fitted to the pinned Eden in 0.9.4; never run on a console)

Part 2 of the JIT pack, `patches/eden-jit/0002-JIT-direct-page-table-copy.py`. It is a script of anchored edits (each anchor is a line or a few; a missing or repeated anchor
refuses all edits) and not a diff. 0.9.3 wrote its anchors from Eden master and did not fit the pinned Eden; 0.9.4 takes both forms (`constexpr`/`inline` on `Store` and
`MarkRasterizerCached`; the stride branch or the plain `shl`), is tested on copies of the pinned files (`tests/jit_pack/fixtures_pinned/`), and its emitters were run on
a PC with `tools/jit-harness/` (identical results in the four configurations).

- **The table** (`common/page_table.h`, `page_table.cpp`; everything under `#ifdef __ORBIS__`): `PageTable::direct_entries`, a
  `SparseLargeVector<std::atomic<u64>>` of the same size as `entries`, allocated by `Resize` for the first table sized while
  `ps4_direct_table` is set. Slot `i` is the pointer of entry `i` when the entry is not marked (`ExtractPointer`, i.e. `host - guest`, sign-extended),
  else 0. `ps4_direct_entries_base/_bytes` and `ps4_direct_mirror_base` (defined weakly in `frontend/jit_pack.h`) name the table for
  `PageEntryData` (which finds its slot by address) and for the JIT. The table's destructor clears them.
- **Writers:** `Store` and `MarkRasterizerCached` zero the slot, change the entry, then `SyncDirect` (write the slot from the entry; if the
  entry changed meanwhile, again). `MarkDebug` goes through `Store`. `Memory::Impl::MapPages` with no target calls `PageTable::ZeroDirect` before
  `entries.ZeroRegion`; it zeroes the slots only of host pages that have memory (`&entries.GetOrDefault(i) == &entries.data()[i]`), so an
  unmapped part of the address space gets no memory for its slots.
- **Readers:** `PS4DirectOk(conf)` (`emit_x64_memory.h`) is true when `ps4_direct_live`, `conf.page_table` is the mirrored table, offsets are
  absolute, the table is not mirrored and the stride is 3. `EmitVAddrLookup<A64EmitContext>`: after the range check, `mov page, imm64(mirror);
  mov page, [page + index*8]; test; jz abort; [page + vaddr]`, with a separate scratch register for the index. The page reuse's group lookup
  (`EmitA64EdenPageLookup`) and slow path (`PS4ReuseSlowLookup`) do the same. A32 is untouched.
- **What was not checked:** that Xbyak accepts the emitted forms (`mov r64, imm64`, `[r64 + r64*8]`), that the pinned Eden has the places the
  script edits (the workflow "Check patches" says), and every path that might change an entry without `Store` in the pinned Eden.
- **Tests:** `tests/jit_direct_test.sh` (the script on fixtures of Eden master, a missing anchor per edit, structure of the emitters, the page
  table half on a PC with sanitizers and threads: `tests/jit_pack/direct_test.cpp`) and `tests/jit_pack_scripts_test.sh` (apply and drop in stages).

## The system flexible pool (0.9.0; seen working on a console, 0.9.1 enlarges it)

The PS4 gives a process two kinds of RAM that this port uses: **direct memory** (4608 MiB measured, GPU-visible, what runs out) and
**flexible memory**, which comes in two pools, regular and *system*. The system pool is reached by `sceKernelMapNamedSystemFlexibleMemory`,
which the OpenOrbis headers do not declare and the SDK's stub library may not export. `frontend/ps4_platform.cpp` (`ConfigureSystemFlexible`)
looks it up (the stub, else `sceKernelDlsym(0x2001, ...)`, 0x2001 being the libkernel handle PS4 homebrew commonly uses; not checked on this console), probes how much the pool can map at once, and hands the function and a budget to `Common::Orbis::ConfigureSystemFlexible`
(patch 0029, `orbis_lazy_memory`). A 64 KiB lazy chunk then takes its pages with `MAP_FIXED` (0x10) and `PROT_CPU_RW` (0x3) at the address
it already reserved; if the call fails or returns another address, the pool is switched off for the run and the chunk comes from direct memory.
Giving a chunk back is `sceKernelMunmap` plus a new reservation of the address.
- Constants and the habit of placing blocks above `0x400000000` come from Eden4's runtime, which found that the kernel library allocates its
  own objects at `0x200000000`.
- 0.9.1: it is on by default (`sysflex_mib=auto`, a cap of 1536 MiB). The budget is the smaller of the cap and the probed pool minus 128 MiB, in
  steps of 32 MiB. The probe (`PoolAccepts`, `Ps4Memory::LargestAcceptedMiB`) asks for cap + 128 MiB first and, if refused, bisects between
  160 MiB and that size (about eight questions at most; tested for every pool size from 0 to 2300 MiB in `tests/memory_guard_test.cpp`). It
  assumes the pool accepts every size below one it accepted. Safe mode limits the cap to 512 MiB (`SystemFlexibleProvenMiB`).
- What the 0.9.0 console run showed: the pool exists on firmware 12.02, accepted a 640 MiB probe, gave the whole 512 MiB budget, and the
  128 MiB left was enough for audio and pad to open. It did not show the pool's real size (the probe stopped at 640 MiB).
- The status line of `boot.log` shows `N MiB in system flexible memory` next to the committed guest memory; the lazy-memory counters add
  `system_flexible_bytes` and `system_flexible_refusals`; patch 0030 adds `guest_free_calls`, `guest_free_bytes` and `guest_free_small` (the kernel's
  frees of guest pages, counted in `OnGuestRamFreed` before whole chunks are decided), printed at the end of the `status:` line.
- What is not known: the pool's size, and whether the pages count against another limit when much more than 512 MiB is taken. `tests/lazy_memory_test.sh` runs the real chunk code against a model of these calls, not against the console.

## Memory run of Super Mario Odyssey and the pause default (0.8.4; not yet seen on a console)

- The memory watch (`memory_guard`) reports the LARGEST FREE BLOCK of direct memory. In the Odyssey run it fell from 1790 MiB
  (59 s) to 58 MiB (228 s) while `guest+tables` went from 257 to 1687 MiB; the fall was 600-1100 MiB a minute while the game
  loaded. Free fell 1732 MiB for 1430 MiB of guest growth: the other ~300 MiB are the C heap (heap census 429 -> 517 MiB in
  big blocks), GPU images and JIT.
- `memory_action` defaults to pause. `ApplySettingsFile`/the watch loop in `main.cpp` pause the game when the stage enters
  Critical and the overlay is ready, and show the memory panel (continue, restart, back to the list).
  `Option memory_action` has `default_index` 1, so the menu does not write the line when it is `pause`.
- What the fixed part is made of (from the Odyssey boot.log): 4608 MiB direct memory - 1790 free - 257 guest = ~2560 MiB.
  The known items are the driver arena (1280, 1152 in the tight profile), C-heap carve-outs (4 x 128 MiB by 57 s), a 256 MiB
  GPU stream buffer (`BO 23 (262144 KiB)` in the driver log), JIT caches and fiber stacks.

## ASTC recompression and the whole-thread profile (0.8.3; not yet seen on a console)

- `astc_recompression` (Eden `common/settings.h`): `Uncompressed` | `Bc1` | `Bc3`. With it not `Uncompressed`,
  `texture_cache/util.cpp` decodes each ASTC level on the CPU (`ASTC::Decompress`) and encodes it with `BCN::CompressBC1/3`
  (`stb_dxt`, rows spread over `GetThreadWorkers()`); `maxwell_to_vk.cpp` makes the image BC1/BC3 (sRGB variants too);
  `vk_texture_cache.cpp` `WillUseAcceleratedAstcDecode` is false, so `astc=gpu` falls back to the CPU path. Sizes: RGBA8 = 4 B/px,
  BC3 = 1 B/px, BC1 = 0.5 B/px (`TranscodedAstcSize`, `CalculateConvertedSizeBytes`).
- The profiler (`ps4_platform.cpp`): 24 slots, `RegisterThread` gives the core threads (GPU, VulkanWorker, CPUCore_n, FS,
  nvservices) a slot first and the others at most 14; audio threads (`audio*`, `Audio*`, `DSP_*`), `cemuhook*` and `main` are never
  sampled. `ProfileReport` writes `profile total:` first, then the detail of the core threads and of any thread above 5 % busy.

## Speed and memory options (0.8.2; none run on a console)

- `cpu_clock`: `CoreTiming::GetClockTicks()` multiplies the guest counter by target/1020 MHz (1734 boost, 2040 overclock).
  In multicore the counter comes from the host wall clock, so this scales what the game reads, not what the host does.
- `cpu_ticks`: `CoreTiming::AddTicks()` adds the setting instead of the ticks the JIT consumed; the downcount, which decides
  when a core goes back to `Advance()`, falls by that number. Eden's range is 77-65535 (default 16000 when enabled).
- `async_presentation`: Eden's separate presentation thread (default off outside Android).
- `covers`: the cap of decoded cover pixels (64 MiB, 0 for thumbnails only, nothing for off), `main.cpp` before `RunMainMenu`.
- Safe mode (`safe_mode.h`): a file written at the start of a game and removed after 120 flips or on a deliberate exit.
- Not done on purpose: Eden's `gpu_unswizzle_*` and `frame_gen` settings (compute shaders: the least exercised path of the
  PS4 driver, and ASTC decoding on the GPU already lost the GPU twice), any change to the 256 MiB stream buffer or to the
  JIT cache sizes (patches that cannot be checked against the pinned Eden commit), and fastmem (not in this repository).

## Profile of a freeze (0.8.1; not yet seen on a console)

- `Ps4Hook::SinceLastFlipUs()` is the time since the last queued flip (0 before the first). `main.cpp` opens a profiler
  window when it reaches 0.6 s outside the pause menu, and reports it when `LastStall()` says the freeze ended (dropped if
  the pictures came back in under a second, or if a pause made the gap).
- `Ps4::ProfileWindow(bool report)` only sets an atomic command; the profiler thread (`ps4_platform.cpp`) clears or reports
  the tables itself, so nothing is read while a sample is written. The table holds eight threads: `CPUCore_0..3`,
  `VulkanWorker`, `GPU`, `FS`, `nvservices`.
- What the 0.8.0 run measured (see CHANGELOG 0.8.1): a 3.45 s freeze with 0 ms of GPU wait, 3.3 cores busy, no Eden log
  line and no new pipeline during it; the text textures of the title first sampled 4 and 9 frames after the scene's first
  frame. 0.8.0's text on the geometry-shader rings as the cause of the stall is withdrawn.

## Freezes of the picture (0.8.0; not yet seen on a console)

- `Ps4Hook::LastStall()` (`video_hook.cpp`): in the wrapped `sceVideoOutSubmitFlip`, an interval of `StallGapUs` (1 s) or
  more bumps a counter and records its length, the time and the flip number. Not reset by `TakeTimeline()`.
- `main.cpp` polls it every 8 ms in the game loop. A new stall is logged; if `autoshot` is on, fewer than three freezes
  have been shot and the gap was not made by a pause (`last_pause_us`: the pause menu, or the 0.3 s of a screenshot),
  three timed `take_shot(true)` calls follow (0.4, 2.5 and 6 s). The automatic shot draws no notice.
- How the logs line up: the driver prints `scan-out copy ... over N frames` every 64 frames; the boot log's `stall:` line
  has the flip number. In the 0.7.10 Mario Maker 2 run the freeze ended near flip 2,500 and the driver dumped
  `submit #7627` right after allocating the geometry-shader rings (1 MiB -> 16 MiB, 1.5 MiB -> 24 MiB).

## Themes, icons and covers from the game (0.7.10; not yet seen on a console)

- `Color::BackgroundTop/BackgroundBottom/Panel/Selected/Accent/Teal` are now plain (non-const) values set by
  `ApplyTheme(index)` (`menu_canvas.cpp`); theme 0 restores the original constants exactly, the others are made from
  a hue (HSV). The order of `Themes[]` must match the choices of the `menu_theme` option (a test checks the count).
  `MenuUi::Draw` applies the theme read from the settings file, so the in-game HUD and pause menu, drawn later in the
  same process, keep it. `main.cpp` also applies it when the menu is skipped (nomenu.txt).
- `DrawIcon()` draws the tab icons from rounded rectangles, discs and `Canvas::Line` (a capsule with anti-aliasing).
- `ReadGameIcon()` (`main.cpp`): `FileSys::NSP` / `FileSys::XCI` -> control `NCA` -> `GetRomFS()` -> `ExtractRomFS()` ->
  the first `icon_*.dat` (`icon_AmericanEnglish.dat` preferred). It needs the keys, not the firmware, and no `Core::System`
  (`PatchManager::ParseControlNCA` needs the file-system controller, so it is not used; the base game's control data is
  read, not an update's). The JPEG is decoded by `menu/image_decode.h`.
- `ArchiveLog()` (`ps4_platform.cpp`) runs in `OpenBootLog`, before Eden's logger and the driver start: it moves
  `log/eden_log.txt`, `log/eden_log.txt.old.txt`, `mesa.log`, `mesa.old.log` (when at least 2 KiB) and the old boot log to `logs/`.


## Controllers 2-4 (0.9.1; not yet seen on a console)

- Eden side, from its source: `EmulatedController::LoadVirtualGamepadParams` gives every player slot an input `engine:virtual_gamepad,port:<player
  index>` with fixed button and stick numbers; `VirtualGamepad::SetButtonState(player_index, ...)` writes to that port; and `EmulatedController::SetButton`
  calls `Connect()` when `Settings::values.players[index].connected` is true. `Ps4Eden::ConnectPlayers` (in `frontend/eden_settings.h`) sets that field for
  players 2..N before the core starts. The check is a `requires` expression, so an Eden without `players[i].connected` skips it and `boot.log` says so.
- PS4 side (`ps4_platform.cpp`): `OpenPad` opens the first user's pad as before. `OpenExtraPads(n)` calls `sceUserServiceGetLoginUserIdList` (declared weak) at
  most every 2 seconds, opens a pad (`scePadOpen`, else `scePadGetHandle`) for each other logged-in user until `n` pads are open, and sets the light bar. A
  user who logs out frees the slot (the handle is left open, since this SDK's pad close call was not checked). `ReadPad(index)` reads slot `index`.
- `main.cpp` sends pad `i` to `PollPad(..., i)`. The pause menu, the memory panel and the quit combination read pad 0 only.
- Not checked: that the OpenOrbis stub library exports `sceUserServiceGetLoginUserIdList` (if not, only controller 1 works and the log says so), that a guest
  user's pad can be opened by this process, and how a game reacts to the extra Pro Controllers while Eden is in handheld mode.

## JIT code cache and the build's report on the JIT pack (0.9.4 fix)

- **Cache size.** Eden patch 0031 makes `config.code_cache_size` in `arm_dynarmic_64.cpp` / `_32.cpp` come from `ps4_jit_cache_mib` (0 = 32 MiB, clamped to 16..256), which `frontend/main.cpp` sets from
  `jit_cache_mib` before the core starts; the PC's value is 512 MiB. Dynarmic's A64 interface (`a64_interface.cpp`) counts the two ways a whole flush happens: `GetBlock` finding less than 1 MiB left
  (`ps4_jit_cache_full`) and `Jit::ClearCache()` (`ps4_jit_cache_asked`, the guest's `IC IALLU`). Range invalidations (`IC IVAU`, code mapped or protected by the kernel) do not flush the whole cache and are not counted.
  The symbols are defined weak in `frontend/jit_cache.h`, like the pack's.
- **Report.** `scripts/apply-jit-pack.sh` starts `jit-pack-report.txt` (`NXO_JIT_REPORT` overrides the path), `ci/pack-fallback.sh` adds what it took out and the first error lines, `package-eden.sh` copies it to
  `/app0/assets/misc/`, and `main.cpp` logs it (24 lines at most, 200 characters each) before the JIT lines. A build outside the CI has "no report".
- **Reading the 0.9.4 boot.log** (Mario vs. Donkey Kong, Nov 13): the `profile` lines count a sample as "jit" when the instruction pointer is outside the eboot image and the kernel library, so the rest of a busy thread is the
  emulator itself (compiling blocks, memory callbacks, the emulated kernel). On the three busy `CPUCore_` threads the JIT share was 28-50% of 56-79% busy; the GPU thread 4-11% busy. The stalls (1.0-7.3 s) came with one core at 91% busy.

### 0.9.5, third zip: the extra RAM, the public build

- `sysflex_mib` left the Mandatory tier (`TierOf`), so it is listed in the CPU tab and by `PerGameOffered`; the rest of the memory plumbing (`memory_action`, the arena and VRAM env lines) stays hidden.
- Public build (`EDEN_PS4_PUBLIC`): `Menu::PublicBuild` hides the LOG tab and ignores `show_advanced` (`MenuUi` builds its tab list with `everything = !PublicBuild && ...`); `kPublicBuild` in `main.cpp` turns
  off the screenshot combo, `g_autoshot` and the profiler (`Ps4::SetProfiling(false)` before settings.txt is read). `frontend.cmake` adds `-public` to the build id. `ci/stage-eden.sh` passes `-DEDEN_PS4_PUBLIC=ON`
  when `PUBLIC_BUILD` exists in the repository root.

### 0.9.5, second zip: a game's own settings, `.nro` assets

- **Files** (`frontend/menu/game_config.h`, header only): `KeyFor(game file name)` is the title id (`Covers::TitleIdOf`) or the sanitised stem; `PathFor`, `Load`, `Save` (temporary file then rename, then a
  read back; a text with no setting removes the file) and `Present(dir)` (the keys of the files, to mark games). `per-game/` is made with the other data folders at start.
- **Applying**: `ApplySettingsFile(game)` in `main.cpp` runs `ApplySettingsLines` on `settings.txt` and then on the game's file, with `per_game = true` for the second (it skips `Menu::GlobalOnly` keys). The
  memory plan is read before any game is known (`ApplyMemoryPlan`), which is why `memory_profile` is global only.
- **Menu**: `MenuUi::game_cfg_open_` is a screen over the games tab, not a tab. `game_opts_` is the options of the tabs in order (controls, video, CPU, HUD, system, TESTS) filtered by `PerGameOffered`. A
  `SettingsFile` of the game (`game_file_`) holds the text; `OverrideIndex` is -1 when a line is absent (shown as `global (value)`), `SetOverride` writes a value even if it equals the default. The text is
  loaded and saved through `SystemInfo::load_game_config` / `save_game_config`, filled in by `CollectSystemInfo`; the tests plug in a map. `Pad::R2` / `Pad::L2` are the PS4's 0x200 / 0x100.
- **`.nro`** (`frontend/menu/nro_assets.h`, header only): the u32 at 0x18 of an NRO is the size of the program; the `ASET` header follows (icon offset/size, NACP offset/size, RomFS offset/size, offsets from the
  start of the header). `Read` checks every offset and size against the file size before reading, takes the first NACP title with a name (UTF-8 checked, control characters turned to spaces) and the JPEG
  (at most 4 MiB, must start `FF D8`). `main.cpp` reads it for every `.nro` of `roms/` before the covers, so the title shows even when a picture in `covers/` wins.

### 0.9.7: the code caches in 2 MiB steps, and the texture counters (never run on a console)

- **Why.** The Odyssey run of 0.9.5 (`boot_20261117-170506.log`): the free direct block fell about 1200 MiB a minute until 44 MiB, then the game closed; the four code caches had taken 1 GiB at the start (`jit_cache_mib` x 4, whole).
  Dynarmic's x64 backend already asks for memory by step: `BlockOfCode::EnsureMemoryCommitted(codesize)` is called before every block is compiled (`a64_interface.cpp`), after the check that flushes the cache when less than 1 MiB is left.
  On Windows it commits pages of a reserved range; patch 0034 gives the Orbis build the same behaviour.
- **Patch 0034** (`block_of_code.cpp/.h`). `OrbisCodeAllocator::alloc` reserves the address range (`sceKernelReserveVirtualRange`, 2 MiB aligned) and takes the first two 2 MiB steps at once; if any kernel call of that fails it falls back to the whole
  block, taken the old way (2 MiB or 16 KiB aligned). A step is `sceKernelAllocateDirectMemory(Large, Large)` (alignment `Granule` if that is refused), `sceKernelMapDirectMemory(MapFixed)`, `sceKernelMprotect(rwx)`.
  `BlockOfCode::EnsureMemoryCommitted` raises `committed_size` to the wanted size, rounded up by the allocator. Once the prelude is complete (`orbis_ahead`) it works a little ahead of the code: a step is asked for while
  `codesize + 1 MiB` of room is left and the cache is taken to `codesize + 2 MiB` past the code, because Dynarmic checks `SpaceRemaining()` (and flushes a full cache) *before* it calls `EnsureMemoryCommitted` and then writes a block into what it
  finds, so a step the kernel refuses must still leave that block its room. On a refusal `orbis_limit` is set to what is held and `SpaceRemaining()` honours it, so the next compile flushes the cache when less than 1 MiB is left and
  nothing is written past what exists. The members `committed_size` and `orbis_limit` are declared **before** `constant_pool` in `block_of_code.h`, because the constant pool's constructor calls `EnsureMemoryCommitted`. The prelude is committed
  1 MiB at a time (it was 16 MiB). The counters (`ps4_jit_chunks_large/small`, `ps4_jit_commit_failed`, `ps4_jit_reserved_bytes`, `ps4_jit_committed_bytes`) are weak definitions in `frontend/jit_cache.h`; `ps4_jit_cache_lazy` says which way
  the allocator should go (0 = whole, as until 0.9.6).
- **Frontend.** `jit_cache_commit=lazy|full` (`ParseCommit`), `LazyForStart(requested, safe_mode)` (false in safe mode), `ForStart(requested, roomy, safe_mode, lazy)` (`auto` = 256 MiB roomy / 128 with lazy steps, 64 / 32 without),
  `DescribeCommit` for the `JIT cache memory:` line, `Ps4Memory::BuildArenaTable` (`jit_lazy`: the caches are 4 x 4 MiB at the start and the note says they grow). The option is EXPERIMENTAL in `safe_mode.h` (`value != "full"`).
- **Patch 0035** (`maxwell_dma.cpp`, `texture_cache.h`; counters only). `TextureCache::RefreshContents` counts uploads, bytes and the largest; the three CPU paths of the Maxwell DMA engine that the GPU path declined (`CopyPitchToBlockLinear`,
  `CopyBlockLinearToPitch`, `CopyBlockLinearToBlockLinear`) count copies and bytes. `frontend/texture_traffic.h` defines them (weak), prints the difference every ten seconds when it is not zero and the totals at the end. The asynchronous decode path is not counted.
- **What was checked, and what was not.** On a PC: patches 0034 and 0035 apply in order; the files they change compile with `-fsyntax-only` (0034 with `-D__ORBIS__`, 0035 for `maxwell_dma.cpp`; `texture_cache.h` could not be compiled on a PC because
  the Vulkan headers are not available there, its change is a block of plain counter increments); the frontend's logic is tested. **Not checked:** that the kernel accepts a direct-memory step of 2 MiB aligned to 2 MiB (the 16 KiB
  fallback exists for that), that mapping over the reserved range with `MapFixed` keeps the executable flag, that Dynarmic's flush-when-full works when `orbis_limit` is hit, and the whole of it on a console.

### 0.9.7 fix: the arena's placement and the heap's instruments

- **The failure.** `mesa.log` of the Odyssey run: `arena 1536 MiB, RADV's window 0x3d7c04000..0x435c00000`, then `the arena straddles a 4 GiB boundary - only the part in window 0x3 can serve RADEON_FLAG_32BIT allocations` and, at 160 s,
  `out of 32-bit VA for 16384 bytes`. In `ac_orbis_drm.c` `address32_hi = orbis_va_base >> 32` (RADV's window start, the arena plus the private slice of `0x2004000` bytes: 16 KiB of labels and a ring of fence command buffers), and a request for a 32-bit
  VA range is limited to `min(end of that 4 GiB window, orbis_va_end)`. So the usable part is `Usable32(base, len)` in `frontend/arena_place.h`: here `0x400000000 - 0x3d7c04000` = 643 MiB.
- **The placement** (`arena_place.cpp`). `-Wl,--wrap=sceKernelMapDirectMemory`; `__wrap_sceKernelMapDirectMemory` acts once per start, when armed by `main()` just before `system.Load` (the driver makes its arena inside the Vulkan device creation) and when
  `IsArenaRequest(ORBIS_ARENA_MIB, len, flags, *addr == nullptr)` holds. It calls `Search`, which reserves `len` bytes of address space with `sceKernelReserveVirtualRange(&at = nullptr, len, 0, align)` until the range is `Whole` (RADV's window inside
  one 4 GiB window) or six tries are used; a straddling range stays reserved so that the kernel's first fit offers a later hole next time. The good range is released with `sceKernelMunmap` right before `__real_sceKernelMapDirectMemory` is called with the driver's
  own arguments, and the fillers right after (or before, when nothing better was found, so the call is then the plain one). `Describe` makes the `arena placement:` line. The decision is pure and tested against a model of a first-fit
  address space (`tests/memory_guard_test.cpp`). `arena_align=off`, safe mode, a refused reservation or no whole range in six tries leave the call as the driver made it.
- **Assumption, not verified:** that the search of `sceKernelReserveVirtualRange` with no address (which `ps4_platform.cpp` and Eden patch 0002 already use for the guest address space and the code caches) is the one `sceKernelMapDirectMemory` uses with no
  address; the carve-outs of the C heap land at increasing addresses right after the arena in the logs (`0x435c00000`, `0x43dc00000`, ...), which is what a shared first fit does. If it is not, the line says `STILL across a boundary` and nothing is worse than before.
- **The heap.** `heap_census.cpp`: the threshold of the census is 256 KiB (`Threshold`), the table 16384 entries; `Ps4HeapCensus()` ends with `orbis::mmapDirectReport("heap census")` (weak). `mmap_watch.cpp` (its own file: the census's host test does not link it): `-Wl,--wrap=__mmap` and `__wrap___mmap`: requests of 16 MiB or
  more are recorded in a ring of 32 (`g_big`, size and four return addresses from the frame pointers) with no log call inside (the call may be inside malloc, holding its lock), and `Ps4HeapBigMaps()` prints the last 12. `OrbisFatal` (`ps4_platform.cpp`)
  calls both once. What the Mario Maker 2 log gives: four carve-outs of 128, 128, 256 and 256 MiB in 0.6 s, a refused 512 MiB, the cap `DirectCap` = 12 x 128 MiB of `orbis_mmap.cpp` (compile-time, inside the prebuilt `liborbis-compat.a`), the flexible
  pool with 165 MiB left, direct memory with 38 MiB left.

### 0.9.6: thread placement, the memory table, the CPU line

- **Placement** (`frontend/thread_placement.h`, pure; `ps4_platform.cpp`: `ConfigureThreadPlacement`, `PlaceCurrentThread`). `main()` calls `ConfigureThreadPlacement(cpu_affinity && !safe_mode)` before Eden starts a thread: it finds
  `scePthreadSetaffinity`, `scePthreadGetaffinity`, `scePthreadSelf` and `sceKernelGetCurrentCpu` in libkernel by name (`sceKernelDlsym(0x2001, ...)`, never declared: the SDK may declare them with other types), reads the main
  thread's mask (a 64-bit buffer, so a 32-bit mask works too) and builds a `Plan`. Every thread Eden names reaches `EdenPs4OnThreadNamed` (patch 0006), which registers it for the hang report and then calls
  `PlaceCurrentThread(name)` from that thread: `CPUCore_N` -> core N of the plan alone, any other name -> the secondary mask, `main` and "" -> untouched. The plan takes the six lowest allowed cores, needs five, gives the first
  four to the guest. Nothing is pinned when a function is missing, the mask cannot be read, or fewer than five cores are allowed. A thread created by a pinned thread and never named would inherit its core: Eden names every
  thread it starts (`Common::SetCurrentThreadName`), which is why the placement is done at the naming.
- **Memory table** (`Ps4Memory::BuildArenaTable`/`DescribeArenaTable`, `memory_guard.cpp`): the slices taken at the start (GPU arena from `ORBIS_ARENA_MIB` as it stands after settings, JIT caches = 4 x the cache actually
  used, the stream buffer) and the rest; `Ps4::SystemFlexibleBudgetMiB()` is what the pool probe settled on. Logged once after the JIT cache line.
- **`Ps4::LogCpuFeatures()`**: CPUID leaf 1 and 7 and `xgetbv` (only if OSXSAVE), one line.
- All of it is untried on a console. The pure parts are tested in `tests/memory_guard_test.cpp`; the calls to the kernel are compiled only by the CI.

### 0.9.5 zip 5: the HUD, the pause panel and the menu's sounds

- **HUD and panel** (`frontend/video_hook.cpp`: `BuildHud`, `BuildPanel`): still one opaque bitmap per element, kept alive in every buffer by the same probes; only the pixels changed. `RefreshStats` keeps a history of the last 60 one-second
  rates (`fps_history`) and the worst frame gap of the second (`worst_ms`, from the flip handler); `TargetRate` is 60 when the best second seen is above 40, 30 above 20, else 20; `RateColour` is green from 92% of the target, yellow from 60%, else red. The panel's rows are `label<TAB>value` strings; the values `PanelValueOn` / `PanelValueOff` are drawn as pills. `Settings::portuguese` (from `MenuInPortuguese()`) chooses the language. `SetHudStatsForTest`
  and `tests/video_hook_test.cpp --render DIR` draw both into a fake buffer.
- **Menu sounds** (`frontend/menu/menu_sound.h`, header only): `MakeEffect(SoundId)` and `MakeAmbient(seconds)` return 48 kHz S16 stereo samples made from sine recurrences (a bell is a sine with a quieter octave and a faster
  fading partial; a pad is three sines with the right channel 0.24% sharp for a slow chorus); `Mix` is float and `wrap`s notes past the end of the loop and the echo is circular, so the loop has no seam (the test compares the
  seam with the biggest step anywhere). `Mixer` holds the loop, up to 8 voices and atomics (music, effects, volume, fade); `Render` clips and never wraps; the music fades in and out over 0.6 s; `FadeOut` is a master gain to 0.
- **Events**: `MenuUi::Update` calls `Sound(id)` at the buttons (a direction: Move; X: Select, or Play when it ended the menu; Circle out of a tab or a game's settings: Back; L1/R1: Tab; R2 opening a game's settings: Select;
  OPTIONS: Play) and `TakeSounds()` hands them over once, at most 4 per step and never two Play. `Audio()` and `Volume()` read `menu_audio` / `menu_volume` from the settings text.
- **Console side** (`frontend/menu/rom_menu.cpp`, `MenuAudio`): `sceAudioOutOpen(0xFF, 0, 0, 256, 48000, 1)` and a thread that calls `Mixer::Render` and `sceAudioOutOutput` (the same calls as patch 0002's sink, whose use is the tested one).
  `libSceAudioOut` is already loaded by `LoadSystemModules` and linked (`frontend.cmake`). State shared by `shared_ptr`, threads detached, `Finish()` = chime 0.7 s + fade 0.3 s + `Close()` (stop flag, wait up to 1.5 s).
  **This class is only syntax-checked on a PC** (`g++ -fsyntax-only` with the real header): it is the part of the zip nothing here could run.

### 0.9.5: tiers for options, the CONTROLS tab, the cached background

- **Tiers** (`frontend/menu/settings_model.h`, `Tier`, `TierOf(key)`): a table of keys, not a field of `Option`, so the catalog's initialisers did not change. Mandatory options are not in any tab unless
  `show_advanced=on`; experimental ones are in the TESTS tab (`TabAdvanced`) of a test build; a build with `EDEN_PS4_PUBLIC=1` has neither that tab nor the LOG tab. `main.cpp` does not look at the tier: a
  line written by hand always works. What changed in behaviour is only the defaults of the two JIT switches (on), and safe mode switching them off itself (it only ignored lines that said `on`).
- **Tab numbers are the order shown** (`TabGames` 0, `TabControls` 1, `TabVideo`, `TabCpu`, `TabPerf` (HUD), `TabSystem`, `TabLog`, `TabAdvanced`); `MenuUi::Tabs()` is what L1/R1 walk.
- **Controller models**: `player1_type`..`player4_type` (`auto` writes nothing). `Ps4Eden::ApplyPlayerType` names `Settings::ControllerType` members with `requires`, so an Eden without one logs and keeps its default.
  The menu loop opens the extra pads and reads `ReadPad(i).connected` only while the CONTROLS tab is up.
- **Background**: `MenuUi::DrawBackground` renders the gradient, two `Canvas::Glow` lights and three rings once per theme into `background_` (about 8 MiB) and `memcpy`s it each draw, so the new look costs a copy, not
  a few million square roots, per redraw.

### 0.9.4 fix 2: what the test log showed, safe mode, the cache's pages

- **Reading the 0.9.4 fix boot.log** (`boot.old.log`, Nov 13 evening). Part 2's report line was `TypeError: Path.read_text() got an unexpected keyword argument 'newline'` (3.13-only; the runner has 3.12).
  The same start was in safe mode (`safe mode: the last start ended before the game drew anything`), so `jit_page_reuse`, `jit_direct_table`, `jit_cache_mib=256`, `cpu_ticks`, `cpu_clock` and `astc_recompress`
  were ignored and the 32 MiB cache was used: `thrown away whole 1 time(s)` at the first status, 2 at ~110 s, 3 at ~120 s, 4 at ~140 s, 6 at ~150 s. The freezes (5.75, 2.4, 1.4 s) and 6-12 FPS stretches sit in
  that window (a level load: new code on three cores, each with its own cache, so the same blocks are compiled up to three times). After the last flush the level ran at ~20 FPS (flips alternating 34 and 67 ms)
  with `CPUCore_0..2` 42-79% busy and 54-84% of that in JIT code; the `mesa_*.log` budget lines show the GPU never waited (`waited 0 ms for the GPU ... 0%`).
- **Safe mode** (`frontend/safe_mode.h`): `safe_start.txt` = game name, then the number of starts in a row that did not get to draw. `PreviousStartDied(path, game)` is true for the same game or a count of 2 or more.
  A file without the second line (older zips) counts as one death.
- **Patch 0032**: `OrbisCodeAllocator::alloc` (patch 0005's allocator) tries `sceKernelAllocateDirectMemory` / `sceKernelMapDirectMemory` with alignment and size rounded to 2 MiB, then the old 16 KiB way; it counts
  each outcome in `ps4_jit_cache_large` / `ps4_jit_cache_small`. `a64_interface.cpp` counts `ps4_jit_blocks_compiled` (in `Impl::Compile`, once per block) and `ps4_jit_ranges_invalidated` (in
  `Impl::InvalidateCacheRange`, each of which halts the core that receives it). All four are defined weak in `frontend/jit_cache.h`. Compile counts are relaxed atomics shared by the cores: one add per compiled block
  (a block costs tens of microseconds to compile), nothing on the execution path.

- **Patch 0033** (`texture_cache.h`): `VisitImageView` re-checks a slot whose cached view is `NULL_IMAGE_VIEW_ID` or belongs to an image flagged `Remapped` (`IsValidEntry`, then `CreateImageView`, which replaces a Remapped
  image) and advances `texture_bindings_serial` when it revives one, because `CheckFeedbackLoop` caches its barrier decision by that serial. `UnmapGPUMemory` marks the framebuffer dirty (`Dirty::RenderTargets`, the
  zeta/colour entry) when the remapped image is bound as a render target, since those do not pass through the TIC lookup. A valid sampled view keeps its fast path. Adapted from ProsperoEden-Encore's backports.
- **Controller model fit** (`Ps4Eden::FitControllerStyle`, `frontend/eden_settings.h`): `HIDCore::GetSupportedStyleTag()` is what the game accepted; `EmulatedController::IsControllerSupported` decides whether Eden connects
  a controller. Eden's own fallback (`SetSupportedNpadStyleTag`) knows Pro <-> Dual only, so any other list left the controller disconnected. main.cpp calls the function every 250 ms for the players in use.