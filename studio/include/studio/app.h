#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include "studio/cover_manager.h"
#include "studio/library.h"
#include "studio/packager.h"
#include "studio/pipeline.h"
#include "studio/rom_detector.h"
#include "studio/theme.h"

namespace dragon::studio {

enum class StudioView {
    Library,
    Recompiler,
};

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
    void open_import_portable_dialog();
    void queue_import_portable(const std::filesystem::path& folder_path);
    void import_portable_folder(const std::filesystem::path& folder_path);
    void open_manual_import_rom_dialog();
    void queue_manual_import_rom(const std::filesystem::path& rom_path);
    void launch_portable_game(const GameEntry& entry);
    int console_tab_for_platform(const std::string& platform_key) const;
    void open_cover_file_dialog(const std::string& sha1);
    void queue_assign_cover(const std::string& sha1, const std::filesystem::path& cover_path);
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
    void render_manual_import_modal();
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
    CoverManager cover_mgr_;
    StudioView current_view_ = StudioView::Library;
    int library_console_tab_ = 0;  // 0 = NES, 1 = SNES, 2 = GB/GBC, 3 = GBA
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

    // Asynchronous cover dialog/download queue
    std::mutex pending_cover_mutex_;
    std::vector<std::pair<std::string, std::filesystem::path>> pending_covers_to_assign_;

    // Portable game process tracking (launched from Library)
    int portable_game_pid_ = -1;
    std::string portable_game_sha1_;
    int64_t portable_game_start_unix_ = 0;

    // Import recompiled portable folder & manual fallback modal state
    std::mutex pending_import_mutex_;
    std::filesystem::path pending_import_folder_;
    std::filesystem::path pending_manual_rom_path_;
    bool show_manual_import_modal_ = false;
    PortableInspection pending_inspection_;
    char manual_import_title_buf_[256] = "";
    int manual_import_platform_idx_ = 0;  // 0=NES, 1=SNES, 2=GB, 3=GBC, 4=GBA
    char manual_import_exec_linux_buf_[512] = "";
    char manual_import_exec_win_buf_[512] = "";
    char manual_import_rom_buf_[512] = "";
    bool manual_import_copy_rom_ = true;
    std::string manual_import_error_;
};

}  // namespace dragon::studio
