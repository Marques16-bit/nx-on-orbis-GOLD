// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: reads the firmware straight from a .zip. See firmware_zip.h.
//
// The inflate is the one of RFC 1951 in its smallest form (the canonical-Huffman decoding of zlib's
// "puff"), written to stream: input is read in blocks, output goes through a 32 KiB window.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#include "firmware_zip.h"

namespace Ps4Zip {

namespace {

namespace fs = std::filesystem;

std::uint32_t Crc32(std::uint32_t crc, const std::uint8_t* data, std::size_t size) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

std::uint16_t U16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

/// A decoder of the deflate stream of one zip entry, writing its output to a file.
class Inflater {
public:
    Inflater(std::FILE* in, std::uint64_t compressed_size, std::FILE* out)
        : in_(in), left_(compressed_size), out_(out) {}

    /// 0 on success, a message otherwise. After success crc() and written() hold the results.
    const char* Run() {
        for (;;) {
            const int last = Bits(1);
            const int type = Bits(2);
            if (failed_) {
                return "the compressed data ended early";
            }
            const char* error = nullptr;
            if (type == 0) {
                error = Stored();
            } else if (type == 1) {
                error = Fixed();
            } else if (type == 2) {
                error = Dynamic();
            } else {
                error = "a block of an unknown type";
            }
            if (error != nullptr) {
                return error;
            }
            if (last) {
                break;
            }
        }
        return Flush() ? nullptr : "could not write the output";
    }

    std::uint32_t crc() const { return crc_; }
    std::uint64_t written() const { return total_; }

private:
    struct Huffman {
        std::int16_t count[16];
        std::int16_t symbol[288];
    };

    static constexpr int WindowSize = 1 << 15;

    int Byte() {
        if (pos_ == len_) {
            if (left_ == 0) {
                failed_ = true;
                return 0;
            }
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left_, sizeof(buffer_)));
            len_ = std::fread(buffer_, 1, want, in_);
            pos_ = 0;
            left_ -= len_;
            if (len_ == 0) {
                failed_ = true;
                return 0;
            }
        }
        return buffer_[pos_++];
    }

