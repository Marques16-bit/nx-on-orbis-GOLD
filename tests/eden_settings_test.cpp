// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of frontend/eden_settings.h with mock settings structs (no Eden needed).
//   g++ -std=c++20 -Ifrontend tests/eden_settings_test.cpp frontend/menu/settings_model.cpp -o eden_settings_test
//   ./eden_settings_test
#include <cstdio>
#include <string>

#include "eden_settings.h"
#include "menu/settings_model.h"

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

template <typename T>
struct Setting {
    T value{};
    const T& GetValue() const { return value; }
    void SetValue(const T& v) { value = v; }
};

// An Eden like the current master: Fifo / FifoRelaxed, every setting present.
enum class VSyncNew { Immediate, Mailbox, Fifo, FifoRelaxed };
// An older spelling: FIFO / FIFORelaxed.
enum class VSyncOld { Immediate, Mailbox, FIFO, FIFORelaxed };
enum class Filter { NearestNeighbor, Bilinear, Bicubic, Gaussian, ScaleForce, Fsr, Area };
enum class Aa { None, Fxaa, Smaa };
enum class Dma { Default, Unsafe, Safe };
enum class Aniso { Automatic, Default, X2, X4, X8, X16 };
enum class Gpu { Low, Medium, High };
enum class Res { Res1_4X, Res1_2X, Res3_4X, Res1X, Res5_4X, Res3_2X, Res2X };

struct ValuesNew {
    Setting<VSyncNew> vsync_mode{VSyncNew::Fifo};
    Setting<Filter> scaling_filter{Filter::Bilinear};
    Setting<Aa> anti_aliasing{Aa::None};
    Setting<Dma> dma_accuracy{Dma::Default};
    Setting<Aniso> max_anisotropy{Aniso::Default};
    Setting<Gpu> gpu_accuracy{Gpu::Low};
    Setting<Res> resolution_setup{Res::Res1X};
    Setting<bool> sync_memory_operations{false};
    Setting<bool> skip_cpu_inner_invalidation{false};
    Setting<bool> use_auto_stub{false};
    Setting<unsigned short> speed_limit{100};
    Setting<int> region_index{1};
};

// An Eden with the old vsync spelling, no Medium accuracy, no 1/4x or 5/4x, and only some settings.
enum class GpuTwo { Low, High };
enum class ResOld { Res1_2X, Res3_4X, Res1X };
struct ValuesOld {
    Setting<VSyncOld> vsync_mode{VSyncOld::FIFO};
    Setting<Dma> dma_accuracy{Dma::Default};
    Setting<GpuTwo> gpu_accuracy{GpuTwo::Low};
    Setting<ResOld> resolution_setup{ResOld::Res1X};
};

struct ValuesEmpty {};

using Ps4Eden::Apply;
using Ps4Eden::Result;

} // namespace

