# Changelog

Format: newest first. Versions follow docs/ROADMAP.md. Release notes on GitHub are taken from here.

## [0.9.3] - The JIT's direct table (the 0.9.2 fix: more of the JIT's cost removed; optional, off by default)

**The 0.9.2 test on the console showed what the page reuse does alone: a gain, but a very small one. This is the next step on the same
road, and it is also unverified.** What was run: the edits of the new part on copies of Eden master's files they change (24 structure checks,
`tests/jit_direct_test.sh`: they apply, apply only once, and with any one anchor missing change nothing); the **page table half** of the
part (`common/page_table.h/.cpp` as the edits leave them) built and run on a PC with the console's lazy vector stood in, with address, undefined-behaviour
and thread sanitizers (32 checks: after every `Store`, `MarkRasterizerCached` and zeroing a slot is 0 or exactly the entry's pointer, also
with four threads storing and marking the same entries); the scripts that apply and take out the pack in stages, on throwaway git repositories
(16 checks: `tests/jit_pack_scripts_test.sh`); `jit_pack_test`, `safe_mode_test`, `menu_test`, `eden_settings_test` and `tests/docs_check.sh`.
**Not run: the code that emits the x86 instructions, which was never compiled** (it needs Xbyak, fmt and Boost: not available where this was written), **and Eden's
pinned sources, which are not in this repository: the edits were written against Eden's current ones and made to need as little as possible
of them.** Nothing of it has run on a console.

**What it is.** Each guest memory access the JIT compiles walks the page table: load the 8-byte entry, test its marked bit, mask the
attribute bits off, shift left and arithmetic-shift right to sign-extend, test for zero (about 12 instructions with the address
arithmetic). Eden4 shortens that with a "direct" table that holds `host - guest` ready to use. This port keeps Eden's table and adds one
beside it (`patches/eden-jit/0002-...py`): one 8-byte slot per entry holding exactly the decoded pointer, 0 when the entry is marked, unmapped
or empty. The JIT loads the slot and tests it for zero: **4 instructions fewer per access** (`mov imm64, base; mov slot, [base + index*8]; test; jz`
instead of `mov; bt; jc; mov imm64; and; shl; sar; jz`), and the same short sequence in the page reuse's group lookup and its out-of-line slow
path. Eden's own entries are not changed, so nothing but the JIT reads the new table; A32 games keep the usual lookup.
- **Who writes the slots:** only `PageEntryData` itself (`Store`, `MarkRasterizerCached`; `MarkDebug` goes through `Store`) and the one place that
  zeroes entries (`core/memory.cpp`, unmapping). A slot is zeroed *first* whenever its entry changes (so while it changes the JIT takes the
  callbacks), written *after*, and written again if the entry changed in between (another thread storing or marking the same entry): the last
  write of a slot is always made from an entry that did not change after it. That is tested with threads on a PC.
