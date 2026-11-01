# Changelog

Format: newest first. Versions follow docs/ROADMAP.md. Release notes on GitHub are taken from here.

## [0.7.7-fix] - "Exit game" and "Restart game" that start the app again, the HUD written at every flip, no skipped draws

**Not built or run on a console by the author of this change.** The host tests pass (every file under
`tests/`, with new cases in `video_hook_test`), and the new `ps4_platform.cpp` code was syntax-checked
against stand-ins for the PS4 library; `main.cpp` is checked only by the GitHub build.

What the 0.7.7 run on the console showed (boot.log, boot.old.log, eden_log, mesa.log):
- `sceKernelDlsym` found `sceSystemServiceLoadExec` (address `0x8007e8ea0`, result 0) and the call was made,
  but **never returned**: the system ended the app and started nothing. Both "Exit game" (boot.old.log) and
  "Restart game" (boot.log) ended that way, which is why they "only close the emulator".
- The new shader cache worked as designed (`41 pipelines loaded in 0.2 s`, preload `4096`).
- `hardware_concurrency was asked 1 times`: the `--wrap` link is in effect.

### Fixed (hypothesis, not proven): "Exit game" and "Restart game" only closed the app
- The call used the path `"EBOOT.BIN"` with a NULL `argv`. Titles that start themselves again name their
  executable `"/app0/eboot.bin"` (the form shadPS4 and AnyPS5 document for relaunching a title; neither
  was seen on a PS4 here), so the first way is now `sceSystemServiceLoadExec("/app0/eboot.bin", {NULL})`.
- Because a way that ends the app without bringing it back cannot report its own failure, there is now a
  ladder of four ways (`/app0/eboot.bin` with an empty argv list, `EBOOT.BIN` with one, `/app0/eboot.bin`
  with a NULL argv, `eboot.bin`). Each attempt is written to `restart_state.txt` (and synced) **before** the
  call. The next start judges it: back within 12 s means the app came back by itself and that way is
  remembered and used first; a start much later (opened by hand) marks the way as not working and the next
  restart uses the next one. In the worst case that costs one manual reopen per way that does not work
  (at most three), then it works for good. `boot.log` says all of it with `restart:` lines, and also lists
  what `/app0` holds and whether `/app0/eboot.bin` exists.
- "Exit game" leaves no `restart_game.txt`, so the new process opens on the game list. "Restart game"
  leaves it, so the new process opens the same game (and, if the app has to be opened by hand, it opens that
  game directly).
- **Not known:** whether any of the four ways works on firmware 12.02. If none does, the panel says the app
  is closing and it closes; reopening it by hand gives the game list (or the same game after "Restart game").

### Fixed (hypothesis, not proven): the FPS HUD still blinking
- The 0.7.7 counters show the HUD was present at every flip (`copied: 0 at the flip`) and was repaired once per
  frame after the game painted over it, so a frame going out without it is not what the log shows. Two
  things the probes cannot see are covered instead:
  1. The nine probe pixels cannot see a thin band painted over part of the HUD, so the HUD and the notice are
     now written again **at every flip** whatever the probes say (the game's GPU is done with the frame by
     then), and again every 4 ms for the 40 ms after it, while that buffer is the one on screen.
  2. The buffers may be ordinary cached memory (the log has `ORBIS_VRAM_GARLIC=0`, which puts every image on
     the cache-coherent bus). A write that stays in the CPU's cache is not on the screen's memory until the
     cache evicts it, which would show as a HUD that comes and goes. Every row written is now flushed
     line by line (`clflush`), harmless on write-combined memory.
- New counters in the `video hook:` line: `flips stamped`, `overlays found missing right after a flip` (the
  buffer on screen lost the HUD after the flip: if this is non-zero, something paints over shown buffers) and
  `rewrites while shown`.
- **Not known:** the cause. If it still blinks, send the `video hook:` line and say whether
  `ORBIS_VRAM_GARLIC=0` was set: the driver itself warns that this setting "cost this title two thirds of its
  frame rate" and is for comparison runs only.

### Changed: asynchronous shaders are off by default (Super Mario Maker 2's themes)
- In the Eden source sent for comparison (`vk_pipeline_cache.cpp`, `PipelineCache::BuiltPipeline`): with asynchronous
  shaders on, a draw whose pipeline is not built yet returns no pipeline, and `RasterizerVulkan::PrepareDraw`
  then **skips the draw** (`if (!pipeline) return;`). Draws with a depth buffer, or with more than 6 indices,
  are the ones skipped. Eden's own default is off; this port turned it on.
- The frames of the video (castle and ground as flat silhouettes, a pale object that changes from frame to
  frame, while `shaders built` goes from 41 to 148 in the log) fit layers that are not drawn while their shaders
  compile. That is consistent, **not proven**: no run with it off has been seen. Off means a first run stalls
  while shaders compile (the disk cache, preloaded at start, removes most of that from later runs).
  `async_shaders=on` in settings.txt (or the "Light" profile) brings the old behaviour back for a comparison.
- Note: the Eden zip sent is a newer master (`943f1fc7`), not the pinned `5f142c79` that the build uses; this
  code path has long been the same in yuzu-based emulators, but it was read in the newer one.

### Still not fixed
- RAM use and anything that needs a change inside Eden: unchanged from 0.7.7.

## [0.7.7] - a shader cache with no limit that survives a bad start, "Restart game", a way out that works

**Not built or run on a console by the author of this change.** The host tests pass (every file under
`tests/`), and the new `ps4_platform.cpp` pieces (restart, quit, thread cap) were syntax-checked against
stand-ins for the PS4 library; `main.cpp` is checked only by the GitHub build. Nothing below has been
seen on the console. The "How to read the next boot.log" list at the end says what to look for.

What the 0.7.6 run on the console (video, boot.log, boot.old.log, eden_log, mesa.log) showed: the HUD and
the L3 + R3 menu **no longer flicker** (confirmed by the player on the video), which closes the 0.7.6
change. The same run brought four problems; this version works on those.

### Fixed: leaving the game from the L3 + R3 menu ended on a panel that sent the player to boot.log
- boot.log said `restart: this SDK's system service library has no sceSystemServiceLoadExec`: the SDK's
  stub library does not export it, so "Exit game" could not start the app again and stayed on the panel
  "Could not go back to the game list / boot.log: restart".
