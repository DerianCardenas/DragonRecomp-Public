#include "dragon/file_io.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dragon {

std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open file for reading: " + path.string());
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("Failed to determine file size: " + path.string());
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 && !file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        throw std::runtime_error("Failed to read file contents: " + path.string());
    }
    return buffer;
}

std::vector<uint8_t> try_read_binary_file(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
        return {};
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return {};
    }
    return buffer;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to open text file for reading: " + path.string());
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

void write_text_file(const std::filesystem::path& path, std::string_view content) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("Failed to open text file for writing: " + path.string());
    }
    if (!content.empty() &&
        !file.write(content.data(), static_cast<std::streamsize>(content.size()))) {
        throw std::runtime_error("Failed to write text file: " + path.string());
    }
}

void write_binary_file(const std::filesystem::path& path, std::span<const uint8_t> data) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("Failed to open binary file for writing: " + path.string());
    }
    if (!data.empty() && !file.write(reinterpret_cast<const char*>(data.data()),
                                     static_cast<std::streamsize>(data.size()))) {
        throw std::runtime_error("Failed to write binary file: " + path.string());
    }
}

bool try_write_binary_file(const std::filesystem::path& path,
                           std::span<const uint8_t> data) noexcept {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!data.empty() && !file.write(reinterpret_cast<const char*>(data.data()),
                                     static_cast<std::streamsize>(data.size()))) {
        return false;
    }
    return true;
}

}  // namespace dragon
