# The optimisation lists (0.9.6 and 0.9.7), item by item

The first list (0.9.6, six parts) is below, from item 1 to 6; **the second list (0.9.7, three parts and a texture fix) is the section "The second list (0.9.7)" after item 6.**

The list that came with 0.9.6 has six parts. Each one was checked against what this port already is (Eden `5f142c79` on Mesa RADV, run through
OpenOrbis) and against what the console runs have shown (`docs/STATUS.md`, `docs/TECHNICAL.md`). The rule was: do what applies and can be done
without a way to test it blind; adapt what applies in another form; say plainly, with the reason, what does not apply or is already there.

**Nothing in 0.9.6 has run on a console.** What changed is small on purpose (thread placement, a memory table, one CPU log line); the rest of this
page explains why the other items are not changes.

| # | Item | Verdict | What 0.9.6 does |
|---|---|---|---|
| 1a | Direct GNM, no GNMX | **Does not apply** | nothing (see below) |
| 1b | ASTC on the GPU with compute shaders | **Already there, off on purpose** | nothing: `astc=gpu` is still in the TESTS tab |
| 1c | Onion for dynamic buffers, Garlic for images | **Already how the driver and Eden do it** | nothing |
| 2a | Fastmem | **Not in this repository; its cost was measured** | nothing: the JIT pack's direct table is the cheaper form |
| 2b | Core affinity | **Done** (new) | `cpu_affinity` |
| 2c | SIMD (AVX / SSE4.2) | **Already how Dynarmic works** | a `boot.log` line proves the premise |
| 3a | HLE of the Horizon kernel | **Already how Eden works** | nothing |
| 3b | Audio on its own core, skipping frames | **Placement done; skipping is already how the sink behaves** | the audio thread is placed with the rest |
| 4 | Memory arenas, fixed budget | **Already the design; now written out** | `memory arenas:` table in `boot.log` |
| 5 | Frame fence | **Already how Eden's Vulkan backend works** | nothing |
| 6 | Linear allocators for temporary data | **Already how Eden's Vulkan backend works** | nothing |

## 1. GNM and the GPU

**1a. Direct GNM.** The renderer is Eden's Vulkan backend on Mesa **RADV**, the Vulkan driver for AMD GCN GPUs. RADV already is the thin layer the list asks
for: it compiles shaders to GCN code itself and writes the GPU's command packets (PM4) into its own command buffers; there is no GNMX in the way and no
generic translator between Eden and the GPU. Replacing it with a GNM backend would mean a new renderer for Eden (a different API, a different shader
translation from Maxwell to GCN, all of Eden's draw, compute, texture and presentation paths again), not a setting, and it would give up the driver
fixes this port already depends on (patches 0009, 0010-0014, 0015, 0033). It is not part of a version that has to be stable. The measured limit of this
port is the emulated CPU (`docs/STATUS.md`), not the driver's overhead.

**1b. ASTC with compute shaders.** Eden has the compute decoder; the PS4's GPU cannot read ASTC, so the choice is real. It was tried: the comment at the ASTC default in `frontend/main.cpp` and
`docs/TECHNICAL.md` record that decoding ASTC with Eden's compute pass **lost the GPU twice** in Mario Kart 8 while a race loaded, and `docs/STATUS.md` (test 10)
that the asynchronous CPU mode crashed (an upload into an image already destroyed). The default is therefore the synchronous CPU decode (`astc=cpu`); `astc=gpu`
and `astc=async` are experimental (TESTS tab, test builds only), and `astc_recompress=bc3|bc1` (0.8.3) keeps the decoded textures at a quarter or an eighth of
the memory. Writing the decoder again in PSSL does not apply either: the shader language of this stack is SPIR-V through RADV, and the decoder that exists
is the one that hung the driver. It stays an option for a console run, not a default.

**1c. Onion and Garlic.** The GPU's memory types come from RADV: host-visible memory for buffers the CPU writes (staging, stream, uniforms: Eden asks for
`Upload`/`Stream` memory), device-local for images and render targets. The driver's own arena is `ORBIS_ARENA_MIB`; images it places outside the arena use
Garlic (`ORBIS_VRAM_GARLIC`, and `memory_profile=strict` forces them inside the arena at a cost in GPU speed, as the driver warns). Eden's stream buffer is
128 MiB on this port (patch 0028). Choosing the pools by hand would mean patching the driver, not the emulator.

