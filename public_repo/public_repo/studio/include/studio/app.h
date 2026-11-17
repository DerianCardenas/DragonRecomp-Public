#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include "studio/library.h"
#include "studio/packager.h"
#include "studio/pipeline.h"
#include "studio/rom_detector.h"
#include "studio/theme.h"

namespace dragon::studio {

class StudioApp {
public:
    StudioApp();
    ~StudioApp();

    bool init(int width = 960, int height = 640);
    void run();
    void shutdown();

    void load_rom(const std::filesystem::path& rom_path);
    void queue_load_rom(const std::filesystem::path& rom_path);
    void open_rom_file_dialog();
    void set_export_directory(const std::string& path);

private:
    void process_events();
    void render_ui();

    void render_header();
    void render_drop_zone();
    void render_rom_card();
    void render_pipeline_progress();
    void render_ready_hub();
    void render_error_view();
    void render_package_modal();
    void render_library();
    void track_library_state();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool running_ = false;

    StudioFonts fonts_;

    Pipeline pipeline_;
    RomInfo active_rom_;

    // Persistent game library (shelves view) and bookkeeping for mode/playtime.
    GameLibrary library_{GameLibrary::default_path()};
    PipelineStage prev_stage_ = PipelineStage::Idle;
    bool was_game_running_ = false;
    int64_t game_start_unix_ = 0;
    std::string library_notice_;

    // UI state
    bool fullscreen_option_ = false;
    bool crt_filter_option_ = false;
    bool show_package_modal_ = false;
    PackageResult last_package_result_;
    std::string export_status_message_;

    char export_dir_buf_[512] = "";
#if defined(_WIN32)
    int export_platform_idx_ = 1;  // 0 = Linux, 1 = Windows, 2 = Ambos (Windows default on Win32)
#else
    int export_platform_idx_ = 0;  // 0 = Linux, 1 = Windows, 2 = Ambos
#endif
    bool include_launcher_script_ = true;
    bool include_desktop_entry_ = true;
    // SDL3 bundling is mandatory (M9): always forced true in PackageOptions, no UI toggle.
    bool include_windows_batch_ = true;
    bool bundle_standalone_cmake_ = true;

    // Diagnostic UI state
    int selected_symptom_idx_ = 0;
    std::string diagnostic_status_message_;
    bool diagnostic_status_success_ = false;

    // Asynchronous file dialog queue
    std::mutex pending_rom_mutex_;
    std::filesystem::path pending_rom_to_load_;
};

}  // namespace dragon::studio
