# eden-ps4: the PS4 frontend executable (included from inject.cmake after Eden's targets exist).
set(_fe "${EDEN_PS4_FRONTEND_DIR}")
add_executable(eden-ps4 "${_fe}/main.cpp" "${_fe}/ps4_platform.cpp" "${_fe}/ps4_platform.h"
    "${_fe}/heap_census.cpp" "${_fe}/arena_place.cpp" "${_fe}/mmap_watch.cpp" "${_fe}/arena_place.h" "${_fe}/symbols.cpp" "${_fe}/symbols.h" "${_fe}/menu/rom_menu.cpp" "${_fe}/menu/rom_menu.h"
    "${_fe}/menu/menu_ui.cpp" "${_fe}/menu/menu_ui.h" "${_fe}/menu/menu_canvas.cpp"
    "${_fe}/menu/menu_canvas.h" "${_fe}/menu/settings_model.cpp" "${_fe}/menu/settings_model.h"
    "${_fe}/firmware_zip.cpp" "${_fe}/firmware_zip.h" "${_fe}/video_hook.cpp" "${_fe}/video_hook.h"
    "${_fe}/shader_policy.cpp" "${_fe}/shader_policy.h" "${_fe}/eden_settings.h"
    "${_fe}/memory_guard.cpp" "${_fe}/memory_guard.h"
    "${_fe}/../tests/flat_map_check.cpp"
    "${_fe}/libc_wide_fixes.cpp"
    # compiler-rt 18.1.8 emulated-TLS runtime (the whole build uses -femulated-tls)
    "${_fe}/emutls/emutls.c")
target_include_directories(eden-ps4 PRIVATE "${PROJECT_SOURCE_DIR}/src" "${_fe}")
target_link_libraries(eden-ps4 PRIVATE common core audio_core video_core input_common hid_core
    frontend_common Vulkan::Headers GPUOpen::VulkanMemoryAllocator)
# Eden is built without RTTI; the window class derives from one of its classes.
target_compile_options(eden-ps4 PRIVATE -fno-rtti)

# The upstream scm_rev can stay cached across incremental builds. Give each configured test
# its own identity and retain it beside the binary, so boot.log can be paired with its ELF.
string(TIMESTAMP _ps4_build_stamp "%Y%m%dT%H%M%SZ" UTC)
execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE _ps4_eden_revision OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
# The project version (bump it with every release; see CHANGELOG.md) leads the build id, which is
# printed at the top of boot.log and in the release's ELF name.
set(EDEN_PS4_VERSION "0.9.8" CACHE STRING "nx-on-orbis version")
# 0.9.5: -DEDEN_PS4_PUBLIC=ON builds the release for everyone: no LOG tab, no EXPERIMENTAL tab, no screenshots and no sampling profiler
# (frontend/menu/settings_model.h, PublicBuild; main.cpp, kPublicBuild). The test builds (the default) keep all of it.
# ci/stage-eden.sh turns it on when an empty file named PUBLIC_BUILD is in the root of the repository; the build id then ends in "-public".
option(EDEN_PS4_PUBLIC "Build the release for everyone (no log screen, no experimental options, no screenshots)" OFF)
set(_ps4_build_id "v${EDEN_PS4_VERSION}-${_ps4_build_stamp}-${_ps4_eden_revision}")
if(EDEN_PS4_PUBLIC)
  set(_ps4_build_id "${_ps4_build_id}-public")
  target_compile_definitions(eden-ps4 PRIVATE EDEN_PS4_PUBLIC=1)
endif()
target_compile_definitions(eden-ps4 PRIVATE EDEN_PS4_BUILD_ID="${_ps4_build_id}")
file(WRITE "${CMAKE_BINARY_DIR}/eden-ps4-build-id.txt" "${_ps4_build_id}\n")

