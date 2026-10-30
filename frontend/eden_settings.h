// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4 0.7.4: settings.txt options taken from Eden's own settings screens (VSync, window
// adapting filter, anti-aliasing, DMA accuracy, anisotropic filtering, region, ...).
//
// This port builds against ONE pinned Eden whose source is not in this repository, so the names
// below follow Eden's settings.h but were not compiled against it. Every setting and every enum
// member is therefore checked with a `requires` expression inside a template: one that this Eden
// does not have is skipped (Result::Missing, logged by main.cpp) instead of breaking the build.
//
// Apply() is a template over the settings struct (Settings::Values in main.cpp, a mock in
// tests/eden_settings_test.cpp).

#pragma once

#include <cstdlib>
#include <string>
#include <type_traits>

namespace Ps4Eden {

enum class Result {
    NotMine, // not a key/value this header knows
    Applied, // set
    Missing, // a known option, but this Eden has no such setting or value
};

namespace detail {

// Inside a function with `using E = <the setting's enum>`: set it to E::Member if that member
// exists in this Eden. A discarded `if constexpr` branch is not compiled, so a missing name is safe.
#define PS4_EDEN_SET_ENUM(setting, Member)           \
    if constexpr (requires { E::Member; }) {         \
        (setting).SetValue(E::Member);               \
        return Result::Applied;                      \
    }

template <typename S>
Result SetVsync(S& s, const std::string& value) {
    using E = std::remove_cvref_t<decltype(s.GetValue())>;
    if (value == "immediate") {
        PS4_EDEN_SET_ENUM(s, Immediate)
    } else if (value == "mailbox") {
        PS4_EDEN_SET_ENUM(s, Mailbox)
    } else if (value == "fifo") {
        PS4_EDEN_SET_ENUM(s, Fifo)
        PS4_EDEN_SET_ENUM(s, FIFO)
    } else if (value == "relaxed") {
        PS4_EDEN_SET_ENUM(s, FifoRelaxed)
        PS4_EDEN_SET_ENUM(s, FIFORelaxed)
    }
    return Result::Missing;
}

template <typename S>
Result SetScalingFilter(S& s, const std::string& value) {
    using E = std::remove_cvref_t<decltype(s.GetValue())>;
    if (value == "nearest") {
        PS4_EDEN_SET_ENUM(s, NearestNeighbor)
    } else if (value == "bilinear") {
        PS4_EDEN_SET_ENUM(s, Bilinear)
    } else if (value == "bicubic") {
        PS4_EDEN_SET_ENUM(s, Bicubic)
    } else if (value == "gaussian") {
        PS4_EDEN_SET_ENUM(s, Gaussian)
    } else if (value == "scaleforce") {
        PS4_EDEN_SET_ENUM(s, ScaleForce)
    } else if (value == "fsr") {
        PS4_EDEN_SET_ENUM(s, Fsr)
        PS4_EDEN_SET_ENUM(s, FSR)
    } else if (value == "area") {
        PS4_EDEN_SET_ENUM(s, Area)
    }
    return Result::Missing;
}

template <typename S>
Result SetAntiAliasing(S& s, const std::string& value) {
    using E = std::remove_cvref_t<decltype(s.GetValue())>;
    if (value == "none") {
        PS4_EDEN_SET_ENUM(s, None)
    } else if (value == "fxaa") {
        PS4_EDEN_SET_ENUM(s, Fxaa)
        PS4_EDEN_SET_ENUM(s, FXAA)
    } else if (value == "smaa") {
        PS4_EDEN_SET_ENUM(s, Smaa)
        PS4_EDEN_SET_ENUM(s, SMAA)
    }
    return Result::Missing;
}

template <typename S>
Result SetDmaAccuracy(S& s, const std::string& value) {
    using E = std::remove_cvref_t<decltype(s.GetValue())>;
    if (value == "default") {
        PS4_EDEN_SET_ENUM(s, Default)
    } else if (value == "unsafe") {
        PS4_EDEN_SET_ENUM(s, Unsafe)
    } else if (value == "safe") {
        PS4_EDEN_SET_ENUM(s, Safe)
    }
    return Result::Missing;
}

template <typename S>
Result SetAnisotropy(S& s, const std::string& value) {
    using E = std::remove_cvref_t<decltype(s.GetValue())>;
    if (value == "auto") {
        PS4_EDEN_SET_ENUM(s, Automatic)
    } else if (value == "default") {
        PS4_EDEN_SET_ENUM(s, Default)
    } else if (value == "2") {
        PS4_EDEN_SET_ENUM(s, X2)
    } else if (value == "4") {
        PS4_EDEN_SET_ENUM(s, X4)
    } else if (value == "8") {
        PS4_EDEN_SET_ENUM(s, X8)
    } else if (value == "16") {
        PS4_EDEN_SET_ENUM(s, X16)
    }
    return Result::Missing;
}

template <typename S>
Result SetGpuAccuracyMedium(S& s) {
    using E = std::remove_cvref_t<decltype(s.GetValue())>;
    PS4_EDEN_SET_ENUM(s, Medium)
    return Result::Missing;
}

template <typename S>
Result SetResolutionStep(S& s, const std::string& value) {
    using E = std::remove_cvref_t<decltype(s.GetValue())>;
    if (value == "0.25") {
        PS4_EDEN_SET_ENUM(s, Res1_4X)
    } else if (value == "1.25") {
        PS4_EDEN_SET_ENUM(s, Res5_4X)
    }
    return Result::Missing;
}

/// A number into a numeric setting.
template <typename S>
void SetNumber(S& s, int number) {
    s.SetValue(static_cast<std::remove_cvref_t<decltype(s.GetValue())>>(number));
}

/// 0 = Japan ... 6 = Taiwan, the order of Eden's region list; -1 when the name is not one.
inline int RegionIndex(const std::string& value) {
    static const char* const names[] = {"japan", "usa", "europe", "australia", "china", "korea", "taiwan"};
    for (int i = 0; i < 7; ++i) {
        if (value == names[i]) {
            return i;
        }
    }
    return -1;
}

inline bool OnOff(const std::string& value) {
    return value == "on" || value == "off";
}

/// True when `value` is one of the given words (so an unknown value is NotMine, not "Missing").
template <typename... Words>
bool OneOf(const std::string& value, Words... words) {
    return ((value == words) || ...);
}

#undef PS4_EDEN_SET_ENUM

} // namespace detail

/// One settings.txt line (key and value already without spaces or comments) into `v`.
///   vsync=fifo|mailbox|immediate|relaxed        (Eden default: fifo)
///   scaling_filter=bilinear|nearest|bicubic|gaussian|scaleforce|fsr|area   (default bilinear)
///   anti_aliasing=none|fxaa|smaa                (default none)
///   dma_accuracy=default|unsafe|safe            (default default)
///   anisotropy=default|auto|2|4|8|16            (default default)
///   gpu_accuracy=medium                         (low and high are main.cpp's)
///   resolution=0.25|1.25                        (0.5 ... 2 are main.cpp's)
///   sync_memory=on|off                          (default off)
///   skip_cpu_invalidation=on|off                (default off)
///   auto_stub=on|off                            (default off)
///   speed_percent=50|75|100|150|200             (default 100)
///   region=japan|usa|europe|australia|china|korea|taiwan   (default usa)
template <typename Values>
Result Apply(Values& v, const std::string& key, const std::string& value) {
    if (key == "vsync" && detail::OneOf(value, "fifo", "mailbox", "immediate", "relaxed")) {
        if constexpr (requires { v.vsync_mode.SetValue(v.vsync_mode.GetValue()); }) {
            return detail::SetVsync(v.vsync_mode, value);
        } else {
            return Result::Missing;
        }
    } else if (key == "scaling_filter" && detail::OneOf(value, "bilinear", "nearest", "bicubic", "gaussian", "scaleforce", "fsr", "area")) {
        if constexpr (requires { v.scaling_filter.SetValue(v.scaling_filter.GetValue()); }) {
            return detail::SetScalingFilter(v.scaling_filter, value);
        } else {
            return Result::Missing;
        }
    } else if (key == "anti_aliasing" && detail::OneOf(value, "none", "fxaa", "smaa")) {
        if constexpr (requires { v.anti_aliasing.SetValue(v.anti_aliasing.GetValue()); }) {
            return detail::SetAntiAliasing(v.anti_aliasing, value);
        } else {
            return Result::Missing;
        }
    } else if (key == "dma_accuracy" && detail::OneOf(value, "default", "unsafe", "safe")) {
        if constexpr (requires { v.dma_accuracy.SetValue(v.dma_accuracy.GetValue()); }) {
            return detail::SetDmaAccuracy(v.dma_accuracy, value);
        } else {
            return Result::Missing;
        }
    } else if (key == "anisotropy" && detail::OneOf(value, "default", "auto", "2", "4", "8", "16")) {
        if constexpr (requires { v.max_anisotropy.SetValue(v.max_anisotropy.GetValue()); }) {
            return detail::SetAnisotropy(v.max_anisotropy, value);
        } else {
            return Result::Missing;
        }
    } else if (key == "gpu_accuracy" && value == "medium") {
        if constexpr (requires { v.gpu_accuracy.SetValue(v.gpu_accuracy.GetValue()); }) {
            return detail::SetGpuAccuracyMedium(v.gpu_accuracy);
        } else {
            return Result::Missing;
        }
    } else if (key == "resolution" && (value == "0.25" || value == "1.25")) {
        if constexpr (requires { v.resolution_setup.SetValue(v.resolution_setup.GetValue()); }) {
            return detail::SetResolutionStep(v.resolution_setup, value);
        } else {
            return Result::Missing;
        }
    } else if (key == "sync_memory" && detail::OnOff(value)) {
        if constexpr (requires { v.sync_memory_operations.SetValue(true); }) {
            v.sync_memory_operations.SetValue(value == "on");
            return Result::Applied;
        } else {
            return Result::Missing;
        }
    } else if (key == "skip_cpu_invalidation" && detail::OnOff(value)) {
        if constexpr (requires { v.skip_cpu_inner_invalidation.SetValue(true); }) {
            v.skip_cpu_inner_invalidation.SetValue(value == "on");
            return Result::Applied;
        } else {
            return Result::Missing;
        }
    } else if (key == "auto_stub" && detail::OnOff(value)) {
        if constexpr (requires { v.use_auto_stub.SetValue(true); }) {
            v.use_auto_stub.SetValue(value == "on");
            return Result::Applied;
        } else {
            return Result::Missing;
        }
    } else if (key == "speed_percent" &&
               (value == "50" || value == "75" || value == "100" || value == "150" || value == "200")) {
        if constexpr (requires { v.speed_limit.SetValue(v.speed_limit.GetValue()); }) {
            detail::SetNumber(v.speed_limit, std::atoi(value.c_str()));
            return Result::Applied;
        } else {
            return Result::Missing;
        }
    } else if (key == "region" && detail::RegionIndex(value) >= 0) {
        if constexpr (requires { v.region_index.SetValue(v.region_index.GetValue()); }) {
            detail::SetNumber(v.region_index, detail::RegionIndex(value));
            return Result::Applied;
        } else {
            return Result::Missing;
        }
    }
    return Result::NotMine;
}

} // namespace Ps4Eden
