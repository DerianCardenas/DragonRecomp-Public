#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "dragon/config.h"
#include "dragon/file_io.h"
#include "dragon/log.h"
#include "dragon/save_state.h"
#include "dragon/sha1.h"

namespace {

void test_sha1() {
    // FIPS 180-4 test vectors.
    const std::vector<uint8_t> empty;
    assert(dragon::sha1_hex(empty) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    const char* abc_str = "abc";
    const std::vector<uint8_t> abc(reinterpret_cast<const uint8_t*>(abc_str),
                                   reinterpret_cast<const uint8_t*>(abc_str) + 3);
    assert(dragon::sha1_hex(abc) == "a9993e364706816aba3e25717850c26c9cd0d89d");

    const char* long_str = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const std::vector<uint8_t> long_vec(
        reinterpret_cast<const uint8_t*>(long_str),
        reinterpret_cast<const uint8_t*>(long_str) + std::strlen(long_str));
    assert(dragon::sha1_hex(long_vec) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

void test_config_parser() {
    constexpr const char* kValidToml = R"(
jump_engine = [0x8E04]
stack_tricks = [0x9123]

[game]
name = "Test Game"
platform = "nes"
rom = "test.nes"
sha1 = "EA343F4E445A9050D4B4FBAC2C77D0693B1D0922"
mapper = 0
allow_unofficial = true

[output]
dir = "out_gen"
split_every = 100

[[entry_points]]
addr = 0x8212
bank = 0
name = "GameEngine"

[[jump_tables]]
addr = 0x8E04
count = 4
format = "word"

[[data_ranges]]
start = 0x8000
end = 0x8010

[[inline_data_calls]]
target = 0x8E04
kind = "jump_engine"
)";

    dragon::GameConfig cfg = dragon::parse_game_config_str(kValidToml, "/tmp/game_dir");
    assert(cfg.name == "Test Game");
    assert(cfg.platform == "nes");
    assert(cfg.sha1 == "ea343f4e445a9050d4b4fbac2c77d0693b1d0922");
    assert(cfg.mapper == 0);
    assert(cfg.allow_unofficial == true);
    assert(cfg.split_every == 100);
    assert(cfg.entry_points.size() == 1);
    assert(cfg.entry_points[0].addr == 0x8212);
    assert(cfg.entry_points[0].name == "GameEngine");
    assert(cfg.jump_tables.size() == 1);
    assert(cfg.jump_tables[0].addr == 0x8E04);
    assert(cfg.jump_tables[0].count == 4);
    assert(cfg.data_ranges.size() == 1);
    assert(cfg.data_ranges[0].start == 0x8000);
    assert(cfg.data_ranges[0].end == 0x8010);
    assert(cfg.jump_engine.size() == 1);
    assert(cfg.jump_engine[0] == 0x8E04);
    assert(cfg.stack_tricks.size() == 1);
    assert(cfg.stack_tricks[0] == 0x9123);

    // Unknown key must throw.
    constexpr const char* kUnknownKeyToml = R"(
[game]
name = "Test"
platform = "nes"
rom = "test.nes"
sha1 = "ea343f4e445a9050d4b4fbac2c77d0693b1d0922"
typo_key = 123
)";
    bool caught = false;
    try {
        (void)dragon::parse_game_config_str(kUnknownKeyToml);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    assert(caught);

    // Also test loading actual smb.toml from repo.
    dragon::GameConfig smb_cfg = dragon::load_game_config("nes/games/smb/smb.toml");
    assert(smb_cfg.name == "Super Mario Bros.");
    assert(smb_cfg.platform == "nes");
    assert(smb_cfg.mapper == 0);
}

void test_file_io() {
    const std::filesystem::path test_path =
        std::filesystem::temp_directory_path() / "dragon_test_save.bin";
    const std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};

    // try_read of non-existent file returns empty vector without throwing
    std::filesystem::remove(test_path);
    auto missing = dragon::try_read_binary_file(test_path);
    assert(missing.empty());

    // try_write creates and writes file
    bool ok = dragon::try_write_binary_file(test_path, payload);
    assert(ok);

    // try_read reads the written data
    auto read_back = dragon::try_read_binary_file(test_path);
    assert(read_back == payload);

    std::filesystem::remove(test_path);
}

void test_save_state_header() {
    dragon::SaveStateHeader hdr;
    assert(hdr.is_valid("NONE"));
    assert(!hdr.is_valid("NES "));

    hdr.system[0] = 'N';
    hdr.system[1] = 'E';
    hdr.system[2] = 'S';
    hdr.system[3] = ' ';
    assert(hdr.is_valid("NES "));
    assert(!hdr.is_valid("SNES"));

    std::filesystem::path p = dragon::slot_state_path("/path/to/game.nes", 3);
    assert(p.string() == "/path/to/game.state3");
}

}  // namespace

int main() {
    test_sha1();
    test_config_parser();
    test_file_io();
    test_save_state_header();
    std::printf("dragon_common_tests: all tests passed\n");
    return 0;
}
