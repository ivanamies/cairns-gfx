// tests/test_md5.cpp
//
// Tiny portable MD5 (RFC 1321). Used by Tier G goldens to stamp captured
// pixel buffers + skin SSBO bytes into a per-platform / shared ref. NOT
// security-sensitive -- we only need stability across runs + platforms.

#include "test_seams.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

inline uint32_t LeftRotate(uint32_t x, int c) {
    return (x << c) | (x >> (32 - c));
}

// RFC 1321 constants.
constexpr std::array<uint32_t, 64> kS = {
    7,12,17,22,  7,12,17,22,  7,12,17,22,  7,12,17,22,
    5, 9,14,20,  5, 9,14,20,  5, 9,14,20,  5, 9,14,20,
    4,11,16,23,  4,11,16,23,  4,11,16,23,  4,11,16,23,
    6,10,15,21,  6,10,15,21,  6,10,15,21,  6,10,15,21,
};
constexpr std::array<uint32_t, 64> kK = {
    0xD76AA478,0xE8C7B756,0x242070DB,0xC1BDCEEE,
    0xF57C0FAF,0x4787C62A,0xA8304613,0xFD469501,
    0x698098D8,0x8B44F7AF,0xFFFF5BB1,0x895CD7BE,
    0x6B901122,0xFD987193,0xA679438E,0x49B40821,
    0xF61E2562,0xC040B340,0x265E5A51,0xE9B6C7AA,
    0xD62F105D,0x02441453,0xD8A1E681,0xE7D3FBC8,
    0x21E1CDE6,0xC33707D6,0xF4D50D87,0x455A14ED,
    0xA9E3E905,0xFCEFA3F8,0x676F02D9,0x8D2A4C8A,
    0xFFFA3942,0x8771F681,0x6D9D6122,0xFDE5380C,
    0xA4BEEA44,0x4BDECFA9,0xF6BB4B60,0xBEBFBC70,
    0x289B7EC6,0xEAA127FA,0xD4EF3085,0x04881D05,
    0xD9D4D039,0xE6DB99E5,0x1FA27CF8,0xC4AC5665,
    0xF4292244,0x432AFF97,0xAB9423A7,0xFC93A039,
    0x655B59C3,0x8F0CCC92,0xFFEFF47D,0x85845DD1,
    0x6FA87E4F,0xFE2CE6E0,0xA3014314,0x4E0811A1,
    0xF7537E82,0xBD3AF235,0x2AD7D2BB,0xEB86D391,
};

struct Md5 {
    uint32_t a0 = 0x67452301;
    uint32_t b0 = 0xEFCDAB89;
    uint32_t c0 = 0x98BADCFE;
    uint32_t d0 = 0x10325476;

    void ProcessChunk(const uint8_t* chunk) {
        std::array<uint32_t, 16> m{};
        for (int j = 0; j < 16; ++j) {
            m[j] = static_cast<uint32_t>(chunk[j * 4]) |
                   (static_cast<uint32_t>(chunk[j * 4 + 1]) << 8) |
                   (static_cast<uint32_t>(chunk[j * 4 + 2]) << 16) |
                   (static_cast<uint32_t>(chunk[j * 4 + 3]) << 24);
        }
        uint32_t a = a0;
        uint32_t b = b0;
        uint32_t c = c0;
        uint32_t d = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t f = 0;
            int g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const uint32_t temp = d;
            d = c;
            c = b;
            b = b + LeftRotate(a + f + kK[i] + m[g], kS[i]);
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    std::array<uint8_t, 16> Finalize(const uint8_t* msg, size_t len) {
        const size_t bit_len = len * 8;
        std::vector<uint8_t> padded(msg, msg + len);
        padded.push_back(0x80);
        while ((padded.size() % 64) != 56) {
            padded.push_back(0);
        }
        for (int i = 0; i < 8; ++i) {
            padded.push_back(static_cast<uint8_t>((bit_len >> (8 * i)) & 0xFF));
        }
        for (size_t off = 0; off < padded.size(); off += 64) {
            ProcessChunk(padded.data() + off);
        }
        std::array<uint8_t, 16> out{};
        for (int i = 0; i < 4; ++i) {
            out[i] = static_cast<uint8_t>((a0 >> (8 * i)) & 0xFF);
            out[4 + i] = static_cast<uint8_t>((b0 >> (8 * i)) & 0xFF);
            out[8 + i] = static_cast<uint8_t>((c0 >> (8 * i)) & 0xFF);
            out[12 + i] = static_cast<uint8_t>((d0 >> (8 * i)) & 0xFF);
        }
        return out;
    }
};

}  // namespace

namespace cairns::test_seams {

std::string Md5Hex(const std::vector<uint8_t>& bytes) {
    Md5 m;
    const std::array<uint8_t, 16> digest =
        m.Finalize(bytes.data(), bytes.size());
    std::string out;
    out.reserve(32);
    for (uint8_t b : digest) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out.push_back(buf[0]);
        out.push_back(buf[1]);
    }
    return out;
}

}  // namespace cairns::test_seams
