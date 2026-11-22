#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "ps1/bios_hle.h"
#include "ps1/bus.h"
#include "ps1/cpu_state.h"
#include "ps1/loader.h"

namespace dragon::ps1 {

class Ps1Runtime {
public:
    static constexpr uint32_t kCyclesPerFrame = 564480U;  // 33.8688 MHz / 60 Hz

    Ps1Runtime();

    bool load_file(const std::filesystem::path& path, std::string& error);
    bool load_exe_memory(const uint8_t* data, size_t size, std::string& error);
    void reset();

    void register_recomp_functions(const Ps1RecompFuncEntry* table, size_t count);
    void set_recomp_mode(bool enabled) noexcept { recomp_mode_ = enabled; }
    bool recomp_mode() const noexcept { return recomp_mode_; }

    Ps1RecompFn lookup_recomp(uint32_t addr) const noexcept;

    // Run one 60 Hz frame, step timers/SPU, fire VBlank, and composite framebuffer
    void run_frame();

    // Save / Load state (DRST format, system "PS1 ")
    bool save_state(const std::filesystem::path& path) const;
    bool load_state(const std::filesystem::path& path);

    bool dump_ppm(const std::filesystem::path& path) const;

    Ps1CpuState& cpu() noexcept { return cpu_; }
    const Ps1CpuState& cpu() const noexcept { return cpu_; }
    Ps1Bus& bus() noexcept { return bus_; }
    const Ps1Bus& bus() const noexcept { return bus_; }
    Ps1BiosHle& bios() noexcept { return bios_; }
    const Ps1BiosHle& bios() const noexcept { return bios_; }
    const LoadedPs1Image& image() const noexcept { return image_; }

    const uint32_t* framebuffer() const noexcept { return framebuffer_.data(); }
    uint32_t fb_width() const noexcept { return fb_w_; }
    uint32_t fb_height() const noexcept { return fb_h_; }

    uint64_t frames_executed() const noexcept { return frames_executed_; }
    void note_recomp_call() noexcept { ++recomp_calls_; }
    void note_interp_fallback() noexcept { ++interp_fallbacks_; }
    uint64_t recomp_calls() const noexcept { return recomp_calls_; }
    uint64_t interp_fallbacks() const noexcept { return interp_fallbacks_; }

private:
    void init_loaded_image();

    Ps1CpuState cpu_{};
    Ps1Bus bus_;
    Ps1BiosHle bios_;
    LoadedPs1Image image_;

    std::unordered_map<uint32_t, Ps1RecompFn> recomp_map_;
    bool recomp_mode_ = false;
    uint64_t frames_executed_ = 0;
    uint64_t recomp_calls_ = 0;
    uint64_t interp_fallbacks_ = 0;

    std::vector<uint32_t> framebuffer_;
    uint32_t fb_w_ = 320U;
    uint32_t fb_h_ = 240U;
};

}  // namespace dragon::ps1
