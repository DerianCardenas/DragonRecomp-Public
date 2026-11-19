#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "studio/rom_detector.h"

namespace dragon::studio {

enum class PipelineStage {
    Idle,
    Analyzing,
    Disassembling,
    Emitting,
    CompilingNative,
    Ready,
    Failed,
};

struct RecompStats {
    size_t functions = 0;
    size_t blocks = 0;
    size_t instructions = 0;
    double compilation_time_sec = 0.0;
};

struct FallbackEvent {
    uint8_t bank = 0;
    uint32_t addr = 0;
    uint8_t mode = 0;
    uint32_t from_addr = 0;
};

enum class DiagnosticSymptom {
    GrayScreen,            // Pantalla gris
    BlueScreen,            // Pantalla azul
    FreezeOnBoot,          // Juego congelado al cargar (Pantalla de inicio)
    FreezeInGame,          // Juego congelado al jugar (Durante la partida)
    CrashOnLaunch,         // Juego no abre / Se cierra de golpe
    NoSound,               // Sin sonido
    DistortedSound,        // Sonido diferente / Distorsionado
    WrongColors,           // Colores diferentes / Paleta extraña
    ControlsUnresponsive,  // Controles no responden
    CorruptGraphics,       // Gráficos corruptos / Sprites parpadeando o invisibles
    AbnormalSpeed,         // Velocidad anormal (Muy rápido / Muy lento)
};

struct DiagnosticResult {
    bool success = false;
    std::string summary;
    std::string details;
};

enum class CompileOptLevel {
    None,        // -O0: no optimization
    Fast,        // -O1: 3-5x faster compilation, low RAM usage (default)
    Balanced,    // -O2: heavier optimization passes
    Aggressive,  // -O3: aggressive vectorization & inlining
};

enum class LogLevel {
    Info,
    Message,
    Success,
    Warning,
};

struct CompileLogEntry {
    std::string time;
    LogLevel level = LogLevel::Message;
    std::string message;
};

class Pipeline {
public:
    static constexpr unsigned int kDefaultCompileThreads = 2U;

    Pipeline();
    ~Pipeline();

    // Compilation resource & speed controls (exposed for upcoming Settings UI)
    void set_max_compile_threads(unsigned int threads);
    unsigned int max_compile_threads() const;
    void set_compile_opt_level(CompileOptLevel level);
    CompileOptLevel compile_opt_level() const;

    // Start recompilation asynchronously
    bool start_pipeline(const RomInfo& rom);

    // Cancel current pipeline if running
    void cancel();

    // Query current status
    PipelineStage stage() const;
    float progress() const;
    std::string current_step_description() const;
    std::string error_message() const;
    std::vector<CompileLogEntry> compile_log() const;

    // Output artifacts
    std::filesystem::path output_binary() const;
    std::filesystem::path output_binary_windows() const;
    std::filesystem::path workspace_dir() const;
    std::filesystem::path config_file_path() const;
    const RecompStats& stats() const;

    // Launch game and monitor fallbacks
    bool launch_game(bool fullscreen = false, bool crt_filter = false,
                     bool auto_save_state = false);
    void stop_game();
    bool is_game_running() const;
    std::string last_game_error() const;
    void clear_last_game_error();

    // Learning loop & autonomous diagnostics
    std::vector<FallbackEvent> learned_fallbacks() const;
    size_t pending_learn_count() const;
    bool optimize_and_recompile();
    DiagnosticResult diagnose_and_repair(DiagnosticSymptom symptom);

    // Pipeline steps (public for testing and direct invocation)
    bool generate_initial_toml(const RomInfo& rom, const std::filesystem::path& toml_path);
    bool compile_c_to_binary(const RomInfo& rom, const std::filesystem::path& gen_dir,
                             const std::filesystem::path& out_bin);
    bool compile_windows_binary(const RomInfo& rom, const std::filesystem::path& gen_dir,
                                const std::filesystem::path& out_bin);

private:
    void run_worker(RomInfo rom);
    void monitor_game_process(int pid, int stderr_fd, RomPlatform platform);
    void append_log(LogLevel level, const std::string& msg);

    mutable std::mutex mutex_;
    std::atomic<PipelineStage> stage_{PipelineStage::Idle};
    std::atomic<float> progress_{0.0f};
    std::string step_desc_;
    std::string error_msg_;
    std::string last_game_error_;
    std::chrono::steady_clock::time_point pipeline_start_time_{};
    std::vector<CompileLogEntry> compile_log_;

    std::filesystem::path workspace_dir_;
    std::filesystem::path output_binary_;
    std::filesystem::path output_binary_windows_;
    std::filesystem::path toml_path_;
    RecompStats stats_;
    RomInfo active_rom_;

    std::thread worker_thread_;
    std::thread game_monitor_thread_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<int> running_game_pid_{-1};
    bool last_fullscreen_ = false;
    bool last_crt_ = false;
    bool last_autosave_ = false;
    std::atomic<bool> relaunch_after_compile_{false};
    std::atomic<unsigned int> max_compile_threads_{kDefaultCompileThreads};
    std::atomic<CompileOptLevel> compile_opt_level_{CompileOptLevel::Fast};

    std::vector<FallbackEvent> captured_fallbacks_;
    std::vector<FallbackEvent> unapplied_fallbacks_;
};

}  // namespace dragon::studio
