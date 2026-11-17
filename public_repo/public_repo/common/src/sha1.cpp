#include "dragon/sha1.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace dragon {

namespace {

constexpr uint32_t rotl32(uint32_t value, unsigned int count) {
    return (value << count) | (value >> (32U - count));
}

void process_block(const uint8_t block[64], uint32_t state[5]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24U) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16U) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8U) |
               (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1U);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }
        uint32_t temp = rotl32(a, 5U) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30U);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

}  // namespace

std::string sha1_hex(std::span<const uint8_t> data) {
    uint32_t state[5] = {
        0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U,
    };

    const size_t total_bytes = data.size();
    size_t offset = 0;
    while (offset + 64 <= total_bytes) {
        process_block(data.data() + offset, state);
        offset += 64;
    }

    uint8_t tail[128];
    std::memset(tail, 0, sizeof(tail));
    const size_t rem = total_bytes - offset;
    if (rem > 0) {
        std::memcpy(tail, data.data() + offset, rem);
    }
    tail[rem] = 0x80U;

    const size_t pad_blocks = (rem >= 56) ? 2 : 1;
    const uint64_t bit_len = static_cast<uint64_t>(total_bytes) * 8ULL;
    const size_t len_offset = pad_blocks * 64 - 8;
    for (int i = 0; i < 8; ++i) {
        tail[len_offset + i] = static_cast<uint8_t>((bit_len >> ((7 - i) * 8)) & 0xFFU);
    }

    process_block(tail, state);
    if (pad_blocks == 2) {
        process_block(tail + 64, state);
    }

    char hex[41];
    std::snprintf(hex, sizeof(hex), "%08x%08x%08x%08x%08x", state[0], state[1], state[2], state[3],
                  state[4]);
    return std::string(hex, 40);
}

}  // namespace dragon
