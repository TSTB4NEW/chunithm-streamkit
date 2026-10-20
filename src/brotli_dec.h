// chunithm-streamkit — brotli_dec.h
// 官方 brotli v1.0.9 解码器的 C++ 薄封装
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

extern "C" {
#include "brotli/decode.h"
}

namespace csk {

// 解压 brotli 数据；成功返回 true 并填充 out
inline bool BrotliDecode(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) {
    out.clear();
    if (!in || inLen == 0) return false;

    BrotliDecoderState* st = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!st) return false;

    const uint8_t* nextIn = in;
    size_t availIn = inLen;
    size_t totalOut = 0;
    // 预估输出上限，防止恶意数据无限膨胀
    const size_t kMaxOut = 8u << 20;

    uint8_t buf[32768];
    bool ok = false;
    for (;;) {
        uint8_t* nextOut = buf;
        size_t availOut = sizeof(buf);
        BrotliDecoderResult r =
            BrotliDecoderDecompressStream(st, &availIn, &nextIn, &availOut, &nextOut, nullptr);
        size_t produced = sizeof(buf) - availOut;
        if (produced) {
            if (totalOut + produced > kMaxOut) break;
            out.insert(out.end(), buf, buf + produced);
            totalOut += produced;
        }
        if (r == BROTLI_DECODER_RESULT_SUCCESS) { ok = true; break; }
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) continue;
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) break;   // 数据不完整
        break;                                                     // ERROR
    }
    BrotliDecoderDestroyInstance(st);
    return ok && !out.empty();
}

// 便捷：解压成字符串
inline bool BrotliDecodeStr(const uint8_t* in, size_t inLen, std::string& out) {
    std::vector<uint8_t> v;
    if (!BrotliDecode(in, inLen, v)) return false;
    out.assign((const char*)v.data(), v.size());
    return true;
}

} // namespace csk
