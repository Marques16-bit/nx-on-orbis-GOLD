// SPDX-License-Identifier: GPL-3.0-or-later
// eden-ps4: small image decoders for the game covers of the menu (0.7.9): PNG, baseline JPEG and BMP,
// into 0xAARRGGBB pixels. Header only: the checks in .github/workflows build the menu test with a
// fixed list of source files, and a workflow file cannot be changed by an update zip.
//
// What is NOT supported (the cover is then replaced by a generated tile, and the reason goes to
// boot.log): progressive JPEG, arithmetic-coded JPEG, interlaced PNG, 16-bit-per-channel PNG is read
// at its high byte, CMYK JPEG, compressed BMP. No allocation depends on a size larger than the
// limits below, so a damaged or hostile file cannot make the menu allocate more than ~64 MiB.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Menu::Image {

struct Pixels {
    int w = 0, h = 0;
    std::vector<std::uint32_t> px; // 0xAARRGGBB, row by row, top row first
    bool Ok() const { return w > 0 && h > 0 && px.size() == std::size_t(w) * std::size_t(h); }
};

inline constexpr int MaxSide = 4096;
inline constexpr std::size_t MaxPixels = 16u * 1024u * 1024u; // 64 MiB of pixels

namespace detail {

inline std::uint32_t Rd32be(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
inline std::uint32_t Rd32le(const std::uint8_t* p) {
    return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | p[0];
}
inline bool SizeOk(std::uint32_t w, std::uint32_t h) {
    return w > 0 && h > 0 && w <= std::uint32_t(MaxSide) && h <= std::uint32_t(MaxSide) &&
           std::size_t(w) * std::size_t(h) <= MaxPixels;
}
inline std::uint32_t Argb(unsigned a, unsigned r, unsigned g, unsigned b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// ---- inflate (RFC 1951): the canonical-Huffman decoding of zlib's "puff", reading from memory ------
struct Inflater {
    const std::uint8_t* in;
    std::size_t in_size, in_pos = 0;
    std::uint32_t bitbuf = 0;
    int bitcnt = 0;
    std::vector<std::uint8_t>* out;
    std::size_t limit;
    bool error = false;

    struct Huff {
        std::uint16_t count[16];
        std::uint16_t symbol[320];
    };

    int Bits(int need) {
        std::uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (in_pos >= in_size) {
                error = true;
                return 0;
            }
            val |= std::uint32_t(in[in_pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return int(val & ((1u << need) - 1u));
    }
    static int Construct(Huff* h, const std::uint16_t* length, int n) {
        for (int len = 0; len < 16; ++len) {
            h->count[len] = 0;
        }
        for (int s = 0; s < n; ++s) {
            ++h->count[length[s]];
        }
        if (h->count[0] == n) {
            return 0;
        }
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h->count[len];
            if (left < 0) {
                return left;
            }
        }
        std::uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) {
            offs[len + 1] = std::uint16_t(offs[len] + h->count[len]);
        }
        for (int s = 0; s < n; ++s) {
            if (length[s] != 0) {
                h->symbol[offs[length[s]]++] = std::uint16_t(s);
            }
        }
        return left;
    }
    int Decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= Bits(1);
            if (error) {
                return -1;
            }
            const int count = h.count[len];
            if (code - count < first) {
                return h.symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
    bool Codes(const Huff& lencode, const Huff& distcode) {
        static const std::uint16_t lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const std::uint16_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const std::uint16_t dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const std::uint16_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = Decode(lencode);
            if (symbol < 0) {
                return false;
            }
            if (symbol < 256) {
                if (out->size() >= limit) {
                    return false;
                }
                out->push_back(std::uint8_t(symbol));
            } else if (symbol == 256) {
                return true;
            } else {
                symbol -= 257;
                if (symbol >= 29) {
                    return false;
                }
                const int len = lens[symbol] + Bits(lext[symbol]);
                const int ds = Decode(distcode);
                if (ds < 0 || ds >= 30) {
                    return false;
                }
                const std::size_t dist = dists[ds] + std::size_t(Bits(dext[ds]));
                if (error || dist > out->size() || out->size() + std::size_t(len) > limit) {
                    return false;
                }
                for (int i = 0; i < len; ++i) {
                    out->push_back((*out)[out->size() - dist]);
                }
            }
        }
    }
    bool Run() {
        for (;;) {
            const int last = Bits(1);
            const int type = Bits(2);
            if (error) {
                return false;
            }
            if (type == 0) {
                bitbuf = 0;
                bitcnt = 0;
                if (in_pos + 4 > in_size) {
                    return false;
                }
                const unsigned len = in[in_pos] | (unsigned(in[in_pos + 1]) << 8);
                const unsigned nlen = in[in_pos + 2] | (unsigned(in[in_pos + 3]) << 8);
                in_pos += 4;
                if ((len ^ 0xFFFFu) != nlen || in_pos + len > in_size || out->size() + len > limit) {
                    return false;
                }
                out->insert(out->end(), in + in_pos, in + in_pos + len);
                in_pos += len;
            } else if (type == 1 || type == 2) {
                Huff lencode, distcode;
                std::uint16_t lengths[320];
                if (type == 1) {
                    int s = 0;
                    for (; s < 144; ++s) lengths[s] = 8;
                    for (; s < 256; ++s) lengths[s] = 9;
                    for (; s < 280; ++s) lengths[s] = 7;
                    for (; s < 288; ++s) lengths[s] = 8;
                    Construct(&lencode, lengths, 288);
                    for (s = 0; s < 30; ++s) lengths[s] = 5;
                    Construct(&distcode, lengths, 30);
                } else {
                    static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                    const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
                    if (error || nlen > 286 || ndist > 30) {
                        return false;
                    }
                    int index = 0;
                    for (; index < ncode; ++index) lengths[order[index]] = std::uint16_t(Bits(3));
                    for (; index < 19; ++index) lengths[order[index]] = 0;
                    Huff codes;
                    if (Construct(&codes, lengths, 19) != 0) {
                        return false;
                    }
                    index = 0;
                    while (index < nlen + ndist) {
                        int symbol = Decode(codes);
                        if (symbol < 0) {
                            return false;
                        }
                        if (symbol < 16) {
                            lengths[index++] = std::uint16_t(symbol);
                        } else {
                            int len = 0, rep;
                            if (symbol == 16) {
                                if (index == 0) {
                                    return false;
                                }
                                len = lengths[index - 1];
                                rep = 3 + Bits(2);
                            } else if (symbol == 17) {
                                rep = 3 + Bits(3);
                            } else {
                                rep = 11 + Bits(7);
                            }
                            if (error || index + rep > nlen + ndist) {
                                return false;
                            }
                            while (rep--) lengths[index++] = std::uint16_t(len);
                        }
                    }
                    if (lengths[256] == 0) {
                        return false;
                    }
                    if (Construct(&lencode, lengths, nlen) < 0 || Construct(&distcode, lengths + nlen, ndist) < 0) {
                        return false;
                    }
                }
                if (!Codes(lencode, distcode)) {
                    return false;
                }
            } else {
                return false;
            }
            if (last) {
                return !error;
            }
        }
    }
};

inline bool Inflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>* out, std::size_t limit) {
    Inflater z{data, size, 0, 0, 0, out, limit, false};
    return z.Run();
}

} // namespace detail

// ---- PNG ------------------------------------------------------------------------------------------
inline bool DecodePng(const std::uint8_t* d, std::size_t n, Pixels* img, std::string* why) {
    auto fail = [&](const char* t) { if (why) *why = t; return false; };
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n < 33 || std::memcmp(d, sig, 8) != 0) {
        return fail("not a PNG");
    }
    std::uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    std::vector<std::uint8_t> idat;
    std::uint32_t palette[256] = {};
    int palette_n = 0;
    bool have_header = false, have_end = false;
    std::size_t pos = 8;
    while (pos + 12 <= n && !have_end) {
        const std::uint32_t len = detail::Rd32be(d + pos);
        if (len > n || pos + 12 + std::size_t(len) > n) {
            return fail("PNG chunk runs past the end of the file");
        }
        const std::uint8_t* type = d + pos + 4;
        const std::uint8_t* body = d + pos + 8;
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = detail::Rd32be(body);
            h = detail::Rd32be(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
            have_header = true;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette_n = int(std::min<std::uint32_t>(len / 3, 256));
            for (int i = 0; i < palette_n; ++i) {
                palette[i] = detail::Argb(255, body[i * 3], body[i * 3 + 1], body[i * 3 + 2]);
            }
        } else if (std::memcmp(type, "tRNS", 4) == 0 && ctype == 3) {
            for (std::uint32_t i = 0; i < len && i < 256; ++i) {
                palette[i] = (palette[i] & 0x00FFFFFFu) | (std::uint32_t(body[i]) << 24);
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            if (idat.size() + len > 64u * 1024u * 1024u) {
                return fail("PNG data is too large");
            }
            idat.insert(idat.end(), body, body + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            have_end = true;
        }
        pos += 12 + std::size_t(len);
    }
    if (!have_header || idat.size() < 2) {
        return fail("PNG without header or data");
    }
    if (!detail::SizeOk(w, h)) {
        return fail("PNG size is out of range");
    }
    if (interlace != 0) {
        return fail("interlaced PNG is not supported");
    }
    int channels = 0;
    switch (ctype) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return fail("PNG colour type is not supported");
    }
    if (!(depth == 8 || depth == 16 || (depth <= 4 && (ctype == 0 || ctype == 3)))) {
        return fail("PNG bit depth is not supported");
    }
    const std::size_t bits_per_pixel = std::size_t(depth) * std::size_t(channels);
    const std::size_t stride = (std::size_t(w) * bits_per_pixel + 7) / 8;
    const std::size_t bpp = std::max<std::size_t>(1, bits_per_pixel / 8);
    const std::size_t raw_size = (stride + 1) * std::size_t(h);
    std::vector<std::uint8_t> raw;
    raw.reserve(raw_size);
    if (!detail::Inflate(idat.data() + 2, idat.size() - 2, &raw, raw_size) || raw.size() != raw_size) {
        return fail("PNG data does not inflate");
    }
    // Unfilter in place, line by line.
    std::vector<std::uint8_t> zero(stride, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint8_t* line = raw.data() + std::size_t(y) * (stride + 1);
        const int filter = line[0];
        std::uint8_t* cur = line + 1;
        const std::uint8_t* up = y == 0 ? zero.data() : raw.data() + std::size_t(y - 1) * (stride + 1) + 1;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0;
            const int b = up[i];
            const int c = i >= bpp ? up[i - bpp] : 0;
            int add = 0;
            switch (filter) {
            case 0: add = 0; break;
            case 1: add = a; break;
            case 2: add = b; break;
            case 3: add = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                add = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: return fail("PNG filter type is not valid");
            }
            cur[i] = std::uint8_t(cur[i] + add);
        }
    }
    img->w = int(w);
    img->h = int(h);
    img->px.assign(std::size_t(w) * h, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* line = raw.data() + std::size_t(y) * (stride + 1) + 1;
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint32_t out = 0;
            if (depth == 8 || depth == 16) {
                const int step = depth / 8;
                const std::uint8_t* p = line + std::size_t(x) * std::size_t(channels) * std::size_t(step);
                auto ch = [&](int i) { return unsigned(p[i * step]); };
                switch (ctype) {
                case 0: out = detail::Argb(255, ch(0), ch(0), ch(0)); break;
                case 2: out = detail::Argb(255, ch(0), ch(1), ch(2)); break;
                case 3: out = palette[std::min<unsigned>(ch(0), 255u)]; break;
                case 4: out = detail::Argb(ch(1), ch(0), ch(0), ch(0)); break;
                default: out = detail::Argb(ch(3), ch(0), ch(1), ch(2)); break;
                }
            } else { // 1, 2 or 4 bits: gray or palette
                const std::size_t bit = std::size_t(x) * std::size_t(depth);
                const unsigned v = (line[bit / 8] >> (8 - depth - int(bit % 8))) & ((1u << depth) - 1u);
                if (ctype == 3) {
                    out = palette[v];
                } else {
                    const unsigned g = v * 255u / ((1u << depth) - 1u);
                    out = detail::Argb(255, g, g, g);
                }
            }
            img->px[std::size_t(y) * w + x] = out;
        }
    }
    return true;
}

