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
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

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
check "players is in the SYSTEM tab (README)" 'awk "/^## SYSTEM tab/{f=1;next} /^## /{f=0} f" docs/SETTINGS.md | grep -q "^| \`players\` |"'
check "jit_page_reuse is an option, off by default, in the CPU AND MEMORY tab (README, CHANGELOG 0.9.2)" '
    settings_key jit_page_reuse && settings_default jit_page_reuse off && awk "/^## CPU AND MEMORY tab/{f=1;next} /^## /{f=0} f" docs/SETTINGS.md | grep -q "^| \`jit_page_reuse\` |"'
check "the profile More FPS (test) turns jit_page_reuse on (CHANGELOG 0.9.2)" 'grep "^| More FPS (test) |" docs/SETTINGS.md | grep -q "jit_page_reuse=on"'
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
    bash -n scripts/apply-jit-pack.sh && bash -n scripts/drop-jit-pack.sh && grep -q "apply-jit-pack.sh" scripts/fetch-eden.sh && grep -q "drop-jit-pack.sh" ci/stage-eden.sh'
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
