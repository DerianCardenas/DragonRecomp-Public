#include "psp/hle.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "dragon/log.h"
#include "psp/runtime.h"

namespace dragon::psp {

namespace {

struct NidEntry {
    uint32_t nid;
    const char* name;
};

constexpr NidEntry kKnownNids[] = {
    // ThreadManForUser
    {0x446D8DE6U, "sceKernelCreateThread"},
    {0xF475845DU, "sceKernelStartThread"},
    {0xAA73C935U, "sceKernelExitThread"},
    {0x809CE29BU, "sceKernelExitDeleteThread"},
    {0x9FA03CD3U, "sceKernelDeleteThread"},
    {0x9ACE131EU, "sceKernelSleepThread"},
    {0x82826F70U, "sceKernelSleepThreadCB"},
    {0xD59EAD2FU, "sceKernelWakeupThread"},
    {0xCEADEB47U, "sceKernelDelayThread"},
    {0x68DA9E36U, "sceKernelDelayThreadCB"},
    {0x293B45B8U, "sceKernelGetThreadId"},
    {0xE81CAF8FU, "sceKernelCreateCallback"},
    {0x349D6D6CU, "sceKernelCheckCallback"},
    {0xD6DA4BA1U, "sceKernelCreateSema"},
    {0x28B6489CU, "sceKernelDeleteSema"},
    {0x3F53E640U, "sceKernelSignalSema"},
    {0x4E3A1105U, "sceKernelWaitSema"},
    {0x6D212BACU, "sceKernelWaitSemaCB"},
    {0x58B1F937U, "sceKernelPollSema"},
    {0x55C20A00U, "sceKernelCreateEventFlag"},
    {0xEF9E4C70U, "sceKernelDeleteEventFlag"},
    {0x1FB15A32U, "sceKernelSetEventFlag"},
    {0x812346E4U, "sceKernelClearEventFlag"},
    {0x402FCF22U, "sceKernelWaitEventFlag"},
    {0x328C546AU, "sceKernelWaitEventFlagCB"},
    {0x30FD48F0U, "sceKernelPollEventFlag"},
    {0x369ED59DU, "sceKernelGetSystemTimeLow"},
    {0x82BC5777U, "sceKernelGetSystemTimeWide"},
    {0xDB738F35U, "sceKernelGetSystemTime"},
    {0x110DEC9AU, "sceKernelUSec2SysClock"},
    {0xC8CD158CU, "sceKernelUSec2SysClockWide"},
    {0xBA6B92E2U, "sceKernelSysClock2USec"},
    {0x71BC9871U, "sceKernelChangeThreadPriority"},
    {0x17C1684EU, "sceKernelReferThreadStatus"},
    {0xB7D098C6U, "sceKernelCreateMutex"},
    {0xB011B11FU, "sceKernelLockMutex"},
    {0x6B30100FU, "sceKernelUnlockMutex"},
    {0xF8170FBEU, "sceKernelDeleteMutex"},
    {0x19CFF145U, "sceKernelCreateLwMutex"},
    {0xBEA46419U, "sceKernelLockLwMutex"},
    {0x15B6446BU, "sceKernelUnlockLwMutex"},
    {0x60107536U, "sceKernelDeleteLwMutex"},
    // LoadExecForUser
    {0x05572A5FU, "sceKernelExitGame"},
    {0x4AC57943U, "sceKernelRegisterExitCallback"},
    {0xBD2F1094U, "sceKernelLoadExec"},
    // SysMemUserForUser
    {0x237DBD4FU, "sceKernelAllocPartitionMemory"},
    {0xB6D61D02U, "sceKernelFreePartitionMemory"},
    {0x9D9A5BA1U, "sceKernelGetBlockHeadAddr"},
    {0xA291F107U, "sceKernelMaxFreeMemSize"},
    {0xF919F628U, "sceKernelTotalFreeMemSize"},
    {0x7591C7DBU, "sceKernelSetCompiledSdkVersion"},
    {0x3FC9AE6AU, "sceKernelDevkitVersion"},
    // sceDisplay
    {0x0E20F177U, "sceDisplaySetMode"},
    {0xDEA197D4U, "sceDisplayGetMode"},
    {0x289D82FEU, "sceDisplaySetFrameBuf"},
    {0xEEDA2E54U, "sceDisplayGetFrameBuf"},
    {0x36CDFADEU, "sceDisplayWaitVblank"},
    {0x8EB9EC49U, "sceDisplayWaitVblankCB"},
    {0x984C27E7U, "sceDisplayWaitVblankStart"},
    {0x46F186C3U, "sceDisplayWaitVblankStartCB"},
    {0x9C6EAAD7U, "sceDisplayGetVcount"},
    {0x4D4E10ECU, "sceDisplayIsVblank"},
    {0x773DD3A3U, "sceDisplayGetCurrentHcount"},
    // sceCtrl
    {0x6A2774F3U, "sceCtrlSetSamplingCycle"},
    {0x02BAAD91U, "sceCtrlGetSamplingCycle"},
    {0x1F4011E6U, "sceCtrlSetSamplingMode"},
    {0xDA6B76A1U, "sceCtrlGetSamplingMode"},
    {0x1F803938U, "sceCtrlReadBufferPositive"},
    {0x60B81F86U, "sceCtrlReadBufferNegative"},
    {0x3A622550U, "sceCtrlPeekBufferPositive"},
    {0xC152080AU, "sceCtrlPeekBufferNegative"},
    {0x0B588501U, "sceCtrlReadLatch"},
    {0xB1D0E5CDU, "sceCtrlPeekLatch"},
    // sceGe_user
    {0x1F6752ADU, "sceGeEdramGetSize"},
    {0xE47E40E4U, "sceGeEdramGetAddr"},
    {0x67B01D8EU, "sceGeGetList"},
    {0xAB49E76AU, "sceGeListEnQueue"},
    {0x1C0D95A6U, "sceGeListEnQueueHead"},
    {0x5FB86AB0U, "sceGeListDeQueue"},
    {0xE0D68148U, "sceGeListUpdateStallAddr"},
    {0x03444EB4U, "sceGeListSync"},
    {0xB287BD61U, "sceGeDrawSync"},
    {0xA4FC06A4U, "sceGeSetCallback"},
    {0x05DB22CEU, "sceGeUnsetCallback"},
    {0x438A385AU, "sceGeSaveContext"},
    {0x0BF608FBU, "sceGeRestoreContext"},
    // sceAudio
    {0x5EC81C55U, "sceAudioChReserve"},
    {0x6FC46853U, "sceAudioChRelease"},
    {0x8C1009B2U, "sceAudioOutput"},
    {0x136CAF51U, "sceAudioOutputBlocking"},
    {0xE2D56B2DU, "sceAudioOutputPanned"},
    {0x13F592BCU, "sceAudioOutputPannedBlocking"},
    {0xCB2E439EU, "sceAudioSetChannelDataLen"},
    {0x95FD0C2DU, "sceAudioChangeChannelConfig"},
    {0xB7E1D8E7U, "sceAudioChangeChannelVolume"},
    {0xE9D97901U, "sceAudioGetChannelRestLen"},
    {0x38553111U, "sceAudioSRCChReserve"},
    {0x5C37C0AEU, "sceAudioSRCChRelease"},
    {0xE0727056U, "sceAudioSRCOutputBlocking"},
    // IoFileMgrForUser
    {0x109F50BCU, "sceIoOpen"},
    {0x810C4BC3U, "sceIoClose"},
    {0x6A638D83U, "sceIoRead"},
    {0x42EC03ACU, "sceIoWrite"},
    {0x27EB27B8U, "sceIoLseek"},
    {0x68963324U, "sceIoLseek32"},
    {0xACE946E8U, "sceIoGetstat"},
    {0x55F4717DU, "sceIoChdir"},
    {0xB29DDF9CU, "sceIoDopen"},
    {0xE3EB004CU, "sceIoDread"},
    {0xEB092469U, "sceIoDclose"},
    {0x06A70004U, "sceIoMkdir"},
    {0xF27A9C51U, "sceIoRemove"},
    {0x1117C65FU, "sceIoRmdir"},
    {0x54F5FB11U, "sceIoDevctl"},
    // UtilsForUser
    {0x79D1C3FAU, "sceKernelDcacheWritebackAll"},
    {0x3EE30821U, "sceKernelDcacheWritebackRange"},
    {0xB435DEC5U, "sceKernelDcacheWritebackInvalidateAll"},
    {0x34B9FA9EU, "sceKernelDcacheWritebackInvalidateRange"},
    {0xBFA98062U, "sceKernelDcacheInvalidateRange"},
    {0x920F104AU, "sceKernelIcacheInvalidateAll"},
    {0xC2DF770EU, "sceKernelIcacheInvalidateRange"},
    {0x27CC57F0U, "sceKernelLibcTime"},
    {0x91E4F6A7U, "sceKernelLibcClock"},
    {0x71EC4271U, "sceKernelLibcGettimeofday"},
    // sceRtc
    {0x3F7AD767U, "sceRtcGetCurrentTick"},
    {0xC41C2853U, "sceRtcGetTickResolution"},
    {0x4CFA57B0U, "sceRtcGetCurrentClock"},
    {0xE7C27D1BU, "sceRtcGetCurrentClockLocalTime"},
    {0x7ED29E40U, "sceRtcSetTick"},
    {0x6FF40ACCU, "sceRtcGetTick"},
    // scePower
    {0x2085D15DU, "scePowerGetBatteryLifePercent"},
    {0x0AFD0D8BU, "scePowerIsBatteryExist"},
    {0x87440F5EU, "scePowerIsPowerOnline"},
    {0x737486F2U, "scePowerSetClockFrequency"},
    {0xFEE03A2FU, "scePowerGetCpuClockFrequency"},
    {0x478FE6F5U, "scePowerGetBusClockFrequency"},
    {0x04B7766EU, "scePowerRegisterCallback"},
    {0xEFD3C963U, "scePowerTick"},
    // sceUmdUser
    {0x46EBB729U, "sceUmdCheckMedium"},
    {0xC6183D47U, "sceUmdActivate"},
    {0xE83742BAU, "sceUmdDeactivate"},
    {0x8EF08FCEU, "sceUmdWaitDriveStat"},
    {0x56202973U, "sceUmdWaitDriveStatWithTimer"},
    {0x4A9E5E29U, "sceUmdWaitDriveStatCB"},
    {0x6B4A146CU, "sceUmdGetDriveStat"},
    {0xAEE7404DU, "sceUmdRegisterUMDCallBack"},
    {0xBD2BDE07U, "sceUmdUnRegisterUMDCallBack"},
    {0x20628E6FU, "sceUmdGetErrorStat"},
    // sceUtility
    {0xA5DA2406U, "sceUtilityGetSystemParamInt"},
    {0x34B78343U, "sceUtilityGetSystemParamString"},
    {0x2A2B3DE0U, "sceUtilityLoadModule"},
    {0xE49BFE92U, "sceUtilityUnloadModule"},
    {0xC629AF26U, "sceUtilityLoadAvModule"},
    // ModuleMgrForUser
    {0x977DE386U, "sceKernelLoadModule"},
    {0x50F0C1ECU, "sceKernelStartModule"},
    {0xD1FF982AU, "sceKernelStopModule"},
    {0x2E0911AAU, "sceKernelUnloadModule"},
    {0xCC1D3699U, "sceKernelStopUnloadSelfModule"},
    {0xD675EBB8U, "sceKernelSelfStopUnloadModule"},
    // InterruptManager
    {0xCA04A2B9U, "sceKernelRegisterSubIntrHandler"},
    {0xD61E6961U, "sceKernelReleaseSubIntrHandler"},
    {0xFB8E22ECU, "sceKernelEnableSubIntr"},
    {0x8A389411U, "sceKernelDisableSubIntr"},
    // StdioForUser
    {0x172D316EU, "sceKernelStdin"},
    {0xA6BAB2E9U, "sceKernelStdout"},
    {0xF78BA90AU, "sceKernelStderr"},
};

constexpr uint32_t kPspErrnoNotFound = 0x80010002U;
constexpr uint32_t kPspErrnoBadf = 0x80010009U;

}  // namespace

PspHle::PspHle() {
    reset();
}

const char* PspHle::nid_name(uint32_t nid) noexcept {
    for (const auto& e : kKnownNids) {
        if (e.nid == nid)
            return e.name;
    }
    return nullptr;
}

void PspHle::reset() {
    syscall_to_nid_.clear();
    syscall_to_module_.clear();
    threads_.clear();
    current_thread_idx_ = -1;
    next_uid_ = 0x100;
    mem_blocks_.clear();
    semas_.clear();
    evflags_.clear();
    mutexes_.clear();
    files_.clear();
    dirs_.clear();
    next_fd_ = 3;
    input_ = PspInputState{};
    prev_buttons_ = 0;
    sampling_cycle_ = 0;
    sampling_mode_ = 1;
    display_mode_ = 0;
    vblank_count_ = 0;
    syscalls_handled_ = 0;
    cpu_freq_mhz_ = 222U;
    bus_freq_mhz_ = 111U;
    exit_requested_ = false;
    cwd_ = "ms0:/";
}

void PspHle::register_imports(const LoadedPspImage& img) {
    syscall_to_nid_.clear();
    syscall_to_module_.clear();
    for (const auto& imp : img.imports) {
        syscall_to_nid_[imp.syscall_code] = imp.nid;
        syscall_to_module_[imp.syscall_code] = imp.module_name;
    }
}

void PspHle::set_input(const PspInputState& input) noexcept {
    prev_buttons_ = input_.buttons;
    input_ = input;
}

void PspHle::init_main_thread(PspCpuState& cpu, uint32_t entry_pc, uint32_t sp, uint32_t gp) {
    threads_.clear();
    HleThread main_t{};
    main_t.id = next_uid_++;
    main_t.name = "user_main";
    main_t.state = ThreadState::Running;
    main_t.priority = 32;
    main_t.entry_pc = entry_pc;
    main_t.stack_addr = sp;
    main_t.stack_size = 256U * 1024U;
    threads_.push_back(main_t);
    current_thread_idx_ = 0;

    cpu.pc = entry_pc;
    cpu.npc = entry_pc + 4U;
    cpu.gpr[28] = gp;
    cpu.gpr[29] = sp;
    cpu.gpr[31] = 0x08800000U;  // sentinel return
}

void PspHle::save_active_thread(const PspCpuState& cpu) {
    if (current_thread_idx_ >= 0 && static_cast<size_t>(current_thread_idx_) < threads_.size()) {
        threads_[static_cast<size_t>(current_thread_idx_)].saved_ctx = cpu;
    }
}

void PspHle::restore_active_thread(PspCpuState& cpu) {
    if (current_thread_idx_ >= 0 && static_cast<size_t>(current_thread_idx_) < threads_.size()) {
        void* rt = cpu.runtime;
        const uint64_t cycles = cpu.cycles;
        cpu = threads_[static_cast<size_t>(current_thread_idx_)].saved_ctx;
        cpu.runtime = rt;
        cpu.cycles = cycles;
    }
}

void PspHle::wake_waiting_threads(PspRuntime& rt) {
    const uint64_t now = current_time_us(rt.cpu());
    for (auto& t : threads_) {
        if (t.state != ThreadState::Waiting)
            continue;
        if (t.wake_time_us != 0 && now >= t.wake_time_us) {
            t.wake_time_us = 0;
            t.state = ThreadState::Ready;
        }
    }
}

bool PspHle::schedule_next_thread(PspCpuState& cpu) {
    if (threads_.empty())
        return false;
    save_active_thread(cpu);

    int best_idx = -1;
    int best_prio = 999;
    const size_t n = threads_.size();
    for (size_t offset = 1; offset <= n; ++offset) {
        const size_t idx = (static_cast<size_t>(std::max(0, current_thread_idx_)) + offset) % n;
        const auto& t = threads_[idx];
        if (t.state == ThreadState::Ready || t.state == ThreadState::Running) {
            if (t.priority < best_prio) {
                best_prio = t.priority;
                best_idx = static_cast<int>(idx);
            }
        }
    }

    if (best_idx < 0)
        return false;
    if (best_idx != current_thread_idx_) {
        if (current_thread_idx_ >= 0 &&
            threads_[static_cast<size_t>(current_thread_idx_)].state == ThreadState::Running) {
            threads_[static_cast<size_t>(current_thread_idx_)].state = ThreadState::Ready;
        }
        current_thread_idx_ = best_idx;
        threads_[static_cast<size_t>(best_idx)].state = ThreadState::Running;
        restore_active_thread(cpu);
    }
    return true;
}

void PspHle::on_vblank(PspCpuState& cpu) {
    ++vblank_count_;
    const uint64_t now = current_time_us(cpu);
    for (auto& t : threads_) {
        if (t.state == ThreadState::Waiting && t.wake_time_us != 0 && now >= t.wake_time_us) {
            t.wake_time_us = 0;
            t.state = ThreadState::Ready;
        }
    }
}

uint64_t PspHle::current_time_us(const PspCpuState& cpu) const noexcept {
    // Convert Allegrex cycles (~222 MHz) into monotonic microseconds
    return (cpu.cycles / std::max<uint32_t>(1U, cpu_freq_mhz_)) +
           static_cast<uint64_t>(vblank_count_) * 16667ULL;
}

std::filesystem::path PspHle::resolve_ms0_path(const std::string& guest_path) const {
    std::string rel = guest_path;
    if (rel.rfind("ms0:/", 0) == 0)
        rel = rel.substr(5);
    else if (rel.rfind("ms0:", 0) == 0)
        rel = rel.substr(4);
    else if (rel.rfind("fatms0:/", 0) == 0)
        rel = rel.substr(8);
    while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
        rel.erase(rel.begin());
    // Prevent directory traversal outside save_dir_
    std::filesystem::path base =
        save_dir_.empty() ? std::filesystem::temp_directory_path() / "dragon_psp_ms0" : save_dir_;
    std::filesystem::path clean;
    for (const auto& part : std::filesystem::path(rel)) {
        if (part == ".." || part == ".")
            continue;
        clean /= part;
    }
    return base / clean;
}

void PspHle::handle_syscall(PspRuntime& rt, PspCpuState& cpu, uint32_t code) {
    ++syscalls_handled_;
    const auto it = syscall_to_nid_.find(code);
    if (it == syscall_to_nid_.end()) {
        // Fallback: allow direct syscall index for synthetic tests or unmapped NIDs
        cpu.gpr[2] = 0U;
        return;
    }
    if (!dispatch_nid(rt, cpu, it->second)) {
        // Unimplemented HLE function: return 0 cleanly so games don't crash on non-critical calls
        static std::unordered_map<uint32_t, bool> logged_once;
        if (!logged_once[it->second]) {
            logged_once[it->second] = true;
            const auto mod_it = syscall_to_module_.find(code);
            char msg[160];
            std::snprintf(msg, sizeof(msg), "PSP HLE unimplemented NID 0x%08X (%s)", it->second,
                          mod_it != syscall_to_module_.end() ? mod_it->second.c_str() : "?");
            dragon::log_warn(msg);
        }
        cpu.gpr[2] = 0U;
    }
}

bool PspHle::dispatch_nid(PspRuntime& rt, PspCpuState& cpu, uint32_t nid) {
    PspBus& bus = rt.bus();
    const uint32_t a0 = cpu.gpr[4];
    const uint32_t a1 = cpu.gpr[5];
    const uint32_t a2 = cpu.gpr[6];
    const uint32_t a3 = cpu.gpr[7];
    const uint32_t t0 = cpu.gpr[8];
    uint32_t& v0 = cpu.gpr[2];
    uint32_t& v1 = cpu.gpr[3];

    switch (nid) {
        // ================= sceDisplay =================
        case 0x0E20F177U:  // sceDisplaySetMode(mode, width, height)
            display_mode_ = a0;
            v0 = 0U;
            return true;
        case 0xDEA197D4U:  // sceDisplayGetMode
            if (a0)
                bus.write32(a0, display_mode_);
            if (a1)
                bus.write32(a1, PspGe::kScreenWidth);
            if (a2)
                bus.write32(a2, PspGe::kScreenHeight);
            v0 = 0U;
            return true;
        case 0x289D82FEU:  // sceDisplaySetFrameBuf(topaddr, bufferwidth, pixelformat, sync)
            rt.ge().set_display_framebuf(a0, a1, a2);
            v0 = 0U;
            return true;
        case 0xEEDA2E54U:  // sceDisplayGetFrameBuf
            if (a0)
                bus.write32(a0, rt.ge().display_top_addr());
            if (a1)
                bus.write32(a1, rt.ge().display_buf_width());
            if (a2)
                bus.write32(a2, rt.ge().display_pixel_format());
            v0 = 0U;
            return true;
        case 0x36CDFADEU:  // sceDisplayWaitVblank
        case 0x8EB9EC49U:  // sceDisplayWaitVblankCB
        case 0x984C27E7U:  // sceDisplayWaitVblankStart
        case 0x46F186C3U:  // sceDisplayWaitVblankStartCB
            cpu.vblank_yield = 1U;
            v0 = 0U;
            return true;
        case 0x9C6EAAD7U:  // sceDisplayGetVcount
            v0 = vblank_count_;
            return true;
        case 0x4D4E10ECU:  // sceDisplayIsVblank
            v0 = 0U;
            return true;
        case 0x773DD3A3U:  // sceDisplayGetCurrentHcount
            v0 = static_cast<uint32_t>((cpu.cycles / 128U) % 286U);
            return true;

        // ================= sceCtrl =================
        case 0x6A2774F3U:  // sceCtrlSetSamplingCycle
            v0 = sampling_cycle_;
            sampling_cycle_ = a0;
            return true;
        case 0x02BAAD91U:  // sceCtrlGetSamplingCycle
            if (a0)
                bus.write32(a0, sampling_cycle_);
            v0 = 0U;
            return true;
        case 0x1F4011E6U:  // sceCtrlSetSamplingMode
            v0 = sampling_mode_;
            sampling_mode_ = a0;
            return true;
        case 0xDA6B76A1U:  // sceCtrlGetSamplingMode
            if (a0)
                bus.write32(a0, sampling_mode_);
            v0 = 0U;
            return true;
        case 0x1F803938U:    // sceCtrlReadBufferPositive(pad_data, count)
        case 0x3A622550U:    // sceCtrlPeekBufferPositive
        case 0x60B81F86U:    // sceCtrlReadBufferNegative
        case 0xC152080AU: {  // sceCtrlPeekBufferNegative
            const bool negative = (nid == 0x60B81F86U || nid == 0xC152080AU);
            const uint32_t count = std::clamp(a1, 1U, 64U);
            const uint32_t ts = static_cast<uint32_t>(current_time_us(cpu));
            const uint32_t btns = negative ? ~input_.buttons : input_.buttons;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t p = a0 + i * 16U;
                bus.write32(p + 0U, ts);
                bus.write32(p + 4U, btns);
                bus.write8(p + 8U, (sampling_mode_ == 1U) ? input_.lx : 128U);
                bus.write8(p + 9U, (sampling_mode_ == 1U) ? input_.ly : 128U);
                bus.write16(p + 10U, 0U);
                bus.write32(p + 12U, 0U);
            }
            v0 = count;
            return true;
        }
        case 0x0B588501U:    // sceCtrlReadLatch
        case 0xB1D0E5CDU: {  // sceCtrlPeekLatch
            if (a0) {
                const uint32_t cur = input_.buttons;
                const uint32_t make = cur & ~prev_buttons_;
                const uint32_t brk = ~cur & prev_buttons_;
                bus.write32(a0 + 0U, make);
                bus.write32(a0 + 4U, brk);
                bus.write32(a0 + 8U, cur);
                bus.write32(a0 + 12U, ~cur);
            }
            v0 = 1U;
            return true;
        }

        // ================= sceGe_user =================
        case 0x1F6752ADU:  // sceGeEdramGetSize
            v0 = PspBus::kVramSize;
            return true;
        case 0xE47E40E4U:  // sceGeEdramGetAddr
            v0 = PspBus::kVramBase;
            return true;
        case 0xAB49E76AU:  // sceGeListEnQueue(list, stall, cbid, arg)
        case 0x1C0D95A6U:  // sceGeListEnQueueHead
            v0 = static_cast<uint32_t>(rt.ge().enqueue_list(bus, a0, a1, static_cast<int>(a2)));
            return true;
        case 0xE0D68148U:  // sceGeListUpdateStallAddr(qid, stall)
            v0 = static_cast<uint32_t>(rt.ge().update_stall_addr(bus, static_cast<int>(a0), a1));
            return true;
        case 0x03444EB4U:  // sceGeListSync
            v0 = static_cast<uint32_t>(rt.ge().list_sync(static_cast<int>(a0)));
            return true;
        case 0xB287BD61U:  // sceGeDrawSync
            v0 = static_cast<uint32_t>(rt.ge().draw_sync());
            return true;
        case 0x5FB86AB0U:  // sceGeListDeQueue
        case 0xA4FC06A4U:  // sceGeSetCallback
        case 0x05DB22CEU:  // sceGeUnsetCallback
        case 0x438A385AU:  // sceGeSaveContext
        case 0x0BF608FBU:  // sceGeRestoreContext
            v0 = 1U;
            return true;

        // ================= sceAudio =================
        case 0x5EC81C55U:  // sceAudioChReserve(channel, samplecount, format)
            v0 = static_cast<uint32_t>(rt.audio().reserve_channel(
                static_cast<int32_t>(a0), static_cast<int32_t>(a1), static_cast<int32_t>(a2)));
            return true;
        case 0x6FC46853U:  // sceAudioChRelease(channel)
            v0 = static_cast<uint32_t>(rt.audio().release_channel(static_cast<int32_t>(a0)));
            return true;
        case 0x8C1009B2U:    // sceAudioOutput(channel, vol, buf)
        case 0x136CAF51U: {  // sceAudioOutputBlocking(channel, vol, buf)
            const uint8_t* p = bus.ptr(a2);
            v0 = p ? static_cast<uint32_t>(rt.audio().output_samples(
                         static_cast<int32_t>(a0), static_cast<int32_t>(a1),
                         static_cast<int32_t>(a1), reinterpret_cast<const int16_t*>(p), 0))
                   : 0U;
            return true;
        }
        case 0xE2D56B2DU:    // sceAudioOutputPanned(ch, leftvol, rightvol, buf)
        case 0x13F592BCU: {  // sceAudioOutputPannedBlocking
            const uint8_t* p = bus.ptr(a3);
            v0 = p ? static_cast<uint32_t>(rt.audio().output_samples(
                         static_cast<int32_t>(a0), static_cast<int32_t>(a1),
                         static_cast<int32_t>(a2), reinterpret_cast<const int16_t*>(p), 0))
                   : 0U;
            return true;
        }
        case 0xCB2E439EU:  // sceAudioSetChannelDataLen
            v0 = static_cast<uint32_t>(rt.audio().set_channel_sample_count(
                static_cast<int32_t>(a0), static_cast<int32_t>(a1)));
            return true;
        case 0x95FD0C2DU:  // sceAudioChangeChannelConfig
            v0 = static_cast<uint32_t>(
                rt.audio().set_channel_format(static_cast<int32_t>(a0), static_cast<int32_t>(a1)));
            return true;
        case 0xB7E1D8E7U:  // sceAudioChangeChannelVolume
            v0 = static_cast<uint32_t>(rt.audio().set_channel_volume(
                static_cast<int32_t>(a0), static_cast<int32_t>(a1), static_cast<int32_t>(a2)));
            return true;
        case 0xE9D97901U:  // sceAudioGetChannelRestLen
            v0 = static_cast<uint32_t>(rt.audio().get_channel_rest_len(static_cast<int32_t>(a0)));
            return true;
        case 0x38553111U:  // sceAudioSRCChReserve
            v0 = static_cast<uint32_t>(rt.audio().reserve_src(
                static_cast<int32_t>(a0), static_cast<int32_t>(a1), static_cast<int32_t>(a2)));
            return true;
        case 0x5C37C0AEU:  // sceAudioSRCChRelease
            v0 = static_cast<uint32_t>(rt.audio().release_src());
            return true;
        case 0xE0727056U: {  // sceAudioSRCOutputBlocking(vol, buf)
            const uint8_t* p = bus.ptr(a1);
            v0 = p ? static_cast<uint32_t>(rt.audio().output_src(
                         static_cast<int32_t>(a0), reinterpret_cast<const int16_t*>(p)))
                   : 0U;
            return true;
        }

        // ================= SysMemUserForUser =================
        case 0x237DBD4FU: {  // sceKernelAllocPartitionMemory(part, name, type, size, addr)
            const uint32_t size = a3;
            const uint32_t addr = bus.alloc_user_block(size, 64U);
            if (addr == 0U) {
                v0 = 0x800200D9U;  // SCE_KERNEL_ERROR_NO_MEMORY
                return true;
            }
            HleMemBlock blk{};
            blk.uid = next_uid_++;
            blk.name = a1 ? bus.read_cstring(a1, 32) : "block";
            blk.addr = addr;
            blk.size = size;
            blk.active = true;
            mem_blocks_.push_back(blk);
            v0 = static_cast<uint32_t>(blk.uid);
            return true;
        }
        case 0xB6D61D02U: {  // sceKernelFreePartitionMemory(uid)
            for (auto& b : mem_blocks_) {
                if (b.uid == static_cast<int>(a0) && b.active) {
                    b.active = false;
                    v0 = 0U;
                    return true;
                }
            }
            v0 = 0U;
            return true;
        }
        case 0x9D9A5BA1U: {  // sceKernelGetBlockHeadAddr(uid)
            for (const auto& b : mem_blocks_) {
                if (b.uid == static_cast<int>(a0) && b.active) {
                    v0 = b.addr;
                    return true;
                }
            }
            v0 = 0U;
            return true;
        }
        case 0xA291F107U:  // sceKernelMaxFreeMemSize
        case 0xF919F628U:  // sceKernelTotalFreeMemSize
            v0 = bus.free_user_mem_bytes();
            return true;
        case 0x7591C7DBU:  // sceKernelSetCompiledSdkVersion
            v0 = 0U;
            return true;
        case 0x3FC9AE6AU:      // sceKernelDevkitVersion
            v0 = 0x06060010U;  // 6.60
            return true;

        // ================= ThreadManForUser =================
        case 0x446D8DE6U: {  // sceKernelCreateThread(name, entry, prio, stacksize, attr, opt)
            HleThread t{};
            t.id = next_uid_++;
            t.name = a0 ? bus.read_cstring(a0, 32) : "thread";
            t.entry_pc = a1;
            t.priority = static_cast<int>(a2);
            t.stack_size = std::max(a3, 16384U);
            const uint32_t stk_base = bus.alloc_user_block(t.stack_size, 64U);
            t.stack_addr = (stk_base + t.stack_size) & ~15U;
            t.state = ThreadState::Dormant;
            t.saved_ctx = PspCpuState{};
            t.saved_ctx.pc = t.entry_pc;
            t.saved_ctx.npc = t.entry_pc + 4U;
            t.saved_ctx.gpr[28] = cpu.gpr[28];
            t.saved_ctx.gpr[29] = t.stack_addr;
            t.saved_ctx.gpr[31] = 0x08800000U;
            threads_.push_back(t);
            v0 = static_cast<uint32_t>(t.id);
            return true;
        }
        case 0xF475845DU: {  // sceKernelStartThread(thid, arglen, argp)
            for (auto& t : threads_) {
                if (t.id == static_cast<int>(a0)) {
                    t.state = ThreadState::Ready;
                    t.saved_ctx.pc = t.entry_pc;
                    t.saved_ctx.npc = t.entry_pc + 4U;
                    t.saved_ctx.gpr[4] = a1;  // arglen
                    if (a1 > 0 && a2 != 0) {
                        const uint32_t arg_dst = (t.stack_addr - ((a1 + 15U) & ~15U));
                        std::vector<uint8_t> tmp(a1);
                        bus.read_bytes(a2, tmp.data(), a1);
                        bus.write_bytes(arg_dst, tmp.data(), a1);
                        t.saved_ctx.gpr[5] = arg_dst;
                        t.saved_ctx.gpr[29] = arg_dst - 16U;
                    } else {
                        t.saved_ctx.gpr[5] = 0U;
                    }
                    v0 = 0U;
                    return true;
                }
            }
            v0 = 0x80020198U;
            return true;
        }
        case 0xAA73C935U:    // sceKernelExitThread
        case 0x809CE29BU: {  // sceKernelExitDeleteThread
            if (current_thread_idx_ >= 0 &&
                static_cast<size_t>(current_thread_idx_) < threads_.size()) {
                threads_[static_cast<size_t>(current_thread_idx_)].state = ThreadState::Dead;
            }
            if (!schedule_next_thread(cpu)) {
                cpu.halted = 1U;
            }
            v0 = 0U;
            return true;
        }
        case 0x9FA03CD3U:  // sceKernelDeleteThread
            for (auto& t : threads_) {
                if (t.id == static_cast<int>(a0))
                    t.state = ThreadState::Dead;
            }
            v0 = 0U;
            return true;
        case 0x9ACE131EU:  // sceKernelSleepThread
        case 0x82826F70U:  // sceKernelSleepThreadCB
            if (current_thread_idx_ >= 0 &&
                static_cast<size_t>(current_thread_idx_) < threads_.size()) {
                threads_[static_cast<size_t>(current_thread_idx_)].state = ThreadState::Waiting;
            }
            if (!schedule_next_thread(cpu)) {
                cpu.vblank_yield = 1U;
            }
            v0 = 0U;
            return true;
        case 0xD59EAD2FU:  // sceKernelWakeupThread
            for (auto& t : threads_) {
                if (t.id == static_cast<int>(a0) && t.state == ThreadState::Waiting) {
                    t.state = ThreadState::Ready;
                }
            }
            v0 = 0U;
            return true;
        case 0xCEADEB47U:    // sceKernelDelayThread(usec)
        case 0x68DA9E36U: {  // sceKernelDelayThreadCB
            if (a0 >= 10000U) {
                cpu.vblank_yield = 1U;
            } else {
                cpu.cycles += static_cast<uint64_t>(a0) * cpu_freq_mhz_;
            }
            v0 = 0U;
            return true;
        }
        case 0x293B45B8U:  // sceKernelGetThreadId
            v0 = (current_thread_idx_ >= 0 &&
                  static_cast<size_t>(current_thread_idx_) < threads_.size())
                     ? static_cast<uint32_t>(threads_[static_cast<size_t>(current_thread_idx_)].id)
                     : 1U;
            return true;
        case 0xE81CAF8FU:  // sceKernelCreateCallback
            v0 = static_cast<uint32_t>(next_uid_++);
            return true;
        case 0x349D6D6CU:  // sceKernelCheckCallback
            v0 = 0U;
            return true;
        case 0x71BC9871U:  // sceKernelChangeThreadPriority
            v0 = 0U;
            return true;
        case 0x17C1684EU:  // sceKernelReferThreadStatus
            v0 = 0U;
            return true;

        // Semaphores
        case 0xD6DA4BA1U: {  // sceKernelCreateSema(name, attr, initVal, maxVal, opt)
            HleSema s{};
            s.uid = next_uid_++;
            s.name = a0 ? bus.read_cstring(a0, 32) : "sema";
            s.count = static_cast<int>(a2);
            s.max_count = static_cast<int>(a3);
            s.active = true;
            semas_.push_back(s);
            v0 = static_cast<uint32_t>(s.uid);
            return true;
        }
        case 0x28B6489CU:  // sceKernelDeleteSema
            for (auto& s : semas_)
                if (s.uid == static_cast<int>(a0))
                    s.active = false;
            v0 = 0U;
            return true;
        case 0x3F53E640U:  // sceKernelSignalSema(id, count)
            for (auto& s : semas_) {
                if (s.uid == static_cast<int>(a0) && s.active) {
                    s.count = std::min(s.count + static_cast<int>(a1), s.max_count);
                    break;
                }
            }
            v0 = 0U;
            return true;
        case 0x4E3A1105U:  // sceKernelWaitSema
        case 0x6D212BACU:  // sceKernelWaitSemaCB
        case 0x58B1F937U:  // sceKernelPollSema
            for (auto& s : semas_) {
                if (s.uid == static_cast<int>(a0) && s.active) {
                    if (s.count >= static_cast<int>(a1)) {
                        s.count -= static_cast<int>(a1);
                        v0 = 0U;
                    } else {
                        v0 = (nid == 0x58B1F937U) ? 0x800201A9U : 0U;
                    }
                    return true;
                }
            }
            v0 = 0U;
            return true;

        // Event flags
        case 0x55C20A00U: {  // sceKernelCreateEventFlag(name, attr, bits, opt)
            HleEventFlag ef{};
            ef.uid = next_uid_++;
            ef.name = a0 ? bus.read_cstring(a0, 32) : "evflag";
            ef.bits = a2;
            ef.active = true;
            evflags_.push_back(ef);
            v0 = static_cast<uint32_t>(ef.uid);
            return true;
        }
        case 0xEF9E4C70U:
            for (auto& ef : evflags_)
                if (ef.uid == static_cast<int>(a0))
                    ef.active = false;
            v0 = 0U;
            return true;
        case 0x1FB15A32U:  // sceKernelSetEventFlag
            for (auto& ef : evflags_)
                if (ef.uid == static_cast<int>(a0) && ef.active)
                    ef.bits |= a1;
            v0 = 0U;
            return true;
        case 0x812346E4U:  // sceKernelClearEventFlag
            for (auto& ef : evflags_)
                if (ef.uid == static_cast<int>(a0) && ef.active)
                    ef.bits &= a1;
            v0 = 0U;
            return true;
        case 0x402FCF22U:    // sceKernelWaitEventFlag
        case 0x328C546AU:    // sceKernelWaitEventFlagCB
        case 0x30FD48F0U: {  // sceKernelPollEventFlag
            for (auto& ef : evflags_) {
                if (ef.uid == static_cast<int>(a0) && ef.active) {
                    const bool or_mode = (a2 & 1U) != 0U;
                    const bool matched = or_mode ? ((ef.bits & a1) != 0U) : ((ef.bits & a1) == a1);
                    if (a3)
                        bus.write32(a3, ef.bits);
                    if (matched) {
                        if (a2 & 0x10U)
                            ef.bits = 0U;
                        else if (a2 & 0x20U)
                            ef.bits &= ~a1;
                        v0 = 0U;
                    } else {
                        v0 = (nid == 0x30FD48F0U) ? 0x800201A6U : 0U;
                    }
                    return true;
                }
            }
            v0 = 0U;
            return true;
        }

        // Mutexes
        case 0xB7D098C6U:    // sceKernelCreateMutex
        case 0x19CFF145U: {  // sceKernelCreateLwMutex
            HleMutex m{};
            m.uid = next_uid_++;
            m.active = true;
            mutexes_.push_back(m);
            v0 = static_cast<uint32_t>(m.uid);
            return true;
        }
        case 0xB011B11FU:  // sceKernelLockMutex
        case 0x6B30100FU:  // sceKernelUnlockMutex
        case 0xF8170FBEU:  // sceKernelDeleteMutex
        case 0xBEA46419U:  // sceKernelLockLwMutex
        case 0x15B6446BU:  // sceKernelUnlockLwMutex
        case 0x60107536U:  // sceKernelDeleteLwMutex
            v0 = 0U;
            return true;

        // System time
        case 0x369ED59DU:  // sceKernelGetSystemTimeLow
            v0 = static_cast<uint32_t>(current_time_us(cpu));
            return true;
        case 0x82BC5777U: {  // sceKernelGetSystemTimeWide
            const uint64_t t = current_time_us(cpu);
            v0 = static_cast<uint32_t>(t & 0xFFFFFFFFULL);
            v1 = static_cast<uint32_t>(t >> 32U);
            return true;
        }
        case 0xDB738F35U: {  // sceKernelGetSystemTime(SceKernelSysClock* t)
            const uint64_t t = current_time_us(cpu);
            if (a0) {
                bus.write32(a0, static_cast<uint32_t>(t & 0xFFFFFFFFULL));
                bus.write32(a0 + 4U, static_cast<uint32_t>(t >> 32U));
            }
            v0 = 0U;
            return true;
        }
        case 0x110DEC9AU:  // sceKernelUSec2SysClock
            if (a1) {
                bus.write32(a1, a0);
                bus.write32(a1 + 4U, 0U);
            }
            v0 = 0U;
            return true;
        case 0xC8CD158CU:  // sceKernelUSec2SysClockWide
            v0 = a0;
            v1 = 0U;
            return true;
        case 0xBA6B92E2U:  // sceKernelSysClock2USec
            if (a0) {
                const uint64_t t = static_cast<uint64_t>(bus.read32(a0)) |
                                   (static_cast<uint64_t>(bus.read32(a0 + 4U)) << 32U);
                if (a1)
                    bus.write32(a1, static_cast<uint32_t>(t / 1000000ULL));
                if (a2)
                    bus.write32(a2, static_cast<uint32_t>(t % 1000000ULL));
            }
            v0 = 0U;
            return true;

        // ================= LoadExecForUser =================
        case 0x05572A5FU:  // sceKernelExitGame
            exit_requested_ = true;
            cpu.halted = 1U;
            v0 = 0U;
            return true;
        case 0x4AC57943U:  // sceKernelRegisterExitCallback
        case 0xBD2F1094U:  // sceKernelLoadExec
            v0 = 0U;
            return true;

        // ================= IoFileMgrForUser =================
        case 0x109F50BCU: {  // sceIoOpen(file, flags, mode)
            std::string path = bus.read_cstring(a0, 256);
            if (path.find(':') == std::string::npos) {
                path = cwd_ + path;
            }
            auto f = std::make_unique<HleFile>();
            f->fd = next_fd_++;
            f->open = true;

            if (path.rfind("disc0:/", 0) == 0 || path.rfind("umd0:", 0) == 0) {
                const std::string sub =
                    (path.rfind("disc0:/", 0) == 0) ? path.substr(7) : path.substr(5);
                const IsoEntry* ent = rt.image().find_iso_entry(sub);
                if (!ent || ent->is_dir) {
                    v0 = kPspErrnoNotFound;
                    return true;
                }
                f->is_disc = true;
                f->disc_offset = static_cast<uint64_t>(ent->lba) * 2048ULL;
                f->disc_size = ent->size;
                f->cursor = 0;
                const int fd = f->fd;
                files_.push_back(std::move(f));
                v0 = static_cast<uint32_t>(fd);
                return true;
            }

            // ms0:/ host file backed by save_dir_
            f->host_path = resolve_ms0_path(path);
            std::error_code ec;
            std::filesystem::create_directories(f->host_path.parent_path(), ec);
            std::ios::openmode mode = std::ios::binary;
            const bool write_flag = (a1 & 0x0002U) != 0U;
            const bool create_flag = (a1 & 0x0200U) != 0U;
            const bool trunc_flag = (a1 & 0x0400U) != 0U;
            mode |= std::ios::in;
            if (write_flag || create_flag)
                mode |= std::ios::out;
            if (trunc_flag)
                mode |= std::ios::trunc;
            if (create_flag && !std::filesystem::exists(f->host_path)) {
                std::ofstream create_touch(f->host_path, std::ios::binary);
            }
            f->host_stream.open(f->host_path, mode);
            if (!f->host_stream.is_open()) {
                v0 = kPspErrnoNotFound;
                return true;
            }
            const int fd = f->fd;
            files_.push_back(std::move(f));
            v0 = static_cast<uint32_t>(fd);
            return true;
        }
        case 0x810C4BC3U: {  // sceIoClose(fd)
            for (auto& f : files_) {
                if (f && f->fd == static_cast<int>(a0) && f->open) {
                    f->open = false;
                    if (f->host_stream.is_open())
                        f->host_stream.close();
                    v0 = 0U;
                    return true;
                }
            }
            v0 = kPspErrnoBadf;
            return true;
        }
        case 0x6A638D83U: {  // sceIoRead(fd, data, size)
            for (auto& f : files_) {
                if (f && f->fd == static_cast<int>(a0) && f->open) {
                    if (f->is_disc) {
                        const uint64_t avail =
                            (f->cursor < f->disc_size) ? (f->disc_size - f->cursor) : 0ULL;
                        const size_t n = static_cast<size_t>(std::min<uint64_t>(avail, a2));
                        if (n > 0) {
                            std::vector<uint8_t> buf(n);
                            if (rt.image().read_disc_bytes(f->disc_offset + f->cursor, buf.data(),
                                                           n)) {
                                bus.write_bytes(a1, buf.data(), n);
                                f->cursor += n;
                                v0 = static_cast<uint32_t>(n);
                                return true;
                            }
                        }
                        v0 = 0U;
                        return true;
                    }
                    std::vector<uint8_t> buf(a2);
                    f->host_stream.read(reinterpret_cast<char*>(buf.data()),
                                        static_cast<std::streamsize>(a2));
                    const auto got = static_cast<size_t>(f->host_stream.gcount());
                    if (got > 0)
                        bus.write_bytes(a1, buf.data(), got);
                    f->host_stream.clear();
                    v0 = static_cast<uint32_t>(got);
                    return true;
                }
            }
            v0 = kPspErrnoBadf;
            return true;
        }
        case 0x42EC03ACU: {  // sceIoWrite(fd, data, size)
            if (a0 == 1U || a0 == 2U) {
                // stdout / stderr
                v0 = a2;
                return true;
            }
            for (auto& f : files_) {
                if (f && f->fd == static_cast<int>(a0) && f->open && !f->is_disc) {
                    std::vector<uint8_t> buf(a2);
                    bus.read_bytes(a1, buf.data(), a2);
                    f->host_stream.write(reinterpret_cast<const char*>(buf.data()),
                                         static_cast<std::streamsize>(a2));
                    f->host_stream.flush();
                    v0 = a2;
                    return true;
                }
            }
            v0 = kPspErrnoBadf;
            return true;
        }
        case 0x27EB27B8U:    // sceIoLseek(fd, offset64 in a2/a3, whence in t0)
        case 0x68963324U: {  // sceIoLseek32(fd, offset in a1, whence in a2)
            const int64_t off = (nid == 0x68963324U)
                                    ? static_cast<int64_t>(static_cast<int32_t>(a1))
                                    : static_cast<int64_t>(static_cast<uint64_t>(a2) |
                                                           (static_cast<uint64_t>(a3) << 32U));
            const uint32_t whence = (nid == 0x68963324U) ? a2 : t0;
            for (auto& f : files_) {
                if (f && f->fd == static_cast<int>(a0) && f->open) {
                    if (f->is_disc) {
                        int64_t next = static_cast<int64_t>(f->cursor);
                        if (whence == 0U)
                            next = off;
                        else if (whence == 1U)
                            next += off;
                        else if (whence == 2U)
                            next = static_cast<int64_t>(f->disc_size) + off;
                        f->cursor = static_cast<uint64_t>(std::max<int64_t>(0, next));
                        v0 = static_cast<uint32_t>(f->cursor & 0xFFFFFFFFULL);
                        if (nid == 0x27EB27B8U)
                            v1 = static_cast<uint32_t>(f->cursor >> 32U);
                        return true;
                    }
                    std::ios::seekdir dir = std::ios::beg;
                    if (whence == 1U)
                        dir = std::ios::cur;
                    else if (whence == 2U)
                        dir = std::ios::end;
                    f->host_stream.seekg(off, dir);
                    f->host_stream.seekp(off, dir);
                    const auto pos =
                        static_cast<uint64_t>(std::max<std::streamoff>(0, f->host_stream.tellg()));
                    v0 = static_cast<uint32_t>(pos & 0xFFFFFFFFULL);
                    if (nid == 0x27EB27B8U)
                        v1 = static_cast<uint32_t>(pos >> 32U);
                    return true;
                }
            }
            v0 = kPspErrnoBadf;
            return true;
        }
        case 0xACE946E8U: {  // sceIoGetstat(file, SceIoStat* stat)
            std::string path = bus.read_cstring(a0, 256);
            if (path.rfind("disc0:/", 0) == 0) {
                const IsoEntry* ent = rt.image().find_iso_entry(path.substr(7));
                if (!ent) {
                    v0 = kPspErrnoNotFound;
                    return true;
                }
                for (uint32_t i = 0; i < 64U; i += 4U)
                    bus.write32(a1 + i, 0U);
                bus.write32(a1 + 0U, ent->is_dir ? 0x11FFU : 0x21FFU);
                bus.write32(a1 + 4U, ent->is_dir ? 0x10U : 0x20U);
                bus.write32(a1 + 8U, ent->size);
                v0 = 0U;
                return true;
            }
            const auto hp = resolve_ms0_path(path);
            if (!std::filesystem::exists(hp)) {
                v0 = kPspErrnoNotFound;
                return true;
            }
            for (uint32_t i = 0; i < 64U; i += 4U)
                bus.write32(a1 + i, 0U);
            const bool is_dir = std::filesystem::is_directory(hp);
            bus.write32(a1 + 0U, is_dir ? 0x11FFU : 0x21FFU);
            bus.write32(a1 + 4U, is_dir ? 0x10U : 0x20U);
            if (!is_dir) {
                bus.write32(a1 + 8U, static_cast<uint32_t>(std::filesystem::file_size(hp)));
            }
            v0 = 0U;
            return true;
        }
        case 0x55F4717DU:  // sceIoChdir
            cwd_ = bus.read_cstring(a0, 256);
            if (!cwd_.empty() && cwd_.back() != '/')
                cwd_.push_back('/');
            v0 = 0U;
            return true;
        case 0x06A70004U: {  // sceIoMkdir
            std::error_code ec;
            std::filesystem::create_directories(resolve_ms0_path(bus.read_cstring(a0, 256)), ec);
            v0 = 0U;
            return true;
        }
        case 0xF27A9C51U:    // sceIoRemove
        case 0x1117C65FU: {  // sceIoRmdir
            std::error_code ec;
            std::filesystem::remove(resolve_ms0_path(bus.read_cstring(a0, 256)), ec);
            v0 = 0U;
            return true;
        }
        case 0x54F5FB11U:  // sceIoDevctl
            v0 = 0U;
            return true;

        // ================= UtilsForUser / Cache / Libc =================
        case 0x79D1C3FAU:
        case 0x3EE30821U:
        case 0xB435DEC5U:
        case 0x34B9FA9EU:
        case 0xBFA98062U:
        case 0x920F104AU:
        case 0xC2DF770EU:
            v0 = 0U;
            return true;
        case 0x27CC57F0U: {  // sceKernelLibcTime
            const uint32_t t = static_cast<uint32_t>(std::time(nullptr));
            if (a0)
                bus.write32(a0, t);
            v0 = t;
            return true;
        }
        case 0x91E4F6A7U:  // sceKernelLibcClock
            v0 = static_cast<uint32_t>(current_time_us(cpu));
            return true;
        case 0x71EC4271U: {  // sceKernelLibcGettimeofday
            const uint64_t us = current_time_us(cpu);
            if (a0) {
                bus.write32(a0 + 0U, static_cast<uint32_t>(us / 1000000ULL));
                bus.write32(a0 + 4U, static_cast<uint32_t>(us % 1000000ULL));
            }
            v0 = 0U;
            return true;
        }

        // ================= sceRtc =================
        case 0x3F7AD767U: {  // sceRtcGetCurrentTick
            const uint64_t ticks = 63900000000000000ULL + current_time_us(cpu);
            if (a0) {
                bus.write32(a0, static_cast<uint32_t>(ticks & 0xFFFFFFFFULL));
                bus.write32(a0 + 4U, static_cast<uint32_t>(ticks >> 32U));
            }
            v0 = 0U;
            return true;
        }
        case 0xC41C2853U:  // sceRtcGetTickResolution
            v0 = 1000000U;
            return true;
        case 0x4CFA57B0U:    // sceRtcGetCurrentClock
        case 0xE7C27D1BU: {  // sceRtcGetCurrentClockLocalTime
            if (a0) {
                const std::time_t now = std::time(nullptr);
                std::tm tm_val{};
                localtime_r(&now, &tm_val);
                bus.write16(a0 + 0U, static_cast<uint16_t>(tm_val.tm_year + 1900));
                bus.write16(a0 + 2U, static_cast<uint16_t>(tm_val.tm_mon + 1));
                bus.write16(a0 + 4U, static_cast<uint16_t>(tm_val.tm_mday));
                bus.write16(a0 + 6U, static_cast<uint16_t>(tm_val.tm_hour));
                bus.write16(a0 + 8U, static_cast<uint16_t>(tm_val.tm_min));
                bus.write16(a0 + 10U, static_cast<uint16_t>(tm_val.tm_sec));
                bus.write32(a0 + 12U, 0U);
            }
            v0 = 0U;
            return true;
        }

        // ================= scePower =================
        case 0x2085D15DU:
            v0 = 100U;
            return true;  // Battery 100%
        case 0x0AFD0D8BU:
            v0 = 1U;
            return true;  // Battery exists
        case 0x87440F5EU:
            v0 = 1U;
            return true;   // AC online
        case 0x737486F2U:  // scePowerSetClockFrequency
            if (a1 > 0)
                cpu_freq_mhz_ = a1;
            if (a2 > 0)
                bus_freq_mhz_ = a2;
            v0 = 0U;
            return true;
        case 0xFEE03A2FU:
            v0 = cpu_freq_mhz_;
            return true;
        case 0x478FE6F5U:
            v0 = bus_freq_mhz_;
            return true;
        case 0x04B7766EU:
        case 0xEFD3C963U:
            v0 = 0U;
            return true;

        // ================= sceUmdUser =================
        case 0x46EBB729U:
            v0 = 1U;
            return true;  // UMD present
        case 0xC6183D47U:
            v0 = 0U;
            return true;  // Activate OK
        case 0xE83742BAU:
            v0 = 0U;
            return true;  // Deactivate OK
        case 0x8EF08FCEU:
        case 0x56202973U:
        case 0x4A9E5E29U:
            v0 = 0U;
            return true;
        case 0x6B4A146CU:
            v0 = 0x32U;
            return true;  // PSP_UMD_PRESENT | PSP_UMD_READABLE | PSP_UMD_READY
        case 0xAEE7404DU:
        case 0xBD2BDE07U:
        case 0x20628E6FU:
            v0 = 0U;
            return true;

        // ================= sceUtility =================
        case 0xA5DA2406U: {  // sceUtilityGetSystemParamInt(id, val*)
            uint32_t val = 0U;
            if (a0 == 8U)
                val = 1U;  // Language = English (1)
            else if (a0 == 9U)
                val = 1U;  // Button assignment = X confirm
            if (a1)
                bus.write32(a1, val);
            v0 = 0U;
            return true;
        }
        case 0x34B78343U: {  // sceUtilityGetSystemParamString(id, str, len)
            const char* nick = "DragonPSP";
            if (a1 && a2 > 0) {
                const size_t n = std::min<size_t>(std::strlen(nick) + 1U, a2);
                bus.write_bytes(a1, nick, n);
            }
            v0 = 0U;
            return true;
        }
        case 0x2A2B3DE0U:
        case 0xE49BFE92U:
        case 0xC629AF26U:
            v0 = 0U;
            return true;

        // ================= ModuleMgr / InterruptManager / Stdio =================
        case 0x977DE386U:
        case 0x50F0C1ECU:
        case 0xD1FF982AU:
        case 0x2E0911AAU:
        case 0xCC1D3699U:
        case 0xD675EBB8U:
        case 0xCA04A2B9U:
        case 0xD61E6961U:
        case 0xFB8E22ECU:
        case 0x8A389411U:
            v0 = 0U;
            return true;
        case 0x172D316EU:
            v0 = 0U;
            return true;
        case 0xA6BAB2E9U:
            v0 = 1U;
            return true;
        case 0xF78BA90AU:
            v0 = 2U;
            return true;

        default:
            return false;
    }
}

}  // namespace dragon::psp
