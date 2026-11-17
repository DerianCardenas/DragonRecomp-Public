#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dragon {

std::vector<uint8_t> read_binary_file(const std::filesystem::path& path);
std::vector<uint8_t> try_read_binary_file(const std::filesystem::path& path) noexcept;
std::string read_text_file(const std::filesystem::path& path);
void write_text_file(const std::filesystem::path& path, std::string_view content);
void write_binary_file(const std::filesystem::path& path, std::span<const uint8_t> data);
bool try_write_binary_file(const std::filesystem::path& path, std::span<const uint8_t> data) noexcept;

}  // namespace dragon
