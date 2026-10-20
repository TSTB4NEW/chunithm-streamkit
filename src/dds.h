// chunithm-streamkit — dds.h
// DDS (DXT1/DXT5/未压缩 RGBA8) 解码 + 最小 PNG 编码（stored deflate，无需 zlib）
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace csk {
namespace dds {

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;   // RGBA8
};

static inline uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t Rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void Unpack565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)(c & 0x1F);
    r = (uint8_t)((r5 << 3) | (r5 >> 2));
    g = (uint8_t)((g6 << 2) | (g6 >> 4));
    b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// 解一个 4x4 颜色块，写入目标 RGBA（越界自动裁剪）
static inline void DecodeColorBlock(const uint8_t* blk, uint8_t* dst, uint32_t w, uint32_t h,
                                    uint32_t x0, uint32_t y0, bool dxt1) {
    uint16_t c0 = Rd16(blk), c1 = Rd16(blk + 2);
    uint32_t bits = Rd32(blk + 4);
    uint8_t r[4], g[4], b[4], a[4] = {255, 255, 255, 255};

    Unpack565(c0, r[0], g[0], b[0]);
    Unpack565(c1, r[1], g[1], b[1]);
    if (!dxt1 || c0 > c1) {
        r[2] = (uint8_t)((2 * r[0] + r[1]) / 3);
        g[2] = (uint8_t)((2 * g[0] + g[1]) / 3);
        b[2] = (uint8_t)((2 * b[0] + b[1]) / 3);
        r[3] = (uint8_t)((r[0] + 2 * r[1]) / 3);
        g[3] = (uint8_t)((g[0] + 2 * g[1]) / 3);
        b[3] = (uint8_t)((b[0] + 2 * b[1]) / 3);
    } else {
        r[2] = (uint8_t)((r[0] + r[1]) / 2);
        g[2] = (uint8_t)((g[0] + g[1]) / 2);
        b[2] = (uint8_t)((b[0] + b[1]) / 2);
        r[3] = g[3] = b[3] = 0;
        a[3] = 0;   // 1-bit 透明
    }

    for (uint32_t py = 0; py < 4; ++py) {
        for (uint32_t px = 0; px < 4; ++px) {
            uint32_t x = x0 + px, y = y0 + py;
            if (x >= w || y >= h) continue;
            uint8_t idx = (uint8_t)((bits >> (2 * (py * 4 + px))) & 3);
            uint8_t* o = dst + ((size_t)y * w + x) * 4;
            o[0] = r[idx]; o[1] = g[idx]; o[2] = b[idx]; o[3] = a[idx];
        }
    }
}

// 解一个 DXT5 alpha 块（8 字节）
static inline void DecodeAlphaBlock(const uint8_t* blk, uint8_t* dst, uint32_t w, uint32_t h,
                                    uint32_t x0, uint32_t y0) {
    uint8_t a0 = blk[0], a1 = blk[1];
    uint8_t a[8];
    a[0] = a0; a[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i)
            a[i] = (uint8_t)(((8 - i) * a0 + (i - 1) * a1) / 7);
    } else {
        for (int i = 2; i < 6; ++i)
            a[i] = (uint8_t)(((6 - i) * a0 + (i - 1) * a1) / 5);
        a[6] = 0; a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)blk[2 + i] << (8 * i);
    for (uint32_t py = 0; py < 4; ++py) {
        for (uint32_t px = 0; px < 4; ++px) {
            uint32_t x = x0 + px, y = y0 + py;
            if (x >= w || y >= h) continue;
            uint8_t idx = (uint8_t)((bits >> (3 * (py * 4 + px))) & 7);
            dst[((size_t)y * w + x) * 4 + 3] = a[idx];
        }
    }
}

// 解码 DDS（DXT1 / DXT5 / RGBA8 未压缩）
inline bool Decode(const uint8_t* d, size_t n, Image& out) {
    if (n < 128 || std::memcmp(d, "DDS ", 4) != 0) return false;
    uint32_t h = Rd32(d + 12);
    uint32_t w = Rd32(d + 16);
    uint32_t fourcc = Rd32(d + 84);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;

    out.w = w;
    out.h = h;
    out.rgba.assign((size_t)w * h * 4, 255);
    const uint8_t* src = d + 128;
    size_t avail = n - 128;

    if (fourcc == 0x31545844u || fourcc == 0x35545844u) {   // 'DXT1' / 'DXT5'
        bool dxt1 = (fourcc == 0x31545844u);
        size_t blockSize = dxt1 ? 8 : 16;
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (avail < (size_t)bw * bh * blockSize) return false;
        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                const uint8_t* blk = src + ((size_t)by * bw + bx) * blockSize;
                if (!dxt1) DecodeAlphaBlock(blk, out.rgba.data(), w, h, bx * 4, by * 4);
                DecodeColorBlock(blk + (dxt1 ? 0 : 8), out.rgba.data(), w, h, bx * 4, by * 4, dxt1);
            }
        }
        return true;
    }
    // 未压缩 32bpp
    if (avail >= (size_t)w * h * 4) {
        std::memcpy(out.rgba.data(), src, (size_t)w * h * 4);
        for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
        return true;
    }
    return false;
}

