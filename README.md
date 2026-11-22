# NX on Orbis

An experimental port of the **Eden** Nintendo Switch emulator (a yuzu fork) to a jailbroken
**PS4 Pro** (OpenOrbis toolchain, Mesa RADV Vulkan driver for the PS4 GPU).

> **Status: experimental, under development (v0.9.7).** It boots, has its own menu, and runs real games, but
> slowly: the emulated CPU is the limit, not the GPU. Some games run, some close when the console runs out of
> memory. It is not a way to play Switch games on a PS4 yet. Everything below says what was **measured on a console**
> and what was only written and tested on a PC.

**Resumo em português:** port experimental do emulador Eden (Switch) para PS4 Pro com jailbreak. Abre, tem menu
próprio (português e inglês), roda jogos de verdade, mas devagar (o limite é a CPU emulada). O Super Mario Maker 2
roda; o Super Mario Odyssey passou a abrir e a ficar rodando, mas deixa pouca memória livre. A 0.9.0 trouxe a memória
flexível do sistema (`sysflex_mib`), **que funcionou no console** (os logs voltaram). A 0.9.1 a deixa **ligada por padrão e
maior** (até 1536 MiB), e traz a opção **Controles (jogadores) de 1 a 4** na aba Sistema, **ainda não testada num console**.
A 0.9.2 traz a opção **JIT: reaproveitar a busca de página** (aba CPU e Memória, desligada por padrão): o mesmo efeito do JIT do Eden4,
escrito para a tabela de páginas deste port; **nunca rodou num console** e o código que gera as instruções nem foi compilado aqui.
Se não compilar no CI, o build segue sem ela.
A **0.9.5** (segundo zip: **R2 num jogo abre as opções só daquele jogo**, salvas em `per-game/`, e `.nro` aparece com o nome e o ícone dele) é a versão para todo mundo: opções que ninguém deve mexer ficam **escondidas e fixas** (o JIT agora vem ligado, com o modo seguro como rede), as de teste vão para a aba **TESTES** (a versão pública não terá), aba nova **CONTROLES** (4 jogadores, modelo do controle, quais estão conectados) e o menu ficou mais bonito. Ainda não rodou no console. A 0.9.4 faz a **tabela direta** entrar de verdade no build (no 0.9.3 ela não entrou: o Eden fixado era diferente do que o patch esperava) e o emissor foi rodado pela primeira vez, num PC. A 0.9.3 (a "0.9.2 fix") somou a opção **JIT: tabela de páginas direta** (mesma aba, desligada por padrão): o JIT passa a ler uma cópia já decodificada da tabela de páginas, 4 instruções a menos por acesso à memória; também **nunca rodou num console**. Veja [Créditos](#credits) (inclui o Eden4) e [docs/EDEN4.md](docs/EDEN4.md). A **0.9.4 fix** faz o build contar no `boot.log` o que fez com o pacote do JIT (e por que uma parte ficou de fora), tira o pacote aos poucos se o build falhar, e acrescenta o cache de código do JIT (`jit_cache_mib`) com um contador de quantas vezes ele foi esvaziado (travadas de recompilação).

| | |
|---|---|
| Console | PS4 Pro, firmware 12.02, GoldHEN (the only console it was tested on) |
| Base | Eden `5f142c79` (the commit the PS5 port ProsperoEden pins) + 35 patches in `patches/eden/` (and, since 0.9.2, an optional JIT pack in `patches/eden-jit/`) |
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
- **In a game**, L3 + R3 opens a pause menu (continue, HUD, speed limit, exit or restart the game); an optional HUD shows FPS
  (coloured by whether the game holds its target), a graph of the last minute, CPU cores in use, free direct memory and temperatures; L3 + TouchPad saves a screenshot; a notice in the corner counts
  the shaders being compiled and warns when memory is nearly gone.
- **Colours**: red and blue are swapped by the video hook on the screen (`colour_swap`, on by default), which is what made
  the sky pink and the ground blue in Super Mario Maker 2.
- **Memory care**, because a console process has a hard ~4.5 GiB of direct memory and no swap: a profile picked from what the
  console reports, a watch on the largest free block that **pauses the game** before it runs out, a shader cache with no limit
  that survives a bad start, and (0.9.0, on by default since 0.9.1) a second memory pool.
- **Safe mode**: after a start that died before drawing, the next start turns the experimental lines of `settings.txt` off (and
  keeps the second memory pool to the 512 MiB that has been seen to work).
- **Up to 4 controllers** (0.9.1, `players=1|2|3|4`; menu: SYSTEM tab, "Controllers (players)"): the other users logged in to the
  console each give one DualShock. Not tested on a console yet.
- **Logs** that are kept: `boot.log` (with a `status:` line every few seconds), `mesa.log` (the driver), `log/eden_log.txt`,
  the last 8 runs of each in `logs/`, and a sampling profile of where the CPU time goes.

## How far it got (read from the console logs)

| Game | What happens |
|---|---|
| Super Mario Maker 2 | Plays. The title screen runs at 29-39 fps; after the scene change (the editor) it drops to 9-13 fps. The driver waits 0% for the GPU and the busiest emulated core is 88% busy: **CPU-bound**. One defect is not solved: in one scene the terrain is flat dark grey and some text is missing. |
| Super Mario Odyssey | 0.8.5 closed while loading (direct memory ran out at 1982 MiB of guest memory). With 0.9.0's 512 MiB of system flexible memory it **kept running**: guest memory plateaued at 2152 MiB, the largest free direct block stayed at about 93 MiB (`low`), 9-19 fps, and the run ended by choosing Exit in the pause menu (at 435 s of the log). The game freed **no** guest memory during that run (`0 MiB given back`). 0.9.1 raises the pool to up to 1536 MiB, which should leave about 1 GiB more direct memory free; **not run yet**. |
| Mario Kart 8 Deluxe | Played full races at 13-16 fps in v0.1.0 (the original release), with red and blue swapped. There is no newer measurement. |
| Cuphead | Reached its menu (~30 fps) in v0.1.0. |
| Mario vs. Donkey Kong | Plays and is the best 3D test so far: 10-26 fps in the `status:` lines of the 0.9.0 run, once it was running. Its video cutscenes show a **black screen**: the video decodes (the log shows the decoder working), but what is drawn is black, and the cause is not found; see [docs/ROADMAP.md](docs/ROADMAP.md). |
| SpongeBob: Titans of the Tide | Crashes (seen in 0.6.0); the cause is not found. An Unreal Engine 5 game; too heavy for this CPU as it is. |

The speeds above come from console logs of 0.7.9 to 0.9.0. The 0.8.5 and 0.9.0 logs of Odyssey, Mario vs. Donkey Kong and (0.9.0)
Mario Maker 2 were read; there is no log of 0.9.1 yet, so what it changes is **not confirmed**: [CHANGELOG.md](CHANGELOG.md) says so
entry by entry. Mario Maker 2's 0.9.0 log shows the same CPU-bound behaviour as before.
Tested hardware is one PS4 Pro (firmware 12.02, GoldHEN); a base PS4 or a Slim has never been tried.

## What is new in 0.9.7: the JIT's memory in steps, and the second optimisation list

**0.9.7 fix 2 (third zip).** Nothing here has run on a console. The `.nro` and SpongeBob logs of Nov 21 were both **in safe mode** (a black screen ended by hand was counted as a crash, so the options being tested were switched off): a start now counts as a death only when it crashed (a marker file) or was over within 60 s, and the TESTS tab has `safe_mode=auto|off`. The build now writes a table of the ELF's functions (`symbols.txt`, in the package's assets/misc folder) and `boot.log` puts **function names** under every `eboot+0x...` offset (thread stacks, crash, profile, heap census), checked against the build, so the next `.nro` / SpongeBob log can say what they are stuck in. The preset "More FPS (test)" no longer turns on the two options that never ran on a console. The causes of the `.nro` and SpongeBob stalls and of Mario Maker 2's graphics are **not known yet**. Details and what to send back: [CHANGELOG](CHANGELOG.md).

