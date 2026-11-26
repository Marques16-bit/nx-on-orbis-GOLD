# What the PS5 Switch emulators teach (read in 0.9.8)

Read: **SwitchPS5** (`SwitchPS5-main`: its `src/`, `headless/`, `tools/`, `fixtures/`), **ProsperoEden-Encore** (`prosperoeden-encore.zip`, with `docs/` and a `headless/backports/` folder) and **EglantinePS5** (`EglantinePS5-main.zip`), plus the Eden commits between this project's pin and Eden's current master. The question was: *what can Super Mario Maker 2 and the heavy games on the PS4 take from them?*

**Nothing here was measured.** Nothing in this zip has run on a console; the PS5 ports' own notes say most of their research items are unvalidated there too. "Applies / does not apply" is a judgement from reading code and notes, and says what it was judged against. Licences: all three are GPL-3.0(-or-later), as is this project; nothing was copied, the three Eden patches below were written here from this Eden's code. Eden does not accept AI-assisted contributions: **nothing from this project goes upstream.**

## The short answer

| From | Taken in 0.9.8 | Not taken, and why |
|---|---|---|
| This project's own log (19/11, Mario Maker 2) | **Patch 0036**: ASTC textures are recompressed to BC1/BC3 in slabs of at most 8 MiB, not one block of the whole level (a 2048x2048, 6-layer level is 96 MiB in RGBA8, the size of the big request in the log; that the request was this texture is an inference from the size) | -- |
| SwitchPS5's notes on vertex streams | **Patch 0037**: a vertex stream with instancing on and frequency 0 is per-vertex data (Eden's dynamic vertex-input path made it instanced) | -- |
| Reading of the allocator | **Patch 0038**: an image or buffer the driver refuses inside its memory budget is asked for again outside it, and counted in the log | -- |
| Encore's 2026-10-16 notes | **Patch 0039**: the three edge cases that exist in our Eden too (quad strip with fewer than 2 vertices, INT32_MIN scissor offset, a reversed vertex-binding range) | Encore's other items: see section 2 |
| Eglantine's texture store | -- | An **offline PC tool**, written for one Switch 2 title (a BC7 file per texture, looked up by hash); it is not a conversion inside `video_core` and has nothing to take for a Switch 1 game on a PS4 |
| Eden since the pin | -- | 192 commits; a new pin is a version of its own (section 3) |

What the user's description of these ports got right: the PS5 ports do convert ASTC to BC on the GPU (Eglantine: "while an area loads, on the GPU, to BC1/BC3"). **On this console that route is the one that lost the GPU twice** (the `astc=gpu` option, 0.4). The memory saving that route gives is already in our CPU route (`astc_recompress`), and 0036 makes its peak small.

## 1. What each zip is

- **SwitchPS5** (pins Eden `67bada77`, 2026-10-08): a PS5 port. Its fastmem and page-table changes, its `PS5_NATIVE` source branches and its scheduler/driver layer are written for the PS5's kernel and its AGC driver. **Does not apply** to the PS4 (different kernel calls, different GPU stack).
- **ProsperoEden-Encore**: the same Eden pin as this project (`5f142c79`), the PS5 port with the most documents: about 400 files in `docs/`, most of them dated notes, a `headless/` runtime, tools that read logs, and `headless/backports/` (upstream Eden commits applied on top of the pin as patches). Most of it is PS5-native (fastmem handler, deferred GPU writes, descriptor ring checks for its own driver stub, FSR3/FXAA passes, title profiles from a 9,460-game override list) and **its notes mark many items "research", "OFF by default" or "no console result"** (the ASTC per-layer Vulkan decoder N57: "real but not yet invoked").
- **EglantinePS5**: Encore's relative for Switch 2 titles. Its `tools/texture-store` converts a game's ASTC textures to BC7 on a PC ahead of time (README: "written for one title", "nothing has been run for EglantinePS5 yet").

## 2. Encore's items, one by one

