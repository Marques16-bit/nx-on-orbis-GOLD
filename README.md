# NX on Orbis

An experimental port of the **Eden** Nintendo Switch emulator (a yuzu fork) to a jailbroken
**PS4 Pro** (OpenOrbis toolchain, Mesa RADV Vulkan driver for the PS4 GPU).

> **Status: experimental, under development (v0.9.0).** It boots, has its own menu, and runs real games, but
> slowly: the emulated CPU is the limit, not the GPU. Some games run, some close when the console runs out of
> memory. It is not a way to play Switch games on a PS4 yet. Everything below says what was **measured on a console**
> and what was only written and tested on a PC.

**Resumo em português:** port experimental do emulador Eden (Switch) para PS4 Pro com jailbreak. Abre, tem menu
próprio (português e inglês), roda jogos de verdade, mas devagar (o limite é a CPU emulada). O Super Mario Maker 2
roda; o Super Mario Odyssey fecha quando a memória do console acaba. A versão 0.9.0 traz uma opção nova e
**desligada por padrão** (`sysflex_mib`) que tira memória do jogo de um segundo "balde" de memória do PS4. **Nada da 0.9.0
foi rodado num console ainda.** Veja [Créditos](#credits) (inclui o Eden4) e [docs/EDEN4.md](docs/EDEN4.md).

| | |
|---|---|
| Console | PS4 Pro, firmware 12.02, GoldHEN (the only console it was tested on) |
| Base | Eden `5f142c79` (the commit the PS5 port ProsperoEden pins) + 29 patches in `patches/eden/` |
| Renderer | Vulkan only (`-DENABLE_OPENGL=OFF`), on Mesa RADV "PlayStation 4 Liverpool" (Vulkan 1.3) |
| Toolchain | OpenOrbis 0.5.4 + [orbis-sdk-v1](https://github.com/orbis-ports/orbis-porting-kit/releases/tag/orbis-sdk-v1) (orbis-compat, Mesa RADV for "Liverpool" GFX7), LLVM 18, libc++ 18 built for the PS4 |
| Build | GitHub Actions builds the `.pkg` and the ELF from a tag ([docs/BUILDING.md](docs/BUILDING.md)); [Releases](../../releases) |
| License | GPL-3.0-or-later (see `LICENSE` and `NOTICE.md`) |

## Not affiliated with Eden. Built with AI.

- This project is **not affiliated with, endorsed by, or supported by the Eden project** or its
  developers. **Do not report problems from this port to Eden** (issues, Discord or anywhere else).
- It was written with heavy use of AI assistants (Anthropic's Claude, and OpenAI's Codex for a
  couple of test rounds), directed and tested on the console by its maintainers. The Eden project
  prohibits AI-generated contributions; that is why this lives here, separately, and nothing from
  it has been or will be submitted upstream.
- No keys, firmware, games or Sony system files are included. You need to dump keys and firmware
  from your own Switch (Lockpick_RCM, TegraExplorer / NXDumpTool) and your own games.

## What it does

- **Its own menu** over the PS4 video output (DualShock 4): a games list with covers read from the games themselves, and tabs
  for video, CPU and memory, advanced, system and the log; Portuguese and English; seven colour themes. Every option is saved to
  `settings.txt` at once. The full list is in [docs/SETTINGS.md](docs/SETTINGS.md) (generated from the code).
- **In a game**, L3 + R3 opens a pause menu (continue, HUD, speed limit, exit or restart the game); an optional HUD shows FPS,
  CPU cores in use, free direct memory and temperatures; L3 + TouchPad saves a screenshot; a notice in the corner counts
  the shaders being compiled and warns when memory is nearly gone.
- **Colours**: red and blue are swapped by the video hook on the screen (`colour_swap`, on by default), which is what made
  the sky pink and the ground blue in Super Mario Maker 2.
- **Memory care**, because a console process has a hard ~4.5 GiB of direct memory and no swap: a profile picked from what the
  console reports, a watch on the largest free block that **pauses the game** before it runs out, a shader cache with no limit
  that survives a bad start, and (0.9.0) an optional second memory pool.
- **Safe mode**: after a start that died before drawing, the next start turns the experimental lines of `settings.txt` off.
- **Logs** that are kept: `boot.log` (with a `status:` line every few seconds), `mesa.log` (the driver), `log/eden_log.txt`,
  the last 8 runs of each in `logs/`, and a sampling profile of where the CPU time goes.

## How far it got (read from the console logs)

| Game | What happens |
|---|---|
| Super Mario Maker 2 | Plays. The title screen runs at 29-39 fps; after the scene change (the editor) it drops to 9-13 fps. The driver waits 0% for the GPU and the busiest emulated core is 88% busy: **CPU-bound**. One defect is not solved: in one scene the terrain is flat dark grey and some text is missing. |
| Super Mario Odyssey | Closes while loading: direct memory runs out (free block 58 MiB, then a crash 7 s later), because about 2.5 GiB are already taken by the emulator before the game uses any, and the game's memory then grew by 1.4 GiB. 0.8.4-0.9.0 try to give it room; **not confirmed on a console**. |
| Mario Kart 8 Deluxe | Played full races at 13-16 fps in v0.1.0 (the original release), with red and blue swapped. There is no newer measurement. |
| Cuphead | Reached its menu (~30 fps) in v0.1.0. |
| SpongeBob: Titans of the Tide | Crashes (seen in 0.6.0); the cause is not found. |

The speeds above come from console logs of 0.7.9 to 0.8.3, and the last logs read here are from **0.8.3**. Versions 0.8.4, 0.8.5 and 0.9.0 were built and unit-tested, but no log
of them has come back, so what they change is **not confirmed**: [CHANGELOG.md](CHANGELOG.md) says so entry by entry.
Tested hardware is one PS4 Pro (firmware 12.02, GoldHEN); a base PS4 or a Slim has never been tried.

## What is new in 0.9.0 (not run on a console)

- **Game memory in the system flexible memory** (`sysflex_mib`, **off** by default; menu: CPU AND MEMORY tab, "Game RAM in system
  flexible memory"). The PS4 has a second pool, outside the ~4.5 GiB of direct memory that big games run out of. With the option
  on, the game's RAM and Eden's big tables take their 64 KiB pieces from that pool first and from direct memory only when the pool
  says no. If the console does not offer the pool, nothing changes. The idea comes from **Eden4** (see below); the code is new.
  Try it with the quick profile "Minimum RAM (test)" (it sets 384 MiB) or by picking 128-512 in the option.
  In `boot.log` look for `system flexible memory: cap ... budget ... MiB` at the start and, in each `status:` line, the number
  of MiB "in system flexible memory". A line `not available on this console or SDK` means the pool is not there.
- Patch 0029 (the pool) is checked by `tests/lazy_memory_test.sh` (29 checks against a model of the kernel calls, with address
  and undefined-behaviour sanitizers). That is a model of the console, not the console.
- This page, [docs/SETTINGS.md](docs/SETTINGS.md), [docs/EDEN4.md](docs/EDEN4.md) and a script that checks them against the code
  (`bash tests/docs_check.sh`).

## What does not work yet, and what is being tried

- **Speed.** The game's own ARM code, translated to x86 and run on a 2.1 GHz Jaguar core, is the bottleneck. The profile in `boot.log`
  adds up the busy cores per thread. The biggest lever that is known and not done is the JIT: Eden4 changes Dynarmic's code
  generation, and that is a project of its own ([docs/EDEN4.md](docs/EDEN4.md)).
- **Memory for Odyssey-sized games.** See above. The numbers: a fixed ~2560 MiB is taken before the game starts (driver arena
  1280, C heap carve-outs, JIT caches, stacks, and the 128 MiB GPU stream buffer since 0.8.5); the game then wants about 1.4 GiB more.
- **Naming the biggest heap owners** needs the ELF that matches the build; keep it from every release.

## Install

[docs/INSTALL.md](docs/INSTALL.md) has the steps (keys, firmware and games in `/data/edenps4/`, then the `.pkg`).
[docs/BUILDING.md](docs/BUILDING.md) rebuilds everything; [docs/UPDATING.md](docs/UPDATING.md) says how an update zip dropped in
the repository is applied.

## Documentation

- [docs/SETTINGS.md](docs/SETTINGS.md): every option and quick profile (generated).
- [docs/EDEN4.md](docs/EDEN4.md): what Eden4 has, what this project took from it, and what it did not and why.
- [docs/STATUS.md](docs/STATUS.md): the closed v0.1.0 experiment: expectations, the 29 console tests, profiling.
- [docs/TECHNICAL.md](docs/TECHNICAL.md): platform findings (the SDK's 16-bit `wmemchr`, the package layout that grants 4.4 GiB, the
  GPU driver's memory behaviour, fault handling, address space), and a section per version.
- [docs/ROADMAP.md](docs/ROADMAP.md): repository map, what not to touch, the version plan.
- [docs/dev-log-es.md](docs/dev-log-es.md): the original development log in Spanish.
- [CHANGELOG.md](CHANGELOG.md): what each version changed, with the evidence.

## Checking this page

`bash tests/docs_check.sh` fails when this page, `docs/SETTINGS.md`, `NOTICE.md` or `docs/EDEN4.md` disagree with the code: the
version, the number of patches, the options named here, the profiles, the links, and the credits.

<a id="credits"></a>
## Credits

- **Eden and yuzu developers**: the emulator.
- **Eden4** (an unofficial PS4 port of Eden, release `Eden4-1.0.0-experimental`, GPL-3.0-or-later): this project took ideas and
  numbers from it for the memory work (see [docs/EDEN4.md](docs/EDEN4.md) for exactly what, and what was left). The Eden4 package does
  not name its author, so it is credited here by project name; if you know the author's name, tell us so it can be added.
  Eden4 in turn builds on **DolphinPS4** by **ShiroKlein** and contributors (the PS4 runtime and the GNM winsys), and credits
  **ProsperoEden**, **SwitchLauncher** by kasobos67, shadPS4 and others in its own `CREDITS.md`.
- **ProsperoEden** by BlackBearReloaded: the PS5 port this started from, and the Eden commit it pins.
- **orbis-ports**: orbis-compat and the PS4 Mesa RADV driver. **OpenOrbis**: the toolchain. **switchbrew**: nx-hbmenu.
  DejaVu fonts.
- **Alejo ([@alechurri](https://github.com/alechurri))**: the original port (v0.1.0): Eden booting on a PS4 Pro and Mario Kart 8
  Deluxe playing races.
- This repository, from v0.2.0 on: continued by its current maintainer, who tests every build on the console, with Claude
  (Anthropic) writing the code.
