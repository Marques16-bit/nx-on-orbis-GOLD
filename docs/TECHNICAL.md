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
  share and hottest code buckets every 30 s); heap census of allocations of 1 MiB or more.
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

