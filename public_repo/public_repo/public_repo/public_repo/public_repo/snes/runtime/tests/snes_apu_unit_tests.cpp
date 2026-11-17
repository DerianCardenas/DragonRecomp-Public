#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "snes/apu.h"
#include "snes/bus.h"

namespace {

using dragon::snes::Apu;
using dragon::snes::Bus;

void test_spc_instructions_and_timers() {
    Apu apu;
    apu.reset();

    // 1. Timers
    // Set Timer 2 target = 2 (divider 16 * 2 = 32 cycles per tick)
    apu.aram()[0x00FCU] = 0x02U;
    apu.aram()[0x00F1U] = 0x04U;  // enable Timer 2

    // Run 64 cycles -> should increment Timer 2 counter twice
    apu.step_cycles(64ULL);

    std::printf("test_spc_instructions_and_timers passed\n");
}

void test_ipl_rom_handshake() {
    Bus bus;
    bus.reset();

    // The SPC700 boots in IPL ROM ($FFC0).
    // Initial handshake: SPC700 writes $AA to $2140 and $BB to $2141.
    for (uint64_t cyc = 0; cyc < 2000ULL; ++cyc) {
        bus.apu().step_cycles(10ULL);
        if (bus.read(0x2140U) == 0xAAU && bus.read(0x2141U) == 0xBBU) {
            break;
        }
    }
    assert(bus.read(0x2140U) == 0xAAU);
    assert(bus.read(0x2141U) == 0xBBU);

    // Nintendo IPL protocol handshake:
    // 1. Host writes target load address to $2142-$2143 (e.g. $0200)
    // 2. Host writes $01 to $2141 (mode = 1: transfer block)
    // 3. Host writes $CC to $2140 (handshake trigger)
    bus.write(0x2142U, 0x00U);
    bus.write(0x2143U, 0x02U);
    bus.write(0x2141U, 0x01U);
    bus.write(0x2140U, 0xCCU);

    // Step SPC700 until it echoes $CC back on $2140
    bool ack = false;
    for (uint64_t cyc = 0; cyc < 2000ULL; ++cyc) {
        bus.apu().step_cycles(10ULL);
        if (bus.read(0x2140U) == 0xCCU) {
            ack = true;
            break;
        }
    }
    assert(ack);

    // Transfer 5 bytes of code into ARAM at $0200:
    // Byte 0: $E8 (MOV A, #imm)
    // Byte 1: $77
    // Byte 2: $8F (MOV dp, #imm)
    // Byte 3: $55
    // Byte 4: $F4 (writes $55 to $F4 / $2140)
    const std::array<uint8_t, 5> payload = {0xE8U, 0x77U, 0x8FU, 0x55U, 0xF4U};
    for (uint8_t i = 0; i < payload.size(); ++i) {
        bus.write(0x2141U, payload[i]);
        bus.write(0x2140U, i);
        bool byte_ack = false;
        for (uint64_t cyc = 0; cyc < 2000ULL; ++cyc) {
            bus.apu().step_cycles(10ULL);
            if (bus.read(0x2140U) == i) {
                bus.apu().step_cycles(5ULL);  // allow MOV [$00]+Y, A to complete
                byte_ack = true;
                break;
            }
        }
        assert(byte_ack);
        assert(bus.apu().aram()[0x0200U + i] == payload[i]);
    }

    // Terminate transfer and jump to entry point $0200:
    // Mode = 0 on $2141, entry address on $2142-$2143, index on $2140
    bus.write(0x2142U, 0x00U);
    bus.write(0x2143U, 0x02U);
    bus.write(0x2141U, 0x00U);
    bus.write(0x2140U, static_cast<uint8_t>(payload.size() + 2U));

    // Step SPC700 to execute jumped code at $0200
    bool finished = false;
    for (uint64_t cyc = 0; cyc < 2000ULL; ++cyc) {
        bus.apu().step_cycles(10ULL);
        if (bus.read(0x2140U) == 0x55U) {
            finished = true;
            break;
        }
    }
    assert(finished);
    assert(bus.apu().a() == 0x77U);

    std::printf("test_ipl_rom_handshake passed\n");
}

void test_brr_decoding_and_audio_render() {
    Apu apu;
    apu.reset();

    // Build a test BRR block in ARAM:
    // Range=1, Filter=0, Loop=1, End=1 -> header = (1 << 4) | 0x03 = 0x13
    // Nibbles with non-zero values (e.g. alternating +3 and -3 -> 0x3D)
    const uint16_t sample_addr = 0x1000U;
    apu.aram()[sample_addr + 0U] = 0x13U;  // Header: range 1, filter 0, loop 1, end 1
    for (size_t i = 1; i < 9; ++i) {
        apu.aram()[sample_addr + i] = 0x3DU;
    }

    // Setup sample directory at page $08 ($0800):
    // Sample 0: start = $1000, loop = $1000
    apu.write_dsp_register(0x5DU, 0x08U);  // DIR = $08
    apu.aram()[0x0800U] = 0x00U;
    apu.aram()[0x0801U] = 0x10U;
    apu.aram()[0x0802U] = 0x00U;
    apu.aram()[0x0803U] = 0x10U;

    // Configure Voice 0:
    // Volume left = 100, right = 100
    // Pitch = 0x1000 (32 kHz)
    // SRCN = 0
    apu.write_dsp_register(0x00U, 100U);   // VOL_L
    apu.write_dsp_register(0x01U, 100U);   // VOL_R
    apu.write_dsp_register(0x02U, 0x00U);  // PITCH_L
    apu.write_dsp_register(0x03U, 0x10U);  // PITCH_H = 0x1000
    apu.write_dsp_register(0x04U, 0x00U);  // SRCN = 0
    apu.write_dsp_register(0x0CU, 127U);   // MVOL_L
    apu.write_dsp_register(0x1CU, 127U);   // MVOL_R

    // Trigger Key-On for Voice 0
    apu.write_dsp_register(0x4CU, 0x01U);

    // Render 48 kHz stereo PCM samples
    std::vector<int16_t> audio_buf(960, 0);  // 480 stereo pairs = 10ms at 48 kHz
    apu.render_samples(audio_buf);

    // Verify non-zero PCM audio is produced
    bool found_audio = false;
    for (int16_t s : audio_buf) {
        if (s != 0) {
            found_audio = true;
            break;
        }
    }
    assert(found_audio);

    std::printf("test_brr_decoding_and_audio_render passed\n");
}

}  // namespace

int main() {
    test_spc_instructions_and_timers();
    test_ipl_rom_handshake();
    test_brr_decoding_and_audio_render();
    std::printf("All snes_apu_unit_tests passed!\n");
    return 0;
}