    int Bits(int need) {
        std::uint32_t value = bitbuf_;
        while (bitcnt_ < need) {
            value |= static_cast<std::uint32_t>(Byte()) << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = value >> need;
        bitcnt_ -= need;
        return static_cast<int>(value & ((1u << need) - 1));
    }

    bool Put(std::uint8_t b) {
        window_[total_ & (WindowSize - 1)] = b;
        ++total_;
        out_buf_[out_len_++] = b;
        return out_len_ < sizeof(out_buf_) || Flush();
    }

    bool Flush() {
        if (out_len_ > 0) {
            if (std::fwrite(out_buf_, 1, out_len_, out_) != out_len_) {
                return false;
            }
            crc_ = Crc32(crc_, out_buf_, out_len_);
            out_len_ = 0;
        }
        return true;
    }

    const char* Stored() {
        bitbuf_ = 0;
        bitcnt_ = 0;
        const int len = Byte() | (Byte() << 8);
        const int inverse = Byte() | (Byte() << 8);
        if (failed_ || len != (~inverse & 0xFFFF)) {
            return "a bad stored block";
        }
        for (int i = 0; i < len; ++i) {
            const int b = Byte();
            if (failed_) {
                return "the compressed data ended early";
            }
            if (!Put(static_cast<std::uint8_t>(b))) {
                return "could not write the output";
            }
        }
        return nullptr;
    }

    /// Builds the decoding table for code lengths; returns 0 for a complete code, > 0 incomplete.
    static int Construct(Huffman& h, const std::int16_t* length, int n) {
        std::memset(h.count, 0, sizeof(h.count));
        for (int s = 0; s < n; ++s) {
            ++h.count[length[s]];
        }
        if (h.count[0] == n) {
            return 0;
        }
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) {
                return left; // over-subscribed
            }
        }
        std::int16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) {
            offs[len + 1] = static_cast<std::int16_t>(offs[len] + h.count[len]);
        }
        for (int s = 0; s < n; ++s) {
            if (length[s] != 0) {
                h.symbol[offs[length[s]]++] = static_cast<std::int16_t>(s);
            }
        }
        return left;
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= Bits(1);
            const int count = h.count[len];
            if (code - count < first) {
                return h.symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (failed_) {
                return -1;
            }
        }
        return -1;
    }

    const char* Codes(const Huffman& lencode, const Huffman& distcode) {
        static const std::int16_t lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                              35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const std::int16_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                              3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const std::int16_t dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                               6145, 8193, 12289, 16385, 24577};
        static const std::int16_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                              6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = Decode(lencode);
            if (symbol < 0) {
                return failed_ ? "the compressed data ended early" : "a bad code";
            }
            if (symbol < 256) {
                if (!Put(static_cast<std::uint8_t>(symbol))) {
                    return "could not write the output";
                }
            } else if (symbol == 256) {
                return nullptr;
            } else {
                symbol -= 257;
                if (symbol >= 29) {
                    return "a bad length code";
                }
                const int len = lens[symbol] + Bits(lext[symbol]);
                const int dsym = Decode(distcode);
                if (dsym < 0 || dsym >= 30) {
                    return "a bad distance code";
                }
                const std::uint64_t dist = static_cast<std::uint64_t>(dists[dsym] + Bits(dext[dsym]));
                if (dist > total_ || dist > WindowSize) {
                    return "a distance before the start";
                }
                for (int i = 0; i < len; ++i) {
                    if (!Put(window_[(total_ - dist) & (WindowSize - 1)])) {
                        return "could not write the output";
                    }
                }
            }
            if (failed_) {
                return "the compressed data ended early";
            }
        }
    }

    const char* Fixed() {
        static Huffman lencode, distcode;
        static bool ready = false;
        if (!ready) {
            std::int16_t lengths[288];
            int s = 0;
            for (; s < 144; ++s) lengths[s] = 8;
            for (; s < 256; ++s) lengths[s] = 9;
            for (; s < 280; ++s) lengths[s] = 7;
            for (; s < 288; ++s) lengths[s] = 8;
            Construct(lencode, lengths, 288);
            for (s = 0; s < 30; ++s) lengths[s] = 5;
            Construct(distcode, lengths, 30);
            ready = true;
        }
        return Codes(lencode, distcode);
    }

    const char* Dynamic() {
        static const std::int16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = Bits(5) + 257;
        const int ndist = Bits(5) + 1;
        const int ncode = Bits(4) + 4;
        if (failed_ || nlen > 286 || ndist > 30) {
            return "a bad dynamic block";
        }
        std::int16_t lengths[320] = {};
        int index = 0;
        for (; index < ncode; ++index) {
            lengths[order[index]] = static_cast<std::int16_t>(Bits(3));
        }
        for (; index < 19; ++index) {
            lengths[order[index]] = 0;
        }
        Huffman lencode, distcode;
        if (Construct(lencode, lengths, 19) != 0) {
            return "a bad code-length code";
        }
        index = 0;
        while (index < nlen + ndist) {
            int symbol = Decode(lencode);
            if (symbol < 0) {
                return "a bad code length";
            }
            if (symbol < 16) {
                lengths[index++] = static_cast<std::int16_t>(symbol);
            } else {
                int len = 0, repeat = 0;
                if (symbol == 16) {
                    if (index == 0) {
                        return "a repeat with nothing before it";
                    }
                    len = lengths[index - 1];
                    repeat = 3 + Bits(2);
                } else if (symbol == 17) {
                    repeat = 3 + Bits(3);
                } else {
                    repeat = 11 + Bits(7);
                }
                if (index + repeat > nlen + ndist) {
                    return "too many code lengths";
                }
                while (repeat--) {
                    lengths[index++] = static_cast<std::int16_t>(len);
                }
            }
            if (failed_) {
                return "the compressed data ended early";
            }
        }
        if (lengths[256] == 0) {
            return "no end-of-block code";
        }
        const int err1 = Construct(lencode, lengths, nlen);
        if (err1 < 0 || (err1 > 0 && nlen - lencode.count[0] != 1)) {
            return "a bad literal/length code";
        }
        const int err2 = Construct(distcode, lengths + nlen, ndist);
        if (err2 < 0 || (err2 > 0 && ndist - distcode.count[0] != 1)) {
            return "a bad distance code table";
        }
        return Codes(lencode, distcode);
    }

    std::FILE* in_;
    std::uint64_t left_;
    std::FILE* out_;
    std::uint8_t buffer_[1 << 16];
    std::size_t pos_ = 0, len_ = 0;
    std::uint32_t bitbuf_ = 0;
    int bitcnt_ = 0;
    bool failed_ = false;
    std::uint8_t window_[WindowSize];
    std::uint64_t total_ = 0;
    std::uint8_t out_buf_[1 << 16];
    std::size_t out_len_ = 0;
    std::uint32_t crc_ = 0;
};

struct Entry {
    std::string name;
    std::uint16_t flags = 0, method = 0;
    std::uint32_t crc = 0, compressed = 0, size = 0, header_offset = 0;
};

bool IsNca(const std::string& name) {
    return name.size() > 4 && std::equal(name.end() - 4, name.end(), ".nca", [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
           });
}

std::string BaseName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // Anonymous namespace

