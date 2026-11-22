#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "psp/audio.h"
#include "psp/bus.h"
#include "psp/cpu_state.h"
#include "psp/ge.h"
#include "psp/hle.h"
#include "psp/loader.h"

namespace dragon::psp {

class PspRuntime {
public:
    static constexpr uint32_t kCyclesPerFrame = 3700000U;  // ~222 MHz / 60 Hz

    PspRuntime();

    bool load_file(const std::filesystem::path& path, std::string& error);
    bool load_elf_memory(const uint8_t* data, size_t size, std::string& error);
    void reset();

    void register_recomp_functions(const PspRecompFuncEntry* table, size_t count);
    void set_recomp_mode(bool enabled) noexcept { recomp_mode_ = enabled; }
    bool recomp_mode() const noexcept { return recomp_mode_; }

    PspRecompFn lookup_recomp(uint32_t addr) const noexcept;

    // Run one 60 Hz frame and composite the 480x272 ABGR8888 framebuffer.
    void run_frame();

    // Save / Load state (DRST format)
    bool save_state(const std::filesystem::path& path) const;
    bool load_state(const std::filesystem::path& path);

    // Dump current 480x272 framebuffer as binary PPM (P6)
    bool dump_ppm(const std::filesystem::path& path) const;

    PspCpuState& cpu() noexcept { return cpu_; }
    const PspCpuState& cpu() const noexcept { return cpu_; }
    PspBus& bus() noexcept { return bus_; }
    const PspBus& bus() const noexcept { return bus_; }
    PspGe& ge() noexcept { return ge_; }
    const PspGe& ge() const noexcept { return ge_; }
    PspAudio& audio() noexcept { return audio_; }
    const PspAudio& audio() const noexcept { return audio_; }
    PspHle& hle() noexcept { return hle_; }
    const PspHle& hle() const noexcept { return hle_; }
    const LoadedPspImage& image() const noexcept { return image_; }
    const uint32_t* framebuffer() const noexcept { return framebuffer_.data(); }

    uint64_t frames_executed() const noexcept { return frames_executed_; }
    uint64_t recomp_calls() const noexcept { return recomp_calls_; }
    uint64_t interp_fallbacks() const noexcept { return interp_fallbacks_; }
    void note_recomp_call() noexcept { ++recomp_calls_; }
    void note_interp_fallback() noexcept { ++interp_fallbacks_; }

private:
    void init_loaded_image();

    PspCpuState cpu_{};
    PspBus bus_;
    PspGe ge_;
    PspAudio audio_;
    PspHle hle_;
    LoadedPspImage image_;

    std::unordered_map<uint32_t, PspRecompFn> recomp_map_;
    bool recomp_mode_ = false;
    uint64_t frames_executed_ = 0;
    uint64_t recomp_calls_ = 0;
    uint64_t interp_fallbacks_ = 0;

    std::vector<uint32_t> framebuffer_;  // 480 x 272 ABGR8888
};

}  // namespace dragon::psp