// ---- BMP (uncompressed 24 / 32 bit, and 8 bit with a palette) -----------------------------------------
inline bool DecodeBmp(const std::uint8_t* d, std::size_t n, Pixels* img, std::string* why) {
    auto fail = [&](const char* t) { if (why) *why = t; return false; };
    if (n < 54 || d[0] != 'B' || d[1] != 'M') {
        return fail("not a BMP");
    }
    const std::uint32_t data_at = detail::Rd32le(d + 10), header = detail::Rd32le(d + 14);
    if (header < 40) {
        return fail("BMP header is not supported");
    }
    const std::int32_t w = std::int32_t(detail::Rd32le(d + 18));
    std::int32_t h = std::int32_t(detail::Rd32le(d + 22));
    const int bits = d[28] | (d[29] << 8);
    const std::uint32_t compression = detail::Rd32le(d + 30);
    const bool top_down = h < 0;
    if (top_down) {
        h = -h;
    }
    if (w <= 0 || h <= 0 || !detail::SizeOk(std::uint32_t(w), std::uint32_t(h))) {
        return fail("BMP size is out of range");
    }
    if (compression != 0 && !(compression == 3 && bits == 32)) {
        return fail("compressed BMP is not supported");
    }
    if (bits != 24 && bits != 32 && bits != 8) {
        return fail("BMP bit depth is not supported");
    }
    const std::size_t row = ((std::size_t(w) * std::size_t(bits) + 31) / 32) * 4;
    if (std::size_t(data_at) + row * std::size_t(h) > n) {
        return fail("BMP data runs past the end of the file");
    }
    std::uint32_t palette[256] = {};
    if (bits == 8) {
        const std::size_t at = 14 + header;
        const std::uint32_t colours = std::min<std::uint32_t>(detail::Rd32le(d + 46) ? detail::Rd32le(d + 46) : 256, 256);
        if (at + std::size_t(colours) * 4 > n) {
            return fail("BMP palette runs past the end of the file");
        }
        for (std::uint32_t i = 0; i < colours; ++i) {
            palette[i] = detail::Argb(255, d[at + i * 4 + 2], d[at + i * 4 + 1], d[at + i * 4]);
        }
    }
    img->w = w;
    img->h = h;
    img->px.assign(std::size_t(w) * std::size_t(h), 0);
    for (std::int32_t y = 0; y < h; ++y) {
        const std::int32_t src_y = top_down ? y : h - 1 - y;
        const std::uint8_t* line = d + data_at + row * std::size_t(src_y);
        for (std::int32_t x = 0; x < w; ++x) {
            std::uint32_t out;
            if (bits == 24) {
                out = detail::Argb(255, line[x * 3 + 2], line[x * 3 + 1], line[x * 3]);
            } else if (bits == 32) {
                out = detail::Argb(255, line[x * 4 + 2], line[x * 4 + 1], line[x * 4]); // the 4th byte is often 0 in BMP
            } else {
                out = palette[line[x]];
            }
            img->px[std::size_t(y) * std::size_t(w) + std::size_t(x)] = out;
        }
    }
    return true;
}

