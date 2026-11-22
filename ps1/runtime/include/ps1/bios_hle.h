#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ps1/cpu_state.h"

namespace dragon::ps1 {

class Ps1Runtime;

class Ps1BiosHle {
public:
    Ps1BiosHle();
    void reset();

    // Handle BIOS vector calls at 0x800000A0 (A), 0x800000B0 (B), 0x800000C0 (C)
    void handle_vector(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t vector_addr);

    // Handle R3000A SYSCALL instruction ($a0 = function: 1=EnterCrit, 2=ExitCrit, etc.)
    void handle_syscall(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t code);

    // Notify VBlank to update PAD buffer & deliver VBlank events
    void on_vblank(Ps1Runtime& rt);

    uint64_t calls_handled() const noexcept { return calls_handled_; }
    const std::string& console_output() const noexcept { return console_output_; }

private:
    struct BiosEvent {
        uint32_t handle = 0;
        uint32_t cls = 0;
        uint32_t spec = 0;
        uint32_t mode = 0;
        uint32_t handler_pc = 0;
        bool open = false;
        bool enabled = false;
        bool delivered = false;
    };

    struct BiosFile {
        int fd = 0;
        bool open = false;
        uint32_t iso_lba = 0;
        uint32_t size = 0;
        uint32_t cursor = 0;
    };

    void handle_a_table(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t fn);
    void handle_b_table(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t fn);
    void handle_c_table(Ps1Runtime& rt, Ps1CpuState& cpu, uint32_t fn);

    std::vector<BiosEvent> events_;
    std::vector<BiosFile> files_;
    uint32_t pad_buf1_addr_ = 0;
    uint32_t pad_buf2_addr_ = 0;
    bool pad_started_ = false;
    uint64_t calls_handled_ = 0;
    std::string console_output_;
};

}  // namespace dragon::ps1
