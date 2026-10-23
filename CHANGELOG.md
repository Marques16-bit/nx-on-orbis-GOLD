# Changelog

Format: newest first. Versions follow docs/ROADMAP.md. Release notes on GitHub are taken from here.

## [0.3.0] - in preparation

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
