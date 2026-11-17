#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "nes/runtime.h"

namespace {

using dragon::nes::Apu;
using dragon::nes::Bus;
using dragon::nes::Cartridge;
using dragon::nes::cpu_init;
using dragon::nes::cpu_reset;
using dragon::nes::cpu_run_frames_interpreter;
using dragon::nes::cpu_step_with_ppu;
using dragon::nes::load_ines_rom;

void test_length_counters_and_status_register() {
    Bus bus;
    CpuState cpu{};
    cpu_init(cpu, &bus);

    // Initially all length counters are 0
    assert((bus.read(0x4015U) & 0x1FU) == 0x00U);

    // Writing $4003 while channel 1 is disabled in $4015 must NOT load length counter
    bus.write(0x4015U, 0x00U);
    bus.write(0x4003U, 0x08U);  // index 1 -> 254 if enabled
    assert(bus.apu().length_counter(0U) == 0U);
    assert((bus.read(0x4015U) & 0x01U) == 0U);

    // Enable Pulse 1, Pulse 2, Triangle, Noise ($4015 = $0F)
    bus.write(0x4015U, 0x0FU);
    bus.write(0x4003U, static_cast<uint8_t>(0U << 3U));  // index 0 -> length 10
    bus.write(0x4007U, static_cast<uint8_t>(3U << 3U));  // index 3 -> length 2
    bus.write(0x400BU, static_cast<uint8_t>(1U << 3U));  // index 1 -> length 254
    bus.write(0x400FU, static_cast<uint8_t>(5U << 3U));  // index 5 -> length 4

    assert(bus.apu().length_counter(0U) == 10U);
    assert(bus.apu().length_counter(1U) == 2U);
    assert(bus.apu().length_counter(2U) == 254U);
    assert(bus.apu().length_counter(3U) == 4U);
    assert((bus.read(0x4015U) & 0x0FU) == 0x0FU);

    // Set halt flag on Pulse 1 ($4000 bit 5), leave others unhalted
    bus.write(0x4000U, 0x20U);

    // Writing $4017 with bit 7 = 1 immediately clocks quarter + half frame!
    bus.write(0x4017U, 0x80U);
    assert(bus.apu().length_counter(0U) == 10U);   // halted -> unchanged
    assert(bus.apu().length_counter(1U) == 1U);    // decremented from 2 -> 1
    assert(bus.apu().length_counter(2U) == 253U);  // decremented from 254 -> 253
    assert(bus.apu().length_counter(3U) == 3U);    // decremented from 4 -> 3

    // Clock once more -> Pulse 2 length counter reaches 0 and clears bit 1 in $4015
    bus.write(0x4017U, 0x80U);
    assert(bus.apu().length_counter(1U) == 0U);
    assert((bus.read(0x4015U) & 0x0FU) == 0x0DU);

    // Disabling channels via $4015 immediately zeroes their length counters
    bus.write(0x4015U, 0x00U);
    assert(bus.apu().length_counter(0U) == 0U);
    assert(bus.apu().length_counter(2U) == 0U);
    assert(bus.apu().length_counter(3U) == 0U);
    assert((bus.read(0x4015U) & 0x0FU) == 0x00U);
}

void test_frame_counter_4step_5step_and_irq() {
    Bus bus;
    CpuState cpu{};
    cpu_init(cpu, &bus);

    // Configure 4-step mode with IRQ enabled ($4017 = $00)
    bus.write(0x4017U, 0x00U);
    assert(!bus.apu().frame_irq());

    // Step 29827 CPU cycles -> just before step 4 IRQ
    bus.apu().step_cycles(29827ULL);
    assert(!bus.apu().frame_irq());

    // Step 1 cycle to 29828 -> frame IRQ flag must be set!
    bus.apu().step_cycle();
    assert(bus.apu().frame_irq());
    assert(bus.irq_pending());

    // Reading $4015 returns bit 6 = 1 and clears the frame IRQ flag
    const uint8_t status1 = bus.read(0x4015U);
    assert((status1 & 0x40U) != 0U);
    assert(!bus.apu().frame_irq());
    assert((bus.read(0x4015U) & 0x40U) == 0U);

    // Verify $4017 bit 6 = 1 inhibits and clears frame IRQ
    bus.write(0x4017U, 0x00U);
    bus.apu().step_cycles(29829ULL);
    assert(bus.apu().frame_irq());
    bus.write(0x4017U, 0x40U);  // Inhibit IRQ
    assert(!bus.apu().frame_irq());
    bus.apu().step_cycles(30000ULL);
    assert(!bus.apu().frame_irq());

    // Verify 5-step mode ($4017 = $80) never sets frame IRQ
    bus.write(0x4017U, 0x80U);
    bus.apu().step_cycles(40000ULL);
    assert(!bus.apu().frame_irq());

    // Verify CPU IRQ delivery when I == 0 and APU frame IRQ fires
    Bus irq_bus;
    // Place simple program at $8000: CLI; loop: JMP $8001; IRQ handler at $8100: INC $10; LDA
    // $4015; RTI
    const std::vector<uint8_t> code = {
        0x58,  // $8000: CLI
        0x4C,
        0x01,
        0x80,  // $8001: JMP $8001
    };
    const std::vector<uint8_t> irq_handler = {
        0xAD, 0x15, 0x40,  // $8100: LDA $4015 (acknowledge frame IRQ)
        0xE6, 0x10,        // $8103: INC $10
        0x40,              // $8105: RTI
    };
    irq_bus.load_direct(0x8000U, code);
    irq_bus.load_direct(0x8100U, irq_handler);
    irq_bus.write_direct(0xFFFCU, 0x00U);
    irq_bus.write_direct(0xFFFDU, 0x80U);
    irq_bus.write_direct(0xFFFEU, 0x00U);
    irq_bus.write_direct(0xFFFFU, 0x81U);

    CpuState irq_cpu{};
    cpu_init(irq_cpu, &irq_bus);
    cpu_reset(irq_cpu);
    irq_bus.write(0x4017U, 0x00U);  // 4-step mode, IRQ enabled

    while (irq_cpu.cycles < 31000ULL && irq_bus.read(0x0010U) == 0U) {
        cpu_step_with_ppu(irq_cpu);
    }
    assert(irq_bus.read(0x0010U) == 1U);
    assert(!irq_bus.apu().frame_irq());
}

void test_pulse_sweep_and_channels() {
    Apu apu;
    apu.reset();

    // Enable Pulse 1 and Pulse 2
    apu.write_register(0x4015U, 0x03U);

    // Configure Pulse 1 & Pulse 2 with timer_period = 0x0100 (256), constant volume 12, 50% duty
    apu.write_register(0x4000U, 0x9CU);
    apu.write_register(0x4002U, 0x00U);
    apu.write_register(0x4003U, 0x09U);  // period high = 1, length index = 1 (254)

    apu.write_register(0x4004U, 0x9CU);
    apu.write_register(0x4006U, 0x00U);
    apu.write_register(0x4007U, 0x09U);

    // Enable negate sweep with shift = 1, period = 0 on both Pulse 1 and Pulse 2:
    // change = 256 >> 1 = 128.
    // Pulse 1 (ones' complement): 256 - 128 - 1 = 127.
    // Pulse 2 (two's complement): 256 - 128 = 128.
    apu.write_register(0x4001U, 0x89U);  // enabled=1, period=0, negate=1, shift=1
    apu.write_register(0x4005U, 0x89U);

    // Clock half-frame twice via $4017 = $80: first reloads sweep divider, second updates period
    apu.write_register(0x4017U, 0x80U);
    apu.write_register(0x4017U, 0x80U);

    // Test Triangle ultrasonic silencing when period < 2
    apu.write_register(0x4015U, 0x04U);  // Triangle only
    apu.write_register(0x4008U, 0xFFU);  // control=1, linear=127
    apu.write_register(0x400AU, 0x01U);  // period = 1 (< 2 -> silenced!)
    apu.write_register(0x400BU, 0x08U);
    apu.write_register(0x4017U, 0x80U);  // clock linear counter
    apu.step_cycles(64ULL);
    assert(apu.current_mixer_output() == 0.0f);

    // Increase Triangle period to 32 -> should produce non-zero triangle wave output
    apu.write_register(0x400AU, 32U);
    bool saw_nonzero_tri = false;
    for (int i = 0; i < 64; ++i) {
        apu.step_cycle();
        if (apu.current_mixer_output() > 0.0f) {
            saw_nonzero_tri = true;
            break;
        }
    }
    assert(saw_nonzero_tri);

    // Test Noise 15-bit LFSR in normal mode and mode 1
    apu.reset();
    apu.write_register(0x4015U, 0x08U);  // Noise only
    apu.write_register(0x400CU, 0x1FU);  // constant volume 15
    apu.write_register(0x400EU, 0x00U);  // period index 0 (4 cycles), normal mode
    apu.write_register(0x400FU, 0x08U);  // load length counter
    bool saw_noise_high = false;
    bool saw_noise_low = false;
    for (int i = 0; i < 128; ++i) {
        apu.step_cycle();
        if (apu.current_mixer_output() > 0.0f) {
            saw_noise_high = true;
        } else {
            saw_noise_low = true;
        }
    }
    assert(saw_noise_high && saw_noise_low);
}

void test_dmc_reader_and_irq() {
    Bus bus;
    // Fill $C000-$C010 with 0xFF (delta +2 steps)
    std::vector<uint8_t> sample_data(17, 0xFFU);
    bus.load_direct(0xC000U, sample_data);

    // Direct load $4011 = 32
    bus.write(0x4011U, 32U);
    assert(bus.apu().dmc_output_level() == 32U);
    assert(bus.apu().current_mixer_output() > 0.0f);

    // Configure DMC: IRQ enabled, no loop, fastest rate ($0F -> 54 cycles), addr=$C000, len=1 byte
    bus.write(0x4010U, 0x8FU);
    bus.write(0x4012U, 0x00U);  // $C000
    bus.write(0x4013U, 0x00U);  // length = 1 byte

    // Enable DMC in $4015 -> immediately fetches the 1 sample byte, reaching 0 bytes remaining ->
    // sets DMC IRQ!
    bus.write(0x4015U, 0x10U);
    assert(bus.apu().dmc_irq());
    assert((bus.read(0x4015U) & 0x80U) != 0U);

    // Step 16 * 54 cycles (8 clocks to finish initial silent cycle and load sample_buffer,
    // + 8 clocks to shift out all 8 bits of 0xFF and modulate output_level from 32 -> 48)
    bus.apu().step_cycles(54ULL * 16ULL);
    assert(bus.apu().dmc_output_level() == 48U);

    // Writing $4015 clears DMC IRQ flag
    bus.write(0x4015U, 0x00U);
    assert(!bus.apu().dmc_irq());
}

void test_smb_audio_generation() {
    const std::filesystem::path rom_path = "roms/nes/smb.nes";
    if (!std::filesystem::exists(rom_path)) {
        std::printf("nes_apu_unit_tests: %s not present; skipping SMB audio check.\n",
                    rom_path.string().c_str());
        return;
    }

    const Cartridge cart = load_ines_rom(rom_path);
    Bus bus;
    bus.load_cartridge(cart);
    CpuState cpu{};
    cpu_init(cpu, &bus);
    cpu_reset(cpu);

    // Run 180 frames to title screen, press Start at frame 180..188, advance 172 frames past the
    // silent "WORLD 1-1" intro screen into active World 1-1 gameplay (frame 360), then capture
    // 20 frames of Overworld theme music.
    cpu_run_frames_interpreter(cpu, 180ULL);
    bus.set_controller(0U, dragon::nes::kButtonStart);
    cpu_run_frames_interpreter(cpu, 8ULL);
    bus.set_controller(0U, 0U);
    cpu_run_frames_interpreter(cpu, 172ULL);

    bus.apu().clear_samples();
    cpu_run_frames_interpreter(cpu, 20ULL);

    std::vector<int16_t> samples(bus.apu().available_samples());
    const size_t popped = bus.apu().pop_samples(samples.data(), samples.size());
    assert(popped > 1000U);

    size_t nonzero_count = 0U;
    int16_t min_sample = 0;
    int16_t max_sample = 0;
    for (size_t i = 0U; i < popped; ++i) {
        if (samples[i] != 0) {
            ++nonzero_count;
        }
        min_sample = std::min(min_sample, samples[i]);
        max_sample = std::max(max_sample, samples[i]);
    }

    std::printf("nes_apu_unit_tests: SMB generated %zu samples (%zu non-zero, range=[%d, %d])\n",
                popped, nonzero_count, static_cast<int>(min_sample), static_cast<int>(max_sample));
    assert(nonzero_count > popped / 2U);
    assert(max_sample - min_sample > 500);
}

}  // namespace

int main() {
    test_length_counters_and_status_register();
    test_frame_counter_4step_5step_and_irq();
    test_pulse_sweep_and_channels();
    test_dmc_reader_and_irq();
    test_smb_audio_generation();
    std::printf("nes_apu_unit_tests: all APU unit tests passed\n");
    return 0;
}
