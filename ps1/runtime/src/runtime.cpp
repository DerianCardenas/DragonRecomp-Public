#include "ps1/runtime.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "dragon/save_state.h"
#include "ps1/interpreter.h"

namespace dragon::ps1 {

namespace {
constexpr uint32_t kPs1SaveVersion = 1U;
}

Ps1Runtime::Ps1Runtime() : framebuffer_(320U * 240U, 0xFF000000U) {
    cpu_.runtime = this;
}

bool Ps1Runtime::load_file(const std::filesystem::path& path, std::string& error) {
    if (!load_ps1_image(path, image_, error))
        return false;
    init_loaded_image();
    return true;
}

bool Ps1Runtime::load_exe_memory(const uint8_t* data, size_t size, std::string& error) {
    image_ = LoadedPs1Image{};
    if (!load_ps1_exe_buffer(data, size, image_, error))
        return false;
    init_loaded_image();
    return true;
}

void Ps1Runtime::init_loaded_image() {
    cpu_ = Ps1CpuState{};
    cpu_.runtime = this;
    bus_.load_image(image_);
    bios_.reset();

    cpu_.pc = image_.initial_pc;
    cpu_.npc = image_.initial_pc + 4U;
    cpu_.gpr[28] = image_.initial_gp;
    cpu_.gpr[29] = image_.sp_base + image_.sp_offset;
    cpu_.gpr[30] = cpu_.gpr[29];
    cpu_.gpr[31] = 0xBFC00000U;
    cpu_.cop0[12] = 0x10900000U;  // COP0 + COP2 (GTE) usable
    cpu_.cop0[15] = 0x00000002U;  // R3000A PRID

    frames_executed_ = 0;
    recomp_calls_ = 0;
    interp_fallbacks_ = 0;
    std::fill(framebuffer_.begin(), framebuffer_.end(), 0xFF000000U);
}

void Ps1Runtime::reset() {
    if (!image_.exe_body.empty()) {
        init_loaded_image();
    } else {
        cpu_ = Ps1CpuState{};
        cpu_.runtime = this;
        cpu_.cop0[12] = 0x10900000U;
        cpu_.cop0[15] = 0x00000002U;
        bus_.reset();
        bios_.reset();
    }
}

void Ps1Runtime::register_recomp_functions(const Ps1RecompFuncEntry* table, size_t count) {
    recomp_map_.clear();
    if (!table)
        return;
    for (size_t i = 0; i < count; ++i) {
        if (table[i].fn)
            recomp_map_[table[i].addr] = table[i].fn;
    }
}

Ps1RecompFn Ps1Runtime::lookup_recomp(uint32_t addr) const noexcept {
    const auto it = recomp_map_.find(addr);
    return (it != recomp_map_.end()) ? it->second : nullptr;
}

void Ps1Runtime::run_frame() {
    cpu_.vblank_yield = 0U;

    if (!cpu_.halted) {
        if (recomp_mode_ && !recomp_map_.empty()) {
            if (auto fn = lookup_recomp(cpu_.pc)) {
                ++recomp_calls_;
                fn(&cpu_);
            } else {
                Ps1Interpreter::run_slice(*this, cpu_, 200000ULL);
            }
        } else {
            Ps1Interpreter::run_slice(*this, cpu_, 200000ULL);
        }
    }

    bus_.step_timers(kCyclesPerFrame);
    bus_.on_vblank();
    bios_.on_vblank(*this);
    bus_.spu().step_samples(735U);  // 44100 / 60
    bus_.gpu().present_frame(framebuffer_, fb_w_, fb_h_);
    ++frames_executed_;
}

bool Ps1Runtime::save_state(const std::filesystem::path& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;

    dragon::SaveStateHeader hdr{};
    std::memcpy(hdr.magic, "DRST", 4);
    std::memcpy(hdr.system, "PS1 ", 4);
    hdr.version = kPs1SaveVersion;
    hdr.payload_size =
        static_cast<uint32_t>(sizeof(Ps1CpuState) + Ps1Bus::kScratchpadSize + Ps1Bus::kMainRamSize +
                              Ps1Gpu::kVramPixels * 2U + Ps1Spu::kSpuRamSize);

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&cpu_), sizeof(cpu_));
    f.write(reinterpret_cast<const char*>(bus_.scratchpad()), Ps1Bus::kScratchpadSize);
    f.write(reinterpret_cast<const char*>(bus_.main_ram()), Ps1Bus::kMainRamSize);
    f.write(reinterpret_cast<const char*>(bus_.gpu().vram()), Ps1Gpu::kVramPixels * 2U);
    f.write(reinterpret_cast<const char*>(bus_.spu().ram()), Ps1Spu::kSpuRamSize);
    return f.good();
}

bool Ps1Runtime::load_state(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    dragon::SaveStateHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f || !hdr.is_valid("PS1 ", kPs1SaveVersion))
        return false;

    Ps1CpuState loaded_cpu{};
    f.read(reinterpret_cast<char*>(&loaded_cpu), sizeof(loaded_cpu));
    f.read(reinterpret_cast<char*>(bus_.scratchpad()), Ps1Bus::kScratchpadSize);
    f.read(reinterpret_cast<char*>(bus_.main_ram()), Ps1Bus::kMainRamSize);
    f.read(reinterpret_cast<char*>(bus_.gpu().vram()), Ps1Gpu::kVramPixels * 2U);
    f.read(reinterpret_cast<char*>(bus_.spu().ram()), Ps1Spu::kSpuRamSize);
    if (!f)
        return false;

    loaded_cpu.runtime = this;
    cpu_ = loaded_cpu;
    return true;
}

bool Ps1Runtime::dump_ppm(const std::filesystem::path& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << "P6\n" << fb_w_ << " " << fb_h_ << "\n255\n";
    for (uint32_t px : framebuffer_) {
        const uint8_t rgb[3] = {
            static_cast<uint8_t>(px & 0xFFU),
            static_cast<uint8_t>((px >> 8U) & 0xFFU),
            static_cast<uint8_t>((px >> 16U) & 0xFFU),
        };
        f.write(reinterpret_cast<const char*>(rgb), 3);
    }
    return f.good();
}

}  // namespace dragon::ps1
