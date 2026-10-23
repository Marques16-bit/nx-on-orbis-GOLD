# PS4 toolchain for Eden: the orbis-sdk toolchain, with LLVM 18's libc++ (built by eden-ps4 in
# libcxx18/) in place of the SDK's libc++ 11, and the include overlay that makes musl's headers
# agree with it (toolchain/include-overlay: math.h, stdlib.h).
# Where things live. The CI (and anyone building off Windows) sets NXO_ROOT to this repository;
# without it the defaults are the original author's Windows machine.
if(DEFINED ENV{NXO_ROOT})
  set(NXO_ROOT "$ENV{NXO_ROOT}")
else()
  set(NXO_ROOT "C:/Users/alejo/eden-ps4")
endif()
include("${NXO_ROOT}/sdk-dl/orbis-sdk-v1/toolchain/orbis-sdk.cmake")
set(EDEN_PS4_LIBCXX "${NXO_ROOT}/libcxx18")
set(EDEN_PS4_OVERLAY "${NXO_ROOT}/toolchain/include-overlay")
string(REPLACE "-isystem ${OO_PS4_TOOLCHAIN}/include/c++/v1"
       "-nostdinc++ -fexperimental-library -isystem ${EDEN_PS4_LIBCXX}/include/c++/v1 -isystem ${EDEN_PS4_OVERLAY}"
       CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT}")
string(REPLACE "-isystem ${ORBIS_COMPAT_DIR}/include"
       "-isystem ${EDEN_PS4_OVERLAY} -isystem ${ORBIS_COMPAT_DIR}/include"
       CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
if(NOT CMAKE_CXX_FLAGS_INIT MATCHES "libcxx18")
  message(FATAL_ERROR "eden-ps4.cmake: could not swap the SDK's libc++ include for libcxx18")
endif()
# -L to the new libc++ first, so -lc++ finds it and never the SDK's (which also carries an
# unwinder and libc++abi 11 inside it); the unwinder then comes from the SDK's libunwind.a.
string(REPLACE "-L${OO_PS4_TOOLCHAIN}/lib" "-L${EDEN_PS4_LIBCXX}/lib -L${OO_PS4_TOOLCHAIN}/lib"
       CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")
set(CMAKE_C_STANDARD_LIBRARIES   "-lc++experimental -lc++ -lc++abi -lunwind -lc -lkernel ${ORBIS_CRT1}")
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES}")
# Frame pointers everywhere: the console's crash reports walk rbp (no debugger on the PS4).
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -fno-omit-frame-pointer")
# Emulated TLS for everything built here (Eden and its bundled libraries): Eden has ~9 KiB of
# initialised thread_local data, and on the console the native static-TLS block came out
# misplaced (eden-ps4 test 1: KernelCore's thread_local read the neighbouring field). ProsperoEden
# builds Eden the same way on the PS5. The runtime is compiler-rt's emutls.c (frontend/emutls).
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -femulated-tls")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -femulated-tls")
# The PS4's CPU (AMD Jaguar): SSE4.2, AVX, BMI1, F16C, MOVBE, POPCNT/LZCNT, tuned for its 128-bit
# units. Without it the compiler targets baseline x86-64 (SSE2), and races were CPU-bound (test 22).
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -march=btver2")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -march=btver2")
# On a Linux host clang's FreeBSD driver, given no --sysroot, also searches the host's /usr/include:
# __has_include(<linux/random.h>) and configure checks then see Linux headers that do not exist on
# the PS4 (libc++'s random.cpp: "'asm/types.h' file not found"). -nostdlibinc drops the system
# directories and keeps the compiler's own headers plus the -isystem paths. On Windows there is
# no /usr/include, so the flags stay exactly as they were.
if(NOT CMAKE_HOST_WIN32)
  set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -nostdlibinc")
  set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -nostdlibinc")
endif()
# A Linux host also has its own libraries and headers (zlib, lz4, OpenSSL, opus...). Left alone,
# find_package() picks them up and puts /usr/include on the compile line ahead of the SDK's headers
# ("/usr/include/stdint.h: 'bits/libc-header-start.h' file not found"), or worse, links host
# libraries into the PS4 executable. Only the PS4 prefixes may be searched for packages, libraries
# and headers; programs (glslang, python, git...) still come from the host. On Windows there is
# nothing of the kind to find, and none of this is applied.
if(NOT CMAKE_HOST_WIN32)
  list(APPEND CMAKE_FIND_ROOT_PATH "${NXO_ROOT}/prefix" "${EDEN_PS4_LIBCXX}" "${OO_PS4_TOOLCHAIN}")
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(ENV{PKG_CONFIG_PATH} "")
  set(ENV{PKG_CONFIG_LIBDIR} "${NXO_ROOT}/prefix/lib/pkgconfig")
endif()
