// gi_bake_image.cpp — see gi_bake_image.h.

#include "gi_bake_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace gi_bake_image {

namespace {

const uint32_t* crc_table() {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    return table;
}

void put_u32be(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));  out.push_back(uint8_t(v));
}

void put_chunk(std::vector<uint8_t>& out, const char type[4],
               const uint8_t* data, size_t len) {
    put_u32be(out, uint32_t(len));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    const uint32_t crc = crc32(out.data() + start, out.size() - start);
    put_u32be(out, crc);
}

// zlib stream with stored (uncompressed) deflate blocks. Deterministic and
// dependency-free; a stored block carries at most 65535 bytes.
void zlib_store(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
    out.push_back(0x78); out.push_back(0x01);   // CMF/FLG: deflate, no preset, level 0
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, len - pos);
        const bool last = (pos + n >= len);
        out.push_back(last ? 1 : 0);
        out.push_back(uint8_t(n & 0xFF));  out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(~n & 0xFF)); out.push_back(uint8_t((~n >> 8) & 0xFF));
        out.insert(out.end(), data + pos, data + pos + n);
        pos += n;
    } while (pos < len);
    put_u32be(out, adler32(data, len));
}

bool encode_png(uint32_t w, uint32_t h, int bit_depth, const uint8_t* rows_bytes,
                size_t row_bytes, std::vector<uint8_t>& out) {
    if (w == 0 || h == 0) return false;
    out.clear();
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);
    uint8_t ihdr[13];
    ihdr[0] = uint8_t(w >> 24); ihdr[1] = uint8_t(w >> 16); ihdr[2] = uint8_t(w >> 8); ihdr[3] = uint8_t(w);
    ihdr[4] = uint8_t(h >> 24); ihdr[5] = uint8_t(h >> 16); ihdr[6] = uint8_t(h >> 8); ihdr[7] = uint8_t(h);
    ihdr[8] = uint8_t(bit_depth); ihdr[9] = 2 /* RGB */; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    put_chunk(out, "IHDR", ihdr, sizeof ihdr);
    // Filter byte 0 (None) in front of every row.
    std::vector<uint8_t> raw;
    raw.reserve((row_bytes + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rows_bytes + size_t(y) * row_bytes,
                   rows_bytes + size_t(y + 1) * row_bytes);
    }
    std::vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    zlib_store(raw.data(), raw.size(), z);
    put_chunk(out, "IDAT", z.data(), z.size());
    put_chunk(out, "IEND", nullptr, 0);
    return true;
}

void float_to_rgbe(float r, float g, float b, uint8_t out[4]) {
    auto clean = [](float v) { return (std::isfinite(v) && v > 0.0f) ? v : 0.0f; };
    r = clean(r); g = clean(g); b = clean(b);
    const float m = std::max(r, std::max(g, b));
    if (m < 1e-32f) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    int e = 0;
    const float f = std::frexp(m, &e);           // m = f * 2^e, f in [0.5, 1)
    const float scale = f * 256.0f / m;
    out[0] = uint8_t(std::min(255.0f, r * scale));
    out[1] = uint8_t(std::min(255.0f, g * scale));
    out[2] = uint8_t(std::min(255.0f, b * scale));
    out[3] = uint8_t(e + 128);
}

void rgbe_to_float(const uint8_t in[4], float out[3]) {
    if (in[3] == 0) { out[0] = out[1] = out[2] = 0.0f; return; }
    const float f = std::ldexp(1.0f, int(in[3]) - (128 + 8));
    out[0] = (in[0] + 0.5f) * f;
    out[1] = (in[1] + 0.5f) * f;
    out[2] = (in[2] + 0.5f) * f;
}

