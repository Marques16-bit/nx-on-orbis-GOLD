// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: the decisions about shaders that need no Eden header, so a host test can run them.
//
//   - how big the game's on-disk pipeline cache may be when the game starts (shader_preload);
//   - the text of the on-screen shader counter (the PS4's version of Cemu's "compiling shaders").
//
// Why a size limit and not a count with a stop token: Eden's PipelineCache::LoadDiskResources ends
// with workers.WaitForRequests(stop_token); once the token is stopped that call stops every thread
// of the pipeline worker pool, for good. A preload cut short that way leaves nobody to build the
// pipelines the game asks for afterwards (asynchronous shaders then never finish), so the cache is
// kept small BEFORE Eden reads it and Eden is always allowed to read what is left to the end.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace Ps4Shader {

/// What one pipeline of the cache is taken to weigh on disk. An estimate: boot.log prints the real
/// file size and Eden's own "Total Pipeline Count" line is in eden_log.txt, so it can be corrected.
inline constexpr std::uint64_t BytesPerPipeline = 32 * 1024;

enum class CacheAction {
    NoFile,       // no vulkan.bin yet: nothing to limit
    Kept,         // within the limit (or no limit)
    SetAside,     // over the limit: parked as vulkan.bin.bak (never deleted), a new cache starts
    Failed,       // over the limit but it could not be moved
};

struct CacheCheck {
    CacheAction action = CacheAction::NoFile;
    std::uint64_t size = 0;   // bytes of vulkan.bin before
    std::uint64_t limit = 0;  // bytes allowed (0 with `unlimited` false means "none")
    bool unlimited = false;
};

/// `cap` is the shader_preload setting: -1 all, 0 none, otherwise a number of pipelines.
/// `dir` is the game's cache folder (cache/shader/<title id>). 0.7.7: a cache over the limit is parked
/// under a new name (vulkan.bin.bak, .bak2, .bak3, ...), never deleted and no earlier backup replaced.
CacheCheck LimitCache(const std::filesystem::path& dir, int cap);

/// Moves vulkan.bin to the first free vulkan.bin.bak / .bak2 / .bak3 ... Nothing is deleted. False
/// when there is no vulkan.bin or it could not be moved; `parked_as` gets the new name.
bool ParkCache(const std::filesystem::path& dir, std::filesystem::path* parked_as);

// ---- 0.7.7: surviving a start that dies while the disk cache loads, without losing the cache ----
//
// Eden builds every pipeline of vulkan.bin before the game starts, several at a time. Three logs
// (tests 11, 0.7.2 and 0.7.6) end with the app dying inside those builder threads. Until 0.7.6 the
// answer was to throw the whole cache away. Now the app keeps a marker file while the load runs, and a
// start that finds it (the last one died) goes down a ladder: first the same cache with ONE builder
// thread (a race between builders is the leading suspect), then, if that dies too, the cache is
// parked (kept) and the game starts with an empty one. The marker is removed when the load ends.
struct LoadMarker {
    bool present = false;
    int attempt = 0;      // the number of the start that wrote it: 1 first, 2 after one death ...
    std::string title;    // the title id the load was for
};
LoadMarker ReadMarker(const std::filesystem::path& file);
bool WriteMarker(const std::filesystem::path& file, int attempt, const std::string& title);

enum class LoadMode {
    Normal, // no marker: load the cache as it is
    Safe,   // the last start died while loading: same cache, one builder thread
    Empty,  // that died too: park this game's cache, start with an empty one
};
/// What this start should do, given the marker the last one left (if any) and this game's title id.
/// A marker left by another game gives Safe (the cause may be the console, not the cache) with the
/// cache untouched.
LoadMode DecideLoad(const LoadMarker& marker, const std::string& title);
/// The attempt number to write for this start.
int NextAttempt(const LoadMarker& marker, const std::string& title);
const char* LoadModeName(LoadMode mode);

/// Eden's ShadersBuilding() counts the shaders of the current burst and starts again from 0 after a
/// quiet spell. This keeps a running total across bursts.
void CountBuilding(int building, int* last, int* total);

/// The counter's text for the screen, or "" when there is nothing to say.
/// `free_mib` is the largest free block of direct memory; below `LowMemoryMiB` a warning is added.
inline constexpr std::uint64_t LowMemoryMiB = 160;
std::string NoticeText(int building, int total, std::uint64_t free_mib, bool portuguese);

} // namespace Ps4Shader
