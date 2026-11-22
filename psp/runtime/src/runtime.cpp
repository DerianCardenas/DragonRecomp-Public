#include "psp/runtime.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "dragon/save_state.h"
#include "psp/interpreter.h"

namespace dragon::psp {

namespace {
constexpr uint32_t kPspSaveVersion = 1U;
}  // namespace

PspRuntime::PspRuntime() : framebuffer_(PspGe::kScreenWidth * PspGe::kScreenHeight, 0xFF000000U) {
    cpu_.runtime = this;
}

bool PspRuntime::load_file(const std::filesystem::path& path, std::string& error) {
    if (!load_psp_image(path, image_, error))
        return false;
    init_loaded_image();
    return true;
}

bool PspRuntime::load_elf_memory(const uint8_t* data, size_t size, std::string& error) {
    image_ = LoadedPspImage{};
    if (!load_psp_elf_buffer(data, size, image_, error))
        return false;
    init_loaded_image();
    return true;
}

void PspRuntime::init_loaded_image() {
    cpu_ = PspCpuState{};
    cpu_.runtime = this;
    bus_.load_image(image_);
    ge_.reset();
    audio_.reset();
    hle_.reset();
    hle_.register_imports(image_);

    // Allocate 256 KB stack near top of user RAM
    const uint32_t stack_top = 0x09FFF000U;
    hle_.init_main_thread(cpu_, image_.entry_point, stack_top, image_.gp_value);
    frames_executed_ = 0;
    recomp_calls_ = 0;
    interp_fallbacks_ = 0;
    std::fill(framebuffer_.begin(), framebuffer_.end(), 0xFF000000U);
}

void PspRuntime::reset() {
    if (!image_.segments.empty()) {
        init_loaded_image();
    } else {
        cpu_ = PspCpuState{};
        cpu_.runtime = this;
        bus_.reset();
        ge_.reset();
        audio_.reset();
        hle_.reset();
    }
}

void PspRuntime::register_recomp_functions(const PspRecompFuncEntry* table, size_t count) {
    recomp_map_.clear();
    if (!table)
        return;
    for (size_t i = 0; i < count; ++i) {
        if (table[i].fn)
            recomp_map_[table[i].addr] = table[i].fn;
    }
}

PspRecompFn PspRuntime::lookup_recomp(uint32_t addr) const noexcept {
    const auto it = recomp_map_.find(addr);
    return (it != recomp_map_.end()) ? it->second : nullptr;
}

void PspRuntime::run_frame() {
    cpu_.vblank_yield = 0U;

    if (!cpu_.halted) {
        if (recomp_mode_ && !recomp_map_.empty()) {
            if (auto fn = lookup_recomp(cpu_.pc)) {
                ++recomp_calls_;
                fn(&cpu_);
            } else {
                PspInterpreter::run_slice(*this, cpu_, 250000ULL);
            }
        } else {
            PspInterpreter::run_slice(*this, cpu_, 250000ULL);
        }
    }

    hle_.on_vblank(cpu_);
    ge_.present_frame(bus_, framebuffer_.data());
    ++frames_executed_;
}

bool PspRuntime::save_state(const std::filesystem::path& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;

    dragon::SaveStateHeader hdr{};
    std::memcpy(hdr.magic, "DRST", 4);
    std::memcpy(hdr.system, "PSP ", 4);
    hdr.version = kPspSaveVersion;
    hdr.payload_size = static_cast<uint32_t>(sizeof(PspCpuState) + PspBus::kScratchpadSize +
                                             PspBus::kVramSize + PspBus::kMainRamSize);

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&cpu_), sizeof(cpu_));
    f.write(reinterpret_cast<const char*>(bus_.scratchpad()), PspBus::kScratchpadSize);
    f.write(reinterpret_cast<const char*>(bus_.vram()), PspBus::kVramSize);
    f.write(reinterpret_cast<const char*>(bus_.main_ram()), PspBus::kMainRamSize);
    return f.good();
}

bool PspRuntime::load_state(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    dragon::SaveStateHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f || !hdr.is_valid("PSP ", kPspSaveVersion)) {
        return false;
    }

    PspCpuState loaded_cpu{};
    f.read(reinterpret_cast<char*>(&loaded_cpu), sizeof(loaded_cpu));
    f.read(reinterpret_cast<char*>(bus_.scratchpad()), PspBus::kScratchpadSize);
    f.read(reinterpret_cast<char*>(bus_.vram()), PspBus::kVramSize);
    f.read(reinterpret_cast<char*>(bus_.main_ram()), PspBus::kMainRamSize);
    if (!f)
        return false;

    loaded_cpu.runtime = this;
    cpu_ = loaded_cpu;
    return true;
}

bool PspRuntime::dump_ppm(const std::filesystem::path& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << "P6\n" << PspGe::kScreenWidth << " " << PspGe::kScreenHeight << "\n255\n";
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

}  // namespace dragon::psp
