#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace dragon {

// Computes the lowercase 40-character hexadecimal SHA-1 digest of `data`.
std::string sha1_hex(std::span<const uint8_t> data);

}  // namespace dragon