// One RGBE channel of a scanline, new-style RLE: runs of >= 4 identical bytes
// become (128 + n, value); everything else becomes literal chunks (n, bytes),
// both with n <= 128.
void rle_channel(const uint8_t* v, uint32_t w, std::vector<uint8_t>& out) {
    uint32_t i = 0;
    while (i < w) {
        // Measure a run at i.
        uint32_t run = 1;
        while (i + run < w && v[i + run] == v[i] && run < 127) ++run;
        if (run >= 4) {
            out.push_back(uint8_t(128 + run));
            out.push_back(v[i]);
            i += run;
            continue;
        }
        // Literal chunk: extend until the next run of >= 4 or 128 bytes.
        uint32_t start = i;
        uint32_t n = 0;
        while (i < w && n < 128) {
            uint32_t r = 1;
            while (i + r < w && v[i + r] == v[i] && r < 4) ++r;
            if (r >= 4) break;
            ++i; ++n;
        }
        if (n == 0) { ++i; n = 1; }   // cannot happen, but never loop forever
        out.push_back(uint8_t(n));
        out.insert(out.end(), v + start, v + start + n);
    }
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) {
    const uint32_t* t = crc_table();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

float linear_to_srgb(float v) {
    v = std::min(1.0f, std::max(0.0f, v));
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float tonemap_ldr(float v) {
    if (!std::isfinite(v) || v <= 0.0f) return 0.0f;
    constexpr float kWhite = 4.0f;
    const float m = v * (1.0f + v / (kWhite * kWhite)) / (1.0f + v);
    return std::min(1.0f, std::max(0.0f, m));
}

bool encode_png_rgb8(uint32_t w, uint32_t h, const uint8_t* pixels, std::vector<uint8_t>& out) {
    return encode_png(w, h, 8, pixels, size_t(w) * 3, out);
}

bool encode_png_rgb16(uint32_t w, uint32_t h, const uint16_t* pixels, std::vector<uint8_t>& out) {
    std::vector<uint8_t> be(size_t(w) * h * 6);
    for (size_t i = 0; i < size_t(w) * h * 3; ++i) {
        be[i * 2]     = uint8_t(pixels[i] >> 8);
        be[i * 2 + 1] = uint8_t(pixels[i] & 0xFF);
    }
    return encode_png(w, h, 16, be.data(), size_t(w) * 6, out);
}

bool encode_hdr(uint32_t w, uint32_t h, const float* rgb, std::vector<uint8_t>& out) {
    if (w == 0 || h == 0) return false;
    out.clear();
    char header[96];
    const int n = std::snprintf(header, sizeof header,
                                "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %u +X %u\n", h, w);
    out.insert(out.end(), header, header + n);
    std::vector<uint8_t> rgbe(size_t(w) * 4);
    std::vector<uint8_t> chan(w);
    const bool rle = (w >= 8 && w < 32768);
    for (uint32_t y = 0; y < h; ++y) {
        const float* row = rgb + size_t(y) * w * 3;
        for (uint32_t x = 0; x < w; ++x)
            float_to_rgbe(row[x * 3], row[x * 3 + 1], row[x * 3 + 2], &rgbe[size_t(x) * 4]);
        if (!rle) {
            out.insert(out.end(), rgbe.begin(), rgbe.end());
            continue;
        }
        out.push_back(2); out.push_back(2);
        out.push_back(uint8_t(w >> 8)); out.push_back(uint8_t(w & 0xFF));
        for (int c = 0; c < 4; ++c) {
            for (uint32_t x = 0; x < w; ++x) chan[x] = rgbe[size_t(x) * 4 + c];
            rle_channel(chan.data(), w, out);
        }
    }
    return true;
}

bool decode_hdr(const uint8_t* data, size_t len, uint32_t& w, uint32_t& h,
                std::vector<float>& rgb) {
    // Header: lines until an empty line, then the resolution line.
    size_t pos = 0;
    auto read_line = [&](std::string& line) -> bool {
        line.clear();
        while (pos < len && data[pos] != '\n') line.push_back(char(data[pos++]));
        if (pos >= len) return false;
        ++pos;
        return true;
    };
    std::string line;
    if (!read_line(line) || line.rfind("#?", 0) != 0) return false;
    for (;;) {
        if (!read_line(line)) return false;
        if (line.empty()) break;
    }
    if (!read_line(line)) return false;
    // "-Y <h> +X <w>" — the only orientation this module writes or reads.
    if (line.rfind("-Y ", 0) != 0) return false;
    char* cursor = nullptr;
    const unsigned long uh = std::strtoul(line.c_str() + 3, &cursor, 10);
    if (!cursor || std::strncmp(cursor, " +X ", 4) != 0) return false;
    char* end = nullptr;
    const unsigned long uw = std::strtoul(cursor + 4, &end, 10);
    if (!end || *end != '\0' || uw == 0 || uh == 0 || uw > 65535 || uh > 65535) return false;
    w = uint32_t(uw); h = uint32_t(uh);
    rgb.assign(size_t(w) * h * 3, 0.0f);
    std::vector<uint8_t> scan(size_t(w) * 4);
    for (uint32_t y = 0; y < h; ++y) {
        if (pos + 4 > len) return false;
        const bool rle = (w >= 8 && w < 32768 && data[pos] == 2 && data[pos + 1] == 2 &&
                          ((uint32_t(data[pos + 2]) << 8) | data[pos + 3]) == w);
        if (rle) {
            pos += 4;
            for (int c = 0; c < 4; ++c) {
                uint32_t x = 0;
                while (x < w) {
                    if (pos >= len) return false;
                    uint8_t count = data[pos++];
                    if (count > 128) {
                        count = uint8_t(count - 128);
                        if (pos >= len || x + count > w) return false;
                        const uint8_t v = data[pos++];
                        for (uint8_t k = 0; k < count; ++k) scan[size_t(x++) * 4 + c] = v;
                    } else {
                        if (count == 0 || pos + count > len || x + count > w) return false;
                        for (uint8_t k = 0; k < count; ++k) scan[size_t(x++) * 4 + c] = data[pos++];
                    }
                }
            }
        } else {
            if (pos + size_t(w) * 4 > len) return false;
            std::memcpy(scan.data(), data + pos, size_t(w) * 4);
            pos += size_t(w) * 4;
        }
        for (uint32_t x = 0; x < w; ++x)
            rgbe_to_float(&scan[size_t(x) * 4], &rgb[(size_t(y) * w + x) * 3]);
    }
    return true;
}

void to_rgb16(const float* rgb, size_t texels, std::vector<uint16_t>& out) {
    out.resize(texels * 3);
    for (size_t i = 0; i < texels * 3; ++i) {
        float v = rgb[i];
        if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
        const float s = std::min(1.0f, v / kPng16Scale) * 65535.0f;
        out[i] = uint16_t(s + 0.5f);
    }
}

void to_rgb8_tonemapped(const float* rgb, size_t texels, std::vector<uint8_t>& out) {
    out.resize(texels * 3);
    for (size_t i = 0; i < texels * 3; ++i)
        out[i] = uint8_t(linear_to_srgb(tonemap_ldr(rgb[i])) * 255.0f + 0.5f);
}

bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const size_t wrote = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    const bool ok = std::fclose(f) == 0 && wrote == bytes.size();
    if (!ok) { std::remove(tmp.c_str()); return false; }
    fs::rename(tmp, target, ec);
    if (ec) {
        // Windows refuses to rename over an existing file on some filesystems.
        fs::remove(target, ec);
        ec.clear();
        fs::rename(tmp, target, ec);
    }
    return !ec;
}

} // namespace gi_bake_image