int main() {
    {
        ValuesNew v;
        Check(Apply(v, "vsync", "mailbox") == Result::Applied && v.vsync_mode.value == VSyncNew::Mailbox, "vsync=mailbox");
        Check(Apply(v, "vsync", "relaxed") == Result::Applied && v.vsync_mode.value == VSyncNew::FifoRelaxed, "vsync=relaxed");
        Check(Apply(v, "vsync", "fifo") == Result::Applied && v.vsync_mode.value == VSyncNew::Fifo, "vsync=fifo");
        Check(Apply(v, "vsync", "immediate") == Result::Applied && v.vsync_mode.value == VSyncNew::Immediate, "vsync=immediate");
        Check(Apply(v, "vsync", "bogus") == Result::NotMine, "an unknown value is NotMine");
        Check(Apply(v, "scaling_filter", "fsr") == Result::Applied && v.scaling_filter.value == Filter::Fsr, "scaling_filter=fsr");
        Check(Apply(v, "scaling_filter", "nearest") == Result::Applied && v.scaling_filter.value == Filter::NearestNeighbor, "scaling_filter=nearest");
        Check(Apply(v, "anti_aliasing", "smaa") == Result::Applied && v.anti_aliasing.value == Aa::Smaa, "anti_aliasing=smaa");
        Check(Apply(v, "dma_accuracy", "safe") == Result::Applied && v.dma_accuracy.value == Dma::Safe, "dma_accuracy=safe");
        Check(Apply(v, "anisotropy", "16") == Result::Applied && v.max_anisotropy.value == Aniso::X16, "anisotropy=16");
        Check(Apply(v, "anisotropy", "auto") == Result::Applied && v.max_anisotropy.value == Aniso::Automatic, "anisotropy=auto");
        Check(Apply(v, "gpu_accuracy", "medium") == Result::Applied && v.gpu_accuracy.value == Gpu::Medium, "gpu_accuracy=medium");
        Check(Apply(v, "gpu_accuracy", "low") == Result::NotMine, "gpu_accuracy=low stays main.cpp's");
        Check(Apply(v, "resolution", "0.25") == Result::Applied && v.resolution_setup.value == Res::Res1_4X, "resolution=0.25");
        Check(Apply(v, "resolution", "1.25") == Result::Applied && v.resolution_setup.value == Res::Res5_4X, "resolution=1.25");
        Check(Apply(v, "resolution", "0.75") == Result::NotMine, "resolution=0.75 stays main.cpp's");
        Check(Apply(v, "sync_memory", "on") == Result::Applied && v.sync_memory_operations.value, "sync_memory=on");
        Check(Apply(v, "skip_cpu_invalidation", "on") == Result::Applied && v.skip_cpu_inner_invalidation.value, "skip_cpu_invalidation=on");
        Check(Apply(v, "auto_stub", "on") == Result::Applied && v.use_auto_stub.value, "auto_stub=on");
        Check(Apply(v, "auto_stub", "off") == Result::Applied && !v.use_auto_stub.value, "auto_stub=off");
        Check(Apply(v, "speed_percent", "150") == Result::Applied && v.speed_limit.value == 150, "speed_percent=150");
        Check(Apply(v, "speed_percent", "123") == Result::NotMine, "speed_percent=123 is not a menu value");
        Check(Apply(v, "region", "europe") == Result::Applied && v.region_index.value == 2, "region=europe is index 2");
        Check(Apply(v, "region", "japan") == Result::Applied && v.region_index.value == 0, "region=japan is index 0");
        Check(Apply(v, "region", "taiwan") == Result::Applied && v.region_index.value == 6, "region=taiwan is index 6");
        Check(Apply(v, "region", "mars") == Result::NotMine, "region=mars is NotMine");
        Check(Apply(v, "cpu_accuracy", "auto") == Result::NotMine, "a key of main.cpp is NotMine");
    }
    {
        ValuesOld v;
        Check(Apply(v, "vsync", "fifo") == Result::Applied && v.vsync_mode.value == VSyncOld::FIFO, "old spelling: vsync=fifo -> FIFO");
        Check(Apply(v, "vsync", "relaxed") == Result::Applied && v.vsync_mode.value == VSyncOld::FIFORelaxed, "old spelling: relaxed -> FIFORelaxed");
        Check(Apply(v, "dma_accuracy", "unsafe") == Result::Applied && v.dma_accuracy.value == Dma::Unsafe, "a setting this Eden has works");
        Check(Apply(v, "gpu_accuracy", "medium") == Result::Missing, "no Medium in this Eden: Missing, not a build error");
        Check(Apply(v, "resolution", "0.25") == Result::Missing && Apply(v, "resolution", "1.25") == Result::Missing,
              "no 1/4x or 5/4x in this Eden: Missing");
        Check(Apply(v, "scaling_filter", "fsr") == Result::Missing, "no scaling_filter setting: Missing");
        Check(Apply(v, "region", "usa") == Result::Missing, "no region_index setting: Missing");
        Check(Apply(v, "speed_percent", "100") == Result::Missing, "no speed_limit setting: Missing");
    }
    {
        ValuesEmpty v;
        Check(Apply(v, "vsync", "fifo") == Result::Missing && Apply(v, "anti_aliasing", "none") == Result::Missing &&
                  Apply(v, "sync_memory", "off") == Result::Missing && Apply(v, "auto_stub", "on") == Result::Missing,
              "an Eden with none of them: every option is Missing");
        Check(Apply(v, "mode", "docked") == Result::NotMine, "...and other keys are still NotMine");
    }
    {
        // The menu and this header must agree: every value the menu can write for one of the 0.7.4
        // options is a value Apply() knows (never NotMine), and on a full Eden it is applied.
        const char* const keys[] = {"vsync", "scaling_filter", "anti_aliasing", "anisotropy", "dma_accuracy", "sync_memory",
                                    "speed_percent", "skip_cpu_invalidation", "auto_stub", "region"};
        bool all_found = true, all_known = true, all_applied = true;
        for (const char* key : keys) {
            const Menu::Option* option = Menu::FindOption(key);
            if (option == nullptr) {
                all_found = false;
                continue;
            }
            for (const Menu::Choice& choice : option->choices) {
                ValuesNew full;
                all_known = all_known && Apply(full, key, choice.value) != Result::NotMine;
                all_applied = all_applied && Apply(full, key, choice.value) == Result::Applied;
            }
        }
        Check(all_found, "the menu has every 0.7.4 option");
        Check(all_known, "every value the menu writes is known to Apply()");
        Check(all_applied, "...and applied on an Eden that has every setting");
        // gpu_accuracy and resolution share their keys with main.cpp: only the new values are this header's.
        bool gpu = true, res = true;
        for (const Menu::Choice& choice : Menu::FindOption("gpu_accuracy")->choices) {
            ValuesNew full;
            const std::string v = choice.value;
            gpu = gpu && (v == "medium" ? Apply(full, "gpu_accuracy", v) == Result::Applied : Apply(full, "gpu_accuracy", v) == Result::NotMine);
        }
        for (const Menu::Choice& choice : Menu::FindOption("resolution")->choices) {
            ValuesNew full;
            const std::string v = choice.value;
            const bool mine = v == "0.25" || v == "1.25";
            res = res && (mine ? Apply(full, "resolution", v) == Result::Applied : Apply(full, "resolution", v) == Result::NotMine);
        }
        Check(gpu, "gpu_accuracy: the menu's low/high stay main.cpp's, medium is Apply()'s");
        Check(res, "resolution: the menu's 0.25 and 1.25 are Apply()'s, the others main.cpp's");
        const Menu::Option* region = Menu::FindOption("region");
        Check(region != nullptr && std::string(region->choices[size_t(region->default_index)].value) == "usa", "region defaults to USA, like Eden");
    }
    std::printf(g_failures == 0 ? "eden settings test: PASSED\n" : "eden settings test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