// 盒式降采样：把长边限制到 maxSide（封面显示尺寸只有 142px，300px 存储纯属浪费）
// —— 最小 PNG 编码（zlib stored 块，不压缩）——
inline void Downscale(Image& img, uint32_t maxSide) {
    if (img.w <= maxSide && img.h <= maxSide) return;
    uint32_t dw, dh;
    if (img.w >= img.h) {
        dw = maxSide;
        dh = (uint32_t)((uint64_t)img.h * maxSide / img.w);
    } else {
        dh = maxSide;
        dw = (uint32_t)((uint64_t)img.w * maxSide / img.h);
    }
    if (dw < 8) dw = 8;
    if (dh < 8) dh = 8;

    std::vector<uint8_t> out((size_t)dw * dh * 4, 255);
    for (uint32_t y = 0; y < dh; ++y) {
        uint32_t sy0 = (uint32_t)((uint64_t)y * img.h / dh);
        uint32_t sy1 = (uint32_t)((uint64_t)(y + 1) * img.h / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (uint32_t x = 0; x < dw; ++x) {
            uint32_t sx0 = (uint32_t)((uint64_t)x * img.w / dw);
            uint32_t sx1 = (uint32_t)((uint64_t)(x + 1) * img.w / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (uint32_t sy = sy0; sy < sy1; ++sy) {
                const uint8_t* row = img.rgba.data() + (size_t)sy * img.w * 4;
                for (uint32_t sx = sx0; sx < sx1; ++sx) {
                    const uint8_t* px = row + (size_t)sx * 4;
                    r += px[0]; g += px[1]; b += px[2]; a += px[3]; ++n;
                }
            }
            if (!n) n = 1;
            uint8_t* o = out.data() + ((size_t)y * dw + x) * 4;
            o[0] = (uint8_t)(r / n); o[1] = (uint8_t)(g / n);
            o[2] = (uint8_t)(b / n); o[3] = (uint8_t)(a / n);
        }
    }
    img.rgba.swap(out);
    img.w = dw;
    img.h = dh;
}


static inline uint32_t Crc32(const uint8_t* d, size_t n) {
    // C++11 魔法静态：线程安全初始化（避免并发请求时表未写完导致 CRC 错误）
    struct CrcTable {
        uint32_t v[256];
        CrcTable() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                v[i] = c;
            }
        }
    };
    static const CrcTable t;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = t.v[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// PNG / zlib 都是大端
static inline void Put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)((x >> 24) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)(x & 0xFF));
}

static inline void PutChunk(std::vector<uint8_t>& out, const char* type,
                            const uint8_t* data, size_t len) {
    Put32(out, (uint32_t)len);
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    uint32_t crc = Crc32(out.data() + start, out.size() - start);
    Put32(out, crc);
}

inline std::vector<uint8_t> EncodePng(const Image& img) {
    std::vector<uint8_t> out;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    // IHDR: w, h, bitdepth=8, colortype=6(RGBA), compression=0, filter=0, interlace=0
    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(img.w >> 24); ihdr[1] = (uint8_t)(img.w >> 16);
    ihdr[2] = (uint8_t)(img.w >> 8);  ihdr[3] = (uint8_t)(img.w);
    ihdr[4] = (uint8_t)(img.h >> 24); ihdr[5] = (uint8_t)(img.h >> 16);
    ihdr[6] = (uint8_t)(img.h >> 8);  ihdr[7] = (uint8_t)(img.h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    PutChunk(out, "IHDR", ihdr, sizeof(ihdr));

    // 原始扫描线：每行前加 filter=0
    size_t stride = (size_t)img.w * 4;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * img.h);
    for (uint32_t y = 0; y < img.h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), img.rgba.begin() + (size_t)y * stride,
                   img.rgba.begin() + (size_t)(y + 1) * stride);
    }

    // zlib 流：header + stored deflate 块 + adler32
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;
        z.push_back(0x00);                       // BFINAL=0, BTYPE=00 (stored)
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    // 空 stored 块标记结束
    z.push_back(0x01); z.push_back(0x00); z.push_back(0x00);
    z.push_back(0xFF); z.push_back(0xFF);
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t c : raw) { s1 = (s1 + c) % 65521; s2 = (s2 + s1) % 65521; }
    Put32(z, (s2 << 16) | s1);

    PutChunk(out, "IDAT", z.data(), z.size());
    PutChunk(out, "IEND", nullptr, 0);
    return out;
}

} // namespace dds
} // namespace csk