## 2. CPU and the operating system

**2a. Fastmem.** Eden has fastmem; the port's `experimental-fastmem` branch (tests 25-29, `docs/STATUS.md`) made it work on the PS4 with an 8 GiB view. It was
**not kept**: ~370,000 slow-path redirects per session (a 16 KiB host page against four 4 KiB guest pages: a host page that cannot alias four consecutive
backing pages is left unmapped, and each fault compiles a block again), and ~128 MiB more of C heap, which was enough to run direct memory out while a race loaded.
That branch is not in this repository, and rebuilding a fast path that took five console tests to understand, with no console, would be the opposite of a stable
version. What 0.9.2 and 0.9.3 did instead is the same saving by another road: the JIT pack's page reuse and **direct table** (4 instructions fewer per guest memory
access, no signal handler, 8 bytes per mapped page), on by default since 0.9.5 with safe mode as the net.

**2b. Core affinity. Done: `cpu_affinity=auto|off`** (`frontend/thread_placement.h`, `ps4_platform.cpp`). The four threads that run the Switch's cores
(`CPUCore_0..3`) each get a PS4 core of their own; every other named thread (GPU, Vulkan worker, presenter, audio feeder, file system and GPU services, shader
builders) is kept on the cores that remain, so none of them lands on a core running guest code. How it decides:
- the console is asked which cores the process may use (`scePthreadGetaffinity` of the main thread); if it cannot tell, **nothing is pinned**;
- at most six cores are used (an application gets six of the eight), and at least five are needed (four for the game, one for the rest);
- it follows the cores the console allows (`0x7C` means cores 2 to 6), not fixed numbers;
- the main thread and unnamed threads are left alone; a pin that the console refuses is counted and ignored;
- `boot.log` says what it found (`thread placement: ...`), gives a line for each of the first 20 threads with the core each ended up on
  (`sceKernelGetCurrentCpu` asked from the thread itself), and the setting is in the CPU AND MEMORY tab ("Núcleos da CPU fixos").
- The kernel functions are found by name in libkernel (`sceKernelDlsym`, like the system flexible pool's), so a missing function cannot break the link.

It is **a guess about the PS4's scheduler**, not a measurement: `docs/STATUS.md` put thread placement at "a few percent". Pinning can also lose (a guest core
shares its core's time with the system in a way the plan cannot see). So it is a switch, per game too (R2), a start after a crash runs without it, and the first
console run decides the default: compare the FPS with `cpu_affinity=off`.

**2c. SIMD.** The JIT is Dynarmic's x86-64 backend: it translates the Switch's NEON instructions to the SSE/AVX forms the host has, chosen at run time from
CPUID, and the whole build is `-march=btver2` (Jaguar: SSE4.2, AVX, F16C, BMI1, no AVX2/FMA3). There is nothing to add on the emulator's side. What was not known is
whether the console's system saves the AVX registers for a thread (without that, Dynarmic would see no AVX): 0.9.6 writes one line at the start of `boot.log`,
`cpu features: ...`, with SSE4.1/4.2, AVX (and "system saves its state"), AVX2, FMA, F16C, BMI1, so the next log shows it.

## 3. The kernel and the audio

**3a. HLE of Horizon.** Eden **is** a high-level emulator of the Horizon kernel: the SVCs are C++ functions in `core/hle/kernel/svc/`, kernel objects are host
objects, and its fibers (512 KiB stacks here, patch 0008) switch guest threads without the host's scheduler. The cost is in the JIT'd guest code, not in the
kernel layer. There is no lower-level kernel emulation to replace.

**3b. Audio.** The sink is already native (patch 0002: `sceAudioOut`, a thread fed in 256-frame grains of 48 kHz, paced by the port). Eden's shared sink queue
already behaves as the list asks: when the guest's mixer is late the sink plays silence instead of waiting (underrun, `sink_stream.cpp`), and the guest side
waits when the queue is full (`max_queue_size`). 0.9.6 adds the one missing piece: the audio feeder (`AudioOutPS4`) and Eden's audio threads are placed with the
other non-guest threads (2b), so they are not competing with the game's cores.

## 4. Memory arenas

The design is already a fixed plan, decided at the start and made of slices: the GPU driver's arena (`ORBIS_ARENA_MIB`, 1280 MiB on a roomy console, chosen by
`memory_profile`), the JIT's code caches (`jit_cache_mib`, 4 caches), Eden's 128 MiB stream buffer, guest RAM committed as the game touches it (first from the
console's **system flexible pool**, `sysflex_mib`, which is not part of the direct memory at all), and the texture-cache budgets sized to the driver (patch 0025).
The list's figures (4 GB for the Switch's RAM, 512 MB each for JIT, shaders and temporary GPU data) add up to more than the **4608 MiB of direct memory a PS4 Pro
gives the process**, and a game that needs less than 4 GiB of its 4 GiB address space is common, so committing the guest RAM at the start would cost games
memory they never use (Mario Kart 8 already runs out). What the list asks for, "nothing is requested during the game, a full slice cleans itself", exists in each slice:

| Slice | What happens at its ceiling |
|---|---|
| JIT code cache | the cache is emptied whole and refilled (it is never grown); `boot.log` counts the flushes |
| Texture cache | Eden's garbage collector and the budgets of patch 0025 drop the least recently used images |
| Shader cache | `shader_preload` and the shader policy (`shader_policy.h`) bound what is compiled at the start and count the cache |
| Guest RAM | committed in chunks as the game touches it; the free direct block is watched and the game is paused before it runs out (`memory_action`) |

What 0.9.6 adds is the table in `boot.log` (`memory arenas: ...`), the same numbers in one place, so a crash or a slowdown in the next log belongs to a named
slice. **It changes nothing in how memory is used.** `sysflex_mib` stays in the menu (the third zip of 0.9.5 asked for it back: some games open more RAM the more
they are given, and need less) as the per-game override of the plan; it is the one place where "a bit less for this game" is possible.

## 5. Frame fence

Eden's Vulkan backend is built on it: `MasterSemaphore` (`vk_master_semaphore.h`) gives every submission a **tick**, signalled by the GPU through a timeline
semaphore (a fence where timeline semaphores are missing), and everything that must outlive the GPU's use of it is released by tick:
`Scheduler::Wait`/`IsFree(tick)`, the staging buffer pool (`vk_staging_buffer_pool.h`, `TickFrame`), the descriptor and buffer caches' `TickFrame`, the texture
cache's delayed destruction (patch 0016, "fence texture retirement", is this port's own check that an image is not reused before the fence). It is the "list of
discarded resources per frame, freed when the GPU signals frame N" the list describes. Nothing to add.

## 6. Linear allocators

