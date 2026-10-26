// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for frontend/firmware_zip.cpp: extracts zips made by another program (Python's zipfile)
// and compares every byte. Usage: firmware_zip_test DIR  (DIR holds the zips and "expected/").
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "firmware_zip.h"

namespace fs = std::filesystem;
static int g_failures = 0;
static void Check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}
static std::string Slurp(const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), {}};
}

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    const fs::path dir = argv[1];
    for (const char* name : {"stored.zip", "deflated.zip", "nested.zip", "mixed.zip"}) {
        const fs::path out = dir / (std::string("out-") + name);
        fs::remove_all(out);
        const auto r = Ps4Zip::ExtractNcas(dir / name, out);
        bool same = r.failed == 0 && r.extracted > 0;
        size_t files = 0;
        for (const auto& e : fs::directory_iterator(dir / "expected")) {
            if (e.path().extension() == ".nca" && std::string(name) != "mixed.zip") {
                same = same && Slurp(e.path()) == Slurp(out / e.path().filename());
                ++files;
            }
        }
        Check(same, std::string(name) + ": " + std::to_string(r.extracted) + " extracted, bytes identical");
        const auto again = Ps4Zip::ExtractNcas(dir / name, out);
        Check(again.extracted == 0 && again.skipped == r.extracted, std::string(name) + ": a second run skips them");
        Check(!fs::exists(out / "readme.txt"), std::string(name) + ": only .nca files are taken");
    }
    {
        const auto r = Ps4Zip::ExtractNcas(dir / "corrupt.zip", dir / "out-corrupt");
        Check(r.failed >= 1 && !fs::exists(dir / "out-corrupt" / "bad.nca") && !fs::exists(dir / "out-corrupt" / "bad.nca.part"),
              "corrupt.zip: the checksum catches it and nothing is left behind (" + r.first_error + ")");
    }
    {
        const auto r = Ps4Zip::ExtractNcas(dir / "notazip.zip", dir / "out-x");
        Check(r.failed == 1 && r.extracted == 0, "notazip.zip is refused (" + r.first_error + ")");
        const auto r2 = Ps4Zip::ExtractNcas(dir / "missing.zip", dir / "out-y");
        Check(r2.failed == 1, "a missing file is reported");
        const auto r3 = Ps4Zip::ExtractNcas(dir / "empty.zip", dir / "out-z");
        Check(r3.failed == 1 && r3.extracted == 0, "a zip with no .nca says so (" + r3.first_error + ")");
    }
    std::printf(g_failures == 0 ? "firmware zip test: PASSED\n" : "firmware zip test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