**0.9.7 fix (second zip).** The console test of 0.9.7 showed what ended the two big games, and it was not the code caches. **Super Mario Odyssey** died of the driver running out of *32-bit address space* (`out of 32-bit VA`): the driver's
memory arena sat across a 4 GiB boundary, so only 643 MiB of its 1503 could serve shader and descriptor allocations; where the kernel puts the arena changes at every start. The arena is now **placed inside one 4 GiB window**
(`arena_align=on`, hidden, on by default, off in safe mode; `boot.log`: `arena placement:`). **Super Mario Maker 2** died of the C heap asking for 512 MiB with 38 MiB of direct memory left; who asked is not yet known, so this zip adds what will say it
(`heap census:` down to 256 KiB, `heap big maps:` for requests of 16 MiB or more, both also printed when the process dies) and warns when `settings.txt` raises the arena above the console's profile. Nothing of this has run on a console. Details: [CHANGELOG](CHANGELOG.md).

The Odyssey log of 0.9.5 showed what ends that game: the free direct memory fell about 1200 MiB a minute until 44 MiB were left, with 1 GiB of it taken at the start for four JIT code caches that were mostly empty. In 0.9.7 each cache
**reserves** its range and takes memory **in 2 MiB steps as the code grows** (`jit_cache_commit=lazy`, the default; Eden patch 0034), so the memory goes to the game, and the ceiling can be larger (256 MiB per core on a roomy console), which
empties the cache, a stall, far less often. A start after a crash takes it whole, as before. New `boot.log` lines: `JIT cache memory:` and `texture traffic:` (Eden patch 0035, counters only: how much texture data the CPU moves,
for Super Mario Maker 2's blinking classic-theme textures). The second optimisation list (GPU offload, zero-copy memory, AOT pre-compilation) was read against the code and the logs: one item became the change above, two do not apply, each with the
reason in [docs/OPTIMIZATION.md](docs/OPTIMIZATION.md). **Nothing of it has run on a console.**

## What is new in 0.9.6: the optimisation list, read against the code

One new thing, `cpu_affinity` (the four threads that run the Switch's cores each get a PS4 core, the GPU, audio and service threads share the rest), and two logs that were
missing (`memory arenas:`, `cpu features:`). The rest of the list (11 items) turned out to be how Eden, its Vulkan backend and the RADV driver already work, or things that were built
and measured and not kept: [docs/OPTIMIZATION.md](docs/OPTIMIZATION.md) says which and why. **Nothing of it has run on a console.**

## What is new in 0.9.5: a menu made for people (first to fifth zip)

**Fifth zip:** a **new HUD** (the frame rate coloured by whether the game holds its target, a graph of the last minute, CPU, memory and temperatures), a **new L3 + R3 menu** (a card with on/off pills and key caps) and **sounds and music in the menu**
(made by the program, no files; `menu_audio` and `menu_volume` in the SYSTEM tab; the menu only, never the game). Drawn and tested on a PC only; the audio port has never been tried on a console, so it opens with a marker file that keeps the menu silent after a crash there.

**Third zip:** the **extra RAM** option (`sysflex_mib`) is back in the CPU AND MEMORY tab, because some games open only with less of it (and R2 can set it for one game only). The build for everyone
(`PUBLIC_BUILD` file in the repository root, see docs/BUILDING.md) has no log tab, no test options, no screenshots and no profiler.

**Second zip:** press **R2** on a game in the list to open that game's own options (resolution, console mode, GPU and CPU accuracy, controllers and their models, region...). They are saved in
`per-game/<title id>.txt` and applied on top of `settings.txt` only when that game starts; a game that has some is marked in the list. The list also shows the **name and icon of each `.nro`**
homebrew from the file itself. Not run on a console yet.

The goal of 0.9.5 is the version for everyone. This first zip changes the **menu**, not the speed: the options a person should not touch are **hidden and fixed** at the value this port needs (the two JIT
switches are now **on by default**, with safe mode as the net), the ones that are only for testing moved to a **TESTS** tab that a public build will not have, there is a new **CONTROLS** tab (four
players, the controller model the game sees, which controllers are connected), and the menu has a new look (soft lights, shadows, key caps with the real button shapes). **None of it has run on a
console yet**; the menu is tested on a PC by drawing every screen. Details in the [CHANGELOG](CHANGELOG.md); the options and who sees them in [docs/SETTINGS.md](docs/SETTINGS.md).

## What is new in 0.9.4: the direct table is in the build now, and the JIT's code ran for the first time (on a PC)

**0.9.4 fix 2:** the test of the fix ran in safe mode (a crash of another game, hours before, had turned it on: every experimental option was ignored) and the build still left out part 2 of the
JIT pack: its script needed Python 3.13 and the GitHub runner has 3.12 (the new report in `boot.log` showed the `TypeError`). Fixed both: the script runs on every Python 3, safe mode now applies only to the same
game that died (or after two deaths in a row), and the JIT code cache, which the same log showed being thrown away whole 6 times in 200 s at 32 MiB, is 64 MiB per core by default on this console
(`jit_cache_mib`, menu CPU) and is taken on 2 MiB boundaries (patch 0032). `boot.log` now also counts the blocks the JIT compiles every 10 s. Details in [CHANGELOG.md](CHANGELOG.md).

**0.9.4 fix:** the first 0.9.4 `boot.log` still said the build had no direct table, and showed that a faster page lookup is only a few per cent of the CPU time (the JIT's code is 28-50% of
what the CPU threads do). So the fix makes the build explain itself (`boot.log` now quotes what the CI did with the JIT pack and why a part is missing), takes a failing pack out one part at a time
whatever the error, and adds the one thing the log points at: the JIT code cache (32 MiB per core, `jit_cache_mib`) with a counter of how often it is thrown away whole (a recompile stall).

The 0.9.3 `boot.log` said the build did not have the direct table: its edit script was written against Eden master and the pinned Eden differs in two
places. 0.9.4 fixes the script, tests it on the pinned Eden's own files, and runs Dynarmic's x64 backend (pinned Eden + this pack) on a PC against
random AArch64 memory programs in four configurations, with identical results ([tools/jit-harness/](tools/jit-harness/README.md)). What a console will
show is the gain: turn on `jit_page_reuse` and `jit_direct_table` (or the "More FPS (test)" profile). Details in [CHANGELOG.md](CHANGELOG.md).

## What is new in 0.9.3 (not run on a console): the 0.9.2 fix

The 0.9.2 test showed the page reuse alone gives a very small gain. 0.9.3 adds the next piece of the JIT:

- **JIT: direct page table** (`jit_direct_table=on|off`; menu: CPU AND MEMORY tab; **off by default**; turn it on together with the page
  reuse). Beside Eden's page table the port keeps a second table with every entry already decoded, and the JIT reads that instead of decoding
  the entry each time: 4 instructions fewer per memory access. It is the effect of Eden4's direct table, made without changing Eden's own
  table. The quick profile "More FPS (test)" turns it on.
  - **Never run on a console, and its code generator was never compiled.** Expect a few percent, not a doubling. The page table side is
    tested on a PC (32 checks, with threads); the edits are a script that changes nothing if the pinned Eden does not look as expected, and the
    CI builds again without them if they do not compile (`boot.log` says `JIT direct table: ...`). Details in [CHANGELOG.md](CHANGELOG.md).

## What was new in 0.9.2 (not run on a console)

- **JIT: reuse page lookups** (`jit_page_reuse=on|off`; menu: CPU AND MEMORY tab; **off by default**). The JIT reads the page table once
  for a group of nearby memory accesses instead of once per access, which is less CPU work (the CPU is what limits every game here). It
  is the effect of Eden4's "page reuse", written for Eden's own flat page table (Eden4's code needs its two-level table), as an optional
  patch to Dynarmic in `patches/eden-jit/`. The quick profile "More FPS (test)" turns it on.
  - **Never run on a console, and its code generator was never compiled** (no Xbyak/fmt/Boost where it was written; the grouping logic was
    run against a stand-in of the IR, 38 checks). If it does not fit the pinned Eden it is skipped, and if it does not compile the CI
    builds again without it; `boot.log` says `JIT page reuse: ...` with what happened and how many groups were formed.
  - Not ported from Eden4: the two-level table, the register allocator changes and the code-cache windows (reasons in
    [CHANGELOG.md](CHANGELOG.md)).

## What was new in 0.9.1 (not run on a console)

- **Game memory in the system flexible memory is no longer an experiment** (`sysflex_mib`; menu: CPU AND MEMORY tab, "Game RAM in
  system flexible memory"). The PS4 has a second pool, outside the ~4.5 GiB of direct memory that big games run out of, and the
  0.9.0 console run showed it works: the pool accepted a 640 MiB probe, all 512 MiB of the budget were used, audio and the
  controller kept working, and Odyssey went on running to 2152 MiB. Now:
  - the default is **automatic**: up to 1536 MiB, limited by what the pool turns out to have (the app finds that with a binary
    search of up to 2176 MiB and always leaves 128 MiB to the system); the menu offers off, 256, 512, 768, 1024, 1536 and 2048 (an old `sysflex_mib=384` or `=128` line from 0.9.0 is read as auto);
  - it is **no longer ignored by safe mode**; after a start that died before drawing it is limited to 512 MiB instead (the amount
    seen to work), not switched off;
  - the quick profile "Minimum RAM (test)" uses 2048.
  A game's RAM and Eden's big tables take their 64 KiB pieces from the pool first and from direct memory only when the pool says
  no. Look in `boot.log` for `system flexible memory: cap ... budget ... MiB` and, in each `status:` line, the MiB "in system
  flexible memory". The idea comes from **Eden4** (see below); the code is new.
- **Patch 0030** counts what the emulated kernel gives back: the `status:` line now ends with `kernel frees N calls / M MiB (K under
  64 KiB)`. In the Odyssey run nothing was given back, so the game keeps its own heap and "free the previous area at each loading
  screen" has nothing to free from the outside; these numbers will show whether that holds in other games.
- **Controllers (players)** (`players=1|2|3|4`; menu: SYSTEM tab). Eden already has one virtual gamepad input per player slot, and a
  slot marked connected connects itself when it gets input (read in Eden's source). The frontend marks players 2-4 connected and reads
  one DualShock per extra logged-in user (a second controller logs in as a guest when its PS button is pressed; the app looks every
  2 seconds and lights it red, green or pink). The default is 1, which changes nothing. The PS4 side and the settings side are
  tested on a PC with stand-ins; the **real connection in Eden and on the console is not tested**. Look for `controllers:` in `boot.log`.
- Checks: `tests/lazy_memory_test.sh` (34 checks, with address and undefined-behaviour sanitizers, against a model of the kernel
  calls, not the console), `tests/docs_check.sh`, and the other host tests listed in [CHANGELOG.md](CHANGELOG.md).

## What does not work yet, and what is being tried

- **Speed.** The game's own ARM code, translated to x86 and run on a 2.1 GHz Jaguar core, is the bottleneck. The profile in `boot.log`
  adds up the busy cores per thread. The biggest lever that is known is the JIT: Eden4 changes Dynarmic's code
  generation ([docs/EDEN4.md](docs/EDEN4.md)). 0.9.2 carries one of its changes, the page lookup reuse, and 0.9.3 a second one, the direct table, as an optional pack written for
  this port's page table (off by default, never run on a console); the others are not done. The pinned Eden's source is still not in this
  repository, so none of this is compiled until the CI does it (see the roadmap).
- **Memory for Odyssey-sized games.** See above. The numbers: a fixed ~2560 MiB is taken before the game starts (driver arena
  1280, C heap carve-outs, JIT caches, stacks, and the 128 MiB GPU stream buffer since 0.8.5); the game then wants about 1.4 GiB more.
  0.9.0 moved 512 MiB of that out of direct memory; 0.9.1 can move up to 1536 MiB.
- **Cutscenes of Mario vs. Donkey Kong** stay black: the decode works, the picture does not reach the screen. The first decoded frame
  is saved by the build as `/data/edenps4/vic_frame.raw` and `vic_frame.txt`; sending those two files would show whether the frame
  is real.
- **Naming the biggest heap owners** needs the ELF that matches the build; keep it from every release.

## Install

[docs/INSTALL.md](docs/INSTALL.md) has the steps (keys, firmware and games in `/data/edenps4/`, then the `.pkg`).
[docs/BUILDING.md](docs/BUILDING.md) rebuilds everything; [docs/UPDATING.md](docs/UPDATING.md) says how an update zip dropped in
the repository is applied.

## Documentation

- [docs/SETTINGS.md](docs/SETTINGS.md): every option and quick profile (generated).
- [docs/OPTIMIZATION.md](docs/OPTIMIZATION.md): the 0.9.6 and 0.9.7 optimisation lists, item by item: what was done, what is already there, what does not apply.
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
- **ProsperoEden-Encore** by niakw (GPL-3.0): the community PS5 continuation, read in full for 0.9.5 as the reference for the menu, the option policy and what comes next (sounds, formats, backports). Its ideas guide this work. Since zip 4 of 0.9.5 one patch (0033, texture-cache views) is adapted from its Eden backports, with the PS5-only code removed.
- **orbis-ports**: orbis-compat and the PS4 Mesa RADV driver. **OpenOrbis**: the toolchain. **switchbrew**: nx-hbmenu.
  DejaVu fonts.
- **Alejo ([@alechurri](https://github.com/alechurri))**: the original port (v0.1.0): Eden booting on a PS4 Pro and Mario Kart 8
  Deluxe playing races.
- This repository, from v0.2.0 on: continued by its current maintainer, who tests every build on the console, with Claude
  (Anthropic) writing the code.