// ---- JPEG (baseline and extended sequential, Huffman, 8 bit; 1 or 3 components) ----------------------
namespace detail {

struct JpegHuff {
    std::uint8_t bits[17] = {};
    std::uint8_t values[256] = {};
    int mincode[17] = {}, maxcode[18] = {}, valptr[17] = {};
    bool present = false;
    void Build() {
        int code = 0, k = 0;
        for (int len = 1; len <= 16; ++len) {
            valptr[len] = k;
            mincode[len] = code;
            code += bits[len];
            k += bits[len];
            maxcode[len] = bits[len] ? code - 1 : -1;
            code <<= 1;
        }
        maxcode[17] = 0x7FFFFFFF;
        present = true;
    }
};

struct JpegBits {
    const std::uint8_t* d;
    std::size_t n, pos;
    std::uint32_t buf = 0;
    int cnt = 0;
    bool hit_marker = false;
    int Bit() {
        if (cnt == 0) {
            int byte = 0;
            if (pos < n && !hit_marker) {
                byte = d[pos];
                if (byte == 0xFF) {
                    const int next = pos + 1 < n ? d[pos + 1] : 0xD9;
                    if (next == 0) {
                        pos += 2;
                    } else {
                        hit_marker = true; // a marker: feed zeros until the caller deals with it
                        byte = 0;
                    }
                } else {
                    ++pos;
                }
            }
            buf = std::uint32_t(byte);
            cnt = 8;
        }
        --cnt;
        return int((buf >> cnt) & 1u);
    }
    int Receive(int s) {
        int v = 0;
        for (int i = 0; i < s; ++i) {
            v = (v << 1) | Bit();
        }
        return v;
    }
    static int Extend(int v, int s) { return s == 0 ? 0 : (v < (1 << (s - 1)) ? v - (1 << s) + 1 : v); }
    int Decode(const JpegHuff& h) {
        int code = 0;
        for (int len = 1; len <= 16; ++len) {
            code = (code << 1) | Bit();
            if (h.maxcode[len] >= 0 && code <= h.maxcode[len] && code >= h.mincode[len]) {
                return h.values[h.valptr[len] + code - h.mincode[len]];
            }
        }
        return -1;
    }
    void Align() { cnt = 0; }
};

inline const std::uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
                                         41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15,
                                         23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

inline void Idct8x8(const float* in, std::uint8_t* out, int stride) {
    static float table[8][8];
    static bool ready = false;
    if (!ready) {
        for (int x = 0; x < 8; ++x) {
            for (int u = 0; u < 8; ++u) {
                table[x][u] = float((u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
            }
        }
        ready = true;
    }
    float tmp[64];
    for (int y = 0; y < 8; ++y) { // rows: over u
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) {
                s += table[x][u] * in[y * 8 + u];
            }
            tmp[y * 8 + x] = s;
        }
    }
    for (int x = 0; x < 8; ++x) { // columns: over v
        for (int y = 0; y < 8; ++y) {
            float s = 0;
            for (int v = 0; v < 8; ++v) {
                s += table[y][v] * tmp[v * 8 + x];
            }
            const int value = int(std::lround(s)) + 128;
            out[y * stride + x] = std::uint8_t(std::clamp(value, 0, 255));
        }
    }
}

} // namespace detail

