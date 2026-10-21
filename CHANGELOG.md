# Changelog

Format: newest first. Versions follow docs/ROADMAP.md. Release notes on GitHub are taken from here.

## [0.2.0] - in preparation

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