- Now `RestartApp()` looks the function up **by name inside the console's own libSceSystemService**
  (`sceKernelLoadStartModule` + `sceKernelDlsym`, both declared weak so the link cannot break on them).
  Every step is logged with a `restart:` prefix.
- If the system still refuses, the app says so on the panel ("The app is closing ...") and **ends itself**
  (`_exit(0)`) after two and a half seconds, instead of staying open on an idle screen. The player opens it
  again from the home screen.
- **Not known:** whether `sceKernelDlsym` finds the function on firmware 12.02. The `restart:` lines in
  boot.log will say; if both ways fail, "Exit game" now closes the app, which is the best the code can do.

### Added: "Restart game" in the L3 + R3 menu
- A new row between the shader notice and "Exit game", also in the memory alert panel (where it is the way
  to give the memory back). It writes `restart_game.txt` (the game's path) and starts the app again; the
  new process reads the file first thing, deletes it **before** using it (a start that dies cannot loop) and
  opens that game without the game list. If the app cannot start itself again the file stays, and the game
  starts the next time the app is opened (the panel says so).

### Changed: the shader cache has no limit and never loses what it saved
- Why the cache seemed to save nothing and games stopped opening: 0.7.6 limited the preload to ~8 MiB and
  moved a bigger cache aside, and a start that died while the cache loaded moved the whole cache to
  `.bad` (boot.old.log: 384 of 795 pipelines built, then a write to a null address in a builder thread,
  with the driver arena at 1024 MiB).
- **`shader_preload` now defaults to `all`** (no limit), as in Cemu: every shader the game compiles stays on
  disk and is loaded at the next start. A `4096` choice (~128 MiB) joins the menu; a number is a size of
  `N x 32 KiB`, only a way to save memory.
- **Nothing is deleted.** A cache over a chosen limit is kept as `vulkan.bin.bak`, then `.bak2`, `.bak3`...
  (no earlier backup is replaced), and a new cache starts. The `<title id>.bad` folders made by 0.7.6 are
  not touched: renaming one back to `<title id>` gives that game its old cache back.
- **A death while the cache loads is recovered in steps, without losing the cache:** while Eden builds the
  pipelines of `vulkan.bin` the app keeps `cache/shader/loading.marker` (`attempt N <title id>`), removed
  when the load ends. If the next start finds it: (1) the same cache is loaded with **one** builder thread
  (a race between the builder threads is the leading suspect; Eden sizes the pool from
  `std::thread::hardware_concurrency()`, which `frontend.cmake` now routes through a wrapper with
  `--wrap`; the cap is lifted when the load ends); (2) if that start dies too, `vulkan.bin` is parked as
  `.bak` and the game starts with an empty cache. A marker left by another game gives step 1 only.
  0.7.6's per-game `<title id>.loading` files are read as an attempt-1 marker.
- **Not known:** the real cause of the crash in the builder threads (arena exhaustion and a race are both
  possible). With no limit a very large cache still needs the arena to hold every pipeline at once; the
  ladder keeps the app from dying in a loop, it does not make a huge cache fit. boot.log prints
  `X of Y pipelines built (free direct memory ...)` every 64 pipelines and the load time, so the next log
  shows whether memory or a race is the cause.

### Changed: memory classes (`memory_guard`)
  | class | driver arena | shader preload | driver pipeline cache |
  |---|---|---|---|
  | roomy (4300 MiB or more, the Pro measured) | 1280 | all | on |
  | standard (3800 to 4299) | 1280 | 4096 (~128 MiB) | on |
  | tight (under 3800) | 1152 | 1024 (~32 MiB) | off |
- The tight arena was 1024 MiB: the one run that died loading Super Mario Maker 2's shaders had that
  value, so it is now 1152. The menu's arena help text says to avoid 1024. The "Save RAM" profile follows
  (1152, cache up to ~32 MiB). The "More video memory" profile no longer touches the shader cache.
- The class limits for other consoles are still **guesses** (nobody has measured a Fat or a Slim).

### Added: the profile "Exact graphics (test)" (VIDEO tab, first row)
- High GPU accuracy, reactive flushing, safe DMA, synchronised memory, synchronous shaders and
  `ORBIS_VRAM_GARLIC` back to its default (images outside the driver's arena). Slower. It exists to
  compare against the other profiles on the Super Mario Maker 2 themes below, not as a fix.

### Not fixed: Super Mario Maker 2's themes (8-bit, Mario World, Mario 3, New Super Mario Bros. U)
- In the video the sprites alternate between sharp and flat/blurred (a castle as a flat dark-blue
  silhouette). That looks like a texture sampled at a very low mip level or a render target read before it
  was complete, **but the cause was not found**: no Eden source is available here and the logs do not
  contain the failure. Things the logs show that could be involved: images kept inside the arena
  (`ORBIS_VRAM_GARLIC=0` makes every image live in the driver's arena, and the driver warns it costs about
  2/3 of the GPU speed), asynchronous shaders, and the stream-buffer synchronisation. Try the new profile,
  then remove `env=ORBIS_VRAM_GARLIC=0` from settings.txt if it is there, and say which one changes it.

### Not fixed: the amount of RAM
- The 0.7.6 boot.log at Super Mario Maker 2's title screen adds up to: ~1.29 GiB of guest RAM (what the
  game really uses), the 1.28 GiB driver arena, ~0.9 GiB held by the C heap (the census sees ~475 MiB in
  large allocations), and the JIT and the rest; the largest free block stayed at 764 MiB. So most of it is
  accounted for, not wasted by the frontend. Real savings need changes inside Eden (stream buffer from
  256 to 128 MiB, texture and buffer cache budgets), which cannot be written without the pinned Eden
  source: roadmap 0.8.0. The build's `eden-ps4` ELF would show which allocations are large; it is not in
  this repository's zip.

### How to read the next boot.log
- `restart:` lines (what the console said to the lookup of `sceSystemServiceLoadExec`).
- `shader cache: hardware_concurrency was asked N times, M answers cut by the cap`: N = 0 means the
  `--wrap` did not take effect and the one-thread step does nothing.
- `shader cache: ... mode safe` / `mode empty` after a death, `... pipelines loaded in X s` after a good load.
- `restart_game.txt: starting ... again` on the start after "Restart game".

## [0.7.6] - flicker of the HUD and the pause menu, memory profiles and a memory watch

**Not built or run on a console by the author of this change.** The host tests pass (every file under
`tests/`, including the new `memory_guard_test`), and the new `main.cpp` functions were compiled
against stand-ins for Eden and the PS4 library; the real `main.cpp` is checked only by the GitHub
build, and nothing below has been seen on the console. Read the boot.log lines in "How to read the
next boot.log" to find out whether it worked.

Note on versions: the zip received as "0.7.5" carried the 0.7.4 sources (version string, CHANGELOG and
files). There is no 0.7.5 in it, so this is built on 0.7.4 and goes straight to 0.7.6.

### Fixed (hypothesis, not proven): the HUD and the L3 + R3 menu flickering (seen in Super Mario Maker 2)
- Cause as read from the code: the game's GPU rewrites a whole video-out buffer for every frame, and
  the overlay was written into a buffer only once or twice around the flip. When the GPU finished
  after that, the frame was shown without the overlay for one screen refresh, and the next frame (a
  different buffer) did it again: a flicker at the game's frame rate. It is not because the HUD is
  2D; every overlay element (HUD, shader/memory notice, pause panel) had the same problem.
- Now the overlay is built once as a bitmap and **kept alive in every video-out buffer**: the worker
  thread checks a few probe pixels of each buffer (one read each, the buffers are slow to read) and
  copies the bitmap again the moment the game has painted over it; the frame is also stamped inside
  the wrapped flip call, so a frame whose GPU work is already done never goes out without it.
  Elements are copied row by row in their final pixels (no "clear, then draw" step a refresh could catch
  half way), and are redrawn when a buffer is registered again.
- The 0.7.3 drawing stays as `overlay_mode=classic`.
- Host test: a model of a display that takes frames from the buffers while a "GPU" repaints them.
  Frames shown without the overlay, classic against keep-alive: 4.6% to 0.8%, 23.5% to 2.2%, 58.8% to
  1.8% (three load levels). That is the model, not the console.

### Added: memory profiles (`frontend/memory_guard.{h,cpp}`)
- At start the app reads how much direct memory the console gives it (`sceKernelGetDirectMemorySize`)
  and picks the memory defaults from it, **before** `settings.txt` is applied (a line there still
  wins):

  | class | direct memory | driver arena | shader preload | driver pipeline cache |
  |---|---|---|---|---|
  | roomy | 4300 MiB or more (the PS4 Pro measured: 4608) | 1280 | 256 | on |
  | standard | 3800 to 4299 | 1152 | 128 | on |
  | tight | under 3800 | 1024 | 128 | off |

  On a console with as much memory as the Pro that was measured nothing changes from 0.7.4. The
  class limits for the other models are **guesses**: nobody has measured a Fat or a Slim yet. The boot.log
  line `memory: N MiB of direct memory ...` shows what the console reported; send it and the table
  can be fixed (0.10.0 in the roadmap). `memory_profile` overrides the guess meanwhile.
- `memory_profile=auto|roomy|standard|tight|strict`. `strict` is `tight` plus `ORBIS_VRAM_GARLIC=0`
  (every GPU image inside the driver's arena: a hard cap, experimental, never tried).
- The driver arena menu gets a 1152 MiB choice (default still 1280).
- No `sceKernelIsNeoMode` or any other new SDK call is used to tell the models apart: a symbol the
  SDK stub does not export would break the link, and the build cannot be tried here.

### Added: the memory watch
- Once a second the app samples the largest free block of direct memory (what actually runs out: the
  C heap takes 128 MiB at a time) and keeps its trend. Stages: ok, **low** under 160 MiB, **critical**
  under 64 MiB, with a 32 MiB hysteresis so a block hovering at a limit does not flap.
- The corner notice shows `MEMORIA BAIXA: 120 MiB livres, caindo 20 MiB/min (~90 s)` (the speed and
  the time to the critical level are shown only while the block is really falling), or `MEMORIA
  CRITICA`. A race at ~100 MiB that is not falling shows the plain low-memory line and no panic.
- `memory_action=warn|pause` (default warn). With `pause`, reaching the critical level pauses the game
  and opens a panel: continue anyway, or leave to the game list, instead of the app dying with the game
  on screen. It needs the video overlay.
- The `status:` line in boot.log gains the stage and the trend in MiB/min.
- **What this does not do:** it does not free memory that Eden holds, so it does not stop a game
  that really needs more than the console has. Reclaiming memory (Eden's 256 MiB stream buffer to
  128 MiB, texture and buffer-cache limits, GARLIC images) means changes inside Eden written against the
  pinned source, which is not in this repository: 0.8.0 in the roadmap. This version warns early,
  can pause, and measures, so the next log says where the memory goes.

### Added: menu rows and `settings.txt` keys
- PERFORMANCE tab: `overlay_mode=keepalive|classic`, `overlay_poll=1|2|4|8` (ms between checks, default 1),
  `overlay_flip_stamp=on|off` (default on). `overlay_delay` now says it is for the classic mode.
- CPU & MEMORY tab: `memory_profile`, `memory_action`.
- The "Save RAM" preset also sets `memory_profile=tight` and `memory_action=pause`.
- SYSTEM tab: "Direct memory" shows free of total and the class; "Memory plan" shows what the
  profile set (the data folder and logs rows were merged to make room).

### How to read the next boot.log
- `overlay: keep-alive drawing ...` at the start, then every 30 s `video hook: ... keep-alive (copied: A at the
  flip, B repairs after the game painted over it, C for new content; D probe reads)`. If B is growing
  while you play and the HUD no longer flickers, the cause above was right. If it still flickers with B
  growing, send the log: the repair is too slow, try `overlay_poll=1` with `overlay_flip_stamp=on`.
  If it still flickers and B stays near zero, the flicker is not the game painting over the overlay.
- `memory: ... profile auto -> roomy ...` at the start; `memory watch: ok -> low ...` when the stage changes.
- To compare with the old behaviour: `overlay_mode=classic`.

### Host tests
- New `tests/memory_guard_test.cpp` (tiers, plans, stages, hysteresis, trend, text) and many keep-alive
  checks in `tests/video_hook_test.cpp`; `tests/menu_test.cpp` checks the new options against the values
  `main.cpp` accepts.
- `.github/workflows/check-patches.yml` gains the `eden_settings` step that the 0.7.4 notes said it had
  (it was missing) and the `memory_guard` step. A workflow file cannot be changed through the update
  zip: paste this version's `.github/workflows/check-patches.yml` in by hand.

## [0.7.4] - options from Eden's own settings screens

Written from screenshots of Eden's settings (Android, Portuguese) and the source of 0.7.3. **Not built
or run on a console by the author of this change**: the host tests below pass; `main.cpp` is checked
only by the GitHub build, and no option has been tried on the console. The Eden this port builds
against is not in this repository, so the setting and enum names follow Eden's `settings.h` from
memory and from Eden's public commits, not from the pinned source (see "Why each option is guarded").

### Added: menu rows and `settings.txt` keys
Same screens, same defaults as Eden (a line only appears in `settings.txt` when the option is not at
its default):

- VIDEO tab:
  - `vsync=fifo|mailbox|immediate|relaxed`: VSync mode (default fifo).
  - `scaling_filter=bilinear|nearest|bicubic|gaussian|scaleforce|fsr|area`: window adapting filter
    (default bilinear). It matters when the internal resolution differs from the screen's.
  - `anti_aliasing=none|fxaa|smaa` (default none).
  - `anisotropy=default|auto|2|4|8|16` (default default).
  - `dma_accuracy=default|unsafe|safe` (default default).
  - `sync_memory=on|off`: sync memory operations (default off).
  - `speed_percent=50|75|100|150|200`: speed limit percentage (default 100).
  - `gpu_accuracy` gets `medium` (before: low, high).
  - `resolution` gets `0.25` and `1.25` (before: 0.5, 0.75, 1, 1.5, 2).
- CPU & MEMORY tab: `skip_cpu_invalidation=on|off`: skip inner CPU invalidation (default off). It
  spends less CPU, the PS4's weak point, and may cause glitches or hangs: experimental.
- ADVANCED tab: `auto_stub=on|off` (default off; fills in missing services, can add crashes) and
  `region=japan|usa|europe|australia|china|korea|taiwan` (default usa, like Eden).

### Why each option is guarded
- `frontend/eden_settings.h` applies every new option with a `requires` check inside a template. A
  setting or an enum member that the pinned Eden does not have is **skipped**, never a build error,
  and `boot.log` says `settings.txt: vsync=mailbox (this Eden has no such setting, ignored)`. The
  old and new spellings of the VSync names (`FIFO` / `Fifo`) are both tried.
- `tests/eden_settings_test.cpp` checks it against three mock Edens (every setting, an older one, none)
  and checks that every value the menu can write is a value the header understands. It is a new
  step in `.github/workflows/check-patches.yml`.

### What was left out on purpose
Android-only or driver-specific items of those screens (Adreno max clock, BCn patching, Fix Bloom
Effects, Optimized Vertex Buffers, NCE, fastmem, the system MMU, GPU logging) and the ones that change
how Eden's threads run (GPU async emulation, asynchronous presentation, pipeline worker threads,
GPU unswizzle): none of those has been shown to work with the PS4 driver or with this port's patches.
Turbo and slow speed were not added because their setting names are not known.

### Not changed
Everything else of 0.7.3. In particular the boot logs of 0.7.3 are not analysed here.

## [0.7.3] - real exit to the game list, safe shader preload, shader counter

Written from the logs of a 0.7.2-fix run of Super Mario Maker 2 (`boot.log`, `boot.old.log`,
`eden_log.txt`, `eden_log.txt.old`) and from reading the current Eden master. **Not built or run
on a console by the author of this change**: the host tests below pass, `main.cpp` and
`ps4_platform.cpp` are checked only by the GitHub build, and every behaviour on the console is
untested. See "For the next log".

### Fixed: what the logs showed
- **`resolution` was never applied.** Every log says `settings.txt: resolution=0.75 (unknown,
  ignored)`: the check `Res1X == 2` assumed an old enum numbering, and this Eden counts 1/4x first.
  The scales are now set by name (`Res1_2X`, `Res3_4X`, `Res1X`, `Res3_2X`, `Res2X`).
- **The HUD and the L3 + R3 panel flickered.** 0.7.2 drew only after the flip was over, that is,
  after the frame was already on screen: the part already scanned out had no HUD and the next
  frame was a different buffer. Now each frame is drawn in while its flip is still queued (after a
  short wait for the GPU, `overlay_delay`, 2 ms) and once more after the flip, in case the GPU wrote
  over it. A frame the game flips while the menu is up gets the panel too. `boot.log` counts
  frames drawn "hidden" and "late" in the `video hook:` line.
- **Shader preload no longer kills Eden's shader threads.** The 0.7.1 limit stopped Eden's load
  with a stop token, but `LoadDiskResources` answers a stop by ending every thread of its pipeline
  worker pool (`WaitForRequests`), so after a capped preload (and with "None") nothing was left to
  build the shaders the game asked for. Now the size of `vulkan.bin` is limited before Eden reads
  it (about 32 KiB per pipeline: 128 = ~4 MiB, 256 = ~8 MiB, 1024 = ~32 MiB, all = no limit); a
  bigger cache is kept as `vulkan.bin.bak` and a new one starts. Eden then reads the rest to the
  end, and `boot.log` prints progress every 64 pipelines.
- **`eden_log.txt` no longer loses its last lines**: `log_flush_line` is turned on (the log of a 253 s
  session ended at 158 s). If this Eden has no such setting, `boot.log` says so.

### Changed: "Exit game" really leaves the game
- The L3 + R3 menu's last row, and the Options + touch pad hold, and a game that closes itself,
  now start the app again (`sceSystemServiceLoadExec`), so it opens on the game list. Eden cannot be
  rebuilt inside one run and its teardown crashed in test 2, so a new process is the safe way back.
  The call is weak: if the SDK lacks it, or the system refuses, the app stays up with a message
  asking to close it with the PS button, and `boot.log` has the reason (`restart: ...`).
- The menu has a new row: shader and memory notice on/off.

### Added
- **Shader counter** (like Cemu's): "Compilando shaders: N (total M)" in the bottom left corner,
  from Eden's own `ShaderNotify`, kept as a running total. A "LOW MEMORY: N MiB free" warning is
  added when the largest free direct block is under 160 MiB. Option `shader_notice` (PERFORMANCE tab).
- **Memory options**: `driver_cache` (the driver's own pipeline cache file, on/off), the preload
  size limit above, and two profiles: "Save RAM" (0.75x, 1024 MiB arena, ~4 MiB shader cache, no
  driver cache) and "More video memory" (1792 MiB arena, ~32 MiB cache).
- `overlay_delay` (0, 1, 2, 4, 8 ms) to tune the HUD timing without a new build.
- `frontend/shader_policy.{h,cpp}` and `tests/shader_policy_test.cpp`; new cases in
  `tests/video_hook_test.cpp` and `tests/menu_test.cpp`.

### What the logs say that this version does not change
- 0.7.2-fix Super Mario Maker 2: ~40 fps in the menus, then 10-13 fps from 119 s with the
  game CPU-bound (CPUCore_1 at 93% busy, the GPU at 5-22%), at the same time as three heap
  carve-outs took the largest free direct block from 608 MiB to 123 MiB. The two events coincide;
  the logs do not prove one causes the other.
- `boot.old.log`: the crash during the shader cache load (signal 10, four threads at the same
  address, registers holding ASCII text such as "eo_core/", which looks like memory overwritten
  or freed under the pipeline threads). It cannot be symbolized without the ELF of that build
  (`v0.7.2-fix-20261029T234052Z-15f9f8859b80`). Send the ELF if it happens again.
- The Eden zip sent for comparison is a newer master, not the pinned `5f142c79`: 15 of the 27
  patches do not apply to it. No Eden patch was added or changed in 0.7.3; everything is in `frontend/`.

### For the next log
- Send `boot.log`, `boot.old.log`, `eden_log.txt` and the old one, and the ELF of the build.
- Look in `boot.log` for: `restart:` lines (did the app start again?), `video hook: ... drawn N
  hidden / M late` (if most draws are late, raise `overlay_delay`), `shader cache: vulkan.bin is ...`
  (real size of the cache, to correct the 32 KiB per pipeline estimate), and `eden_log.txt: every line
  is flushed`.
- Proposed for a later version (needs an Eden patch, so it must be written against the pinned
  commit): a preload limit inside `load_graphics` that skips entries instead of stopping the load.

## [0.7.2-fix] - the video hook stays out of the boot

In 0.7.2 the games stopped starting. The log of a Super Mario Maker 2 run ends at 53.7 s, while the
shader cache is still compiling (183 of 2660 pipelines logged) and before the game starts, so the
drawing code had no frame to draw yet. The only code new in 0.7.2 that is alive at that moment is the
video hook's worker thread, which was started before the game loaded and, once a second, asked the
system for the clocks, the CPU/SoC temperatures and the free direct memory, with the HUD on or off
(the HUD is off by default). This log alone cannot prove the cause (Eden's log loses its last lines
when the app dies; `boot.log` was not sent), so this fix takes every 0.7.2 addition out of the boot.

### Fixed
- **The worker thread is no longer started before the game.** It starts the first time the game
  registers its video buffers (`boot.log`: "video hook: worker started"). Until then the hook does
  nothing, so the boot is the same as in 0.7.1.
- **With the HUD off the worker makes no call to the system at all** (no clocks, no temperatures, no
  free-memory query). They run only while the HUD is on.
- **If the worker thread cannot be created** (direct memory is tight on this console) the game now
  keeps running without the HUD and the pause menu, and `boot.log` says so; before, the exception
  would have ended the app.

### Tests
- 3 new cases in `tests/video_hook_test.cpp` (nothing runs before the buffers are registered, the
  worker starts at the first registration, no memory or temperature calls with the HUD off). The last
  one fails on the 0.7.2 code and passes now.

### For the next log
- Send `boot.log` (and `boot.old.log`) together with `log/eden_log.txt`: Eden's log loses its last
  lines when the app dies, while `boot.log` is written line by line and has the phase and the crash
  report (`!! CRASH ... eboot+0x...`).
- In this `eden_log.txt` 13 lines are cut in pieces and 32 are out of order (the log writer of Eden,
  two threads at once). Not changed here; it does not affect the game.
- If a game still does not open, **Clear the shader cache** (SYSTEM tab) rules the cache out: this
  log had one of 2660 pipelines.

## [0.7.2] - in preparation

Colours, the HUD and the L3 + R3 menu worked in 0.7.1; the HUD and the menu glitched, and at the Super
Mario Maker 2 menu the screen went black with sound. The log of that run (240 s, 476 pipelines, no
error) shows Eden rendering the whole time: the fault is in what reaches the screen, and the only code
of this port on that path is the video hook.

### Changed: the video hook is much lighter and draws at a safe moment
- **The thread that presents frames now only notes which buffer was flipped.** It used to draw the HUD
  there, before the real flip, and to ask the system for memory and temperatures once a second.
- **Drawing is done by a worker thread, only after the flip is over** (`sceVideoOutIsFlipPending`
  is 0): until then the GPU may still be writing the frame, which overwrote or half-drew the HUD (the
  glitch). The worker also takes the HUD's numbers, outside the lock.
- **It checks that the buffers are linear.** The tiling mode in the registration is logged; if it is
  not linear nothing is drawn (a linear write into a tiled surface is noise) and the menu says so.
  The colour swap does not depend on it.
- Every registration is logged at once in `boot.log` ("video hook: registered ... tiling ...",
  "registered AGAIN" when the swapchain is recreated), so the next log shows whether the black screen
  comes with a new swapchain.

### Added
- **PERFORMANCE tab** (DESEMPENHO): the shader preload limit, the HUD (on, size, corner), the
  overlay switch and the new **"Draw the HUD and menu over the game"** (`overlay_draw`). With it off
  only the colour swap stays, which separates "the drawing" from "the swap" if a black screen
  returns.
- 4 new host test cases (a flip still pending, a tiled buffer) in `tests/video_hook_test.cpp`.

### Not done, and why
- The other Eden video and performance settings (GPU and CPU overclock, VSync mode, anisotropic
  filtering, extra dynamic-state levels...) need their exact names in Eden's settings headers; a
  wrong name breaks the build. Only the ones this port already compiles with are in the menu. The
  artifact `eden-sources` (Check patches, manual run) gives me the headers.
- SpongeBob: Titans of the Tide: no log of it yet. After a try, the LOG tab (previous session) has
  the end of its log.

## [0.7.1] - built by GitHub, shader preload limited

### Fixed: games stopped opening (found in the log of a 0.7.0 run)
- The log ends at 78 s with **2185 pipelines** compiled in about 7 s and the game itself not yet
  started (the earlier working log had 9). Eden's `LoadDiskResources` compiles every pipeline of the
  game's disk cache before the game starts, and each stays in the driver's memory; a cache grown over
  many sessions used the whole arena (the log shows it at 1024 MiB, VRAM available 1.29 GiB) and the
  app died in that queue.
- The preload is now **limited** (`shader_preload`: none, 128, **256** (default), 1024, all, in the
  VIDEO tab); the rest of the cache compiles when the game asks for it and is saved as before.
- **It recovers by itself.** A marker file (`cache/shader/<title id>.loading`) exists while the cache
  loads; if the app died in it, the next start finds it, moves that game's cache to
  `<title id>.bad` and starts with an empty one (the log says so).
- A new SYSTEM row, **Clear the shader cache** (asks twice), for a game that will not open.
- `overlay` (ADVANCED tab): turns the video hook (HUD, L3 + R3 menu, colour swap) off in one step if
  it is ever the suspect. In this log it was not: the renderer was up and the hook had nothing to do
  with the compile queue.

### Memory options
Only two driver memory knobs exist in this port: the arena (`env=ORBIS_ARENA_MIB`, 1024 to 1792 MiB;
**1280 is the default, the log showed 1024**) and the textures-in-the-arena switch
(`ORBIS_VRAM_GARLIC`); with `vram` (conservative / aggressive) and now `shader_preload` they are the
memory options in the menu. More need Eden's own settings names (artifact `eden-sources`).

## [0.7.0] - built by GitHub, games stopped opening

### Added
- **A hook on the video output** (`frontend/video_hook.cpp`, linked with `--wrap` on
  `sceVideoOutRegisterBuffers` and `sceVideoOutSubmitFlip`): RADV's WSI registers its swapchain
  images as the video-out buffers, so with their addresses known the app can draw over the game
  without any change to Eden. Tested on a PC with recorders in place of the real calls.
- **Colours: "swap red and blue on the screen"** (`colour_swap`, ADVANCED tab, **on** by default):
  the registered pixel format is turned into its red/blue mirror. This is the fix for the Super Mario
  Maker 2 colours (sky pink, ground blue); it is also what the original author suspected for the whole
  image. The format Eden registered and the one used are in `boot.log` ("video hook: ...").
- **Performance HUD** (`hud`, `hud_size`, `hud_position`): FPS and frame time (flips per second, the
  real display rate), CPU cores in use (CPU time over wall time, out of 8), free direct memory and
  the CPU / SoC temperatures. Off by default; also toggled from the pause menu.
- **Pause menu on L3 + R3**, in Portuguese or English: Continue, HUD on/off, speed limit on/off,
  Exit game. The game is paused (`Core::System::Pause()`), the panel is drawn into the frame on the
  screen and the game resumes with `Run()`. Save states are not in the menu: Eden has none.
- The SYSTEM tab shows the **shader cache** (games with one, MiB): Eden already keeps one cache per
  game in `cache/shader/<title id>/` and loads it when the game starts (the log of the Mario Maker 2
  run shows `LoadDiskResources` and "Vulkan driver pipelines cached at ..."); nothing had to be built.
- `ci/stage-eden.sh` retries the build without the HUD temperatures if the SDK's stub library lacks
  the two system functions they call.

### Not yet
- **Game covers.** Reading a game's icon needs Eden's loader, which needs a `Core::System`, and the
  menu opens before the system exists. Moving the menu after the system is created changes a boot
  path that works; it is the first item of 0.8.0, with the boot log as the check.
- More Eden settings from the PC and Android versions: the names have to be checked against Eden's
  settings headers (`Check patches`, manual run, artifact `eden-sources`) before they are used.
- 30 to 60 fps: not changed here. The HUD is to measure it: CPU cores in use against the display
  rate says whether the emulated CPU, the GPU or the pacing is the limit.

## [0.6.0] - built by GitHub

First version after Super Mario Maker 2 opened. Not everything asked for is here: see "Not yet".

### Added
- **Firmware straight from the .zip.** Put `Firmware 20.5.0.zip` (or the .nca files) in `firmware/`;
  when a game starts, its .nca files are taken out into the registered folder (any folder inside the
  zip, checked against each file's CRC, written through a temporary name, files already there are
  left alone). Own inflate, no new library; tested byte for byte against zips made by Python
  (stored, deflated at levels 1 and 9, nested folders, corrupt, not a zip, no .nca).
- The SYSTEM tab says whether the firmware is installed, is in `firmware/` as .nca files or as a
  .zip, or is missing.
- **Clean game titles** in the list: "Super Mario Maker 2 [01009B90006DC000][v0].nsp" shows as
  "Super Mario Maker 2".
- The app logs when the system asks it to close (SIGTERM / SIGHUP / SIGINT) and leaves at once.

### Changed
- The "Default" quick profile now also puts the B8G8R8A8 colour fix back to Automatic.
- The help of the B8G8R8A8 fix says what swapped colours look like (pink sky, blue ground).

### Found in the Super Mario Maker 2 log and video
- The colours are swapped in **part** of the image (the sky pink, the ground blocks cyan, the "2" of
  the logo right). The log says `PS4 BGRA workaround ON (mode 1)` while the self-test of the same
  run says sampling B8G8R8A8 is already correct: the fix was forced to On, which swaps the textures
  that did not need it. **Set "B8G8R8A8 colour fix" (ADVANCED tab) to Automatic or Off**, or pick the
  Default quick profile, and look again.
- About 480 `Unmapped Read8 @ 0x38` lines: the game reads a null pointer's field; harmless for the
  run but it fills the log.

### Not yet (needs Eden's own code, which this repository does not hold)
- **Game covers**, an **in-game menu on L3+R3**, and an **FPS / memory / temperature HUD** all have to
  be drawn over the frames Eden presents; **save states** do not exist in Eden. They need the
  `eden-sources` artifact (Actions > Check patches and host tests > Run workflow).
- A clean return to the PS4 home screen: the app still stays on the green light when a game ends
  (returning from `main()` is reported as CE-34878-0); what the system sends on "Close Application"
  is now logged, to see it in the next test.
- 30 fps in Super Mario Maker 2 and 60 fps are goals, not results: the emulated CPU is the limit.
- Why SpongeBob: Titans of the Tide crashes: needs the LOG tab of that run.

## [0.5.0] - built by GitHub, shown to fix the data folder

The version to test Mario Kart 8 Deluxe and Super Mario Maker 2 with.

### Fixed
- **Folders and files that FTP and file managers could not use** (could not create `keys` /
  `firmware` / `roms`, could not delete `edenps4`, an empty-looking folder). The app now runs with
  no umask, so what it and Eden create is open to everyone; at start it opens every entry already
  below `/data/edenps4` (`chmod` 0777 / 0666); and an empty `edenps4` that the app cannot write in is
  moved aside (`edenps4.bak<time>`) and made again. The result is the first line of `boot.log`.
- **Options that were lost on restart.** `settings.txt` is written to a temporary file and renamed,
  or written in place if the rename fails; it is read back to be sure, and the menu shows
  "options saved" or, in red, that it could not (details in the SYSTEM tab: writing test and the
  size of `settings.txt`).

### Added
- **Folders made at the first start**: `keys`, `firmware`, `roms`, `updates`, `log`, `nand`, `sdmc`,
  `play_time` (its absence was why the play-time file was never made), `shader`, `cache`,
  `screenshots`, `dump`, `load`, and a `LEIA-ME.txt` that says what goes in each.
- **A LOG tab**: the end of the previous session's log (`boot.old.log`) and of this one, on the TV.
  After a crash or a hang, open the app again and photograph the tab: no FTP needed. The log of the
  session is also kept in memory, so it shows even if the file cannot be written.
- **Quick profiles** on the first row of the VIDEO tab: Default, Light (0.75x resolution, low GPU
  accuracy, no reactive flushing, ...) and Compatibility (high GPU accuracy, reactive flushing, auto
  CPU accuracy, no dynamic state, ...). They only set the options, to try combinations fast; they
  do not promise a game will open.
- New options: shader cache on disk (`shader_cache`) and textures inside the driver arena
  (`env=ORBIS_VRAM_GARLIC=0`, experimental, last resort).
- **A new icon and home-screen background**: a hybrid handheld (original drawing, own colours, no
  logos) with the planet on its screen and the name.

### Changed
- The Homebrew Menu entry is now "Emulator test (Homebrew Menu)", and says that its
  `launchInit() failed` message is **expected**: nx-hbmenu needs the Switch's nx-hbloader, which this
  emulator does not provide (the original author noted it in test 2). If the message shows, the CPU,
  GPU, text and pad of the emulator are working. A Homebrew Menu that lists programs needs the
  loader's environment (`envHasNextLoad`) to be provided by Eden's NRO loader: planned.
- `Check patches` (manual run) also keeps Eden's NRO loader and settings headers, for that work.
- Version 0.5.0. Colours (red and blue swapped) are now 0.6.0.

### Known
- Not verified on a console. The permission repair was tested on a PC as an ordinary user (folder
  made, tree opened, empty unwritable folder moved aside, populated one left alone); whether it is
  what stops FTP from working on the console is to be seen in the next test.

## [0.4.0] - built by GitHub, shown working on a PS4 Pro

First version with a menu. Includes everything of 0.3.0, the first one built by GitHub (the package
installed and started on a PS4 Pro).

### Added
- **A menu that opens at start**, in tabs (L1/R1 switch them): **Games** (the list, with sizes, and
  the built-in Homebrew Menu as a last entry), **Video**, **CPU and memory**, **Advanced** and
  **System** (language, restore defaults, and the facts that explain a game that does not start:
  keys, firmware, free memory, where the logs are). Left/Right change a value, Cross goes to the
  next one, Triangle restores the default, Circle goes back, OPTIONS plays the last game
  highlighted. Every change is saved to `settings.txt` as it is made (the lines of options left at
  their default are removed; comments and hand-written lines are kept), so it applies to the game
  that starts next.
- The menu is in **Portuguese and English** (System tab), with accents (`menu_font_latin.h`).
- New `settings.txt` keys, all in the menu: `mode` (handheld/docked), `resolution`
  (0.5, 0.75, 1, 1.5, 2), `speed_limit`, `multicore`, `vram` (conservative/aggressive) and
  `language`.
- **A new icon and home-screen background** (`assets/icon0.png`, `assets/pic1.png`, drawn by
  `tools/make-icons.py`), used by the package instead of the old probe icon.
- `tests/menu_test.cpp`: 72 host checks (settings.txt reading and writing, navigation, every tab
  drawn in both languages); `Check patches and host tests` runs it.
- `ci/prune-caches.sh`: the workflow deletes the old caches, keeping the newest of each kind.
- The SYSTEM tab tests whether the app can write in `/data/edenps4` and says whether `boot.log` exists
  there ("Gravação / Writing"), so an empty folder over FTP can be told apart from a write problem.

### Changed
- The built-in Homebrew Menu **no longer starts by itself** when there are no games: the menu opens
  and offers it as an entry. (On the first package built by GitHub it ended in
  "Error: launchInit() failed"; see Known.)
- **The build workflow keeps one file: the `.pkg`** (artifact `NX-on-Orbis-pkg`). The ELF is kept
  only when "keep_elf" is ticked on a manual run; the diagnostics only after a failure; the release
  has the `.pkg` only. The Eden sources for the colour work are kept only on a manual run of
  `Check patches`.
- Version 0.4.0.

### Known
- Not verified on a console. The menu was checked on a PC (layout, text, navigation, settings.txt);
  the video-out and pad code that shows it on the PS4 is the same technique as the old game list.
- `resolution` assumes Eden numbers its scales 0.5x..2x as 0..4 (`Res1X` is 2); the line is ignored
  and logged if it does not.
- "launchInit() failed" in the Homebrew Menu: to be diagnosed from `boot.log` and
  `log/eden_log.txt` of that run.
- Colours (red and blue swapped) are still open: they move to 0.5.0.

## [0.3.0] - built by GitHub, not tagged

Includes everything of 0.2.0, which is not built on its own. The first version that can be built
without the original author's machine.

### Added
- **Build on GitHub**: the workflow `Build PS4 package` (`.github/workflows/build.yml`) builds the
  `.pkg` and the ELF on an Ubuntu 24.04 runner: PS4 SDK and OpenOrbis tools, LLVM 18 libc++, OpenSSL,
  FFmpeg, Eden with the patches and the frontend, then the package. Run it from the Actions tab, or
  push a tag `vX.Y.Z` to also publish a release whose notes are this file's section for that
  version (a pre-release while the version is 0.x). The dependencies are cached; the compiler cache
  is kept even after a failed run; every stage's log, the SDK layout report and the CMake logs are
  uploaded as the `build-diagnostics` artifact.
- All the logic lives in `ci/*.sh` (`run.sh`, one `stage-*.sh` per stage), so build fixes arrive
  with an update zip and the workflow file almost never changes.
- `check-patches.yml` also keeps Eden's presentation sources (swapchain, present, blit) as the
  `eden-sources` artifact, and checks the syntax of every script and that `CHANGELOG.md` has the
  section of the current version.

### Changed
- `toolchain/eden-ps4.cmake`, `toolchain/runtimes-ps4.cmake`, `tools/env-build.sh`,
  `tools/ps4-cc.sh`, `configure-eden.sh` and `package-eden.sh` take the repository location from
  `NXO_ROOT` (and clang from `NXO_LLVM_BIN`). Without them they use the original Windows paths, as
  before. `package-eden.sh` now also runs on Linux.
- `docs/BUILDING.md` starts with the GitHub build.
- Version 0.3.0 (`EDEN_PS4_VERSION` in `frontend/frontend.cmake`).

### Fixed (found by the first runs on GitHub)
- `scripts/fetch-eden.sh` ended with `git log | head -3`: on Eden's long history `head` closed the
  pipe early and, with `set -o pipefail`, the script exited with 141 (SIGPIPE) after the patches had
  applied fine. Same pattern removed from `ci/`.
- The libc++ stage's sparse checkout lacked `llvm/utils` (CMake: "llvm/utils/llvm-lit is not an
  existing directory").
- libc++ stage: "error: No thread API". The PS4 flags pass `-U__FreeBSD__`, which switches off
  libc++'s automatic thread-API choice; `LIBCXX_HAS_PTHREAD_API=ON` now says it explicitly (and puts
  it in `__config_site`, which Eden's build reads later). The stage also builds with `ninja -k 0`, and
  a failed stage lists its first 25 error lines in the job summary, so one run shows every error.
- libc++ stage: "'asm/types.h' file not found" in `random.cpp`. Given no `--sysroot`, clang's FreeBSD
  driver also searched the Linux runner's own `/usr/include`, so `__has_include(<linux/random.h>)`
  was true. `-nostdlibinc` (non-Windows hosts only) now keeps the compiler's own headers and the
  SDK's, in the libc++, OpenSSL/FFmpeg and Eden flags; the same leak would have hit their configure
  checks.
- Eden stage: `CMake Error: The source directory "/deps/eden" does not exist`. `tools/env-build.sh`
  (rewritten in this version) used a variable named `R` and unset it, which erased the `R` of
  `configure-eden.sh`, the script that sources it. It now uses a private name. The Eden build also
  runs with `ninja -k 0`.
- Eden stage: every file of `src/common` failed with "`/usr/include/stdint.h`: 'bits/libc-header-start.h'
  file not found". Eden's CMake found host (Linux) libraries and put `/usr/include` on the compile
  line ahead of the SDK. `toolchain/eden-ps4.cmake` now restricts package, library and header
  searches to the PS4 prefixes (non-Windows hosts only), and the stage checks `build.ninja` right
  after configure and stops within minutes if a host path is still there.
- Package stage: "No usable version of libssl was found" from `PkgTool.Core` (OpenOrbis' packaging
  tool, a self-contained .NET program old enough to look only for OpenSSL 1.x; it builds the SFO
  without cryptography and dies at `pkg_build` on a system that only has OpenSSL 3). The stage now
  always hands it OpenSSL 1.1: Ubuntu 20.04's package unpacked privately into the work folder (nothing
  is installed on the system) and `CLR_OPENSSL_VERSION_OVERRIDE=1.1`. It also turns ICU off and
  records what the tool is (`logs/pkgtool-env.txt`).
- The compiler-cache folder is always created, so saving the cache after an early failure no longer
  warns.

### Known
- The build workflow is still being brought up stage by stage (the SDK stage and the libc++ configure
  checks already run on GitHub). Not verified on a console.

## [0.2.0] - not released on its own (included in 0.3.0)

First version after the original author closed the experiment. No change to the emulator core, the
patches or the package layout: only the frontend, tests, docs and automation.

### Fixed (frontend brought back in line with what v0.1.0 was documented to be)
- The profiler hang of test 22: a late sampling signal was treated as a stack-dump request and
  logged from inside the signal handler, which could freeze every thread that logs. It is now
  dropped, and the profiler pauses while a hang dump runs.
- `cpu_accuracy` defaults to `unsafe` (as the dev log says test 24 did) and `settings.txt` accepts
  `cpu_accuracy=unsafe|auto|accurate`, which INSTALL.md already documented.
- The `status:` line says "largest free direct block": the kernel call behind that number returns
  the largest free block, not the total.
- The build id (first line of `boot.log`) starts with the project version (`v0.2.0-...`) instead
  of `test22`.

### Performance
- `frontend/heap_census.cpp`: `free()` and `realloc()` no longer take a global spin lock and scan
  the live table on every call from every thread. A lock-free filter answers "was this pointer ever
  recorded?" with one atomic load. Census output is unchanged. Host test: about 3x faster in a
  6-thread malloc/free stress test (measured on a 1-core machine; not yet measured on the
  console).

### Added
- `docs/ROADMAP.md`: repository map, the do-not-touch list, the audit findings, the version plan
  up to 1.0.0 and how a version is tested.
- `docs/UPDATING.md` and `.github/workflows/apply-update.yml`: put `update-*.zip` in the repository
  root and the workflow applies it.
- `.github/workflows/check-patches.yml`: checks that the 27 patches apply to Eden `5f142c79` and
  that the host tests pass.
- `tests/heap_census_test.cpp`: host test for the heap census (correctness and `--bench`).

### Known
- Not yet verified on a console. A package must be built and played before this is called
  released.

## [0.1.0] - 2026-10-07

Original release by Alejo (@alechurri): Eden boots on a PS4 Pro, Mario Kart 8 Deluxe plays races at
13-16 fps with red and blue swapped. See docs/STATUS.md.