| # | Item (Encore doc) | Us |
|---|---|---|
| 1 | Clear scissor at INT32_MIN (`CLEAR_SCISSOR_OVERFLOW`) | **Taken** (0039). The same code is in our Eden (`clamp_rect_to_render_area`). |
| 2 | Quad-strip draw count below 2 vertices (`QUAD_STRIP_DRAW_COUNT`) | **Taken** (0039). Same line in `MakeDrawParams`. |
| 3 | Reversed vertex-binding range (`VERTEX_BINDING_RANGE`) | **Taken** (0039). Encore says itself it is "defensive, not evidence that a game reaches it". |
| 4 | First use of the quad-index buffer waits for the GPU (`QUAD_INDEX_INITIAL_FENCE`) | Not taken: a stall once per run, no evidence it matters, touches buffer-cache lifetime. |
| 5 | `DrawTexture` should drain guest invalidations before locking the texture cache | Not taken: upstream reworked `DrawTexture` in #4559 (`1848b231`); whether that covers the drain was not checked, and no game is known to hit it. Belongs to a pin move. |
| 6 | Upstream **#4473 / #4477** (`headless/backports/eden-4473-4477.patch`, 12 files): CPU/GPU dirty-tracking races (made for UE5 crashes), simpler GPU fences, **GPU buffer readback removed** | Not taken. It applies cleanly to the pin plus our patches, but it rewrites buffer-cache synchronisation and removes a setting, was written for UE5 titles, and nothing in our logs points at it. It would be the first thing to try *if* Mario Maker 2's missing layers turn out to be a stale-buffer problem. |
| 7 | Descriptor ring / queue preflight, title cache isolation, deferred DMA, deferred JIT quiescence, FSR3, glyph and font tooling, audio guards | **Does not apply**: they are about the PS5's own driver stub and runtime, which this project does not have. |
| 8 | ASTC research items (N54 to N61: per-layer decoder, dirty-layer planner) | Not taken: OFF, not invoked and not validated even on their console; and they are the GPU route. |

## 3. Eden since the pin

192 commits (`5f142c79`, 2026-09-09 to `1cd2ecd5`, 2026-11-24). Read from the commit titles. The ones that look useful to a PS4:

- **Dynarmic** (the JIT, x86-64 here): "fix seven A32 and x64 defects" (#4598), "A64: flush deferred emits on long basic blocks and align page table sign bit" (#4659), "fix A32 crash due to wrong LEA encoding" (#4444), "fix bad arm codegen on relocations" (#4458), "fix sign extension logic on x64" (#4415, later partly reverted).
- **GPU / textures**: "remove `scheduler.Finish()` after ASTC decode, which waits for the GPU" (#4752; that is the GPU ASTC route, which we do not use), "fix TOTK dark squares" (#4565), "ReinterpretImage / MarkModification fixes" (#4546), "fix viewport Y flip for 2D games" (#4701), "DrawTexture scissor, aliasing and blit state" (#4559).
- **Shaders**: "Revert: improve asynchronous shader building" (#4512). Upstream took asynchronous shader building **out**: our default is already off (`async_shaders`), and the menu says why.
- **Memory**: #4432 only removes the polyfill under `ScratchBuffer`'s `make_unique_for_overwrite`; the pin already allocates without clearing.
- **Threads**: "multithreading refactor" (#4254/#4548) and "drop affinity pinning, fix BOTW perf regression" (#4603).

**What a move would cost** (tried here on a PC, nothing built): our 39 patches applied one after the other with `git am` onto master: **11 applied, 28 did not** (some of the 28 only because an earlier one did not). It is a rebase of the memory, JIT and texture patches, a CI build and a console run: **a version of its own**, not part of 0.9.8.

## 4. What to try on the console, cheapest first

1. Mario Maker 2 with `astc_recompress=bc1` and **without** `env=ORBIS_ARENA_MIB=1536`: the log's `heap big maps:` and `heap census:` lines say whether the big block is gone; a warning "refused inside the memory budget; asked again outside it" in the Eden log (0038) says the driver's budget was too tight.
2. `env=ORBIS_GS_RING_SCALE=32` against the missing layers (`docs/OTHER-PORTS.md` 1.2).
3. Boost Mode off and on, to read the `cpu clock:` line (`docs/OTHER-PORTS.md` 2.2).
4. The `.nro` black screen: `safe_mode=off`, five minutes.
5. If the layers are still missing: `async_shaders=off` is already the default; the next candidate is upstream #4473/#4477 (item 6 above), as its own patch.
