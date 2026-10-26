# Changelog

Format: newest first. Versions follow docs/ROADMAP.md. Release notes on GitHub are taken from here.

## [0.6.0] - in preparation

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