Result ExtractNcas(const fs::path& zip, const fs::path& target) {
    Result result;
    const auto fail = [&](const std::string& what) {
        ++result.failed;
        if (result.first_error.empty()) {
            result.first_error = what;
        }
    };
    std::FILE* f = std::fopen(zip.string().c_str(), "rb");
    if (f == nullptr) {
        fail("cannot open " + zip.string());
        return result;
    }
    struct Closer {
        std::FILE* f;
        ~Closer() { std::fclose(f); }
    } closer{f};

    // The end-of-central-directory record is in the last 64 KiB + 22 bytes.
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    const long tail = std::min<long>(size, 65557);
    std::vector<std::uint8_t> end(static_cast<size_t>(tail));
    std::fseek(f, size - tail, SEEK_SET);
    if (tail < 22 || std::fread(end.data(), 1, end.size(), f) != end.size()) {
        fail("not a zip file: " + zip.filename().string());
        return result;
    }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; --i) {
        if (U32(&end[static_cast<size_t>(i)]) == 0x06054b50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        fail("not a zip file (no directory): " + zip.filename().string());
        return result;
    }
    const std::uint16_t count = U16(&end[static_cast<size_t>(eocd) + 10]);
    const std::uint32_t cd_size = U32(&end[static_cast<size_t>(eocd) + 12]);
    const std::uint32_t cd_offset = U32(&end[static_cast<size_t>(eocd) + 16]);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
        fail("zip64 archives are not supported: " + zip.filename().string());
        return result;
    }
    std::vector<std::uint8_t> cd(cd_size);
    std::fseek(f, static_cast<long>(cd_offset), SEEK_SET);
    if (std::fread(cd.data(), 1, cd.size(), f) != cd.size()) {
        fail("the zip's directory is cut short");
        return result;
    }

    std::vector<Entry> entries;
    for (size_t p = 0, n = 0; n < count && p + 46 <= cd.size(); ++n) {
        if (U32(&cd[p]) != 0x02014b50u) {
            break;
        }
        Entry e;
        e.flags = U16(&cd[p + 8]);
        e.method = U16(&cd[p + 10]);
        e.crc = U32(&cd[p + 16]);
        e.compressed = U32(&cd[p + 20]);
        e.size = U32(&cd[p + 24]);
        const size_t name_len = U16(&cd[p + 28]), extra_len = U16(&cd[p + 30]), comment_len = U16(&cd[p + 32]);
        e.header_offset = U32(&cd[p + 42]);
        if (p + 46 + name_len > cd.size()) {
            break;
        }
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), name_len);
        if (IsNca(e.name)) {
            entries.push_back(std::move(e));
        }
        p += 46 + name_len + extra_len + comment_len;
    }

    std::error_code ec;
    fs::create_directories(target, ec);
    for (const Entry& e : entries) {
        const std::string name = BaseName(e.name);
        if (name.empty() || name[0] == '.') {
            continue;
        }
        const fs::path to = target / name;
        std::error_code e2;
        if (fs::exists(to, e2) && fs::file_size(to, e2) == e.size) {
            ++result.skipped;
            continue;
        }
        if ((e.flags & 1) != 0) {
            fail(name + ": encrypted entries are not supported");
            continue;
        }
        if (e.method != 0 && e.method != 8) {
            fail(name + ": compression method " + std::to_string(e.method) + " is not supported");
            continue;
        }
        std::uint8_t local[30];
        std::fseek(f, static_cast<long>(e.header_offset), SEEK_SET);
        if (std::fread(local, 1, sizeof(local), f) != sizeof(local) || U32(local) != 0x04034b50u) {
            fail(name + ": a bad local header");
            continue;
        }
        const long data = static_cast<long>(e.header_offset) + 30 + U16(local + 26) + U16(local + 28);
        std::fseek(f, data, SEEK_SET);

        fs::path part = to;
        part += ".part";
        std::FILE* out = std::fopen(part.string().c_str(), "wb");
        if (out == nullptr) {
            fail(name + ": cannot write in " + target.string());
            continue;
        }
        std::uint32_t crc = 0;
        std::uint64_t written = 0;
        const char* error = nullptr;
        if (e.method == 0) {
            std::vector<std::uint8_t> block(1 << 16);
            for (std::uint64_t left = e.compressed; left > 0;) {
                const size_t n = static_cast<size_t>(std::min<std::uint64_t>(left, block.size()));
                if (std::fread(block.data(), 1, n, f) != n) {
                    error = "the data is cut short";
                    break;
                }
                if (std::fwrite(block.data(), 1, n, out) != n) {
                    error = "could not write the output";
                    break;
                }
                crc = Crc32(crc, block.data(), n);
                written += n;
                left -= n;
            }
        } else {
            auto* inflater = new Inflater(f, e.compressed, out);
            error = inflater->Run();
            crc = inflater->crc();
            written = inflater->written();
            delete inflater;
        }
        const bool wrote_ok = std::fclose(out) == 0;
        if (error == nullptr && (!wrote_ok || written != e.size || crc != e.crc)) {
            error = !wrote_ok ? "could not write the output" : "the file does not match its checksum";
        }
        if (error != nullptr) {
            std::remove(part.string().c_str());
            fail(name + ": " + error);
            continue;
        }
        std::remove(to.string().c_str());
        if (std::rename(part.string().c_str(), to.string().c_str()) != 0) {
            std::remove(part.string().c_str());
            fail(name + ": cannot rename into place");
            continue;
        }
        ++result.extracted;
    }
    if (entries.empty() && result.failed == 0) {
        fail("no .nca files in " + zip.filename().string());
    }
    return result;
}

} // namespace Ps4Zip
