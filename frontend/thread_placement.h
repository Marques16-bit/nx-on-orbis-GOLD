// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.9.6: which PS4 core each of Eden's threads runs on. Pure logic (no console calls), so it is tested on a PC
// (tests/memory_guard_test.cpp); ps4_platform.cpp asks the console for the cores the process may use and applies what this says.
//
// The idea (the 0.9.6 list, item 2): the four threads that run the Switch's four cores (CPUCore_0..3, one per emulated
// core, always busy) get a core of their own each, so the operating system cannot move one in the middle of a frame and the
// caches stay warm; every other named thread (the GPU command thread, the Vulkan worker, the presenter, the audio feeder,
// the file system and GPU services, the shader builders) is kept on the remaining cores, so none of them lands on a core
// that is running the game's code.
//
// What it cannot know, and the console must show: whether this is faster than the system's own scheduling (it is a
// guess about a 6-core Jaguar; the first run on a console decides it). settings.txt: cpu_affinity=off puts every thread
// back where the system puts it, and a start after a crash (safe mode) does that by itself.
//
// Careful choices: it only acts when the console says which cores the process may use (the mask of the main thread, asked
// once), uses at most six of them (an application gets six of the eight, the other two belong to the system), needs at
// least five (four for the game and one for the rest), and leaves the main thread and the unnamed threads alone.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace Ps4Placement {

/// settings.txt: cpu_affinity=auto|off
enum class Mode { Auto, Off };
inline bool ParseMode(std::string_view text, Mode* out) {
    if (text == "auto" || text == "on") {
        *out = Mode::Auto;
        return true;
    }
    if (text == "off") {
        *out = Mode::Off;
        return true;
    }
    return false;
}

/// The Switch has four cores; Eden runs each on its own host thread.
constexpr int GuestCores = 4;
/// An application gets six of the PS4's eight cores.
constexpr int MaxUsableCores = 6;

enum class Role {
    Free,      // left to the system
    Guest,     // one of the Switch's cores: a core of its own
    Secondary, // everything else: the cores the guest threads do not use
};

struct Decision {
    Role role = Role::Free;
    std::uint64_t mask = 0; // 0 for Free
};

/// The index of a guest core thread (-1 if `name` is not one). "CPUThread" is Eden's single-core mode: it counts as core 0.
inline int GuestIndexOf(std::string_view name) {
    if (name == "CPUThread") {
        return 0;
    }
    constexpr std::string_view prefix = "CPUCore_";
    if (name.size() == prefix.size() + 1 && name.substr(0, prefix.size()) == prefix) {
        const char digit = name[prefix.size()];
        if (digit >= '0' && digit < '0' + GuestCores) {
            return digit - '0';
        }
    }
    return -1;
}

class Plan {
public:
    Plan() = default;

    /// `allowed`: the bit mask of cores the process may run on, as the console reports it for the main thread.
    explicit Plan(std::uint64_t allowed) : allowed_(allowed) {
        for (int core = 0; core < 64 && static_cast<int>(cores_.size()) < MaxUsableCores; ++core) {
            if ((allowed >> core) & 1u) {
                cores_.push_back(core);
            }
        }
        if (static_cast<int>(cores_.size()) < GuestCores + 1) {
            cores_.clear(); // nothing sensible to do with fewer than five cores
            return;
        }
        for (size_t i = GuestCores; i < cores_.size(); ++i) {
            secondary_ |= std::uint64_t{1} << cores_[i];
        }
    }

    bool Active() const { return !cores_.empty(); }
    std::uint64_t Allowed() const { return allowed_; }
    std::uint64_t SecondaryMask() const { return secondary_; }
    int Cores() const { return static_cast<int>(cores_.size()); }
    /// The mask of the core the guest thread `index` (0..3) runs on; 0 if the plan is not active.
    std::uint64_t GuestMask(int index) const {
        return Active() && index >= 0 && index < GuestCores ? std::uint64_t{1} << cores_[static_cast<size_t>(index)] : 0;
    }

    /// What to do with the calling thread, from the name Eden gave it. The main thread, the frontend's own threads and unnamed
    /// threads are never passed here with an effect: "main" and "" are left alone.
    Decision For(std::string_view name) const {
        if (!Active() || name.empty() || name == "main") {
            return {};
        }
        const int guest = GuestIndexOf(name);
        if (guest >= 0) {
            return {Role::Guest, GuestMask(guest)};
        }
        return {Role::Secondary, secondary_};
    }

    /// One line for boot.log.
    std::string Describe() const {
        char line[256];
        if (!Active()) {
            std::snprintf(line, sizeof(line), "thread placement: not used (the process may use cores 0x%llx: fewer than %d usable)",
                          static_cast<unsigned long long>(allowed_), GuestCores + 1);
            return line;
        }
        std::string guest, rest;
        for (size_t i = 0; i < cores_.size(); ++i) {
            std::string& into = static_cast<int>(i) < GuestCores ? guest : rest;
            into += (into.empty() ? "" : ",") + std::to_string(cores_[i]);
        }
        std::snprintf(line, sizeof(line), "thread placement: the process may use cores 0x%llx -> the Switch's cores on PS4 cores %s, every other named thread on %s",
                      static_cast<unsigned long long>(allowed_), guest.c_str(), rest.c_str());
        return line;
    }

private:
    std::uint64_t allowed_ = 0;
    std::vector<int> cores_;
    std::uint64_t secondary_ = 0;
};

} // namespace Ps4Placement
