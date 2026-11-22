#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "psp/cpu_state.h"

namespace dragon::psp {

class PspRuntime;
struct LoadedPspImage;

// PSP controller button bitmask (matches sceCtrlData::Buttons)
enum PspCtrlButton : uint32_t {
    kPspCtrlSelect = 0x000001U,
    kPspCtrlStart = 0x000008U,
    kPspCtrlUp = 0x000010U,
    kPspCtrlRight = 0x000020U,
    kPspCtrlDown = 0x000040U,
    kPspCtrlLeft = 0x000080U,
    kPspCtrlLTrigger = 0x000100U,
    kPspCtrlRTrigger = 0x000200U,
    kPspCtrlTriangle = 0x001000U,
    kPspCtrlCircle = 0x002000U,
    kPspCtrlCross = 0x004000U,
    kPspCtrlSquare = 0x008000U,
    kPspCtrlHome = 0x010000U,
};

struct PspInputState {
    uint32_t buttons = 0;
    uint8_t lx = 128;  // Analog X: 0..255 (128 = center)
    uint8_t ly = 128;  // Analog Y: 0..255 (128 = center)
};

class PspHle {
public:
    static constexpr uint32_t kSyscallBase = 0x2000U;

    PspHle();
    void reset();
    void register_imports(const LoadedPspImage& img);
    void set_save_dir(const std::filesystem::path& dir) { save_dir_ = dir; }

    // Set current pad input (called each frame by frontend)
    void set_input(const PspInputState& input) noexcept;
    const PspInputState& input() const noexcept { return input_; }

    // Main syscall entry point (from interpreter or recompiled code)
    void handle_syscall(PspRuntime& rt, PspCpuState& cpu, uint32_t code);

    // Direct NID dispatch (also handy for unit tests)
    bool dispatch_nid(PspRuntime& rt, PspCpuState& cpu, uint32_t nid);

    // Cooperative thread scheduler hooks
    void init_main_thread(PspCpuState& cpu, uint32_t entry_pc, uint32_t sp, uint32_t gp);
    bool schedule_next_thread(PspCpuState& cpu);
    void on_vblank(PspCpuState& cpu);

    uint32_t vblank_count() const noexcept { return vblank_count_; }
    uint64_t syscalls_handled() const noexcept { return syscalls_handled_; }
    bool exit_requested() const noexcept { return exit_requested_; }

    // Resolve a known PSP user-mode NID to its canonical name (or nullptr).
    static const char* nid_name(uint32_t nid) noexcept;

private:
    enum class ThreadState : uint8_t {
        Dormant = 0,
        Ready,
        Running,
        Waiting,
        Dead,
    };

    struct HleThread {
        int id = 0;
        std::string name;
        ThreadState state = ThreadState::Dormant;
        int priority = 32;
        uint32_t entry_pc = 0;
        uint32_t stack_addr = 0;
        uint32_t stack_size = 0;
        uint64_t wake_time_us = 0;
        int wait_sema = 0;
        int wait_evflag = 0;
        uint32_t wait_evflag_bits = 0;
        uint32_t wait_evflag_mode = 0;
        uint32_t wait_evflag_out_addr = 0;
        PspCpuState saved_ctx{};
    };

    struct HleMemBlock {
        int uid = 0;
        std::string name;
        uint32_t addr = 0;
        uint32_t size = 0;
        bool active = false;
    };

    struct HleSema {
        int uid = 0;
        std::string name;
        int count = 0;
        int max_count = 1;
        bool active = false;
    };

    struct HleEventFlag {
        int uid = 0;
        std::string name;
        uint32_t bits = 0;
        bool active = false;
    };

    struct HleMutex {
        int uid = 0;
        std::string name;
        int count = 0;
        int owner_tid = 0;
        bool active = false;
    };

    struct HleFile {
        int fd = 0;
        bool open = false;
        bool is_disc = false;
        uint64_t disc_offset = 0;
        uint64_t disc_size = 0;
        uint64_t cursor = 0;
        std::filesystem::path host_path;
        std::fstream host_stream;
    };

    struct HleDir {
        int dd = 0;
        bool open = false;
        std::vector<std::pair<std::string, uint32_t>> entries;  // name, size
        size_t index = 0;
    };

    void save_active_thread(const PspCpuState& cpu);
    void restore_active_thread(PspCpuState& cpu);
    void wake_waiting_threads(PspRuntime& rt);
    uint64_t current_time_us(const PspCpuState& cpu) const noexcept;
    std::filesystem::path resolve_ms0_path(const std::string& guest_path) const;

    std::unordered_map<uint32_t, uint32_t> syscall_to_nid_;
    std::unordered_map<uint32_t, std::string> syscall_to_module_;

    std::vector<HleThread> threads_;
    int current_thread_idx_ = -1;
    int next_uid_ = 0x100;

    std::vector<HleMemBlock> mem_blocks_;
    std::vector<HleSema> semas_;
    std::vector<HleEventFlag> evflags_;
    std::vector<HleMutex> mutexes_;
    std::vector<std::unique_ptr<HleFile>> files_;
    std::vector<HleDir> dirs_;
    int next_fd_ = 3;  // 0=stdin, 1=stdout, 2=stderr

    PspInputState input_{};
    uint32_t prev_buttons_ = 0;
    uint32_t sampling_cycle_ = 0;
    uint32_t sampling_mode_ = 1;
    uint32_t display_mode_ = 0;
    uint32_t vblank_count_ = 0;
    uint64_t syscalls_handled_ = 0;
    uint32_t cpu_freq_mhz_ = 222U;
    uint32_t bus_freq_mhz_ = 111U;
    bool exit_requested_ = false;
    std::string cwd_ = "ms0:/";
    std::filesystem::path save_dir_;
};

}  // namespace dragon::psp
