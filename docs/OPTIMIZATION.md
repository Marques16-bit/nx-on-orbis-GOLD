# The optimisation list (0.9.6), item by item

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

## What to look for in the next `boot.log`

- `cpu features:` (AVX, and whether the system saves its state);
- `thread placement:` lines: the cores allowed, the 20 first placements with `running on core N`;
- `memory arenas:` the table;
- then the FPS of a game that was measured before, with `cpu_affinity=auto` and with `cpu_affinity=off`.