# RADV, statically linked through orbis-compat's Vulkan loader shim (its vkloader/CMakeLists.txt,
# inlined: deferred code cannot add a subdirectory).
set(_vkl "${ORBIS_COMPAT_DIR}/vkloader")
add_library(ps4-vkloader STATIC "${_vkl}/vkloader.c" "${_vkl}/vkthunks.c")
# PRIVATE: Eden compiles against its own Vulkan headers.
target_include_directories(ps4-vkloader PRIVATE "${_vkl}" "${ORBIS_MESA_SRC}/include")
file(GLOB _orbis_zlib "${ORBIS_MESA_BUILD}/subprojects/zlib-*/libz.a")
target_link_libraries(ps4-vkloader INTERFACE
    -Wl,--whole-archive "${ORBIS_MESA_BUILD}/src/amd/vulkan/libvulkan_radeon.a" -Wl,--no-whole-archive
    ${_orbis_zlib} -lSceGnmDriver -lSceVideoOut)
target_link_libraries(eden-ps4 PRIVATE ps4-vkloader)

# System libraries the frontend and the audio sink call.
target_link_libraries(eden-ps4 PRIVATE -lSceAudioOut -lScePad -lSceUserService -lSceSystemService
    -lSceSysmodule -lSceNet)

# Video hook (video_hook.cpp): RADV's WSI registers its buffers and flips through these two calls;
# the wrappers let the HUD, the pause menu and the colour swap draw over the game without Eden.
target_link_options(eden-ps4 PRIVATE -Wl,--wrap=sceVideoOutRegisterBuffers -Wl,--wrap=sceVideoOutSubmitFlip)
# 0.7.7: std::thread::hardware_concurrency() (libc++, std::__1) goes through __wrap_ in ps4_platform.cpp, so a
# start that follows a death while the shader cache loaded can build with one thread. If this symbol is spelled
# differently in the SDK's libc++ the wrap is simply never used and boot.log says the cap was never asked.
target_link_options(eden-ps4 PRIVATE -Wl,--wrap=_ZNSt3__16thread20hardware_concurrencyEv)
# The HUD's temperatures call two system functions; ci/stage-eden.sh retries with this OFF if the
# stub library of the SDK does not have them.
option(EDEN_PS4_HUD_TEMPERATURE "Show temperatures in the HUD" ON)
if(EDEN_PS4_HUD_TEMPERATURE)
  target_compile_definitions(eden-ps4 PRIVATE EDEN_PS4_HUD_TEMPERATURE=1)
endif()

# 0.9.7 fix: the driver's request for its memory arena goes through __wrap_sceKernelMapDirectMemory (arena_place.cpp), which places the arena inside one
# 4 GiB window (the Odyssey test died of an arena that straddled a boundary). Every other call is passed on unchanged.
target_link_options(eden-ps4 PRIVATE -Wl,--wrap=sceKernelMapDirectMemory)
# Heap census (heap_census.cpp): every allocation goes through the wrappers.
target_link_options(eden-ps4 PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
    -Wl,--wrap=free -Wl,--wrap=posix_memalign -Wl,--wrap=aligned_alloc)
# 0.9.7 fix: musl's malloc gets its memory through __mmap; the wrapper (mmap_watch.cpp) writes the requests of 16 MiB or more into a ring and passes
# every call on unchanged. If this build's musl never calls __mmap by that name the ring stays empty and the census says so.
target_link_options(eden-ps4 PRIVATE -Wl,--wrap=__mmap)

if(COMMAND ps4_create_eboot)
    ps4_create_eboot(eden-ps4)
endif()

# The boost::unordered_flat_map check also builds as a PC program (tests/); here it runs at startup.
set_source_files_properties("${_fe}/../tests/flat_map_check.cpp" PROPERTIES COMPILE_DEFINITIONS EDEN_PS4_NO_MAIN)
# The console's libc does not search 4-byte wchar_t units; these replace its wide memory functions.
set_source_files_properties("${_fe}/libc_wide_fixes.cpp" PROPERTIES COMPILE_OPTIONS "-fno-builtin")