- **Memory:** 8 bytes per mapped 4 KiB page (about 2 MiB for 1 GiB of guest memory in use), lazy like Eden's own table here: memory only where it is touched. The table's memory is given to a slot's chunk when touched, by the same fault handler as the entries.
- **What it needs:** the table setup Eden configures (absolute offsets, 8-byte entries, no mirroring) and the first page table sized after
  `jit_direct_table=on` is read (the guest process's); for another table the JIT uses the usual lookup.
- **What to expect, honestly:** the lookup is only part of what a memory access costs and this removes about a third of it. Fastmem (no
  lookup at all) measured +10-30% on this console in the 0.8 tests (`docs/STATUS.md`), so a few percent is what this can give, with the page
  reuse (0.9.2) that it works together with. It is not a doubling, and the CPU-bound games' limit is not only the JIT.

**Safeguards, because it is unverified.**
- **Off by default.** Menu, CPU AND MEMORY tab, "JIT: direct page table" (`jit_direct_table=on` in `settings.txt`, independent of
  `jit_page_reuse`); the quick profile "More FPS (test)" turns it on. Safe mode ignores it (experimental).
- **It cannot break the build, in two steps.** The edits are a script of anchored changes (`patches/eden-jit/0002-...py`) and not a diff,
  because the pinned Eden is not at hand: each needs only a line or a few to be there, and if one is missing **nothing is written** and the
  build goes on with part 1 only (page reuse); if the edits fit but do not compile, `ci/stage-eden.sh` takes out **only part 2** and builds
  again, and if that fails too, the whole pack. The errors of each step are in the `jit-pack-errors` artifact. The "Check patches" workflow
  says which parts fit the pinned Eden and runs the page table test against that Eden's own `page_table.h`.
- **`boot.log` says what happened:** `JIT direct table: ...` at the start (off / on / "this build does not have it") and when the game has drawn and
  when it is left, with the number of lookups compiled to read the table. "on, but no page table got a direct copy" means the option was on and
  no table was made for it (the JIT then reads the entries as before).

**What can still go wrong.** A bug in the emitted code is a game that hangs, closes or runs wrong; a slot that is not zero when its entry is
(a path that changes an entry without `Store`) is a write to memory that is not the game's any more. The paths that change entries in Eden
master are the ones above; the pinned Eden may have another. To check it: open a game with it off and on, look at the `JIT direct table:` lines
and the speed, and send `boot.log`. If it breaks, `jit_direct_table=off` is back to 0.9.2.

## [0.9.2] - The JIT's page reuse, written for this port's page table (optional, off by default)

**Not built or run on a console by the author of this change, and the part that matters most was never compiled.** What was run: the
page reuse **pass** (the IR side of the patch) cut out of the patch and run on a PC against a stand-in of Dynarmic's IR
(`tests/jit_pack_test.sh`, 38 checks: what groups, what ends a group, what the lookup instruction holds, and that the pass changes
nothing when the switch is off or the table setup is not the expected one); the scripts that apply and take out the pack, on throwaway
git repositories (fits, does not fit, switched off, taken out); `jit_pack_test`, `safe_mode_test`, `menu_test`, `eden_settings_test`,
`memory_guard_test` and `tests/docs_check.sh`. **Not run: the code that emits the x86 instructions** (it needs Xbyak, fmt and Boost,
which are not available where this was written, and the pinned Eden's Dynarmic is not in this repository either: the patch was made
against Eden's current Dynarmic sources). Nothing of it has run on a console.

**What it is.** Eden4 makes the JIT look a memory page up once for a group of nearby accesses instead of once per access (its "page
reuse" pass, after ProsperoEden). Eden4 does it on top of its own two-level page table with a "direct" mirror, which this port does not
have (Eden's flat table is what patches 0007 and 0027 are built on), so the code could not be copied. This is the same effect written for
the flat table (`patches/eden-jit/0001-...`, four Dynarmic files):
- an IR pass groups the accesses of a block whose addresses are one value plus constants, within 256 bytes, with no exclusive, ordered,
  barrier, supervisor call or exception in between; a group of two or more shares one new instruction, `A64EdenPageLookup`, which
  reads the page table entry once (masked and sign-extended like any lookup, "absolute" offsets) or gives 0 when the span crosses a page
  or the page must take the callback path;
- each access of the group tests that value and reads or writes `[entry + address]` directly, or runs its own full lookup out of line
  (a copy of the usual lookup), or the usual callback; a group never changes what an access does, only how many times the table is read.
- Every register of the lookup is allocated before its first branch (Eden4's own register allocation bug, found with its PC harness, was
  a late scratch register in the lookup).
- The pass only runs for the table setup Eden configures (absolute offsets, 8-byte entries, an address space that is range checked and
  has at most 42 bits, misalignment checked at page boundaries); with another setup it does nothing and `boot.log` says why.

**Safeguards, because it is unverified.**
- **Off by default.** Menu, CPU AND MEMORY tab, "JIT: reuse page lookups" (`jit_page_reuse=on` in `settings.txt`); the quick profile "More
  FPS (test)" turns it on. Safe mode ignores it (it counts as experimental).
- **It cannot break the build.** `scripts/fetch-eden.sh` applies the pack with `git apply --check` first and skips it, with a warning,
  if it does not fit the pinned Eden. If it fits but does not compile, `ci/stage-eden.sh` takes it out again and builds a second time
  without it; the compiler's errors are kept in the `jit-pack-errors` artifact of that run. `patches/eden-jit/MODE` (`on` / `off`) or the
  variable `NXO_JIT_PACK` leave the pack out on purpose.
- **`boot.log` says what happened:** `JIT page reuse: ...` once at the start (off / on / "this build does not have the pack"), and
  again when the game has drawn and when it is left, with the number of groups formed and the accesses in them. Zero groups with the
  switch on means the pass did not run (the line gives the reason bits).

**Not ported from Eden4, and why.** The two-level table and the direct mirror (a different memory layout); the register allocator changes
(R13 and moves into callee-saved registers: a separate, riskier change to code every block uses); the code cache protection in windows
(this port's cache is already writable and executable, patch 0005, so there is no per-block protection to save); Eden4's diagnostics.

**What can still go wrong.** A wrong lookup is a game that hangs, closes or runs wrong, not a build error. A page that another thread
turns into a "callback" page in the few instructions between a group's lookup and its last access is read directly (the same race the
ordinary lookup has, with a window of up to 256 bytes of code). To check it: open a game with it off and on, look at the `JIT page
reuse:` lines and the speed, and send `boot.log`.

## [0.9.1] - The system flexible pool is no longer an experiment, and up to 4 controllers

**Not built or run on a console by the author of this change.** What was run: patch 0030 applied after 0029 and the lazy-memory file it
changes rebuilt from the patches and run on a PC against a model of the kernel calls (`tests/lazy_memory_test.sh`, 34 checks with
address and undefined-behaviour sanitizers); `memory_guard_test` (the probe's search checked for every pool size from 0 to 2300 MiB),
`menu_test`, `safe_mode_test` and `eden_settings_test` (stand-ins for Eden's settings); `ps4_platform.cpp` compiled against stand-ins
for the SDK (only the four errors that were already there); `tests/docs_check.sh`. `main.cpp` and the Eden-side controller connection
cannot be compiled or run without the pinned Eden and a console.

**What the 0.9.0 logs showed** (Odyssey, Mario vs. Donkey Kong and Mario Maker 2 came back from the console): the system flexible pool
exists on this console (firmware 12.02) and a probe of 640 MiB was accepted; the whole 512 MiB budget was used (`512 MiB in system
flexible memory`), the 128 MiB reserve was left, audio and the controller opened and worked; Super Mario Odyssey, which 0.8.5 lost at
1982 MiB of guest memory, ran on to a plateau of 2152 MiB with the largest free direct block at 93 MiB, 9-19 fps, and was left by choosing
Exit in the pause menu. Over the whole run the game gave back `0 MiB` of guest memory. Mario Maker 2 did not change (CPU-bound). Mario vs.
Donkey Kong runs at 10-26 fps; its video cutscenes are black.

**Changed: `sysflex_mib` is on by default and larger.**
- Default **auto** = up to 1536 MiB, still limited by the pool: the frontend now finds the pool's size with a **binary search** (the first
  question is cap + 128 MiB; a roomy pool answers it in one probe, otherwise about eight probes at most) instead of stepping down 32 MiB at
  a time. The 128 MiB reserve and the 32 MiB steps are unchanged. A refusal during the run still switches the pool off and direct memory
  takes over (0.9.0 behaviour).
- Values: `auto`, `off`, 256, 512, 768, 1024, 1536, 2048 in the menu. **`sysflex_mib=128` and `=384` in an old `settings.txt` (the 0.9.0 quick profile "Minimum RAM (test)" wrote 384) are read as auto**, with a line in `boot.log`; the 0.9.0 values were meant for testing the pool, not as a choice.
- **Safe mode** no longer ignores it. After a start that died before drawing the cap is limited to **512 MiB**, the amount a console has
  been seen to give, instead of being switched off. An explicit `off` is kept.
- The quick profile "Minimum RAM (test)" now uses 2048 (it was 384).
- **Not known:** how much the pool can really map at once (the 0.9.0 probe stopped at 640 MiB), and whether the kernel counts these pages
  against something else when 1.5 GiB are taken. The `boot.log` line `system flexible memory: cap N MiB, the pool could map M MiB at once
  ...` and the `in system flexible memory` figure of each `status:` line answer both. If a start dies, the next start is safe mode (512 MiB).

**New: patch 0030, the kernel's frees are counted.** `OnGuestRamFreed` now counts calls, bytes and the calls under 64 KiB (too small to
return a chunk). The `status:` line ends with `kernel frees N calls / M MiB (K under 64 KiB)`. Nothing about what is given back changes. The
Odyssey run gave nothing back, so the idea of freeing the previous area at each black loading screen does not apply from outside: the game
keeps its own heap and does not return it to the kernel. A different way would be needed for it (listed in the roadmap).

**New: controllers 2-4** (`players=1|2|3|4`, SYSTEM tab, "Controllers (players)"; default 1, which changes nothing).
- Eden has one virtual gamepad input per player slot (port = player index) and connects a slot that gets input while its `connected` setting is
  true. `Ps4Eden::ConnectPlayers` (in `eden_settings.h`, written as a `requires` check like the other settings, so an Eden without the field
  just reports "has no player settings") marks players 2..N connected.
- On the PS4, controllers 2-4 are the other **logged-in users** (`sceUserServiceGetLoginUserIdList`, looked up weakly like other calls: if
  the SDK's stub library has no such call, `boot.log` says so and only controller 1 works). A second DualShock logs in as a guest when its PS
  button is pressed; the app looks every 2 seconds, lights the pad (red, green, pink) and sends each pad to its own port. The pause menu
  (L3 + R3) and the quit combination stay with controller 1; nothing is sent from the others while the menu is open, and a pad that goes away
  gets one neutral state so no button stays pressed.
- `boot.log` lines: `controllers: players=N, ...`, `controllers: player 2 is user 0x...`, `controllers: player 2 connected`.
- **Not tested** on a console or inside Eden: the PC tests cover the settings and the menu only.

**Not in 0.9.1, and why.**
- **The JIT changes from Eden4** (Dynarmic): each is a patch to the pinned Eden's Dynarmic sources, and that source is not in this
  repository. A patch that fails to apply stops the whole build, so none was written blind. To do it, the run of the "Check patches" workflow
  produces the `eden-sources` artifact; with it, each change can be applied with `git apply --check` first.
- **Black cutscenes in Mario vs. Donkey Kong:** the log shows the video decoding; what is missing is where the picture is lost. No change was
  made without that. `vic_frame.raw` and `vic_frame.txt` in `/data/edenps4/` (the first decoded frame, saved by patch 0016) would show whether
  the decoded frame is real.
- **More fps for it:** the status lines show the CPU, not the GPU, as the limit (cores 43-70% busy while waiting). The `vsync` option
  (`immediate` / `mailbox`) can be tried; the driver may ignore it.
- **SpongeBob (Unreal Engine 5)** and **Mario Maker 2**: nothing changed; no cause found that a change here could address.

## [0.9.0] - Eden4 read in full, game memory in the system flexible pool, and a README that is checked against the code

**Not built or run on a console by the author of this change.** The new patch (0029) was applied with `git am --keep-cr` the way
`scripts/fetch-eden.sh` does, and the lazy-memory file it changes was rebuilt from the patches and run on a PC against a model of the
kernel calls (`tests/lazy_memory_test.sh`, 29 checks with address and undefined-behaviour sanitizers, identical over 12 runs). The
frontend change was checked by compiling `ps4_platform.cpp` against stand-ins for the SDK (only the four errors that were already
there). The other host tests pass. Nothing here proves that the console's system flexible pool exists on firmware 12.02 or how big it is.

**Eden4 analysed.** A new unofficial PS4 port of Eden, `Eden4-1.0.0-experimental`, was read part by part (see `docs/EDEN4.md`): what it
does, what this project already has, what was taken and what was not, with the reason for each. Short version: the memory idea was
taken; its Dynarmic JIT changes (the likeliest speed gain), its reclaim-and-retry in the Vulkan staging pool, a 16 MiB VMA block, its
HOME menu and a seventh-core package variant were not, because none of them can be verified without a console and some overlap this
repository's patches. They are listed there as next steps. The Eden4 author's name is not in its package, so it is credited by project name
(README, `NOTICE.md`).

**New: `sysflex_mib`, game memory in the system flexible pool (off by default).** Eden4 keeps its C heap in the PS4's *system flexible
memory*, a pool that is not part of the ~4.5 GiB of direct memory that the Odyssey run exhausted. Here the C heap is not in this repository
(it is `orbis-compat`), but the part that grows while a game loads is: the 64 KiB lazy chunks of guest RAM and of Eden's tables.
- **Patch 0029** lets those chunks take memory from the pool until a budget is used, then, or when the pool refuses a chunk, from direct memory
  as before (a refusal switches the pool off for the rest of the run). With no call or no budget nothing changes. A chunk is zeroed, as the
  direct path does, and given back with `sceKernelMunmap`.
- **The frontend** looks the call up (`sceKernelMapNamedSystemFlexibleMemory`: the SDK's stub library, else `sceKernelDlsym` on libkernel),
  probes how much the pool can map at once (hints `0x1000000000`, `0x800000000`, `0x600000000`, a block unmapped right after), keeps 128 MiB
  of it for the system, and passes the rest as the budget.
- **Menu and `settings.txt`:** `sysflex_mib=off|128|256|384|512` (CPU AND MEMORY tab, "Game RAM in system flexible memory"). The quick
  profile "Minimum RAM (test)" sets 384. Safe mode turns it off like the other experimental lines.
- **How to tell whether it worked, in `boot.log`:** `system flexible memory: cap N MiB, the pool could map M MiB at once, reserve 128 MiB ->
  budget B MiB for guest memory` at the start (or `not available on this console or SDK`, or `probe of N MiB refused`), and the `status:` line
  now says how many MiB of the committed guest memory are `in system flexible memory`. The largest free direct block in the same line should
  stay higher than before by about that amount.
- **Not known:** whether this firmware offers the pool to a homebrew process, how large it is, and whether the kernel counts these pages
  against something else. It is off so that a wrong guess costs nothing; turn it on and read the log.

**Changed: README rewritten** for what the project is now (it described v0.1.0), with the games tried and their measured speeds taken from
the logs, what is confirmed on a console and what is not, and the credits, Eden4 first among the new ones. **New:** `docs/SETTINGS.md`
(every option and profile, generated from the menu's own catalog by `tools/settings-doc.cpp`), `docs/EDEN4.md`, an Eden4 section in
`NOTICE.md`, `docs/INSTALL.md` brought up to date, and `tests/docs_check.sh`, which fails when the README, `docs/SETTINGS.md`, `NOTICE.md` or
`docs/EDEN4.md` disagree with the code (version, number of patches, options and profiles named, links, credits). The GitHub workflow that runs
the host tests cannot be changed by an update zip, so run it by hand: `bash tests/docs_check.sh` and `bash tests/lazy_memory_test.sh`.

**Still open (honest):** the console runs of 0.8.4, 0.8.5 and 0.9.0 are missing, the Odyssey gap is estimated at 270-470 MiB after 0.8.5 and
this option may or may not close it, the JIT work from Eden4 is not done, and naming the biggest heap owners needs the ELF of the build.

## [0.8.5] - The 256 MiB buffer found, and cut in half

**Not built or run on a console by the author of this change.** The patch was applied with `git am --keep-cr` (the way
`scripts/fetch-eden.sh` does) to a copy of the file taken from Eden's source; the lines it changes were also read at the
pinned commit `5f142c79` and are the same. No other patch touches that file. The host tests pass.

**Where the 256 MiB buffer comes from.** In the driver log of the Odyssey run, `BO 23 (262144 KiB, heap 0x6) is on GARLIC`
is the largest single GPU allocation. It is Eden's own **stream buffer** (`StagingBufferPool`, `vk_staging_buffer_pool.cpp`):
`MAX_STREAM_BUFFER_SIZE` is 256 MiB on every system except FreeBSD, where Eden uses 128 MiB (its comment says *NIX drivers
are more sensitive to big stream buffers). The PS4 is FreeBSD-based but OpenOrbis does not define `__FreeBSD__`, so the port got
the PC size. It is not in `orbis-compat`.

**Changed: patch 0028** adds `|| defined(__ORBIS__)` to that condition, so the PS4 gets the 128 MiB buffer Eden already
uses on FreeBSD: **128 MiB of direct memory back in every game**. The buffer is split into regions; a smaller one means a
frame can queue less upload data before it waits for the GPU, which is what Eden accepts on FreeBSD. If a game shows more
waiting on uploads than before (the driver's `WaitForFences` per-frame figure), say so and the size can be made a setting.

## [0.8.4] - The renderer is Vulkan, why Odyssey ran out of memory, and the game pausing before it does

**Not built or run on a console by the author of this change.** The host tests pass; the change to the program is one default
and its text.

**Which graphics API is it? Vulkan, and nothing else.** `ApplyPs4Settings()` sets `renderer_backend = Vulkan` before Eden
starts, `configure-eden.sh` builds Eden with `-DENABLE_OPENGL=OFF` (no OpenGL renderer exists in the program), and every log of every run agrees: the Eden log names `Render.Vulkan` and the device
"PlayStation 4 Liverpool (RADV ORBIS)", the driver log counts `QueueSubmit2`, `QueuePresentKHR`, `AcquireNextImage2` and
`WaitForFences` (Vulkan calls) per frame, and the profiles show the threads `VulkanWorker`, `VkPipelineBuilder` and
`VulkanPresent`. So the garbled pictures are not the OpenGL problems that emulators are known for; they come from the
Vulkan driver on this GPU (see the 0.7.x/0.8.0 notes for the geometry that goes missing) or from options that skip draws
(`async_shaders=on` skips a draw whose pipeline is not built yet: the Odyssey run had it on).

**The vulkan-ps4 repository (`vulkan-ps4-main`).** A Vulkan 1.0 driver of about 11 000 lines of C that translates Vulkan to the
console's GNM commands (PM4) over opengnm and a SPIR-V to GCN compiler (opengnm-psbc), MIT licensed. It is a different way
to reach the same GPU, not an upgrade for this port: Eden needs Vulkan 1.1+ and a long list of extensions that it does not
offer, while the driver already in use (Mesa RADV, Vulkan 1.3, found in the logs) offers them. Nothing in it is used by this
release. Its memory handling (`VkDeviceMemory` straight on the console's direct memory, two heaps) is the same model as the
arena in use here, so it does not show a way to need less.

**Why Super Mario Odyssey closed (boot.log of the run, 0.8.3, with `astc_recompress=bc1`, `resolution=0.75`, arena 1280).**
- At 59 s: guest 257 MiB, largest free direct block 1790 MiB. So about 2560 MiB are already taken before the game has used any
  memory of its own (driver arena 1280, C heap in 128 MiB carve-outs 640+, a 256 MiB GPU stream buffer, JIT caches, stacks).
- The guest then grew to 1687 MiB while the game loaded (+1430 MiB) and the free block fell to 58 MiB, at up to 1100 MiB a
  minute. `memory watch: low -> critical` came at 228.8 s and `CRASH signal 11` at 235.7 s with `1 failed commits`: a 64 KiB
  piece of guest memory could not be committed, because no direct memory was left.
- `0 MiB given back` the whole run: the game never gave guest memory back (its own allocator keeps what it took), so the
  give-back path (0.7.x) has nothing to return. Nothing was lost to it.
- Super Mario Maker 2 does not get there: its lowest free block in the two 0.8.3 runs was 396 MiB (BC3) and 503 MiB (BC1),
  the guest 1523 and 1288 MiB. Odyssey needs about 400-600 MiB more than this build leaves.

**How the original Eden handles it: it does not.** On a PC the 4 GiB of guest memory, the driver's memory and Eden's caches live in
the machine's RAM and, past that, in swap; the only thing Eden does by itself is the texture/buffer cache collector that keeps
GPU memory under a budget (this port already ties that budget to the driver arena, patch 0025). A console process with a hard
4608 MiB of direct memory and no swap has nothing to fall back on, so the way to run a game of that size is to take memory from
the fixed part (the 2560 MiB above), not to wait for the game to give it back.

**Changed: `memory_action` is now `pause` by default** (it was `warn`). When the largest free block falls below 64 MiB
the game is paused with a choice (continue anyway, restart the game, back to the game list) instead of the app
closing about 7 s later. `memory_action=warn` in settings.txt (or "Só avisar" in the menu) brings the old behaviour back. It
falls back to the warning when the overlay is not ready. The Mario Maker 2 runs never went below 396 MiB, so they are not
affected.

**What to try for Odyssey now (nothing here is new code, only the settings that remove the most):** the "RAM mínima (teste)"
profile (arena 1152 MiB, BC3, 0.5x, no driver cache, shader preload 256, thumbnails only), and `async_shaders=off` (the default) so
no draw is skipped while a shader builds.

**What is still needed to shrink the fixed 2560 MiB (not done, and why).** The heap census names its biggest owners only by
addresses in the program (`eboot+0x67dc <- 0x61f1d7 <- 0x66448c`: 96 MiB in one block, 4 x 16 MiB twice, 24 MiB twice): turning
those into names needs the matching ELF of the build, which the logs do not carry. The C heap's carve-out cap and the
256 MiB stream buffer are in `orbis-compat` and in the driver, which are not in this repository.

## [0.8.3] - What the 0.8.2 run says about the CPU, ASTC textures kept compressed, and a profile that adds up the cores

**Not built or run on a console by the author of this change.** All host tests pass; `ps4_platform.cpp` was syntax-checked
against stand-ins for the SDK (only the errors that were already there). Every new option is off by default.

**What the 0.8.2 run shows (Eden and driver logs of the run; its own boot.log was not among the files: the one sent is the
next start, which stops at the menu).**
- **There is no error of the emulated CPU in it.** The lines marked Error in the Eden log are three `BSD_USA: Network isn't
  initialized`, two `Unknown engine name: joycon` (a controller driver this build does not have) and one `GetHostIPv4Address:
  no interface` (the game looks for a network the console has not given it). None of them touches the CPU emulation or the
  speed, and nothing like an unmapped read or write, a JIT failure or a crash is there.
- **The limit is still the speed of the emulated CPU.** The driver's own accounting over the last 5 s windows: 3.1-3.6 cores
  burned, **0 ms waited for the GPU**, 462-503 draws and 105-125 render passes a frame, a frame of 75-139 ms. Nothing in the
  files says the GPU is late; everything says the processor is busy. It is not a fault that can be fixed by correcting a
  line: the guest's own code, run by the JIT, is what takes the time (earlier profiles: one guest core 85-88 % busy, 90-95 %
  of it inside JIT code).
- **`THE GPU HAS NOT FINISHED submit #8800` was not a hang.** The driver's counters go on after the dump (2688 -> 2752
  frames shown, further 5 s budget lines), so the GPU did finish. It is the driver's "four submissions waiting" rule firing
  during the same scene change that freezes the picture for ~3.4 s.
- **Where the 3.1-3.6 cores go is not known.** The profile of 0.8.0 explained about 2.0 cores (guest cores 25 + 88 + 77 %,
  GPU 6 %, Vulkan worker 2 %) at 41 fps in the title; the driver's figures are for the slow scene, which no profile covered.

**New: `profile total:`.** The sampling profiler used to look at 8 threads. It now samples every thread Eden names (24 slots;
the guest cores, GPU, Vulkan, file system and GPU services always get a slot) except the audio threads and the motion client,
and the report starts with one line that adds the busy shares up: `profile total: N.N cores busy in the M threads sampled
(CPUCore_1 88%, VkPipelineBuilder 40%, ...)`. A thread that was idle in the window writes nothing else; a busy one writes its
hot code as before. Compare that number with the driver's `BUDGET ... cores busy` for the same seconds: whatever the first
leaves out is in a thread this still does not sample. Each thread is now sampled about 10 times a second instead of 30, which
is plenty for a busy share.

**New: ASTC textures kept compressed** (`astc_recompress=off|bc3|bc1`, Eden's `astc_recompression`). The PS4 cannot read ASTC,
so Eden decodes every ASTC texture on the CPU and keeps it as RGBA8 (4 bytes a pixel). With BC3 it recompresses the decoded
picture on the CPU (with `stb_dxt`, on Eden's worker threads) and keeps 1 byte a pixel; BC1 keeps half a byte (alpha becomes
all-or-nothing). That is a quarter or an eighth of the texture memory, so the texture cache (a 1280 MiB arena that starts
collecting at 704 MiB) holds four to eight times as many textures before it throws some away and has to decode them again. The
cost: CPU time at each upload (the CPU is already the limit), and a little quality (gradients, BC1 alpha). Not tried on a
console: it needs the PS4 driver's BC1/BC3 formats, which GCN supports, but this was not seen working here. The "Minimum RAM"
profile now includes BC3. In safe mode (0.8.2) any BC setting is ignored for one start after a start that died.
How much of SMM2 is ASTC is not known: the title's textures in this log are all small (at most 1280x720).

**Asked for and not done.** A faster ASTC decoder or a GPU one: Eden's GPU decode (compute shaders) already lost the GPU twice on
this console (see 0.4), and a faster CPU decoder is a change to Eden that cannot be checked against the pinned commit. **Fastmem**
is still the one change that would lift the CPU limit, and it is not in this repository.

**To test.** Run SMM2 once with the options as they are, to get the `profile total:` lines (send `boot.log` and `boot.old.log`),
then once with `astc_recompress=bc3` and compare the title's fps and the `status:` memory lines.

## [0.8.2] - More options for speed and for memory, and a safe mode

**Not built or run on a console by the author of this change.** All host tests pass (the menu test grew to 139 checks, the
Eden-options test knows the three new settings, and `safe_mode_test` is new); the snippets of `main.cpp` that this version
touches were compiled on their own with warnings on, so the GitHub build is their first real compile against Eden. **Every
new option is off by default: a start with the settings of 0.8.1 behaves exactly like 0.8.1.**

**What this version does NOT do, so nobody hopes for it.** The logs say Super Mario Maker 2 is limited by the emulated CPU
(one guest core busy about 88 % of the time, the GPU never waited on). Nothing here gives the PS4 more CPU: the single
change that would (fastmem, a direct window onto the game's memory) lives in a branch that is not in this repository and
cannot be written blind. The GPU-side options below help a game that is limited by the GPU, and much less one that is
limited by the CPU. Nothing in Eden's source of this version lets the port shrink the big buffers (the 256 MiB stream
buffer, the driver's image memory) either: that needs a patch to Eden that could not be checked against the pinned commit,
so none was written.

**New options for speed** (each one is applied only if the pinned Eden has the setting, as in 0.7.4: otherwise `boot.log`
says "this Eden has no such setting" and nothing breaks).
- **Emulated CPU clock** (`cpu_clock=normal|boost|overclock`, Eden's `fast_cpu_time`). In Eden the guest's time counter is
  multiplied by 1734/1020 (boost) or 2040/1020 (overclock). It changes the time the GAME reads, not the work the PS4 does:
  a game that limits its own FPS with timers may run freer, another may run its audio or physics fast, or hang.
- **Custom CPU ticks** (`cpu_ticks=off|1000|4000|16000|21000`, Eden's `use_custom_cpu_ticks` + `cpu_ticks`). Replaces the
  cycle count the emulated CPU adds after each run of code with a fixed number, which changes how often the game's threads
  go back to the scheduler. Eden suggests 77-21000 and warns of hangs. Which value helps on this PS4 is not known.
- **Asynchronous presentation** (`async_presentation=on|off`). Presents frames from a separate thread (off on Eden for
  PCs). On the PS4 presentation goes through the video driver and the HUD hook, so it may flicker or hang.
- **Profile "More FPS (test)"**: 0.75x resolution, low GPU accuracy, no reactive flushing or synchronised memory, inner CPU
  invalidations skipped, asynchronous presentation, no extra filters. A starting point to compare, not a promise.

**New options for memory.**
- **Covers in the game list** (`covers=full|thumbs|off`, read when the app starts). The decoded covers (up to 64 MiB) sit in
  the C heap while the menu is open and part of it may stay mapped when the game starts; `thumbs` keeps only the small
  tiles, `off` decodes nothing. This is the one memory saving that is entirely in the port's own code.
- **Profile "Minimum RAM (test)"**: 0.5x resolution (the GPU images that live outside the driver's arena take a quarter of
  the space), 1152 MiB arena, conservative VRAM, a shader cache loaded up to ~8 MiB (what is over is kept as
  `vulkan.bin.bak`, never deleted), no driver cache, the tight memory profile, a pause when memory turns critical, and the
  game list with thumbnails only. The picture gets much softer.
- The options that already existed (arena size, memory profile, shader preload, driver cache, VRAM mode, the pause on
  critical memory) are unchanged; the new profiles only combine them.

**Safe mode.** `safe_start.txt` is written when a game starts and removed when the game has shown 120 pictures or when
the game is left on purpose. A start that finds it knows the last one died before drawing anything, and for that one
start ignores the experimental lines of settings.txt (`cpu_clock` above normal, `cpu_ticks`, `async_presentation=on`,
`skip_cpu_invalidation=on`, `auto_stub=on`, `astc=gpu|async`, `dma_accuracy=unsafe`, `memory_profile=strict`, textures
inside the arena). boot.log lists each ignored line, and the screen says so for 15 s. settings.txt is not changed: the
next start that gets past the first pictures puts everything back. Known limit: a start ended by hand before the first
picture looks like a crash, and costs one start without the experimental lines.

**To test (one thing at a time, and send boot.log back).** Look at `tl:` and `status:` lines (FPS) and the "largest free
direct block" in `status:` (memory). Compare Default, "More FPS" and each option alone; the HUD (L3 + R3) shows the FPS.

## [0.8.1] - The scene-change stall is the game loading (0.8.0's guess withdrawn), and a profile of every freeze

**Not built or run on a console by the author of this change.** The host tests pass (all of them, `video_hook_test` with
the new `SinceLastFlipUs` checks); `ps4_platform.cpp` was syntax-checked against stand-ins for the SDK (only the errors that
were already there), and the new loop code of `main.cpp` was compiled on its own with warnings on, so the GitHub build is
its first real compile. **No graphics change was made, on purpose: the logs say the grey terrain / missing text of Super
Mario Maker 2 is not a rendering error of the port, and the way to make it vanish is speed (the next versions).**

What the 0.8.0 run showed (boot log, `eden_` and `mesa_` logs of that run; the three screenshots the version took by
itself were not among the files, so the pictures have not been looked at). The automatic screenshots and the `stall:` lines
worked on the console: a 3.45 s freeze ended on flip #2470, and the three pictures were saved 0.7, 2.8 and 6.3 s after it.
- **The 0.8.0 guess is wrong.** 0.8.0 said the stall is the first use of the driver's geometry-shader path. This run says
  otherwise: the 80 pipelines came from the shader cache and **no pipeline was created** from the 96th second to the end;
  the driver says **it waited 0 ms for the GPU in every 5 s window**, and the window that holds the stall shows **3.3 cores
  busy**; the Eden log has **no line at all between 96.3 s and 125.8 s** (no image, view or pipeline made while the picture
  was frozen); the `GS ring ... allocated` lines of the driver come **after** the stall, at the first geometry-shader draw of
  the new scene; and the `GPU HAS NOT FINISHED` dump of the earlier runs did not appear this time although the freeze did.
  The freeze is CPU work of the game (it loads the next demo course: 43 MiB more memory committed, three cores busy), and
  its length is the same each time (3.45 s, 3.40 s, 3.45 s) because it is the same amount of work. It is the speed of the
  emulated CPU, not a fault.
- **The missing layers appear when the game draws them.** Eden makes an image view the first time a draw samples a texture.
  The text textures of the title ("Press ZL + ZR" and the version line: BC4 images of 370x100 and 512x32) get their first
  view about **4 and 9 frames** after the first frame of the new scene, so nothing sampled them before: the game does not
  draw that text in its first frames (the frames of that phase take 0.1-0.6 s each: 313 draws and 34 render passes per
  frame, 115 ms just to record). At 60 fps the same 9 frames are 0.15 s, which nobody sees; at 5-10 fps they are 1-2 s.
  So the grey terrain and the missing text are most likely the game's own order of drawing, slowed down, not something the
  port draws wrongly. This is an inference from the logs, not a proof: the screenshots would confirm it.
- **Memory is tight but steady.** The largest free block went from 1959 MiB at the start to 1124 MiB at 82 s and 508 MiB at
  132 s (the game's memory 88 -> 1283 MiB, plus six 128 MiB carve-outs of the heap). The warning level is 160 MiB; the
  editor took it to 80 MiB in an earlier run. Worth a look in the performance work.

**New: a profile of every freeze.** 0.6 s after the flips stop (and not while the pause menu is open), a fresh window of
the sampling profiler is opened (`Ps4::ProfileWindow(false)`); when the picture comes back the window is written to
boot.log (`Ps4::ProfileWindow(true)`): `profile [CPUCore_n]`, `[VulkanWorker]`, `[GPU]`, and now also `[FS]` (the file
system service: reads and decrypts the game's files) and `[nvservices]`, each with its busy share and hottest code buckets.
Three per run; a gap made by the pause menu or a screenshot is not profiled. `stall:` lines say when it starts and ends.
The profiler's usual 30 s report is unchanged, but it now samples eight threads instead of six.

**What to send next (or not):** nothing is needed to go on with the performance work; the next stall profile in any boot.log
says where the 3.4 s go. The `shots/` pictures of a stall would settle the pop-in inference.

## [0.8.0] - What the Mario Maker 2 logs show, and a freeze detector with automatic screenshots

> **Corrected by 0.8.1:** the bullet below that blames the stall on the first use of the geometry-shader path is withdrawn.

**Not built or run on a console by the author of this change.** The host tests pass (all of them, `video_hook_test` with
the new stall checks). The new code in `main.cpp` was checked by compiling it on its own with warnings on, not inside the
whole program, so the GitHub build is its first real compile. **The grey terrain of Super Mario Maker 2 is NOT fixed in
this version**: its cause is not proven, and a change made without proof would be a guess. What is proven is below, and
this version adds the way to settle the rest in one run.

What the Mario Maker 2 run of 0.7.10 showed (boot log, `eden_` and `mesa_` logs of that run, and the two screenshots of
the run before). Archiving the logs per run (0.7.10) worked: this time the Eden log of the right game arrived.
- **The defect is of the first seconds of a scene, and it goes away by itself.** The two screenshots show the same title
  demo with the course timer at **500** (terrain flat grey, no "Press ZL + ZR", no "Ver. 1.0.0") and, 3.2 s later, at
  **499** (everything right). The grey is the dark shadow layer of the terrain; the textured layers (the tiles and the
  text) are the ones missing. At 10 fps, 3 s is only 30 frames of the game, so the defect is visible 6 times longer than
  it would be at 60 fps.
- **It is not only the asynchronous shaders.** That run had `async_shaders=on` (chosen in the menu): the scene change
  created **55 pipelines in 0.16 s** and, with the builder threads competing with the game for the 8 cores, they were all
  built only after about 35 s (shaders built 3, 35, 60, 80). Draws wait for their pipeline with that setting, so missing
  layers are expected with it on (the menu text says so). But the earlier run with it **off** and 187 shaders already built
  showed the same defect, so it is not the only cause. Off stays the default.
- **A 3.4 s stall comes with the scene change, and the driver log says where.** boot.log measures it twice: 3.45 s (the
  run of the two screenshots) and 3.40 s (this run). In the two driver logs that cover a scene change in full (the run of
  1 November and this one) the same lines appear close together: `GS ring 1048576 B requested -> 16777216 B allocated`
  (the first geometry shader of the process; two more logs show the same ring lines), then `THE GPU HAS NOT FINISHED submit
  #7423` (`#7627` in this run): the command buffer stays unfinished for 4 submissions. The Eden log shows **6 pipelines
  with a geometry shader (points)** created at the same instant. So the stall is in the first use of the geometry-shader
  path of the driver, which is not in this repository. Whether the missing layers are drawn by those pipelines is a
  hypothesis, not a finding.
- Nothing else is wrong in the logs: no Eden error, no shader compile error, the texture self-tests (BGRA, R8G8, A2B10G10R10)
  pass. Memory: the largest free block fell to 80 MiB ("low") in the editor and stayed there (-2 MiB/min at the end), the
  game's own memory peaked at 1519 MiB: tight, not leaking.
- `no new frame for 15 s during running` at the start is the game loading (46 s with no picture); the thread dump it
  prints is expected there.

**New: freeze detector and automatic screenshots.**
- `Ps4Hook::LastStall()` remembers the last time two flips were 1 s or more apart (length, end time, flip number).
- boot.log gets `stall: the picture froze for N s and came back on flip #F`. The driver log counts the same frames in its
  `scan-out copy ... over N frames` lines, so the two logs can be lined up, and its last `GPU HAS NOT FINISHED` line
  belongs to that freeze. A gap made by the pause menu or by a screenshot says so and is not counted as a freeze.
- After each of the first **three** freezes of a run, three screenshots are taken by themselves, 0.4 s, 2.5 s and 6 s after
  it (`shots/shot_NNNN.bmp`, no notice drawn on them). They show how the start-of-scene defect evolves without anyone
  pressing L3 + TouchPad at the right moment. `autoshot=off` in settings.txt turns it off.

**What to send next:** play Super Mario Maker 2 up to the title screen and wait for the demo to restart (about a minute),
then send `shots/`, `boot.log` and the newest `logs/eden_*` and `logs/mesa_*`. With `async_shaders` off.

## [0.7.10] - A menu with colours and icons, covers from inside the games, logs that are not lost

**Not built or run on a console by the author of this change.** The host tests pass (`menu_test`, `video_hook_test`,
`image_decode_test` with sanitizers, and the rest). The new code in `main.cpp` that opens a game dump was checked only
by compiling it against the headers of an Eden master (with stand-ins for `fmt`), not against the Eden the build pins,
so the GitHub build is its first real compile.

What the 0.7.9 run on the console showed (boot log of the Super Mario Maker 2 run, two screenshots, the driver's log):
- The title screen runs at 29-39 fps; after a 3.4 s stall (a scene change) it drops to 9-13 fps. In that scene the driver
  counts **28-34 render passes and 290-320 draws per frame** (against 6 passes and 16 draws before), waits **0 %** for
  the GPU, and the busiest emulated CPU core is 88 % busy: the slow part is the game's own thread, not the GPU. This is
  the "9-13 fps in the editor" and it is a CPU matter (the performance work comes next).
- The two screenshots (L3 + TouchPad works; the picture shows the HUD and the "Print salvo" notice) show the same title
  screen in two scenes. The airship scene is drawn right. In the other scene the **terrain is a flat dark grey** and the
  **"Press ZL + ZR" text and "Ver. 1.0.0" are missing**; everything else (title, Mario, coins, clouds, door) is fine.
  **The cause is not found yet**: the `eden_log.txt` that came with the run is the log of another game (SpongeBob), because
  Eden's log was rotated twice at every start and the Mario Maker 2 one was overwritten. Nothing is changed in the
  graphics in this version, on purpose: a guess without that log would be a coin toss.
- That SpongeBob log has 2,756 "Unmapped Read/Write" lines at the start (the game writing into its own heap, which is
  not mapped). Not looked at yet; noted for later.

**New: the menu.**
- Seven colour themes (blue, green, purple, orange, pink, red, grey): SYSTEM tab, "Cor do menu" / "Menu colour", applied at
  once and kept by "restore defaults". The HUD and the pause menu use the same colours. Saved as `menu_theme=` in settings.txt.
- Icons drawn in code (no image files): controller, monitor, chip, sliders, gear, document, gauge on the tabs.
- The tab bar and the title line follow the theme.

**New: covers from the games themselves.** At the start the menu opens each `.nsp` / `.xci` with Eden's own classes,
finds the control data and takes the icon (a 256 x 256 JPEG) out of it with your `prod.keys`; no emulator core runs.
- The first read of each game is kept in `cache/icons/`, so the next start does not open the games again.
- A picture in `covers/` (title id, file name or title) still wins, to replace a cover.
- boot.log: `covers from the games themselves: N read (M from cache/icons), F failed, ...`, and `cover of <file>: <why>` for
  the first failures (no keys, an update-only file, an unreadable dump). A game with no cover gets the coloured tile.
- Limits: 12 s for all the games, 64 MiB of covers.

**New: logs that are not lost.** Besides `boot.log`, the logs of Eden (`log/eden_log.txt`) and of the driver (`mesa.log`)
of the run before are moved to `logs/` before they are replaced: `logs/eden_<time>.log`, `logs/mesa_<time>.log`, `logs/boot_<time>.log`,
the newest 8 of each. (Smaller than 2 KiB, a menu-only run, they are not kept.)

**Still open (honest):** the grey terrain and the missing text in Mario Maker 2, the speed in the editor, the memory use.
To go on with the graphics I need the `logs/eden_*.log` and `logs/mesa_*.log` of **one run of Mario Maker 2 that shows the
grey terrain**, and the picture from `shots/`.

## [0.7.9] - Covers in the games list, and a log that tells a slow game from a stuttering one

**Not built or run on a console by the author of this change.** Everything below passes the host tests
(`menu_test`, `video_hook_test`, `image_decode_test` with sanitizers); `main.cpp` is checked only by the
GitHub build, and `ps4_platform.cpp` was only syntax-checked. Everything in 0.7.8 (L3 + TouchPad picture) is included.

**New: covers.** The JOGOS / GAMES tab is a two-pane screen: the list on the left with a thumbnail for each
game, and on the right a card with the selected game's big cover, its title, its size and its title id. A game
with no picture gets a coloured tile with its initials (the same colours every time), so the list is never bare.
- Put pictures in `/data/edenps4/covers/` (the folder is created at the first start). The name is the **title
  id** (`01009B90006DC000.png`), the dump's **file name** (`Super Mario Maker 2 [01009B90006DC000][v0].jpg`) or the
  **title** (`Super Mario Maker 2.png`). PNG, JPG and BMP; upper/lower case does not matter.
- boot.log says what happened: `covers: N picture(s) in covers/, M of K game(s) got one, F failed (first: file: why) ...`.
- Limits so a wrong folder cannot stall the start: 12 MiB a file, 4096 px a side, 64 MiB of covers (past that
  only thumbnails), 8 s of decoding.

**New: a log built to answer the open problems.**
- `tl:` lines every 2 s: flips in the window, the average and longest interval between flips, and how many were
  longer than 34 / 50 / 100 / 250 ms. 9 fps made of a steady 110 ms is a slow game; 9 fps made of 30 fps and
  long stalls is a stutter. (For the Mario Maker 2 editor speed and the HUD.)
- `logs/boot_YYYYMMDD-HHMMSS.log`: the logs of the last 8 runs are kept (every Exit, Restart and game start
  rotated the log, so the game's log was lost after two starts), and the start of each run is stamped with the date and time.
- Still there from before: `status:` every 10 s (fps, speed, memory), the hook line, the heap census, the Mesa
  log's BUDGET lines.

**Still open (honest):** the HUD "blinking", the Mario Maker 2 theme graphics, the memory use and the speed in the
editor are **not fixed** in this version, only better measured. To go further I need, from one run: boot.log and the
`logs/` folder, `mesa.log`, `log/eden_log.txt`, 2-3 pictures from `shots/` of the glitch, and the ELF (the
`NX-on-Orbis-elf` artifact of the build) to turn the profile addresses into function names.

## [0.7.8] - "Print screen" inside the app, and a false "no new frame" alarm gone

**Not built or run on a console by the author of this change.** The host tests pass (every file under
`tests/`, with new cases in `video_hook_test` for the picture file); `main.cpp` is checked only by the
GitHub build, and the new `ps4_platform.cpp` lines are two small changes to an existing function.

What the 0.7.7-fix run on the console showed (boot.log, boot.old.log, eden_log, mesa.log of 2026-11-01):
- **"Exit game" works.** `exit chosen in the pause menu` -> `sceSystemServiceLoadExec("/app0/eboot.bin", {NULL})`
  -> the next start logs `restart: way 0 (/app0/eboot.bin) brought the app back by itself in 3 s` (and 2 s the
  time after) and opens on the game list. So the first way of the ladder works on this console. "Restart game"
  does not appear in these logs: it is still unseen.
- The shader cache works: `148 pipelines loaded in 0.5 s`, no stall, nothing crashed.
- The HUD was in the buffer on screen at every one of 3,467 flips (`0 overlays found missing right after a
  flip`, 23,475 rewrites while shown). Nothing in any log shows the HUD missing. In the phone video of 0.7.6
  the HUD box is present in all 125 frames; what changes is the brightness of the **whole picture** (a patch of
  sky goes between 87 and 190 on a 0 to 255 scale about once a second), which a phone camera on a TV can do by
  itself. A video cannot tell the two apart, which is why this version adds a way to look at the real pixels.
- Super Mario Maker 2 runs at about 38 fps in its menus, then 9 to 13 fps once the editor is used. Over those
  seconds the Vulkan driver reports 105 to 119 render passes and 500 to 850 draws per frame (against 4 to 6
  passes before), the game's main CPU core (`CPUCore_1`) is 92 to 96% busy in translated game code, and the
  CPU waited 0 ms for the GPU. `shaders built` stays at 187 from 182 s on while the speed stays at 12 to 13,
  so it is not shader compiling. Asynchronous shaders were off in this run (the 0.7.7-fix default), so whether
  the theme graphics are still wrong is not something these logs can answer.
- The driver's `THE GPU HAS NOT FINISHED submit #7423` message came with a dump of the command stream (about
  8,500 lines of `mesa.log`). The frame did finish and the game went on; the dump is the driver's own
  diagnostic and not something this app prints.

### Added: L3 + TouchPad saves a picture of the screen
- Press **L3 + TouchPad** (not R3, not Options) while the game runs: the game pauses for a moment (a 0.3 s wait
  for the frames already in flight, then the copy), the picture on screen is saved as
  `/data/edenps4/shots/shot_0001.bmp` (then `_0002`, and so on; nothing is overwritten) and the game goes on.
  A line in the bottom left says where it went for four seconds, and `boot.log` has a `screenshot:` line.
- It is the buffer that was flipped last, **with the HUD as it is in that buffer**. The colours are read the
  way the console reads the buffer (the same way the HUD's own colours are chosen: its dark blue box is
  dark blue on the TV), so the picture looks like the TV. 24-bit BMP, 1920x1080, about 6 MB each; every
  viewer opens it.
- Use: press it **while the glitch is on screen** (a Mario Maker 2 theme with flat silhouettes, the HUD
  "fading"), and send the files from `shots/` with the logs. A picture that shows the HUD missing would
  settle that question; a picture with the HUD intact while the phone video "blinks" would settle it the
  other way.
- Not known: how long the copy takes on the console (the buffers can be slow memory to read; the time is in
  the `screenshot:` line), and whether L3 + TouchPad collides with anything a game does with those buttons.

### Fixed: a "no new frame for 15 s" alarm 3 s after the game started
- The menu draws a frame only when something changes, so the last frame counted was old when the game began,
  and the watch (15 s) reported a stall, with a dump of every thread, 0.6 s after `== running`. A stall is
  now counted from the moment the watch is armed at the earliest. Mario Maker 2 does take about 30 s to its
  first frame, so a dump at 15 s of that is still possible and still honest.

### Not changed, and why
- **The HUD**: the cause is not known. 0.7.7-fix writes it at every flip and again for 40 ms; the next
  logs and pictures say whether anything is left.
- **Mario Maker 2 themes**: see above; no new evidence either way. With the pictures it can be seen.
- **Speed in the editor** (9 to 13 fps): the game's main thread is the limit. Making it faster is a job on the
  CPU emulation (Eden's own code), which needs the build's ELF to read the profile addresses
  (`v0.7.7-fix-20261101T124718Z-03796441cd1e`); the logs print addresses like `0x790c00` that mean nothing
  without its symbols.
- **RAM**: unchanged; the largest free block ended at 561 MiB with the game's committed memory at 1,486 MiB,
  and nothing crashed. The 1,280 MiB driver arena, the game's memory and the heap are all in use.
- **`env=ORBIS_VRAM_GARLIC=0`** is still in the `settings.txt` of the run. The driver prints that it "cost
  this title two thirds of its frame rate" and is "for comparison runs only". One run without it, with a
  picture taken while the HUD is on screen, would show if the HUD still holds in the faster memory.

### How to read the next boot.log
- `screenshot: saved ...` / `screenshot: NOT saved (...)` with the time it took.
- `restart:` lines (the ladder) when "Restart game" is used.
- The `video hook:` line as before (`erased after a flip`, `rewrites while shown`).

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
