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
    SetAside,     // over the limit: moved to vulkan.bin.bak, a new cache starts
    Failed,       // over the limit but it could not be moved
};

struct CacheCheck {
    CacheAction action = CacheAction::NoFile;
    std::uint64_t size = 0;   // bytes of vulkan.bin before
    std::uint64_t limit = 0;  // bytes allowed (0 with `unlimited` false means "none")
    bool unlimited = false;
};

/// `cap` is the shader_preload setting: -1 all, 0 none, otherwise a number of pipelines.
/// `dir` is the game's cache folder (cache/shader/<title id>). The previous vulkan.bin.bak is replaced.
CacheCheck LimitCache(const std::filesystem::path& dir, int cap);

/// Eden's ShadersBuilding() counts the shaders of the current burst and starts again from 0 after a
/// quiet spell. This keeps a running total across bursts.
void CountBuilding(int building, int* last, int* total);

/// The counter's text for the screen, or "" when there is nothing to say.
/// `free_mib` is the largest free block of direct memory; below `LowMemoryMiB` a warning is added.
inline constexpr std::uint64_t LowMemoryMiB = 160;
std::string NoticeText(int building, int total, std::uint64_t free_mib, bool portuguese);

} // namespace Ps4Shader