inline bool DecodeJpeg(const std::uint8_t* d, std::size_t n, Pixels* img, std::string* why) {
    auto fail = [&](const char* t) { if (why) *why = t; return false; };
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return fail("not a JPEG");
    }
    struct Comp {
        int id = 0, h = 1, v = 1, tq = 0, td = 0, ta = 0, pred = 0;
        std::vector<std::uint8_t> plane;
        int pw = 0, ph = 0;
    };
    std::uint16_t quant[4][64] = {};
    detail::JpegHuff dc[4], ac[4];
    Comp comp[3];
    int ncomp = 0, width = 0, height = 0, restart = 0;
    bool frame = false;
    std::size_t pos = 2;
    while (pos + 4 <= n) {
        if (d[pos] != 0xFF) {
            ++pos;
            continue;
        }
        const int marker = d[pos + 1];
        if (marker == 0xFF) {
            ++pos;
            continue;
        }
        pos += 2;
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01 || marker == 0) {
            continue;
        }
        if (marker == 0xD9) {
            break;
        }
        if (pos + 2 > n) {
            break;
        }
        const std::size_t len = (std::size_t(d[pos]) << 8) | d[pos + 1];
        if (len < 2 || pos + len > n) {
            return fail("JPEG segment runs past the end of the file");
        }
        const std::uint8_t* seg = d + pos + 2;
        const std::size_t seg_len = len - 2;
        if (marker == 0xDB) { // quantisation tables
            std::size_t i = 0;
            while (i < seg_len) {
                const int pq = seg[i] >> 4, tq = seg[i] & 15;
                ++i;
                if (tq > 3 || i + (pq ? 128u : 64u) > seg_len) {
                    return fail("JPEG quantisation table is damaged");
                }
                for (int k = 0; k < 64; ++k) {
                    quant[tq][detail::kZigzag[k]] = pq ? std::uint16_t((seg[i + k * 2] << 8) | seg[i + k * 2 + 1]) : seg[i + k];
                }
                i += pq ? 128u : 64u;
            }
        } else if (marker == 0xC0 || marker == 0xC1) { // baseline / extended sequential
            if (seg_len < 6 || seg[0] != 8) {
                return fail("only 8-bit JPEG is supported");
            }
            height = (seg[1] << 8) | seg[2];
            width = (seg[3] << 8) | seg[4];
            ncomp = seg[5];
            if ((ncomp != 1 && ncomp != 3) || seg_len < 6 + std::size_t(ncomp) * 3 || !detail::SizeOk(std::uint32_t(width), std::uint32_t(height))) {
                return fail("JPEG size or components are not supported");
            }
            for (int i = 0; i < ncomp; ++i) {
                comp[i].id = seg[6 + i * 3];
                comp[i].h = seg[7 + i * 3] >> 4;
                comp[i].v = seg[7 + i * 3] & 15;
                comp[i].tq = seg[8 + i * 3] & 3;
                if (comp[i].h < 1 || comp[i].h > 4 || comp[i].v < 1 || comp[i].v > 4) {
                    return fail("JPEG sampling is not supported");
                }
            }
            frame = true;
        } else if (marker == 0xC2 || marker == 0xC3 || (marker >= 0xC5 && marker <= 0xCF && marker != 0xC8 && marker != 0xCC)) {
            return fail("progressive or arithmetic JPEG is not supported");
        } else if (marker == 0xC4) { // Huffman tables
            std::size_t i = 0;
            while (i + 17 <= seg_len) {
                const int tc = seg[i] >> 4, th = seg[i] & 15;
                detail::JpegHuff& h = tc == 0 ? dc[th & 3] : ac[th & 3];
                int total = 0;
                for (int l = 1; l <= 16; ++l) {
                    h.bits[l] = seg[i + l];
                    total += h.bits[l];
                }
                i += 17;
                if (total > 256 || i + std::size_t(total) > seg_len) {
                    return fail("JPEG Huffman table is damaged");
                }
                std::memcpy(h.values, seg + i, std::size_t(total));
                h.Build();
                i += std::size_t(total);
            }
        } else if (marker == 0xDD && seg_len >= 2) {
            restart = (seg[0] << 8) | seg[1];
        } else if (marker == 0xDA) { // start of scan: decode everything
            if (!frame) {
                return fail("JPEG scan before its frame");
            }
            const int sn = seg[0];
            if (sn != ncomp || seg_len < 1 + std::size_t(sn) * 2 + 3) {
                return fail("JPEG scan with a different component count is not supported");
            }
            for (int i = 0; i < sn; ++i) {
                for (int c = 0; c < ncomp; ++c) {
                    if (comp[c].id == seg[1 + i * 2]) {
                        comp[c].td = (seg[2 + i * 2] >> 4) & 3;
                        comp[c].ta = seg[2 + i * 2] & 3;
                    }
                }
            }
            int hmax = 1, vmax = 1;
            for (int c = 0; c < ncomp; ++c) {
                hmax = std::max(hmax, comp[c].h);
                vmax = std::max(vmax, comp[c].v);
            }
            const int mcu_w = 8 * hmax, mcu_h = 8 * vmax;
            const int mcus_x = (width + mcu_w - 1) / mcu_w, mcus_y = (height + mcu_h - 1) / mcu_h;
            for (int c = 0; c < ncomp; ++c) {
                comp[c].pw = mcus_x * comp[c].h * 8;
                comp[c].ph = mcus_y * comp[c].v * 8;
                comp[c].plane.assign(std::size_t(comp[c].pw) * std::size_t(comp[c].ph), 128);
                if (!dc[comp[c].td].present || !ac[comp[c].ta].present) {
                    return fail("JPEG without its Huffman tables");
                }
            }
            detail::JpegBits bits{d, n, pos + len};
            int until_restart = restart;
            float block[64];
            for (int my = 0; my < mcus_y; ++my) {
                for (int mx = 0; mx < mcus_x; ++mx) {
                    if (restart && until_restart == 0) {
                        bits.Align();
                        // skip to after the RSTn marker
                        while (bits.pos + 1 < n && !(d[bits.pos] == 0xFF && d[bits.pos + 1] >= 0xD0 && d[bits.pos + 1] <= 0xD7)) {
                            ++bits.pos;
                        }
                        bits.pos += 2;
                        bits.hit_marker = false;
                        for (int c = 0; c < ncomp; ++c) comp[c].pred = 0;
                        until_restart = restart;
                    }
                    for (int c = 0; c < ncomp; ++c) {
                        Comp& cp = comp[c];
                        for (int by = 0; by < cp.v; ++by) {
                            for (int bx = 0; bx < cp.h; ++bx) {
                                std::memset(block, 0, sizeof(block));
                                const int t = bits.Decode(dc[cp.td]);
                                if (t < 0 || t > 16) {
                                    return fail("JPEG data is damaged (DC)");
                                }
                                const int diff = t ? detail::JpegBits::Extend(bits.Receive(t), t) : 0;
                                cp.pred += diff;
                                block[0] = float(cp.pred * quant[cp.tq][0]);
                                for (int k = 1; k < 64;) {
                                    const int rs = bits.Decode(ac[cp.ta]);
                                    if (rs < 0) {
                                        return fail("JPEG data is damaged (AC)");
                                    }
                                    const int r = rs >> 4, s = rs & 15;
                                    if (s == 0) {
                                        if (r == 15) {
                                            k += 16;
                                            continue;
                                        }
                                        break;
                                    }
                                    k += r;
                                    if (k > 63) {
                                        return fail("JPEG data is damaged (run)");
                                    }
                                    const int z = detail::kZigzag[k];
                                    block[z] = float(detail::JpegBits::Extend(bits.Receive(s), s) * quant[cp.tq][z]);
                                    ++k;
                                }
                                const int px = (mx * cp.h + bx) * 8, py = (my * cp.v + by) * 8;
                                detail::Idct8x8(block, cp.plane.data() + std::size_t(py) * std::size_t(cp.pw) + std::size_t(px), cp.pw);
                            }
                        }
                    }
                    if (restart) {
                        --until_restart;
                    }
                }
            }
            img->w = width;
            img->h = height;
            img->px.assign(std::size_t(width) * std::size_t(height), 0);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    // Chroma that was stored at a lower resolution is interpolated (centred bilinear, which is
                    // close to libjpeg's "fancy upsampling"); full resolution components are read as they are.
                    auto sample = [&](const Comp& cp) {
                        if (cp.h == hmax && cp.v == vmax) {
                            return int(cp.plane[std::size_t(y) * std::size_t(cp.pw) + std::size_t(x)]);
                        }
                        const int cw = (width * cp.h + hmax - 1) / hmax, ch = (height * cp.v + vmax - 1) / vmax;
                        const float fx = std::clamp((float(x) + 0.5f) * float(cp.h) / float(hmax) - 0.5f, 0.0f, float(cw - 1));
                        const float fy = std::clamp((float(y) + 0.5f) * float(cp.v) / float(vmax) - 0.5f, 0.0f, float(ch - 1));
                        const int ix = int(fx), iy = int(fy), jx = std::min(ix + 1, cw - 1), jy = std::min(iy + 1, ch - 1);
                        const float tx = fx - float(ix), ty = fy - float(iy);
                        auto at = [&](int px, int py) { return float(cp.plane[std::size_t(py) * std::size_t(cp.pw) + std::size_t(px)]); };
                        const float top = at(ix, iy) * (1 - tx) + at(jx, iy) * tx, bottom = at(ix, jy) * (1 - tx) + at(jx, jy) * tx;
                        return int(std::lround(top * (1 - ty) + bottom * ty));
                    };
                    std::uint32_t out;
                    if (ncomp == 1) {
                        const unsigned g = unsigned(sample(comp[0]));
                        out = detail::Argb(255, g, g, g);
                    } else {
                        const float Y = float(sample(comp[0])), Cb = float(sample(comp[1]) - 128), Cr = float(sample(comp[2]) - 128);
                        const int r = int(std::lround(Y + 1.402f * Cr)), g = int(std::lround(Y - 0.344136f * Cb - 0.714136f * Cr)),
                                  b = int(std::lround(Y + 1.772f * Cb));
                        out = detail::Argb(255, unsigned(std::clamp(r, 0, 255)), unsigned(std::clamp(g, 0, 255)), unsigned(std::clamp(b, 0, 255)));
                    }
                    img->px[std::size_t(y) * std::size_t(width) + std::size_t(x)] = out;
                }
            }
            return true;
        }
        pos += len;
    }
    return fail("JPEG without image data");
}

