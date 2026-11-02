// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for frontend/menu/image_decode.h: the PNG, JPEG and BMP decoders against pictures decoded by
// another program (tests/data/*.raw = width, height (2 bytes each, little endian) then RGBA bytes made
// by Pillow from the same file), and against damaged files.
//   g++ -O2 -std=c++20 -Wall -Wextra -Ifrontend tests/image_decode_test.cpp -o image_decode_test
// It also covers frontend/menu/cover_loader.h (the covers/ folder: names, limits, bad files).
//   ./image_decode_test [tests/data]
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "menu/cover_loader.h"
#include "menu/image_decode.h"

static int g_failures = 0;
static void Check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}
static std::vector<std::uint8_t> Read(const std::string& path) {
    std::vector<std::uint8_t> v;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t buf[65536];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            v.insert(v.end(), buf, buf + n);
        }
        std::fclose(f);
    }
    return v;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data";
    struct Case { const char* file; double max_mean_error; };
    const Case cases[] = {{"cover_rgb.png", 0}, {"cover_rgba.png", 0}, {"cover_alpha.png", 0}, {"cover_pal.png", 0},
                          {"cover_gray.png", 0}, {"cover_24.bmp", 0}, {"cover_32.bmp", 0},
                          {"cover_444.jpg", 2.0}, {"cover_422.jpg", 4.0}, {"cover_420.jpg", 4.0}, {"cover_gray.jpg", 2.0},
                          {"cover_odd.jpg", 2.0}};
    for (const Case& c : cases) {
        const auto data = Read(dir + "/" + c.file);
        const auto ref = Read(dir + "/" + c.file + ".raw");
        Menu::Image::Pixels img;
        std::string why;
        const bool ok = Menu::Image::Decode(data.data(), data.size(), &img, &why);
        Check(ok && img.Ok(), std::string(c.file) + ": decodes" + (ok ? "" : " (" + why + ")"));
        if (!ok || ref.size() < 4) {
            continue;
        }
        const int rw = ref[0] | (ref[1] << 8), rh = ref[2] | (ref[3] << 8);
        Check(img.w == rw && img.h == rh, std::string(c.file) + ": size");
        if (img.w != rw || img.h != rh) {
            continue;
        }
        double sum = 0;
        long worst = 0;
        for (int i = 0; i < rw * rh; ++i) {
            const std::uint32_t p = img.px[std::size_t(i)];
            const std::uint8_t* q = ref.data() + 4 + std::size_t(i) * 4;
            const int dr = std::abs(int((p >> 16) & 255) - q[0]), dg = std::abs(int((p >> 8) & 255) - q[1]), db = std::abs(int(p & 255) - q[2]);
            // BMP 32-bit has no alpha in practice; PNG alpha is compared where it exists
            const int da = std::strstr(c.file, "bmp") ? 0 : std::abs(int(p >> 24) - q[3]);
            sum += dr + dg + db + da;
            worst = std::max<long>(worst, std::max({dr, dg, db, da}));
        }
        const double mean = sum / (rw * rh * 4.0);
        char what[200];
        std::snprintf(what, sizeof(what), "%s: pixels match Pillow's (mean error %.3f of 255, worst %ld)", c.file, mean, worst);
        Check(mean <= c.max_mean_error && (c.max_mean_error > 0 || worst == 0), what);
    }
    {
        const auto data = Read(dir + "/cover_prog.jpg");
        Menu::Image::Pixels img;
        std::string why;
        Check(!Menu::Image::Decode(data.data(), data.size(), &img, &why) && why.find("progressive") != std::string::npos,
              "progressive JPEG: refused with a reason (" + why + ")");
    }
    {
        // Damaged files must be refused, never crash or allocate wildly.
        for (const char* file : {"cover_rgb.png", "cover_444.jpg", "cover_24.bmp", "cover_pal.png"}) {
            const auto data = Read(dir + "/" + file);
            int refused = 0, tries = 0;
            for (std::size_t cut = 1; cut < data.size(); cut += std::max<std::size_t>(1, data.size() / 61)) {
                Menu::Image::Pixels img;
                std::string why;
                refused += !Menu::Image::Decode(data.data(), cut, &img, &why);
                ++tries;
            }
            Check(refused > 0, std::string(file) + ": truncated copies are refused (" + std::to_string(refused) + " of " + std::to_string(tries) + ") and none crashed");
            auto bad = data;
            for (std::size_t i = 24; i < bad.size(); i += 7) {
                bad[i] ^= 0x5A;
            }
            Menu::Image::Pixels img;
            std::string why;
            Menu::Image::Decode(bad.data(), bad.size(), &img, &why);
            Check(true, std::string(file) + ": scrambled copy handled without a crash");
        }
        Menu::Image::Pixels img;
        std::string why;
        const std::uint8_t junk[16] = {1, 2, 3};
        Check(!Menu::Image::Decode(junk, sizeof(junk), &img, &why), "junk bytes: refused");
        // a PNG header claiming 60000 x 60000 must not allocate
        std::uint8_t big[33] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0xEA, 0x60, 0, 0, 0xEA, 0x60, 8, 2, 0, 0, 0};
        Check(!Menu::Image::Decode(big, sizeof(big), &img, &why), "a PNG that claims 60000 x 60000: refused before allocating");
    }
    {
        const auto data = Read(dir + "/cover_rgb.png");
        Menu::Image::Pixels img, small, big;
        std::string why;
        Menu::Image::Decode(data.data(), data.size(), &img, &why);
        small = Menu::Image::Fit(img, 48, 48);
        Check(small.w == 48 && small.h == 36, "Fit: 96x72 into 48x48 keeps the proportions (48x36)");
        big = Menu::Image::Fit(img, 400, 400);
        Check(big.w == 96 && big.h == 72, "Fit: never grows unless asked");
        big = Menu::Image::Fit(img, 192, 192, true);
        Check(big.w == 192 && big.h == 144, "Fit: grows when asked");
    }
    {
        // cover_loader.h: which file belongs to which game, and what happens with bad ones
        namespace fs = std::filesystem;
        const fs::path covers = fs::temp_directory_path() / ("covers_test_" + std::to_string(std::rand()));
        fs::remove_all(covers);
        fs::create_directories(covers);
        auto put = [&](const char* from, const std::string& to) { fs::copy_file(fs::path(dir) / from, covers / to, fs::copy_options::overwrite_existing); };
        put("cover_rgb.png", "01009B90006DC000.png");                      // by title id
        put("cover_24.bmp", "celeste.BMP");                                 // by title, other case and extension
        put("cover_420.jpg", "Hollow Knight [0100633007D48000][v0].jpeg");  // by the whole file name
        put("cover_rgba.png", "0100000000010000.png");                      // by title id, game without the tag in the name
        put("cover_gray.jpg", "Dust.jpg");
        put("cover_gray.png", "Dust.png");                                  // png wins over jpg for the same name
        { std::ofstream bad(covers / "Broken.png", std::ios::binary); bad << "this is not a picture"; }
        { std::ofstream empty(covers / "Empty.png", std::ios::binary); }
        { std::ofstream note(covers / "notes.txt"); note << "ignored"; }
        std::vector<Menu::GameEntry> games = {
            {"Super Mario Maker 2 [01009B90006DC000][v0] (US).nsp", 1},
            {"Celeste.nsp", 2},
            {"Hollow Knight [0100633007D48000][v0].nsp", 3},
            {"Super_Mario_Odyssey [0100000000010000].xci", 4},
            {"Dust.nsp", 5},
            {"Broken.nsp", 6},
            {"Empty.nsp", 7},
            {"No picture.nsp", 8},
        };
        const Menu::Covers::Report report = Menu::Covers::Load(covers, games);
        Check(report.dir_found && report.dir_pictures == 8, "the folder is read; only picture files count (" + std::to_string(report.dir_pictures) + ")");
        Check(games[0].cover.Ok() && games[0].thumb.Ok(), "a cover found by title id");
        Check(games[1].cover.Ok(), "a cover found by title, whatever the case of the name and the extension");
        Check(games[2].cover.Ok(), "a cover found by the file name");
        Check(games[3].cover.Ok(), "a cover found by title id when the file name has other words");
        Check(games[4].cover.Ok(), "two files for one name: one is used");
        Check(!games[5].cover.Ok() && !games[6].cover.Ok(), "a broken or empty file leaves the game without a cover");
        Check(!games[7].cover.Ok() && !games[7].thumb.Ok(), "a game with no picture has none");
        Check(report.found == 5 && report.failed == 2, "report: 5 found, 2 failed (" + std::to_string(report.found) + ", " + std::to_string(report.failed) + ")");
        Check(!report.first_error.empty(), "report: says which file failed first (" + report.first_error + ")");
        Check(games[0].cover.w <= Menu::CoverSize && games[0].cover.h <= Menu::CoverSize && games[0].thumb.w <= Menu::ThumbSize &&
                  games[0].thumb.h <= Menu::ThumbSize,
              "sizes are within the card and the list");
        Check(games[0].cover.w == 400 || games[0].cover.h == 400, "a small picture is grown to fill the card");
        // limits
        Menu::Covers::Options tiny;
        tiny.max_cover_bytes = 1;
        std::vector<Menu::GameEntry> again = {{"Celeste.nsp", 2}};
        const Menu::Covers::Report capped = Menu::Covers::Load(covers, again, tiny);
        Check(capped.found == 1 && capped.thumbs_only == 1 && !again[0].cover.Ok() && again[0].thumb.Ok(), "over the memory cap: the thumbnail only");
        Menu::Covers::Options late;
        late.budget_ms = -1;
        std::vector<Menu::GameEntry> slow = {{"Celeste.nsp", 2}};
        const Menu::Covers::Report timed_out = Menu::Covers::Load(covers, slow, late);
        Check(timed_out.skipped_time == 1 && timed_out.found == 0, "out of time: the rest are skipped");
        Menu::Covers::Options small_files;
        small_files.max_file_bytes = 10;
        std::vector<Menu::GameEntry> big = {{"Celeste.nsp", 2}};
        Check(Menu::Covers::Load(covers, big, small_files).failed == 1, "a file over the size limit is refused");
        std::vector<Menu::GameEntry> none = {{"Celeste.nsp", 2}};
        const Menu::Covers::Report missing = Menu::Covers::Load(covers / "nope", none);
        Check(!missing.dir_found && missing.found == 0, "no covers/ folder: nothing happens");
        Check(Menu::Covers::TitleIdOf("A [01009b90006dc000][v0].nsp") == "01009B90006DC000", "title id read from the file name, upper-cased");
        Check(Menu::Covers::TitleIdOf("A [123].nsp").empty() && Menu::Covers::TitleIdOf("A [01009B90006DC00G].nsp").empty(), "not a title id: ignored");
        fs::remove_all(covers);
    }
    std::printf(g_failures == 0 ? "image decode test: PASSED\n" : "image decode test: FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