Also already in the hot paths: the descriptor update queue (`vk_update_descriptor.h`) is a payload array with a cursor that goes back to the start every frame
(`TickFrame`), the staging pool hands out ring slices released by tick, and the stream buffer is a ring. Replacing the C++ containers that Eden allocates on
the host side with per-frame arenas would be a change in hundreds of places of an emulator that cannot be built here, for a gain nobody has measured (the
profile shows the cost in JIT'd guest code, the page tables and the driver, not in `malloc`).

## The second list (0.9.7)

It came with the promise that the three items "make the PS4 run the games almost as if it were native", and with the definitive fix for Super Mario Maker 2's blinking textures. They were read against the code,
the 29 console tests and the logs of 17 November 2026 (Super Mario Odyssey on 0.9.5, SpongeBob on 0.9.6). **Nothing in 0.9.7 has run on a console.** The rule is the same as before: do what applies, adapt what
applies in another form, and say with the reason what does not.

| # | Item | Verdict | What 0.9.7 does |
|---|---|---|---|
| 7 | GPU offload / ACE pipeline (a compute shader translates the Switch's command lists, `DrawIndirect` on the GPU) | **Does not apply** | nothing; `texture traffic:` shows what the CPU still moves |
| 8 | Direct unified memory (zero-copy, hUMA, Garlic/Onion) | **Already how the driver does it where a copy can be avoided; the copy that is left is a conversion** | `texture traffic:` counts it; the code caches no longer take their memory whole |
| 9 | AOT pre-compilation (ARM64 to x86-64 offline, a native blob) | **Adapted: the code caches grow in 2 MiB steps, so compiled code survives longer; no offline translation** | `jit_cache_commit=lazy` (patch 0034) |
| 10 | Mario Maker 2, classic theme: page-fault tracking and sub-region unswizzle | **Page tracking is already there (by the page table, not by `mprotect`); the sub-region upload is not built: it needs the numbers first** | the counters of patch 0035 |

### 7. GPU offload / ACE pipeline

The premise is that the GPU is idle while the CPU translates. The logs say the opposite end of it: in the Odyssey run the driver's own BUDGET lines put the time spent **submitting** at about 0.3 % of the CPU, the
wait for the GPU at **0 ms**, and one core, `CPUCore_0`, at about 80 % (the emulated ARM64 code). The GPU is not the limit, so work moved onto it has nothing to win; the cost is in the JIT'd guest code.
What the item describes is also not what a Switch command list is: Eden does not translate command lists one by one, it runs the Maxwell engines (the macro interpreter and the state tracking) on the CPU and draws with
Vulkan. Doing that on the GPU would be a new renderer (as item 1a), and the driver of this console has already shown it cannot be trusted with the GPU-driven paths: `VK_KHR_draw_indirect_count` and
`VK_EXT_conditional_rendering` **hung the GPU** on this hardware (TECHNICAL.md, "The GPU driver"), and each submission is flattened into a 2 MiB staging buffer by the driver, so large generated command streams do not fit
either. An ACE (asynchronous compute) pipeline would need a second queue to feed: Eden submits everything to one graphics queue, and a compute queue on this driver is something that would have to be tried on a console, with a GPU that already hangs on the paths above. Nothing is added.

### 8. Direct unified memory (zero-copy)

The PS4 does share one memory between the CPU and the GPU, and the port already uses it that way where a copy can go: the host-visible buffers (staging, stream, uniforms) are in the driver's Onion arena, which both
sides see, and the images are in Garlic (item 1c), so nothing is copied "from CPU memory to GPU memory" in between: the CPU writes the staging buffer once and the GPU reads it. What is left is not a copy, it is a **conversion**: the
Switch keeps textures in Maxwell's block-linear (GOB) layout, often ASTC-compressed, which a GCN GPU cannot sample, so every upload goes through an unswizzle (and a decode for ASTC) into a layout the GPU can read.
Mapping the guest RAM straight into the GPU (an imported host pointer) would give the GPU the wrong layout, and it conflicts with how the guest RAM is kept: it is reserved whole and committed in 64 KiB chunks **as the game
touches it** (patches 0002, 0027, 0029, 0030), so a pointer into it has pages that do not exist yet and a GPU read of one is not a fault the emulator can answer. The real memory saving of this item is elsewhere: the
JIT's code caches (item 9). Patch 0035 counts the data the CPU still moves (`texture traffic:`), so the next run puts a number on what a zero-copy path could save instead of a guess.

### 9. Pre-compilation (AOT), adapted

An offline translation of the game's ARM64 to a saved x86-64 blob is not possible to do honestly in this project: Dynarmic's output is not position independent (it embeds the addresses of the page table, of the
callbacks and of each core's state, which are different at every start), the code of a game is only available decrypted at run time (keys and the game's files stay with the person; nothing of them is here), and a blob
saved per game would have to be validated against the game's version and patches. What the item is after, "the first time through an area is slow, the second is not", is real: the log shows compile bursts that freeze
the game for about 1.7 s, and the caches were **flushed when full** while 1 GiB of direct memory was taken for four caches at the start, most of it never used (the Odyssey run lost 1200 MiB of free direct memory in a
minute and died at 44 MiB). So 0.9.7 changes how the caches get their memory (patch 0034):

- each of the four code caches **reserves** its address range at the start (256 MiB on a roomy console, 128 MiB otherwise: `jit_cache_mib=auto`) and takes the direct memory **in 2 MiB steps as the code grows** (the first two steps
  at the start). A game that compiles 40 MiB of code holds 40 MiB per core, not 256: the memory goes to the game, and a large ceiling means the cache is flushed (all compiled code thrown away) far less often;
- memory is asked for a little ahead of the code, so a step the kernel refuses is not a crash: the cache stops growing there and is flushed when less than 1 MiB is left, as a full cache always was; `boot.log` says `THE KERNEL REFUSED A STEP`;
- `jit_cache_commit=full` takes the memory whole as 0.9.6 did (EXPERIMENTAL tier; a start after a crash uses it by itself, as every experimental option);
- `boot.log` says how much is held (`JIT cache memory: X MiB held now of Y MiB reserved ...`), every ten seconds when it changes, and at the end.

The compile time itself does not disappear: a precompiled warm-up would only move it to the loading screen, and compiling a block list on another thread needs Dynarmic's per-core state to be locked. It is not in this
version.

### 10. Super Mario Maker 2, classic theme: textures that blink

The two ideas of the text are both about when a texture the CPU wrote to is uploaded again. What the code does today (Eden `5f142c79` with our patches):

- **Page tracking already exists**, and is done in software, not by `mprotect` and a SIGSEGV handler: the pages a texture lives on are counted as cached (`UpdatePagesCachedCount`, `RasterizerMarkRegionCached`), a guest
  write to one of them takes the slow path (`HandleRasterizerWrite`) instead of a plain store, and ends in `TextureCache::WriteMemory`, which sets the image's `CpuModified` flag; the next draw that uses the image uploads it
  (`RefreshContents`). Doing it with `mprotect` would replace that check with a fault on every first write, and this port already uses SIGSEGV for another job (the commit of the guest RAM, patches 0027/0029/0030): two
  handlers for the same signal is the kind of change that has to run on the console to be trusted.
- **The tracking is per image, not per rectangle.** A game that writes one small piece of a large texture (a DMA copy from linear to block-linear memory into a sub-rectangle) makes `RefreshContents` upload and
  unswizzle the whole image again. The sub-region unswizzle of the text (`UnswizzleSubrect` with x, y, w, h) exists in Eden's texture code, but the upload path (`UploadImageContents`) works on the whole image, and
  keeping a dirty rectangle per image means changing the template code of the texture cache in many places, with no console to try it on. That is the part of the idea that could be right, and it is **not built** in 0.9.7.
- **What is built is the measurement**: patch 0035 counts, in the two places that decide it, how many images are uploaded whole (and how many bytes, and the largest), and how many copies the GPU path declined and the CPU
  swizzled by itself (pitch to block-linear, block-linear to pitch, block-linear to block-linear), and `boot.log` prints them every ten seconds (`texture traffic, last 10 s: ...`) and at the end. If the classic-theme
  textures blink while this line shows a few large uploads per second, the whole-image upload is the cause and the dirty rectangle is the next patch; if it shows nothing, the cause is somewhere else (an image kept as a
  view of the wrong memory, which patch 0033 already looks at) and a dirty rectangle would not help.

### What does not change for a base PS4 or a Slim

The memory plan reads what the console gives the process (the direct memory, `memory_profile`), and the numbers in this page are those of a PS4 Pro. A base PS4 and a Slim have never been tried. The lazy code caches help
them the most: 1 GiB taken at the start is a quarter of what they have.

## What to look for in the next `boot.log`

- `cpu features:` (AVX, and whether the system saves its state);
- `thread placement:` lines: the cores allowed, the 20 first placements with `running on core N`;
- `memory arenas:` the table;
- then the FPS of a game that was measured before, with `cpu_affinity=auto` and with `cpu_affinity=off`.

And for 0.9.7:

- `JIT cache memory:` at the start (lazy or whole, the ceiling per cache) and every ten seconds while it changes: the **held** memory should grow slowly with the game, not jump to the ceiling; `THE KERNEL REFUSED A STEP`
  would be the one line that matters;
- `memory arenas:` again: the line for the JIT caches should be about 16 MiB at the start, not 1024;
- the free direct block of **Super Mario Odyssey** over three minutes or more (it fell 1200 MiB a minute before and the game closed at 44 MiB);
- `texture traffic, last 10 s:` in **Super Mario Maker 2**, classic theme, with the textures blinking, and the line at the end (`whole game`).
