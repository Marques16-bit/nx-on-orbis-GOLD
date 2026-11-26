#!/usr/bin/env bash
# Checks that the pages people read say what the code does. Run from anywhere:  bash tests/docs_check.sh
#
#   - the version in README, CHANGELOG and ROADMAP is the one in frontend/frontend.cmake;
#   - the number of patches in the README is the number of files in patches/eden/;
#   - docs/SETTINGS.md is exactly what the menu's catalog generates (tools/settings-doc.cpp);
#   - every option and profile the README, INSTALL and CHANGELOG name exists, with the value they give;
#   - every file path in backticks and every relative link in those pages exists; every #anchor exists;
#   - the log lines the pages tell you to look for are in the source;
#   - the credits (Eden4, DolphinPS4 / ShiroKlein, ProsperoEden, Alejo) are where they must be;
#   - "N checks" of the lazy-memory test is what that test prints.
#
# It needs g++, git, python3. It writes nothing in the repository.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
checks=0
ok() { checks=$((checks + 1)); printf '  ok    %s\n' "$1"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }
check() { if (eval "$2"); then ok "$1"; else bad "$1"; fi; }  # a subshell: an "exit 1" inside a check fails that check, not the whole script

echo "== version and counts"
VERSION="$(sed -n 's/^set(EDEN_PS4_VERSION "\([^"]*\)".*/\1/p' frontend/frontend.cmake)"
check "frontend.cmake has a version ($VERSION)" '[ -n "$VERSION" ]'
check "CHANGELOG's newest entry is [$VERSION]" '[ "$(grep -m1 "^## \[" CHANGELOG.md | sed "s/^## \[\([^]]*\)\].*/\1/")" = "$VERSION" ]'
check "README says it is v$VERSION" 'grep -q "(v$VERSION)" README.md'
check "ROADMAP has a row for $VERSION" 'grep -q "^| \*\*$VERSION\*\* |" docs/ROADMAP.md'
PATCHES="$(ls patches/eden/*.patch | wc -l | tr -d ' ')"
check "README says $PATCHES patches" 'grep -q "+ $PATCHES patches" README.md'
check "ROADMAP map says ($PATCHES) patches" 'grep -q "patches/eden/\` ($PATCHES)" docs/ROADMAP.md'
check "patches are numbered 0001..$(printf %04d "$PATCHES") with no gap" '
    for i in $(seq 1 "$PATCHES"); do ls patches/eden/$(printf %04d "$i")-*.patch >/dev/null 2>&1 || exit 1; done'
check "README names the Eden commit the build pins" '
    short="$(grep -o "5f142c79[0-9a-f]*" scripts/fetch-eden.sh | head -1 | cut -c1-8)"; [ -n "$short" ] && grep -q "Eden \`$short\`" README.md'

echo "== docs/SETTINGS.md against the menu's catalog"
if g++ -std=c++20 -Wall -Wextra -Ifrontend tools/settings-doc.cpp frontend/menu/settings_model.cpp -o "$WORK/settings-doc" 2>"$WORK/gen.err"; then
    "$WORK/settings-doc" >"$WORK/SETTINGS.md"
    check "docs/SETTINGS.md is what tools/settings-doc.cpp generates" 'diff -q "$WORK/SETTINGS.md" docs/SETTINGS.md >/dev/null'
    if ! diff -q "$WORK/SETTINGS.md" docs/SETTINGS.md >/dev/null; then
        echo "        regenerate: g++ -std=c++20 -Ifrontend tools/settings-doc.cpp frontend/menu/settings_model.cpp -o /tmp/sd && /tmp/sd > docs/SETTINGS.md"
    fi
else
    bad "tools/settings-doc.cpp does not compile"; sed 's/^/        /' "$WORK/gen.err" | head
fi

echo "== options and profiles the pages name"
# Rows of SETTINGS.md look like: | `key` | label | `a` **(default)**, `b` | help |
settings_key() { grep -q "^| \`$1\` |" docs/SETTINGS.md; }
settings_default() { grep "^| \`$1\` |" docs/SETTINGS.md | grep -q "\`$2\` \*\*(default)\*\*"; }
check "sysflex_mib is an option" 'settings_key sysflex_mib'
check "sysflex_mib defaults to auto (README, CHANGELOG 0.9.1)" 'settings_default sysflex_mib auto'
check "sysflex_mib offers auto, off, 256, 512, 768, 1024, 1536 and 2048" 'for v in auto off 256 512 768 1024 1536 2048; do grep "^| \`sysflex_mib\` |" docs/SETTINGS.md | grep -q "\`$v\`" || exit 1; done'
check "players is an option, 1 to 4, default 1 (README, CHANGELOG 0.9.1)" 'settings_key players && settings_default players 1 && for v in 1 2 3 4; do grep "^| \`players\` |" docs/SETTINGS.md | grep -q "\`$v\`" || exit 1; done'
check "players is in the CONTROLS tab (0.9.5)" 'awk "/^## CONTROLS tab/{f=1;next} /^## /{f=0} f" docs/SETTINGS.md | grep -q "^| \`players\` |"'
check "jit_page_reuse is an option, ON by default since 0.9.5, and hidden in the menu (CHANGELOG 0.9.5)" '
    settings_key jit_page_reuse && settings_default jit_page_reuse on && grep "^| \`jit_page_reuse\` |" docs/SETTINGS.md | grep -q "| hidden"'
check "the profile More FPS (test) sets jit_cache_mib=128 (the two JIT switches are on by default now)" 'grep "^| More FPS (test) |" docs/SETTINGS.md | grep -q "jit_cache_mib=128"'
check "memory_action defaults to pause (CHANGELOG 0.8.4)" 'settings_default memory_action pause'
check "colour_swap is on by default (README)" 'settings_default colour_swap on'
check "async_shaders is off by default (README, CHANGELOG)" 'settings_default async_shaders off'
check "the menu has 7 colour themes (README)" '[ "$(grep "^| \`menu_theme\` |" docs/SETTINGS.md | grep -o "\`[a-z]*\`" | wc -l | tr -d " ")" = 7 ]'
check "profile \"Minimum RAM (test)\" exists and sets sysflex_mib=2048 (CHANGELOG 0.9.1)" 'grep "^| Minimum RAM (test) |" docs/SETTINGS.md | grep -q "\`sysflex_mib=2048\`"'
check "README names the tab that has sysflex_mib (CPU AND MEMORY)" 'grep -q "^## CPU AND MEMORY tab" docs/SETTINGS.md && awk "/^## CPU AND MEMORY tab/{f=1;next} /^## /{f=0} f" docs/SETTINGS.md | grep -q "^| \`sysflex_mib\` |"'

# Every `key` or `key=value` the README and INSTALL put in backticks that looks like a settings key must be one the menu knows,
# or be on the short list of things that are not keys.
NOT_KEYS="key status env"
python3 -I - "$ROOT" "$NOT_KEYS" <<'PY' || fail=$((fail + 1))
import re, sys
root, not_keys = sys.argv[1], set(sys.argv[2].split())
doc = open(root + "/docs/SETTINGS.md", encoding="utf-8").read()
keys = set(re.findall(r"^\| `([^`]+)` \|", doc, re.M))
unknown = []
for page in ("README.md", "docs/INSTALL.md"):
    text = open(root + "/" + page, encoding="utf-8").read()
    for token in re.findall(r"`([^`\n]+)`", text):
        m = re.fullmatch(r"([a-z][a-z0-9_]*(?::[A-Z_]+)?)(?:=[^\s`]*)?", token)
        if not m:
            continue
        name = m.group(1)
        if "=" not in token and "_" not in name:
            continue  # a plain word in backticks is not meant as a key
        if name in keys or name in not_keys:
            continue
        unknown.append((page, token))
if unknown:
    for page, token in unknown:
        print("  FAIL  %s names `%s`, which is not an option of the menu" % (page, token))
    sys.exit(1)
print("  ok    every `key` or `key=value` in README and INSTALL is an option of the menu (or on the short list)")
PY
checks=$((checks + 1))

echo "== paths and links"
python3 -I - "$ROOT" <<'PY' || fail=$((fail + 1))
import os, re, sys
root = sys.argv[1]
pages = ["README.md", "NOTICE.md", "docs/EDEN4.md", "docs/INSTALL.md", "docs/SETTINGS.md"]
problems = 0
REPO_ROOTS = ("patches/", "frontend/", "tests/", "tools/", "ci/", "scripts/", "docs/", "licenses/", ".github/", "toolchain/", "assets/", "probe/", "testroms/")
EXTERNAL = {"docs/EDEN4.md": {"docs/ARCHITECTURE.md", "docs/KNOWN_ISSUES.md"}}
def slug(heading):
    s = heading.strip().lower()
    s = re.sub(r"[^\w\s-]", "", s)
    return re.sub(r"\s", "-", s)
for page in pages:
    text = open(os.path.join(root, page), encoding="utf-8").read()
    base = os.path.dirname(page)
    ids = set(re.findall(r'<a id="([^"]+)"', text)) | {slug(h) for h in re.findall(r"^#+\s+(.*)$", text, re.M)}
    for target in re.findall(r"\]\(([^)\s]+)\)", text):
        if target.startswith(("http://", "https://", "mailto:", "../../")):
            continue
        path, _, anchor = target.partition("#")
        if path:
            dest = os.path.normpath(os.path.join(root, base, path))
            if not os.path.exists(dest):
                print("  FAIL  %s links to %s, which does not exist" % (page, target)); problems += 1
                continue
            if anchor:
                other = open(dest, encoding="utf-8").read() if dest.endswith(".md") else ""
                other_ids = set(re.findall(r'<a id="([^"]+)"', other)) | {slug(h) for h in re.findall(r"^#+\s+(.*)$", other, re.M)}
                if anchor not in other_ids:
                    print("  FAIL  %s links to %s, but that page has no such heading" % (page, target)); problems += 1
        elif anchor and anchor not in ids:
            print("  FAIL  %s links to #%s, but the page has no such heading or id" % (page, anchor)); problems += 1
    for token in re.findall(r"`([^`\n]+)`", text):
        # Only paths inside this repository are checked: the ones on the console (/data/..., log/...) and the ones in Eden4's
        # own tree (named in EDEN4.md) are not files here.
        if token in EXTERNAL.get(page, ()):
            continue
        if token.startswith(REPO_ROOTS) and re.fullmatch(r"[A-Za-z0-9_./-]+\.(sh|cpp|h|md|patch|yml|cmake|py|txt)", token):
            if not os.path.exists(os.path.join(root, token)):
                print("  FAIL  %s names the file `%s`, which does not exist" % (page, token)); problems += 1
if problems:
    sys.exit(1)
print("  ok    every relative link, anchor and file path in README, NOTICE, EDEN4, INSTALL and SETTINGS exists")
PY
checks=$((checks + 1))

echo "== log lines the pages point to are in the source"
for phrase in "system flexible memory: cap" "not available on this console or SDK" "probe of" "in system flexible memory" "controllers: players=" "controllers: player %d connected" "kernel frees"; do
    check "source has \"$phrase\"" 'grep -rqF -- "$phrase" frontend/ps4_platform.cpp frontend/main.cpp'
done
for phrase in "system flexible memory: cap N MiB" "not available on this console or SDK" "in system flexible memory" "kernel frees N calls" "controllers: players=N"; do
    check "CHANGELOG/README quote \"$phrase\"" 'grep -qF -- "$phrase" CHANGELOG.md README.md'
done

echo "== credits"
check "README credits Eden4" 'grep -q "\*\*Eden4\*\*" README.md'
check "README credits DolphinPS4 by ShiroKlein" 'grep -q "DolphinPS4" README.md && grep -q "ShiroKlein" README.md'
check "README credits ProsperoEden, Alejo (@alechurri), orbis-ports, OpenOrbis, switchbrew" '
    for w in ProsperoEden "Alejo" "@alechurri" "orbis-ports" OpenOrbis switchbrew; do grep -qF -- "$w" README.md || exit 1; done'
check "README says Eden4 does not name its author (no invented name)" 'tr "\n" " " < README.md | tr -s " " | grep -q "does not name its author"'
check "NOTICE.md has the Eden4 section, DolphinPS4 and ShiroKlein" 'grep -q "^## Eden4" NOTICE.md && grep -q "DolphinPS4" NOTICE.md && grep -q "ShiroKlein" NOTICE.md'
check "docs/EDEN4.md has Taken and Not taken sections" 'grep -q "^## Taken" docs/EDEN4.md && grep -q "^## Not taken" docs/EDEN4.md'
check "patch 0029 credits Eden4" 'grep -q "Eden4" patches/eden/0029-*.patch'
check "patch 0030 exists and changes only the lazy-memory files" '[ "$(grep "^diff --git" patches/eden/0030-*.patch | wc -l | tr -d " ")" = 2 ] && ! grep "^diff --git" patches/eden/0030-*.patch | grep -qv "orbis_lazy_memory"'
check "the menu text of sysflex_mib credits Eden4" 'grep -q "from Eden4" frontend/menu/settings_model.cpp'

echo "== the JIT direct table fits the pinned Eden (0.9.4)"
check "fixtures_pinned has the six files the script edits, from the pinned build" '
    for f in page_table.h page_table.cpp memory.cpp emit_x64_memory.h emit_x64_memory.cpp.inc a64_emit_x64_memory.cpp; do [ -s tests/jit_pack/fixtures_pinned/$f ] || exit 1; done
    grep -q "constexpr void Store(" tests/jit_pack/fixtures_pinned/page_table.h'
check "the CI builds the page table half with the PC stand-ins first in the include path" 'grep -q -- "-Itests/jit_pack/direct_shim -Ideps/eden/src" .github/workflows/check-patches.yml'
check "tools/jit-harness has the harness, the generator, the stubs, the source list and a README" '
    for f in harness.cpp gen.py stubs.cpp dynarmic-sources.txt README.md; do [ -s tools/jit-harness/$f ] || exit 1; done'
check "CHANGELOG 0.9.4 names the boot.log line of 0.9.3 and the cause" '
    awk "/^## \\[0.9.4\\]/{f=1;next} /^## \\[/{f=0} f" CHANGELOG.md | tr "\n" " " | tr -s " " | grep -q "does not have it" && awk "/^## \\[0.9.4\\]/{f=1;next} /^## \\[/{f=0} f" CHANGELOG.md | grep -q "constexpr"'

echo "== the 0.9.4 fix: the build reports on the pack, the JIT cache size"
if out="$(bash tests/pack_fallback_test.sh 2>&1)"; then
    n="$(printf '%s\n' "$out" | sed -n 's/^pack_fallback_test: \([0-9]*\) checks.*/\1/p' | head -n 1)"
    check "the pack fallback test passes, and CHANGELOG says $n checks for it" 'grep -q "$n checks: .tests/pack_fallback_test.sh" CHANGELOG.md'
else
    bad "tests/pack_fallback_test.sh fails"; printf '%s\n' "$out" | tail -n 8 | sed 's/^/        /'
fi
if g++ -std=c++20 -Wall -Wextra -Ifrontend tests/jit_cache_test.cpp -o "$WORK/jit_cache_test" 2>/dev/null && out="$("$WORK/jit_cache_test" 2>&1)"; then
    n="$(printf '%s\n' "$out" | grep -c '^  ok')"
    check "the JIT cache test passes, and CHANGELOG says $n checks for it" 'grep -q "$n checks: .tests/jit_cache_test.cpp" CHANGELOG.md'
else
    bad "tests/jit_cache_test.cpp fails"
fi
check "patch 0031 is a git patch that touches the two cores' config and Dynarmic's A64 interface only" '
    [ "$(grep "^diff --git" patches/eden/0031-*.patch | wc -l | tr -d " ")" = 3 ] && grep "^diff --git" patches/eden/0031-*.patch | grep -q "a64_interface.cpp" &&
    grep -q "extern \"C\" unsigned ps4_jit_cache_mib" patches/eden/0031-*.patch'
check "frontend/jit_cache.h defines the three symbols patch 0031 declares (weak)" '
    for sym in ps4_jit_cache_mib ps4_jit_cache_full ps4_jit_cache_asked; do grep -q "weak)) [a-z ]* $sym" frontend/jit_cache.h && grep -q "$sym" patches/eden/0031-*.patch || exit 1; done'
check "patch 0032 is a git patch that touches the code allocator and Dynarmic's A64 interface only, and frontend/jit_cache.h defines what it declares (weak)" '
    [ "$(grep "^diff --git" patches/eden/0032-*.patch | wc -l | tr -d " ")" = 2 ] && grep "^diff --git" patches/eden/0032-*.patch | grep -q "block_of_code.cpp" &&
    for sym in ps4_jit_cache_large ps4_jit_cache_small ps4_jit_blocks_compiled ps4_jit_ranges_invalidated; do grep -q "weak)) [a-z ]* $sym" frontend/jit_cache.h && grep -q "$sym" patches/eden/0032-*.patch || exit 1; done'
check "patch 0034 is a git patch that touches Dynarmic's code allocator and block_of_code.h only, and frontend/jit_cache.h defines what it declares (weak)" '
    [ "$(grep "^diff --git" patches/eden/0034-*.patch | wc -l | tr -d " ")" = 2 ] && grep "^diff --git" patches/eden/0034-*.patch | grep -q "block_of_code.cpp" && grep "^diff --git" patches/eden/0034-*.patch | grep -q "block_of_code.h" &&
    for sym in ps4_jit_cache_lazy ps4_jit_chunks_large ps4_jit_chunks_small ps4_jit_commit_failed ps4_jit_reserved_bytes ps4_jit_committed_bytes; do grep -q "weak)) [a-z ]* $sym" frontend/jit_cache.h && grep -q "$sym" patches/eden/0034-*.patch || exit 1; done'
check "patch 0035 is a git patch that touches the DMA engine and the texture cache only, and frontend/texture_traffic.h defines what it declares (weak)" '
    [ "$(grep "^diff --git" patches/eden/0035-*.patch | wc -l | tr -d " ")" = 2 ] && grep "^diff --git" patches/eden/0035-*.patch | grep -q "maxwell_dma.cpp" && grep "^diff --git" patches/eden/0035-*.patch | grep -q "texture_cache.h" &&
    for sym in ps4_tex_uploads ps4_tex_upload_bytes ps4_tex_upload_largest ps4_dma_cpu_swizzles ps4_dma_cpu_swizzle_bytes ps4_dma_cpu_unswizzles ps4_dma_cpu_unswizzle_bytes ps4_dma_cpu_bl_copies ps4_dma_cpu_bl_copy_bytes; do
        grep -q "weak)) [a-z ]* $sym" frontend/texture_traffic.h && grep -q "extern unsigned long long $sym" patches/eden/0035-*.patch || exit 1; done'
check "the patches are numbered 0001 to 0039 without a gap, and README and ROADMAP say 39" '
    [ "$(ls patches/eden/*.patch | wc -l | tr -d " ")" = 39 ] && for n in $(seq -f "%04g" 1 39); do [ -f patches/eden/$n-*.patch ] || exit 1; done &&
    grep -q "+ 39 patches in .patches/eden/." README.md && grep -q "patches/eden/. (39)" docs/ROADMAP.md'
check "0.9.7: the changelog and the optimisation page have it (the version is 0.9.8 since the 0.9.8 zip)" '
    grep -q "^## \[0.9.7\]" CHANGELOG.md && grep -q "What is new in 0.9.7" README.md &&
    grep -q "^## The second list (0.9.7)" docs/OPTIMIZATION.md && grep -q "THE KERNEL REFUSED A STEP" docs/OPTIMIZATION.md && grep -q "texture traffic" CHANGELOG.md'
check "0.9.7 fix: the arena is placed inside one 4 GiB window (wrapped, switched, safe-mode off, tested, documented)" '
    grep -q "wrap=sceKernelMapDirectMemory" frontend/frontend.cmake && grep -q "arena_place.cpp" frontend/frontend.cmake && grep -q "__wrap_sceKernelMapDirectMemory" frontend/arena_place.cpp &&
    grep -q "Ps4ArenaPlace::Arm(g_arena_align)" frontend/main.cpp && grep -q "g_safe_mode && g_arena_align" frontend/main.cpp && grep -q "arena_align" frontend/safe_mode.h &&
    grep -q "arena placement" tests/memory_guard_test.cpp && grep -q "arena_align" docs/SETTINGS.md && grep -q "^### 0.9.7 fix" CHANGELOG.md && grep -q "643 MiB" CHANGELOG.md'
check "0.9.7 fix: the heap census sees 256 KiB, a wrapper of __mmap records the big requests, and both are printed when the process dies" '
    grep -q "Threshold = 256 << 10" frontend/heap_census.cpp && grep -q "wrap=__mmap" frontend/frontend.cmake && grep -q "mmap_watch.cpp" frontend/frontend.cmake && grep -q "__wrap___mmap" frontend/mmap_watch.cpp &&
    grep -q "Ps4HeapBigMaps" frontend/main.cpp && grep -q "Ps4HeapBigMaps" frontend/ps4_platform.cpp'
check "0.9.7 fix: the new frontend files compile on a PC (syntax only; the SDK is not needed for them)" '
    ${CXX:-g++} -std=c++20 -fsyntax-only -Wall -Wextra frontend/arena_place.cpp && ${CXX:-g++} -std=c++20 -fsyntax-only -Wall -Wextra frontend/mmap_watch.cpp && ${CXX:-g++} -std=c++20 -fsyntax-only -Wall -Wextra frontend/heap_census.cpp'
check "main.cpp logs the code caches' memory and the texture traffic, and ends with the totals" '
    grep -q "Ps4JitCache::DescribeCommit" frontend/main.cpp && grep -q "Ps4TextureTraffic::Describe(texture_logged" frontend/main.cpp && grep -q "Ps4TextureTraffic::DescribeTotals" frontend/main.cpp &&
    grep -q "jit_cache_commit" frontend/main.cpp'
check "part 2 of the pack reads and writes bytes (Path.read_text(newline=) is Python 3.13 only; the runner has 3.12), and its test tries every interpreter" '
    ! grep -q "newline=" patches/eden-jit/0002-*.py && grep -q "read_bytes" patches/eden-jit/0002-*.py && grep -q "python3.12" tests/jit_direct_test.sh'
check "safe mode names the game and counts the deaths, and main.cpp asks with the game" '
    grep -q "PreviousStartDied(const std::string& path, const std::string& game)" frontend/safe_mode.h && grep -q "PreviousStartDied(SafeStartFile(), game)" frontend/main.cpp'
check "CHANGELOG has the 0.9.4 fix 2 section and names the TypeError and the safe mode cause" '
    grep -q "^### 0.9.4 fix 2" CHANGELOG.md && grep -q "unexpected keyword argument .newline." CHANGELOG.md && grep -q "The test ran in safe mode" CHANGELOG.md'
check "package-eden.sh puts jit-pack-report.txt in the package, and boot.log quotes it" '
    grep -q "assets/misc/jit-pack-report.txt" package-eden.sh && grep -q "assets/misc/jit-pack-report.txt" frontend/main.cpp && grep -q "ReportLines" frontend/main.cpp'

echo "== the JIT direct table (0.9.3)"
check "patch 0033 is a git patch that touches only the texture cache, revalidates NULL and Remapped views, and the CHANGELOG says it is not proven" '
    [ "$(grep "^diff --git" patches/eden/0033-*.patch | wc -l | tr -d " ")" = 1 ] && grep "^diff --git" patches/eden/0033-*.patch | grep -q "texture_cache/texture_cache.h" &&
    grep -q "is_remapped_view" patches/eden/0033-*.patch && ! grep -q "PS5_NATIVE" patches/eden/0033-*.patch &&
    grep -q "not proven to be the whole cause of the MM2 blinking" CHANGELOG.md'
check "the controller model fit is in eden_settings.h, called from main.cpp, tested, and the CHANGELOG says Let's Go is not proven" '
    grep -q "StyleFit FitControllerStyle" frontend/eden_settings.h && grep -q "Ps4Eden::FitControllerStyle(system.HIDCore()" frontend/main.cpp &&
    grep -q "FitControllerStyle(hid, 0)" tests/eden_settings_test.cpp && grep -q "Not proven to be the cause for Let" CHANGELOG.md'
check "the menu sounds: made by the program, the options exist and are global, the audio port is guarded by a marker, and the CHANGELOG says it is untried on a console" '
    grep -q "MakeAmbient" frontend/menu/menu_sound.h && settings_key menu_audio && settings_key menu_volume && settings_default menu_audio all &&
    grep "^| \`menu_audio\` |" docs/SETTINGS.md | grep -q "no (global only)" && grep -q "menu_audio.marker" frontend/menu/rom_menu.cpp &&
    grep -q "sceAudioOutOpen(0xFF" frontend/menu/rom_menu.cpp && grep -q "audio port and .main.cpp. have never been run" CHANGELOG.md'
check "the HUD and the pause panel of zip 5 are in the CHANGELOG and tested" '
    grep -q "^### Fifth zip of 0.9.5" CHANGELOG.md && grep -q "BuildHud" frontend/video_hook.cpp && grep -q "BuildPanel" frontend/video_hook.cpp && grep -q "SetHudStatsForTest" tests/video_hook_test.cpp'
check "0.9.6: cpu_affinity is an option (auto by default), read by main.cpp, off in safe mode, tested, and the CHANGELOG says it is untried" '
    settings_key cpu_affinity && settings_default cpu_affinity auto && grep -q "\"cpu_affinity\"" frontend/main.cpp && grep -q "ConfigureThreadPlacement(g_cpu_affinity && !g_safe_mode)" frontend/main.cpp &&
    grep -q "PlaceCurrentThread(name)" frontend/ps4_platform.cpp && grep -q "Plan six" tests/memory_guard_test.cpp &&
    grep -q "^## \[0.9.6\]" CHANGELOG.md && grep -q "Nothing in this version has run on a console" CHANGELOG.md'
check "0.9.6: docs/OPTIMIZATION.md answers every item of the list, README links it, and the log lines it promises are in the source" '
    [ -f docs/OPTIMIZATION.md ] && grep -q "docs/OPTIMIZATION.md" README.md &&
    for item in "1a. Direct GNM" "1b. ASTC" "1c. Onion" "2a. Fastmem" "2b. Core affinity" "2c. SIMD" "3a. HLE" "3b. Audio" "## 4. Memory arenas" "## 5. Frame fence" "## 6. Linear allocators"; do
        grep -q -- "$item" docs/OPTIMIZATION.md || exit 1
    done &&
    grep -q "memory arenas: " frontend/memory_guard.cpp && grep -q "cpu features: " frontend/ps4_platform.cpp && grep -q "thread placement: " frontend/ps4_platform.cpp'
check "jit_direct_table is an option, ON by default since 0.9.5, and hidden in the menu" '
    settings_key jit_direct_table && settings_default jit_direct_table on && grep "^| \`jit_direct_table\` |" docs/SETTINGS.md | grep -q "| hidden"'
check "the 0.9.5 tiers: colour_swap hidden, cpu_clock in the TESTS tab of the test builds, resolution always shown" '
    grep "^| \`colour_swap\` |" docs/SETTINGS.md | grep -q "| hidden" && grep "^| \`cpu_clock\` |" docs/SETTINGS.md | grep -q "TESTS tab" && grep "^| \`resolution\` |" docs/SETTINGS.md | grep -q "| yes |"'
check "the CONTROLS tab has the four player models, automatic by default (0.9.5)" '
    for n in 1 2 3 4; do settings_key player${n}_type && settings_default player${n}_type auto || exit 1; done'
CL095="$(awk '/^## \[0\.9\.5\]/{f=1;next} /^## \[/{f=0} f' CHANGELOG.md)" # a variable: grep -q on a pipe would end awk early (pipefail)
check "the CHANGELOG 0.9.5 names the public build, the CONTROLS tab and the new keys" '
    grep -q "EDEN_PS4_PUBLIC" <<<"$CL095" && grep -q "CONTROLS tab" <<<"$CL095" && grep -q "player1_type" <<<"$CL095"'

check "the 0.9.5 CHANGELOG says what was not run on a console" 'grep -q "Nothing of this zip has run on a console" <<<"$CL095"'
check "the extra RAM (sysflex_mib) is shown in the menu again, and the public build is built by a PUBLIC_BUILD file (0.9.5, third zip)" '
    grep "^| \`sysflex_mib\` |" docs/SETTINGS.md | grep -q "| yes |" && grep -q "PUBLIC_BUILD" ci/stage-eden.sh && grep -q "PUBLIC_BUILD" docs/BUILDING.md && grep -q "EDEN_PS4_PUBLIC=ON" ci/stage-eden.sh && grep -q "kPublicBuild" frontend/main.cpp'
check "SETTINGS.md has the Per game column, and resolution is yes (R2) while language is global only (0.9.5, second zip)" '
    grep -q "| Per game |" docs/SETTINGS.md && grep "^| \`resolution\` |" docs/SETTINGS.md | grep -q "yes (R2)" && grep "^| \`language\` |" docs/SETTINGS.md | grep -q "no (global only)"'
check "the 0.9.5 CHANGELOG names the per-game files and the .nro reader, and the log line is the one main.cpp writes" '
    grep -q "per-game/<title id>.txt" <<<"$CL095" && grep -q "asset section" <<<"$CL095" && grep -qF "per-game config: %s is applied on top of settings.txt" frontend/main.cpp && grep -qF "per-game config: <file> is applied on top of settings.txt" <<<"$CL095"'
check "main.cpp applies the game's file after settings.txt and skips the global-only keys" '
    grep -q "ApplySettingsFile(game);" frontend/main.cpp && grep -q "per_game && Menu::GlobalOnly(key)" frontend/main.cpp && grep -qF "ApplySettingsLines(in, \"per-game\", true)" frontend/main.cpp'
check "the README tells the user about R2 and per-game/" 'grep -q "R2" README.md && grep -q "per-game/" README.md'
check "game_config.h and nro_assets.h are header only, with no console code" '
    ! grep -q "sce[A-Z]\|Ps4::" frontend/menu/game_config.h frontend/menu/nro_assets.h'
check "part 2 of the pack is a script next to the patch, and the docs say it edits six files" '
    [ -f patches/eden-jit/0002-JIT-direct-page-table-copy.py ] && python3 -I -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" patches/eden-jit/0002-JIT-direct-page-table-copy.py &&
    [ "$(grep -c "^    (\"src/" patches/eden-jit/0002-JIT-direct-page-table-copy.py)" = 6 ]'
check "part 2 only writes after every edit worked (the files are written in a loop after the try)" '
    awk "/except Miss/{m=NR} /for path, text in out.items/{w=NR} END{exit !(m && w && m < w)}" patches/eden-jit/0002-JIT-direct-page-table-copy.py'
check "everything part 2 adds to Edens page table code is under ifdef ORBIS (tests/jit_direct_test.sh checks it)" 'grep -q "outside #ifdef __ORBIS__" tests/jit_direct_test.sh'
check "the switch starts false in jit_pack.h, so the table exists only after jit_direct_table=on" 'grep -q "^__attribute__((weak)) bool ps4_direct_table = false;" frontend/jit_pack.h'
for phrase in "JIT direct table:" "does not have it" "no page table got a direct copy" "read the direct copy"; do
    check "jit_pack.h has \"$phrase\"" 'grep -qF -- "$phrase" frontend/jit_pack.h'
done
check "pack-fallback.sh takes out the last part first, then the whole pack, and stage-eden.sh calls it" 'grep -q "drop-jit-pack.sh \"\$EDEN\" --last" ci/pack-fallback.sh && grep -q "drop-jit-pack.sh \"\$EDEN\" || break" ci/pack-fallback.sh && grep -q "ci/pack-fallback.sh" ci/stage-eden.sh'
check "CHANGELOG 0.9.3 says the code generator was never compiled and quotes the boot.log line" '
    awk "/^## \\[0.9.3\\]/{f=1;next} /^## \\[/{f=0} f" CHANGELOG.md | tr "\n" " " | tr -s " " | grep -q "never compiled" && grep -qF "JIT direct table:" CHANGELOG.md'
if out="$(bash tests/jit_direct_test.sh 2>&1)"; then
    n="$(printf '%s\n' "$out" | sed -n 's/^direct table: \([0-9]*\) checks.*/\1/p' | head -n 1)"
    s="$(printf '%s\n' "$out" | sed -n 's/^jit_direct_test: \([0-9]*\) structure checks.*/\1/p')"
    check "the direct table test passes ($n checks of the page table half, $s structure checks)" '[ -n "$n" ] && [ -n "$s" ]'
    check "README and CHANGELOG say $n checks for the page table half and CHANGELOG $s structure checks" 'grep -q "$n checks" README.md && grep -q "$n checks" CHANGELOG.md && grep -q "$s structure checks" CHANGELOG.md'
else
    bad "tests/jit_direct_test.sh fails"; printf '%s\n' "$out" | tail -n 8 | sed 's/^/        /'
fi
if out="$(bash tests/jit_pack_scripts_test.sh 2>&1)"; then
    n="$(printf '%s\n' "$out" | sed -n 's/^jit_pack_scripts_test: \([0-9]*\) checks.*/\1/p')"
    check "the apply/drop stages test passes, and CHANGELOG says $n checks for it" 'grep -q "$n checks: .tests/jit_pack_scripts_test.sh" CHANGELOG.md'
else
    bad "tests/jit_pack_scripts_test.sh fails"; printf '%s\n' "$out" | tail -n 8 | sed 's/^/        /'
fi

echo "== the JIT pack (0.9.2)"
check "patches/eden-jit has one patch, and MODE says on or off" '
    [ "$(ls patches/eden-jit/*.patch | wc -l | tr -d " ")" = 1 ] && grep -qx -E "on|off" patches/eden-jit/MODE'
check "the pack changes only the four Dynarmic files the docs name" '
    [ "$(grep "^diff --git" patches/eden-jit/*.patch | wc -l | tr -d " ")" = 4 ] &&
    for f in ir/opcodes.inc ir/opt_passes.cpp backend/x64/a64_emit_x64_memory.cpp backend/x64/emit_x64_memory.cpp.inc; do grep -q "^diff --git a/src/dynarmic/src/dynarmic/$f " patches/eden-jit/*.patch || exit 1; done'
check "the pack is off at run time until jit_page_reuse=on (the switch starts false)" 'grep -q "^+bool ps4_page_reuse = false;" patches/eden-jit/*.patch'
check "the pack keeps the register allocation rule: no scratch register after the first branch in the producer" '
    awk "/EmitA64EdenPageLookup\(A64EmitContext/{f=1} f&&/ScratchGpr|UseScratchGpr/{last=NR} f&&/code\.(ja|jnz|jc|jz)\(/{first=NR; exit} END{exit !(last && first && last < first)}" patches/eden-jit/*.patch'
check "apply-jit-pack.sh and drop-jit-pack.sh parse; fetch-eden.sh and stage-eden.sh call them" '
    bash -n scripts/apply-jit-pack.sh && bash -n scripts/drop-jit-pack.sh && bash -n ci/pack-fallback.sh && grep -q "apply-jit-pack.sh" scripts/fetch-eden.sh && grep -q "drop-jit-pack.sh" ci/pack-fallback.sh'
for phrase in "does not have the pack" "groups of nearby accesses share a page lookup" "this page table setup is not handled"; do
    check "jit_pack.h has \"$phrase\"" 'grep -qF -- "$phrase" frontend/jit_pack.h'
done
check "CHANGELOG quotes the boot.log lines \"JIT page reuse:\" and the jit-pack-errors artifact" 'grep -qF "JIT page reuse:" CHANGELOG.md && grep -qF "jit-pack-errors" CHANGELOG.md'
check "the CHANGELOG 0.9.2 entry says the code generator was never compiled or run" 'tr "\n" " " < CHANGELOG.md | tr -s " " | grep -q "never compiled"'
if out="$(bash tests/jit_pack_test.sh 2>&1)"; then
    n="$(printf '%s\n' "$out" | grep -c '^  ok')"
    check "the pass test passes and prints $n checks" 'printf "%s\n" "$out" | grep -q "^all ok"'
    check "README and CHANGELOG say $n checks for it" 'grep -q "$n checks" README.md && grep -q "$n checks" CHANGELOG.md'
else
    bad "tests/jit_pack_test.sh fails"; printf '%s\n' "$out" | tail -n 8 | sed 's/^/        /'
fi

echo "== 0.9.7 fix 2: function names, safe mode, the TESTS tab"
check "CHANGELOG has the 0.9.7 fix 2 section, above the 0.9.7 fix one, and says nothing in it ran on a console" '
    a="$(grep -n "^### 0.9.7 fix 2" CHANGELOG.md | head -1 | cut -d: -f1)"; b="$(grep -n "^### 0.9.7 fix (" CHANGELOG.md | head -1 | cut -d: -f1)"
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] && [ "$(sed -n "${a},${b}p" CHANGELOG.md | grep -c "Nothing in this zip has run on a console")" -ge 1 ]'
check "the symbol table is built into the package (package-eden.sh) and listed in the files of the package" '
    grep -q "make-symbols.py" package-eden.sh && grep -q "assets/misc/symbols.txt\"" package-eden.sh && [ -f tools/make-symbols.py ]'
check "symbols.cpp and symbols.h are part of the executable" 'grep -q "symbols.cpp" frontend/frontend.cmake && grep -q "symbols.h" frontend/frontend.cmake'
check "every dump that prints eboot+0x offsets also asks for names (thread stacks, crash, profile, census, big maps)" '
    grep -q "Ps4Sym::Legend(why)" frontend/ps4_platform.cpp && grep -q "Ps4Sym::Legend(\"the crash\")" frontend/ps4_platform.cpp &&
    grep -q "Ps4Sym::Legend(\"the profile\")" frontend/ps4_platform.cpp && grep -q "Ps4SymLegend(\"the heap census\")" frontend/heap_census.cpp &&
    grep -q "Ps4SymLegend(\"the big heap requests\")" frontend/mmap_watch.cpp'
check "the census and the big-map list reach the symbol reader through weak declarations (heap_census_test links the census alone)" '
    grep -q "weak)) void Ps4SymNote" frontend/heap_census.cpp && grep -q "weak)) void Ps4SymLegend" frontend/heap_census.cpp && grep -q "weak)) void Ps4SymNote" frontend/mmap_watch.cpp'
check "the reader checks that the table belongs to this build before it prints a name" 'grep -q "Ps4SymAnchor" frontend/symbols.cpp && grep -q "does NOT belong to this build" frontend/symbols.cpp'
check "the names logged for the TABLE are what the pages tell you to look for (symbols for ..., symbols: ... is not in this build)" '
    grep -q "symbols for %s" frontend/symbols.cpp && grep -q "is not in this build" frontend/symbols.cpp && grep -q "symbols for" CHANGELOG.md && grep -q "is not in this build" CHANGELOG.md'
check "make-symbols.py turns nm output into the table the reader wants (cut parameters, kept operator(), skipped data symbols, exit 0 on nothing)" '
    out="$(printf "0000000000401000 0000000000000040 T ns::f<int>(int, char) const\n0000000000401040 0000000000000010 t Foo::operator()(int)\n0000000000402000 0000000000000008 D a_variable\n" | python3 -I tools/make-symbols.py)" &&
    printf "%s\n" "$out" | grep -q "^# 2 functions" && printf "%s\n" "$out" | grep -qx "401000 40 ns::f<int>" && printf "%s\n" "$out" | grep -qx "401040 10 Foo::operator()" && ! printf "%s\n" "$out" | grep -q a_variable &&
    printf "" | python3 -I tools/make-symbols.py | grep -q "^# no symbols"'
check "a crash and a fatal error write the safe-mode marker FIRST, before logging" '
    awk "/^void OrbisFatal/{f=1} f&&/WriteCrashMarker\(\);/{print \"ok\"; exit} f&&/Log\(/{exit}" frontend/ps4_platform.cpp | grep -q ok &&
    awk "/^void CrashHandler/{f=1} f&&/WriteCrashMarker\(\);/{print \"ok\"; exit} f&&/Log\(/{exit}" frontend/ps4_platform.cpp | grep -q ok'
check "main.cpp names the marker for the crash handler and writes the alive line, and safe_mode.h has the 20 s rule" '
    grep -q "SetCrashMarker(Ps4Safe::CrashMarkerPath" frontend/main.cpp && grep -q "Ps4Safe::MarkAlive" frontend/main.cpp && grep -q "StayedAliveSeconds = 20" frontend/safe_mode.h'
check "safe_mode is a TESTS-tab option, auto by default, global only, and the public build ignores it" '
    settings_key safe_mode && settings_default safe_mode auto && grep "^| \`safe_mode\` |" docs/SETTINGS.md | grep -q "TESTS tab" && grep "^| \`safe_mode\` |" docs/SETTINGS.md | grep -q "global only" &&
    grep -q "kPublicBuild && LoadSettingsFile().Get(\"safe_mode\")" frontend/main.cpp'
check "the preset More FPS (test) no longer sets the two options that never ran on a console (CHANGELOG 0.9.7 fix 2)" '
    row="$(grep "^| More FPS (test) |" docs/SETTINGS.md)" && [ -n "$row" ] && ! printf "%s\n" "$row" | grep -q "async_presentation=on\|skip_cpu_invalidation=on" && printf "%s\n" "$row" | grep -q "jit_cache_mib=128"'
check "the symbol checks run in a test the CI workflow already builds (a new test file is not run there)" '
    grep -q "symbols_checks.h" tests/safe_mode_test.cpp && grep -q "tests/safe_mode_test.cpp" .github/workflows/check-patches.yml'

echo "== 0.9.7 fix 3: the table's base and anchors, the Eden log tail, the stack scan, 20 s"
check "CHANGELOG has the 0.9.7 fix 3 section above fix 2, says nothing in it ran on a console and that the black screen is not fixed" '
    a="$(grep -n "^### 0.9.7 fix 3" CHANGELOG.md | head -1 | cut -d: -f1)"; b="$(grep -n "^### 0.9.7 fix 2" CHANGELOG.md | head -1 | cut -d: -f1)"
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] && sed -n "${a},${b}p" CHANGELOG.md > /tmp/nxo_fix3_section.$$ &&
    grep -q "Nothing in this zip has run on a console" /tmp/nxo_fix3_section.$$ && grep -q "it does not fix the" /tmp/nxo_fix3_section.$$; r=$?; rm -f /tmp/nxo_fix3_section.$$; [ $r -eq 0 ]'
check "the table is made with base 0 and the header carries the anchor (package-eden.sh, make-symbols.py)" '
    grep -q -- "--base 0x0" package-eden.sh && ! grep -q -- "--base 0x400000" package-eden.sh &&
    printf "0000000000060ab0 0000000000000020 T Ps4SymAnchor\n0000000000070000 0000000000000100 T Other::f\n" | python3 -I tools/make-symbols.py | head -1 | grep -q "anchor=0x60ab0"'
check "make-symbols.py with --base 0x400000 still works and keeps the anchors when it leaves small functions out" '
    printf "0000000000460ab0 0000000000000020 T Ps4SymAnchor\n0000000000470000 0000000000000100 T Other::f\n" | python3 -I tools/make-symbols.py --base 0x400000 | head -1 | grep -q "anchor=0x60ab0" &&
    { printf "0000000000060ab0 0000000000000020 T Ps4SymAnchor\n"; i=0; while [ $i -lt 400 ]; do printf "%016x 0000000000000040 T fn_%d\n" $((0x100000 + i * 64)) $i; i=$((i + 1)); done; } | python3 -I tools/make-symbols.py --max-bytes 3000 | grep -q "^60ab0 20 Ps4SymAnchor$"'
check "the app has both anchors, in two object files, and the reader reads the anchor header and shifts" '
    grep -q "int Ps4SymAnchor(int" frontend/symbols.cpp && grep -q "int Ps4SymAnchor2(int" frontend/main.cpp && grep -q "ParseAnchorHeader" frontend/symbols.cpp && grep -q "Shift(g_notes" frontend/symbols.cpp'
check "the stall dump of the watchdog adds the Eden log tail after both stack dumps, and the thread dump scans the stack when it has no frame chain" '
    [ "$(grep -c "LogEdenTail(\"" frontend/ps4_platform.cpp)" -ge 2 ] && grep -q "ScanForCode" frontend/ps4_platform.cpp && grep -q "chain_frames == 0" frontend/ps4_platform.cpp'
check "the symbol checks cover the anchor header, the shift and the stack scan" '
    grep -q "ParseAnchorHeader" tests/symbols_checks.h && grep -q "Shift(" tests/symbols_checks.h && grep -q "ScanForCode" tests/symbols_checks.h'

echo "== 0.9.7 fix 4: the new menu, the CPU clock line, what the other ports teach"
check "CHANGELOG has the 0.9.7 fix 4 section above fix 3, says nothing in it ran on a console and that the black screen is not fixed, and names the clock line and the analysis" '
    a="$(grep -n "^### 0.9.7 fix 4" CHANGELOG.md | head -1 | cut -d: -f1)"; b="$(grep -n "^### 0.9.7 fix 3" CHANGELOG.md | head -1 | cut -d: -f1)"
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] && sed -n "${a},${b}p" CHANGELOG.md > /tmp/nxo_fix4_section.$$ &&
    grep -q "Nothing in this zip has run on a console" /tmp/nxo_fix4_section.$$ && grep -q "it does not fix the" /tmp/nxo_fix4_section.$$ &&
    grep -q "cpu clock:" /tmp/nxo_fix4_section.$$ && grep -q "docs/OTHER-PORTS.md" /tmp/nxo_fix4_section.$$; r=$?; rm -f /tmp/nxo_fix4_section.$$; [ $r -eq 0 ]'
check "docs/OTHER-PORTS.md says it is reading and not running, names the predication hang, the geometry-shader ring and the measured large pages, and says what could not be checked" '
    grep -q "Nothing here was run" docs/OTHER-PORTS.md && grep -q "SET_PREDICATION" docs/OTHER-PORTS.md && grep -q "ES→GS ring" docs/OTHER-PORTS.md &&
    grep -q "146 ns" docs/OTHER-PORTS.md && grep -q "188 ns" docs/OTHER-PORTS.md && grep -q "not known" docs/OTHER-PORTS.md && grep -q "could \*\*not\*\* be checked" docs/OTHER-PORTS.md'
check "README, ROADMAP, TECHNICAL and INSTALL carry the 0.9.7 fix 4" '
    grep -q "0.9.7 fix 4" README.md && grep -q "0.9.7 fix 4" docs/ROADMAP.md && grep -q "0.9.7 fix 4" docs/TECHNICAL.md && grep -q "0.9.7 fix 4" docs/INSTALL.md &&
    grep -q "OTHER-PORTS.md" README.md'
check "the menu has the column of tabs, the panel, one selection drawing and one scroll bar (and no more arrow marks for scrolling)" '
    grep -q "constexpr int SideW" frontend/menu/menu_ui.cpp && grep -q "void DrawPanel" frontend/menu/menu_ui.cpp && grep -q "void DrawSelection" frontend/menu/menu_ui.cpp &&
    grep -q "void DrawScrollbar" frontend/menu/menu_ui.cpp && ! grep -q "\"▲\"" frontend/menu/menu_ui.cpp && ! grep -q "\"▼\"" frontend/menu/menu_ui.cpp'
check "the menu test checks the new layout (the lit tab, the scroll bar, the card gaps)" '
    grep -q "the column of tabs lights the current tab" tests/menu_test.cpp && grep -q "a tab with more rows than fit shows a scroll bar" tests/menu_test.cpp &&
    grep -q "never reaches into the gap beside it" tests/menu_test.cpp'
check "the CPU clock line is wired (header, the call in the platform file, the checks in a test the CI workflow already builds)" '
    grep -q "cpu_clock.h" frontend/ps4_platform.cpp && grep -q "LogCpuClock();" frontend/ps4_platform.cpp && grep -q "cpu clock: the time-stamp counter" frontend/ps4_platform.cpp &&
    grep -q "cpu_clock_checks.h" tests/safe_mode_test.cpp && grep -q "CpuClockChecks::Run" tests/safe_mode_test.cpp && grep -q "tests/safe_mode_test.cpp" .github/workflows/check-patches.yml'

echo "== 0.9.8: ASTC in slabs, three renderer patches, the PS5 emulators"
check "the version is 0.9.8 and CHANGELOG, README, ROADMAP, TECHNICAL and PS5-PORTS carry it" '
    grep -q "EDEN_PS4_VERSION \"0.9.8\"" frontend/frontend.cmake && grep -q "^## \[0.9.8\]" CHANGELOG.md && grep -q "^## What is new in 0.9.8" README.md &&
    grep -q "^| \*\*0.9.8\*\* |" docs/ROADMAP.md && grep -q "^### 0.9.8" docs/TECHNICAL.md && [ -f docs/PS5-PORTS.md ] && grep -q "PS5-PORTS.md" README.md'
check "the 0.9.8 CHANGELOG entry says that nothing ran on a console and that no FPS is promised" '
    a="$(grep -n "^## \[0.9.8\]" CHANGELOG.md | head -1 | cut -d: -f1)"; b="$(grep -n "^## \[0.9.7\]" CHANGELOG.md | head -1 | cut -d: -f1)"
    sed -n "${a},${b}p" CHANGELOG.md | grep -q "Nothing in this zip has run on a console, and it promises no more FPS"'
check "patch 0036 is a git patch that changes ConvertImage only and carries a header that is the master copy (patches/eden-src/astc_recompress.h)" '
    [ "$(grep "^diff --git" patches/eden/0036-*.patch | wc -l | tr -d " ")" = 2 ] && grep "^diff --git" patches/eden/0036-*.patch | grep -q "texture_cache/util.cpp" &&
    grep "^diff --git" patches/eden/0036-*.patch | grep -q "textures/astc_recompress.h" && grep -q "AstcSlabs::Recompress" patches/eden/0036-*.patch && grep -q "DefaultSlabBytes" patches/eden-src/astc_recompress.h'
check "the slab checks run inside a test the CI workflow already builds, and the real-Eden-code test has its script" '
    grep -q "astc_recompress_checks.h" tests/safe_mode_test.cpp && grep -q "AstcRecompressChecks::Run" tests/safe_mode_test.cpp && grep -q "tests/safe_mode_test.cpp" .github/workflows/check-patches.yml &&
    [ -x tests/astc_recompress_real_test.sh ] && grep -q "astc_recompress_real_test.sh" tests/astc_recompress_real_test.cpp'
check "patches 0037, 0038 and 0039 are git patches of the Vulkan renderer (one or two files each) and say what is not known" '
    [ "$(grep "^diff --git" patches/eden/0037-*.patch | wc -l | tr -d " ")" = 1 ] && grep -q "vk_rasterizer.cpp" patches/eden/0037-*.patch && grep -q "not known whether" patches/eden/0037-*.patch &&
    [ "$(grep "^diff --git" patches/eden/0038-*.patch | wc -l | tr -d " ")" = 1 ] && grep -q "vulkan_memory_allocator.cpp" patches/eden/0038-*.patch && grep -q "is not known" patches/eden/0038-*.patch &&
    [ "$(grep "^diff --git" patches/eden/0039-*.patch | wc -l | tr -d " ")" = 2 ] && grep -q "vk_buffer_cache.cpp" patches/eden/0039-*.patch && grep -q "None is known to be reached" patches/eden/0039-*.patch'
check "the option text of the ASTC compression says slabs and that it never ran on a console (menu and SETTINGS.md)" '
    grep -q "fatias de até 8 MiB" frontend/menu/settings_model.cpp && grep -q "slabs of up to 8 MiB" frontend/menu/settings_model.cpp && grep -q "slabs of up to 8 MiB" docs/SETTINGS.md'
check "PS5-PORTS.md names the three emulators, says nothing was measured, calls the texture store an offline PC tool and counts the pin move" '
    grep -q "SwitchPS5" docs/PS5-PORTS.md && grep -q "ProsperoEden-Encore" docs/PS5-PORTS.md && grep -q "EglantinePS5" docs/PS5-PORTS.md && grep -q "Nothing here was measured" docs/PS5-PORTS.md &&
    grep -q "offline PC tool" docs/PS5-PORTS.md && grep -q "11 applied, 28 did not" docs/PS5-PORTS.md'

echo "== the lazy-memory test"
if out="$(bash tests/lazy_memory_test.sh 2>&1)"; then
    n="$(printf '%s\n' "$out" | grep -c '^  ok')"
    check "the test passes and prints $n checks" 'printf "%s\n" "$out" | grep -q "lazy memory test: PASSED"'
    check "README and CHANGELOG say $n checks" 'grep -q "$n checks" README.md && grep -q "$n checks" CHANGELOG.md'
else
    bad "tests/lazy_memory_test.sh fails"; printf '%s\n' "$out" | tail -5 | sed 's/^/        /'
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "docs check: PASSED ($checks checks)"
else
    echo "docs check: FAILED ($fail of $checks)"
    exit 1
fi
