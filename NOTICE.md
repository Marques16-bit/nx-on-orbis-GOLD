# Notices

**NX on Orbis** is not affiliated with, endorsed by, or supported by the Eden project, yuzu, Nintendo
or Sony. It contains no keys, firmware, games or Sony system software beyond the OpenOrbis sample
modules noted below.

## This repository

- `frontend/`, `toolchain/`, `tools/`, `tests/`, `probe/`, `scripts/` and the patches in
  `patches/eden/`: GPL-3.0-or-later (see `LICENSE`).
- `frontend/emutls/`: from LLVM compiler-rt 18.1.8, Apache-2.0 WITH LLVM-exception
  (`licenses/LLVM-compiler-rt-LICENSE.TXT`).
- `frontend/menu/menu_font.h`: generated from DejaVu Sans Mono (`frontend/menu/FONT-LICENSE.txt`).
- `testroms/hbmenu.nro`: switchbrew nx-hbmenu v3.6.1, ISC (`testroms/hbmenu-LICENSE.txt`).
- `tools/perl5/Locale/Maketext/Simple.pm`: a stub of the Perl module of that name, used only while
  building OpenSSL.

## Eden4 and what this project took from it

**Eden4** (release `Eden4-1.0.0-experimental`) is an unofficial PS4 port of Eden, GPL-3.0-or-later. Its package does not name its
author, so it is credited here by project name. It builds on **DolphinPS4** by **ShiroKlein** and contributors (GPL-2.0-or-later) and
credits ProsperoEden, SwitchLauncher by kasobos67, shadPS4 and others in its own `CREDITS.md`.

From v0.9.0 this project uses **ideas and numeric values** from Eden4, never its source files: keeping memory in the console's
*system flexible memory* instead of direct memory, the 32 MiB step, and the address hints that keep a mapping away from where the
kernel library puts its own objects (`0x1000000000`, `0x800000000`, `0x600000000`). The code here (`patches/eden/0029-*`,
`frontend/ps4_platform.cpp`, `frontend/memory_guard.*`) was written for this repository. What was taken and what was left is in
`docs/EDEN4.md`. Eden4 and this project are both GPL-3.0-or-later, so nothing in them conflicts.

## The binary release

The `.pkg` / ELF is a build of Eden (GPL-3.0-or-later; parts from yuzu, GPL-2.0-or-later) with the
patches and frontend in this repository, statically linked against components of the orbis-ports
SDK `orbis-sdk-v1` and of the toolchain, among them:

| Component | License | Text |
|---|---|---|
| Mesa (RADV for the PS4 GPU) | MIT | `licenses/MIT-mesa.txt` |
| orbis-compat | MIT | `licenses/MIT-orbis-compat.txt` |
| OpenOrbis musl libc | MIT | `licenses/MIT-musl.txt` |
| OpenOrbis toolchain (linker script, sample modules) | GPL-3.0 | `licenses/GPL-3.0.txt` |
| LLVM libc++, libc++abi, libunwind, compiler-rt 18 | Apache-2.0 WITH LLVM-exception | `licenses/LLVM-*-LICENSE.TXT` |
| Khronos Vulkan headers/loader parts | Apache-2.0 | `licenses/Apache-2.0-Khronos.txt` |
| FreeBSD headers | BSD-3-Clause | `licenses/BSD-3-Clause-FreeBSD.txt` |
| zlib | Zlib | `licenses/Zlib-zlib.txt` |
| OpenSSL 3.6.2 | Apache-2.0 | upstream |
| FFmpeg (h264/vp8/vp9 decoders, yadif/scale) | LGPL-2.1-or-later | upstream |
| Eden's bundled dependencies (fmt, boost, zstd, lz4, opus, dynarmic, xbyak, ...) | their own | Eden's source tree |

`licenses/` is copied from the SDK bundle's own `licenses/` folder. The package also contains
`sce_module/libc.prx`, `sce_module/libSceFios2.prx` and `sce_sys/about/right.sprx` from the
OpenOrbis toolchain's samples, as OpenOrbis homebrew packages do.

Corresponding source for a release: this repository at that release's tag (`v0.1.0` for the original one), Eden at commit
`5f142c7926d0c7fcbbd0ce30794d72f638a43b2a` (`scripts/fetch-eden.sh`), the SDK release
`orbis-sdk-v1`, OpenSSL 3.6.2 and FFmpeg `c7b5f1537d` with the options in `docs/BUILDING.md`.