/// Looks at the first bytes and calls the right decoder. False, with a reason in `why`, when the file is not
/// a picture this code can read.
inline bool Decode(const std::uint8_t* d, std::size_t n, Pixels* img, std::string* why) {
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P') {
        return DecodePng(d, n, img, why);
    }
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) {
        return DecodeJpeg(d, n, img, why);
    }
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') {
        return DecodeBmp(d, n, img, why);
    }
    if (why) {
        *why = "not a PNG, JPEG or BMP file";
    }
    return false;
}

/// Scales to fit inside max_w x max_h keeping the proportions (area average when it shrinks, bilinear when
/// it grows). The result is never larger than the source unless `allow_grow`.
inline Pixels Fit(const Pixels& src, int max_w, int max_h, bool allow_grow = false) {
    Pixels out;
    if (!src.Ok() || max_w <= 0 || max_h <= 0) {
        return out;
    }
    double scale = std::min(double(max_w) / src.w, double(max_h) / src.h);
    if (!allow_grow) {
        scale = std::min(scale, 1.0);
    }
    const int dw = std::max(1, int(std::lround(src.w * scale))), dh = std::max(1, int(std::lround(src.h * scale)));
    out.w = dw;
    out.h = dh;
    out.px.assign(std::size_t(dw) * std::size_t(dh), 0);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            double a = 0, r = 0, g = 0, b = 0, weight = 0;
            if (scale < 1.0) { // box filter over the source area this pixel covers
                const double x0 = x / scale, x1 = (x + 1) / scale, y0 = y / scale, y1 = (y + 1) / scale;
                for (int sy = int(y0); sy < std::min(int(std::ceil(y1)), src.h); ++sy) {
                    const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                    for (int sx = int(x0); sx < std::min(int(std::ceil(x1)), src.w); ++sx) {
                        const double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                        const std::uint32_t p = src.px[std::size_t(sy) * std::size_t(src.w) + std::size_t(sx)];
                        const double wgt = wx * wy, pa = double(p >> 24) / 255.0;
                        a += wgt * pa;
                        r += wgt * pa * double((p >> 16) & 255);
                        g += wgt * pa * double((p >> 8) & 255);
                        b += wgt * pa * double(p & 255);
                        weight += wgt;
                    }
                }
            } else { // bilinear
                const double fx = std::clamp((x + 0.5) / scale - 0.5, 0.0, double(src.w - 1));
                const double fy = std::clamp((y + 0.5) / scale - 0.5, 0.0, double(src.h - 1));
                const int ix = int(fx), iy = int(fy), jx = std::min(ix + 1, src.w - 1), jy = std::min(iy + 1, src.h - 1);
                const double tx = fx - ix, ty = fy - iy;
                const std::uint32_t taps[4] = {src.px[std::size_t(iy) * std::size_t(src.w) + std::size_t(ix)],
                                               src.px[std::size_t(iy) * std::size_t(src.w) + std::size_t(jx)],
                                               src.px[std::size_t(jy) * std::size_t(src.w) + std::size_t(ix)],
                                               src.px[std::size_t(jy) * std::size_t(src.w) + std::size_t(jx)]};
                const double wgts[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                for (int i = 0; i < 4; ++i) {
                    const double pa = double(taps[i] >> 24) / 255.0;
                    a += wgts[i] * pa;
                    r += wgts[i] * pa * double((taps[i] >> 16) & 255);
                    g += wgts[i] * pa * double((taps[i] >> 8) & 255);
                    b += wgts[i] * pa * double(taps[i] & 255);
                    weight += wgts[i];
                }
            }
            std::uint32_t px = 0;
            if (weight > 0 && a > 0) {
                const unsigned ao = unsigned(std::lround(255.0 * a / weight));
                px = detail::Argb(std::min(ao, 255u), unsigned(std::clamp(std::lround(r / a), 0l, 255l)),
                                  unsigned(std::clamp(std::lround(g / a), 0l, 255l)), unsigned(std::clamp(std::lround(b / a), 0l, 255l)));
            }
            out.px[std::size_t(y) * std::size_t(dw) + std::size_t(x)] = px;
        }
    }
    return out;
}

} // namespace Menu::Image
