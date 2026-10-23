# LLVM 18 libc++ / libc++abi for the PS4 -> libcxx18/ (docs/BUILDING.md, section 3).
set -euo pipefail
cd "$NXO_ROOT"
mkdir -p deps

if [ ! -d deps/llvm-project/.git ]; then
    rm -rf deps/llvm-project
    git clone --depth 1 --filter=blob:none --sparse --branch llvmorg-18.1.8 \
        https://github.com/llvm/llvm-project.git deps/llvm-project
fi
# llvm/utils is needed too: runtimes/CMakeLists.txt adds llvm/utils/llvm-lit as a subdirectory.
git -C deps/llvm-project sparse-checkout set runtimes libcxx libcxxabi libunwind cmake llvm/cmake \
    llvm/utils third-party

# OpenOrbis has FreeBSD's sendfile: keep libc++'s Linux sendfile path out. The condition that guards
# the sendfile include gets "&& !defined(__ORBIS__)" (BUILDING.md: "line ~40").
python3 - <<'PY'
import re, sys
p = 'deps/llvm-project/libcxx/src/filesystem/operations.cpp'
lines = open(p, encoding='utf-8').read().split('\n')
idx = next((i for i, l in enumerate(lines[:300]) if 'sendfile' in l.lower()), None)
if idx is None:
    sys.exit("no 'sendfile' in the first 300 lines of " + p)
j = idx
while j >= 0 and not re.match(r'\s*#\s*(if|ifdef|ifndef|elif)\b', lines[j]):
    j -= 1
if j < 0:
    sys.exit("no #if above the sendfile line %d of %s" % (idx + 1, p))
l = lines[j]
if '__ORBIS__' in l:
    print('operations.cpp already patched')
else:
    cond = re.sub(r'\s*(//.*|/\*.*\*/)\s*$', '', l)
    m = re.match(r'(\s*#\s*)ifdef\s+(\w+)$', cond)
    if m:
        lines[j] = '%sif defined(%s) && !defined(__ORBIS__)' % (m.group(1), m.group(2))
    else:
        m = re.match(r'(\s*#\s*(?:if|elif))\s+(.*)$', cond)
        if not m:
            sys.exit("cannot patch this line (%d): %s" % (j + 1, l))
        lines[j] = '%s (%s) && !defined(__ORBIS__)' % (m.group(1), m.group(2))
    open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    print('patched line %d of operations.cpp:\n  was: %s\n  now: %s' % (j + 1, l, lines[j]))
for k in range(max(0, j - 1), min(len(lines), j + 6)):
    print('%5d| %s' % (k + 1, lines[k]))
PY

cmake -S deps/llvm-project/runtimes -B build-libcxx -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NXO_ROOT/toolchain/runtimes-ps4.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF \
    -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_ENABLE_NEW_DELETE_DEFINITIONS=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX="$NXO_ROOT/libcxx18"
cmake --build build-libcxx -j"$JOBS"
cmake --install build-libcxx

# As the SDK's own libc++ 11 does.
echo '#undef __FreeBSD__' >> libcxx18/include/c++/v1/__config_site
ls libcxx18/lib
rm -rf build-libcxx deps/llvm-project   # not needed any more; keeps the cache small
