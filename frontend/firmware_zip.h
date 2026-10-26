// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: reads the firmware straight from the .zip it is found as (a "Firmware 20.5.0.zip").
//
// Standard-library only. Handles the two methods the firmware zips use (stored and deflated), checks
// every file's CRC-32, and writes through a temporary name, so a half-copied .nca is never left under
// its real name.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace Ps4Zip {

struct Result {
    std::size_t extracted = 0; // written now
    std::size_t skipped = 0;   // already there with the same size
    std::size_t failed = 0;
    std::string first_error;
};

/// Extracts every *.nca of the zip, from whatever folder it is in, into `target` under its own file
/// name. Does nothing for the entries that are not .nca files.
Result ExtractNcas(const std::filesystem::path& zip, const std::filesystem::path& target);

} // namespace Ps4Zip
