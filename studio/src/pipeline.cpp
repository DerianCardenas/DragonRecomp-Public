#include "studio/pipeline.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <process.h>
#include <windows.h>
#endif

#include <set>
#include "dragon/config.h"
#include "dragon/file_io.h"

namespace dragon::studio {

namespace {

std::filesystem::path get_executable_dir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (len > 0) {
        return std::filesystem::path(buf).parent_path();
    }
#elif defined(__linux__)
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::filesystem::path(buf).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path find_tool(const std::string& name) {
    const auto exe_dir = get_executable_dir();
    std::vector<std::string> names = {name};
#if defined(_WIN32)
    names.push_back(name + ".exe");
#endif
    for (const auto& n : names) {
        const std::vector<std::filesystem::path> candidates = {
            exe_dir / n,
            exe_dir / "nes" / "recompiler" / n,
            exe_dir / "snes" / "recompiler" / n,
            exe_dir / "gb" / "recompiler" / n,
            exe_dir / "gba" / "recompiler" / n,
            exe_dir / "sms" / "recompiler" / n,
            exe_dir / ".." / "nes" / "recompiler" / n,
            exe_dir / ".." / "snes" / "recompiler" / n,
            exe_dir / ".." / "gb" / "recompiler" / n,
            exe_dir / ".." / "gba" / "recompiler" / n,
            exe_dir / ".." / "sms" / "recompiler" / n,
            std::filesystem::current_path() / "build" / "nes" / "recompiler" / n,
            std::filesystem::current_path() / "build" / "snes" / "recompiler" / n,
            std::filesystem::current_path() / "build" / "gb" / "recompiler" / n,
            std::filesystem::current_path() / "build" / "gba" / "recompiler" / n,
            std::filesystem::current_path() / "build" / "sms" / "recompiler" / n,
        };
        for (const auto& p : candidates) {
            if (std::filesystem::exists(p)) {
                return std::filesystem::canonical(p);
            }
        }
    }
    return name;
}

std::string find_c_compiler() {
    if (const char* env_cc = std::getenv("DRAGON_CC")) {
        return env_cc;
    }
    if (const char* env_cc = std::getenv("CC")) {
        return env_cc;
    }
#if defined(_WIN32)
    const std::vector<std::filesystem::path> candidates = {
        "C:\\Program Files\\LLVM\\bin\\clang.exe",
        "C:\\Program Files (x86)\\LLVM\\bin\\clang.exe",
        "C:\\msys64\\ucrt64\\bin\\clang.exe",
        "C:\\msys64\\mingw64\\bin\\clang.exe",
        "C:\\tools\\llvm\\bin\\clang.exe",
        "C:\\Users\\devel\\tools\\w64devkit\\bin\\gcc.exe",
        "C:\\tools\\w64devkit\\bin\\gcc.exe",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
    char buf[MAX_PATH];
    if (SearchPathA(NULL, "clang", ".exe", sizeof(buf), buf, NULL) > 0) {
        return std::string(buf);
    }
    if (SearchPathA(NULL, "gcc", ".exe", sizeof(buf), buf, NULL) > 0) {
        return std::string(buf);
    }
#elif defined(__linux__) || defined(__unix__)
    const std::vector<std::filesystem::path> linux_candidates = {
        "/usr/bin/clang",
        "/usr/local/bin/clang",
        "/usr/bin/gcc",
        "/usr/local/bin/gcc",
    };
    for (const auto& p : linux_candidates) {
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
#endif
    return "clang";
}

std::string find_cxx_compiler() {
    if (const char* env_cxx = std::getenv("DRAGON_CXX")) {
        return env_cxx;
    }
    if (const char* env_cxx = std::getenv("CXX")) {
        return env_cxx;
    }
#if defined(_WIN32)
    const std::vector<std::filesystem::path> candidates = {
        "C:\\Program Files\\LLVM\\bin\\clang++.exe",
        "C:\\Program Files (x86)\\LLVM\\bin\\clang++.exe",
        "C:\\msys64\\ucrt64\\bin\\clang++.exe",
        "C:\\msys64\\mingw64\\bin\\clang++.exe",
        "C:\\tools\\llvm\\bin\\clang++.exe",
        "C:\\Users\\devel\\tools\\w64devkit\\bin\\g++.exe",
        "C:\\tools\\w64devkit\\bin\\g++.exe",
    };
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
    char buf[MAX_PATH];
    if (SearchPathA(NULL, "clang++", ".exe", sizeof(buf), buf, NULL) > 0) {
        return std::string(buf);
    }
    if (SearchPathA(NULL, "g++", ".exe", sizeof(buf), buf, NULL) > 0) {
        return std::string(buf);
    }
#elif defined(__linux__) || defined(__unix__)
    const std::vector<std::filesystem::path> linux_candidates = {
        "/usr/bin/clang++",
        "/usr/local/bin/clang++",
        "/usr/bin/g++",
        "/usr/local/bin/g++",
    };
    for (const auto& p : linux_candidates) {
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
#endif
    return "clang++";
}

std::filesystem::path find_repo_root() {
    auto cur = std::filesystem::current_path();
    for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(cur / "CMakeLists.txt") &&
            std::filesystem::exists(cur / "common")) {
            return cur;
        }
        if (!cur.has_parent_path() || cur == cur.parent_path()) {
            break;
        }
        cur = cur.parent_path();
    }
    return std::filesystem::current_path();
}

std::filesystem::path find_build_root() {
    const auto exe_dir = get_executable_dir();
    const std::vector<std::string> lib_names = {"libdragon_common.a", "dragon_common.lib"};
    for (const auto& lib_name : lib_names) {
        if (std::filesystem::exists(exe_dir / "common" / lib_name)) {
            return std::filesystem::canonical(exe_dir);
        }
        if (std::filesystem::exists(exe_dir.parent_path() / "common" / lib_name)) {
            return std::filesystem::canonical(exe_dir.parent_path());
        }
    }
    const auto repo = find_repo_root();
    for (const auto& lib_name : lib_names) {
        if (std::filesystem::exists(repo / "build" / "common" / lib_name)) {
            return std::filesystem::canonical(repo / "build");
        }
    }
    return exe_dir;
}

std::filesystem::path get_user_knowledge_dir() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "DragonRecomp" / "knowledge";
    }
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "dragonrecomp" / "knowledge";
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "dragonrecomp" / "knowledge";
    }
#endif
    return find_repo_root() / "build" / "user_knowledge";
}

// ROMs whose recompiled code cannot (yet) run correctly natively, so Studio falls back to the
// cycle-accurate interpreter instead. SMW (SNES) depends on dynamic runtime code in WRAM; Pokemon
// Emerald's (GBA) VBlank handler calls into the m4a sound-engine mixer, whose recompiled code
// never returns (see docs/SNES_SPEC.md §S3 and docs/GBA_SPEC.md §6.1). Checked both for a fresh
// TOML generation and when restoring a curated or user-learned config that predates this fallback,
// so a cache saved before the bug was found does not keep reproducing the freeze.
bool requires_interpreter_only(const RomInfo& rom) {
    if (rom.platform == RomPlatform::SNES) {
        return rom.sha1 == "6b47bb75d16514b6a476aa0c73a683a2a4c18765" ||
               rom.clean_name.find("SUPER MARIOWORLD") != std::string::npos ||
               rom.clean_name.find("Super Mario World") != std::string::npos;
    }
    if (rom.platform == RomPlatform::GameBoyAdvance) {
        if (rom.sha1 == "fe1558a3dcb0360ab558969e09b690888b846dd9" ||
            rom.clean_name.find("Pokemon - Edicion Esmeralda") != std::string::npos ||
            rom.clean_name.find("Pokemon Emerald") != std::string::npos) {
            return true;
        }
        // Pokemon Mystery Dungeon: Red/Blue Rescue Team. Its dungeon/move
        // engine dispatches through many function-pointer tables that the
        // static analyzer never discovers ahead of time (unlike NES SMB's
        // single explicitly-configured jump_engine): every new species/move/
        // menu state hit during actual play surfaces a brand new fallback
        // address. The reactive fallback-learn loop never converges for a
        // game this size (dozens of new targets per short play session,
        // observed 2026-11-15) instead of settling into a small fixed set,
        // so recompiling it is a long-running source of instability rather
        // than a path to 0 fallbacks. Run it in the interpreter instead.
        if (rom.sha1 == "afee3b060dd5fd4a68afb1b003456aef3a2af073" ||
            rom.clean_name.find("Mystery Dungeon") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Inserts "interpreter_only = true" right after the [game] table header, unless the content
// already has the key (either value) somewhere.
std::string ensure_interpreter_only(std::string content) {
    if (content.find("interpreter_only") != std::string::npos) {
        return content;
    }
    const auto pos = content.find("[game]");
    if (pos == std::string::npos) {
        return content;
    }
    const auto eol = content.find('\n', pos);
    if (eol == std::string::npos) {
        return content;
    }
    content.insert(eol + 1, "interpreter_only = true\n");
    return content;
}

// Converts a filesystem path into a valid TOML double-quoted string body.
// Uses generic forward slashes so Windows paths never introduce invalid \U or \u escapes.
std::string to_toml_path(const std::filesystem::path& p) {
    std::string s = p.generic_string();
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    return out;
}

// Escapes double quotes and backslashes for TOML string literals.
std::string to_toml_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\\') {
            out += "/";
        } else if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    return out;
}

// Result of running a child process (no shell involved).
struct ProcResult {
    bool spawned = false;  // the process was started (fork+exec succeeded to the point of exec)
    int exit_code = -1;    // process exit status, or -1 if it did not exit normally
    std::string output;    // combined stdout+stderr
};

// Escape a string so it is a valid C string literal body, surrounded by quotes.
// Used to pass -D<macro>="value" safely as a single argv element (no shell).
std::string c_str_literal(const std::string& s) {
#if defined(_WIN32)
    std::string out = "\\\"";
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out += "\\\"";
    return out;
#else
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
#endif
}

// Run a child process with an explicit argument vector — NO shell, so ROM paths
// and titles can never be interpreted as shell metacharacters (fixes the former
// popen injection/quoting bugs). All allocation happens before fork(); the child
// only performs async-signal-safe calls before execvp (fixes the former
// fork()+execv() non-async-signal-safe path). On spawn failure, *err_detail is
// always populated.
#if defined(_WIN32)
ProcResult run_process(const std::vector<std::string>& argv, std::string* err_detail) {
    ProcResult res;
    if (argv.empty()) {
        if (err_detail) {
            *err_detail = "run_process: argv vacio";
        }
        return res;
    }

    std::string cmdline;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0)
            cmdline += " ";
        const std::string& arg = argv[i];
        if (arg.find(' ') != std::string::npos || arg.find('\t') != std::string::npos ||
            arg.empty()) {
            cmdline += "\"" + arg + "\"";
        } else {
            cmdline += arg;
        }
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_read = NULL;
    HANDLE h_write = NULL;
    if (!CreatePipe(&h_read, &h_write, &sa, 0)) {
        if (err_detail)
            *err_detail = "CreatePipe falló";
        return res;
    }
    SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdOutput = h_write;
    si.hStdError = h_write;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    if (!CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        if (err_detail)
            *err_detail = "CreateProcess fallo: error " + std::to_string(GetLastError());
        CloseHandle(h_read);
        CloseHandle(h_write);
        return res;
    }

    CloseHandle(h_write);

    char buf[512];
    DWORD bytes_read = 0;
    while (ReadFile(h_read, buf, sizeof(buf), &bytes_read, NULL) && bytes_read > 0) {
        res.output.append(buf, bytes_read);
    }
    CloseHandle(h_read);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    res.spawned = true;
    res.exit_code = static_cast<int>(exit_code);
    return res;
}
#else
ProcResult run_process(const std::vector<std::string>& argv, std::string* err_detail) {
    ProcResult res;
    if (argv.empty()) {
        if (err_detail) {
            *err_detail = "run_process: argv vacío";
        }
        return res;
    }

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        if (err_detail) {
            *err_detail = std::string("pipe() falló: ") + std::strerror(errno);
        }
        return res;
    }

    // Build the raw argv array before forking so the child allocates nothing.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1U);
    for (const auto& s : argv) {
        cargv.push_back(const_cast<char*>(s.c_str()));
    }
    cargv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        if (err_detail) {
            *err_detail = std::string("fork() falló: ") + std::strerror(errno);
        }
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return res;
    }

    if (pid == 0) {
        // Child: only async-signal-safe calls until exec.
        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }

    close(pipe_fd[1]);
    char buf[512];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0) {
        res.output.append(buf, static_cast<size_t>(n));
    }
    close(pipe_fd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    res.spawned = true;
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}
#endif

// Removes auto-learned GBA entry points that are just consecutive instruction addresses
// (addr - 2 / addr - 4 is also a learned entry). Older runtimes logged every interpreted
// instruction as a fallback, which shattered functions into 1-instruction fragments.
// Returns the number of pruned entries; rewrites the file only when something changed.
size_t sanitize_learned_entry_points(const std::filesystem::path& toml_path,
                                     bool purge_all_learned = false) {
    std::string content;
    if (std::ifstream in(toml_path); in.is_open()) {
        content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    } else {
        return 0U;
    }
    static const std::regex kLearned(
        "\\[\\[entry_points\\]\\]\\r?\\naddr = 0x([0-9A-Fa-f]{8})\\r?\\nthumb = (true|false)\\r?\\n"
        "name = \"Learned_[0-9A-Fa-f]{8}_(thumb|arm)\"\\r?\\n\\r?\\n?");
    std::set<uint32_t> learned;
    for (auto it = std::sregex_iterator(content.begin(), content.end(), kLearned);
         it != std::sregex_iterator(); ++it) {
        learned.insert(static_cast<uint32_t>(std::stoul((*it)[1].str(), nullptr, 16)));
    }
    if (learned.empty()) {
        return 0U;
    }

    std::vector<uint8_t> rom_bytes;
    static const std::regex kRomPath("rom\\s*=\\s*\"([^\"]+)\"");
    std::smatch rom_match;
    if (std::regex_search(content, rom_match, kRomPath)) {
        const std::filesystem::path rpath = rom_match[1].str();
        if (std::ifstream rf(rpath, std::ios::binary); rf.is_open()) {
            rom_bytes.assign((std::istreambuf_iterator<char>(rf)),
                             std::istreambuf_iterator<char>());
        }
    }

    size_t pruned = 0U;
    std::string out;
    out.reserve(content.size());
    auto last = content.cbegin();
    for (auto it = std::sregex_iterator(content.begin(), content.end(), kLearned);
         it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        const uint32_t addr = static_cast<uint32_t>(std::stoul(m[1].str(), nullptr, 16));
        const bool is_thumb = (m[2].str() == "true");

        bool should_prune = purge_all_learned;
        if (!should_prune) {
            if (learned.count(addr - 2U) > 0 || learned.count(addr - 4U) > 0) {
                should_prune = true;
            } else if (!rom_bytes.empty() && addr >= 0x08000000U) {
                const uint32_t off = addr - 0x08000000U;
                if (is_thumb && off + 1 < rom_bytes.size()) {
                    const uint16_t hw = static_cast<uint16_t>(rom_bytes[off]) |
                                        (static_cast<uint16_t>(rom_bytes[off + 1]) << 8);
                    if ((hw & 0xFF00U) == 0xBD00U || (hw & 0xFF00U) == 0xBC00U ||
                        (hw & 0xFF80U) == 0x4700U || (hw & 0xFF87U) == 0x4687U) {
                        should_prune = true;
                    }
                } else if (!is_thumb && off + 3 < rom_bytes.size()) {
                    const uint32_t w = static_cast<uint32_t>(rom_bytes[off]) |
                                       (static_cast<uint32_t>(rom_bytes[off + 1]) << 8) |
                                       (static_cast<uint32_t>(rom_bytes[off + 2]) << 16) |
                                       (static_cast<uint32_t>(rom_bytes[off + 3]) << 24);
                    if ((w & 0x0FFF8000U) == 0x08BD8000U || (w & 0x0FFFFFF0U) == 0x012FFF10U) {
                        should_prune = true;
                    }
                }
            }
        }

        out.append(last, m[0].first);
        if (should_prune) {
            ++pruned;
        } else {
            out.append(m[0].first, m[0].second);
        }
        last = m[0].second;
    }
    out.append(last, content.cend());
    if (pruned > 0U) {
        std::ofstream o(toml_path, std::ios::trunc);
        o << out;
    }
    return pruned;
}

// Returns true if `obj_path` already exists and is at least as new as `c_path`
// and any generated header in `gen_dir`, allowing fast incremental recompilation.
bool is_object_up_to_date(const std::filesystem::path& c_path,
                          const std::filesystem::path& obj_path,
                          const std::filesystem::path& gen_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(obj_path, ec) || std::filesystem::file_size(obj_path, ec) == 0U) {
        return false;
    }
    const auto obj_time = std::filesystem::last_write_time(obj_path, ec);
    if (ec) {
        return false;
    }
    const auto src_time = std::filesystem::last_write_time(c_path, ec);
    if (ec || src_time > obj_time) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(gen_dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".h") {
            const auto h_time = std::filesystem::last_write_time(entry.path(), ec);
            if (!ec && h_time > obj_time) {
                return false;
            }
        }
    }
    return true;
}

bool has_fast_lld_linker(const std::string& cxx_bin) {
    if (cxx_bin.find("clang") == std::string::npos) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path bin_path(cxx_bin);
#if defined(_WIN32)
    if (bin_path.has_parent_path() &&
        std::filesystem::exists(bin_path.parent_path() / "ld.lld.exe", ec)) {
        return true;
    }
    char buf[MAX_PATH];
    return SearchPathA(NULL, "ld.lld", ".exe", sizeof(buf), buf, NULL) > 0;
#else
    if (bin_path.has_parent_path() &&
        std::filesystem::exists(bin_path.parent_path() / "ld.lld", ec)) {
        return true;
    }
    return std::filesystem::exists("/usr/bin/ld.lld", ec) ||
           std::filesystem::exists("/usr/local/bin/ld.lld", ec);
#endif
}

}  // namespace

Pipeline::Pipeline() = default;

Pipeline::~Pipeline() {
    cancel();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (game_monitor_thread_.joinable()) {
        game_monitor_thread_.join();
    }
}

void Pipeline::set_max_compile_threads(unsigned int threads) {
    const unsigned int hw = std::thread::hardware_concurrency();
    const unsigned int upper = (hw > 0U) ? hw : 16U;
    max_compile_threads_.store(std::clamp(threads, 1U, upper));
}

unsigned int Pipeline::max_compile_threads() const {
    return max_compile_threads_.load();
}

void Pipeline::set_compile_opt_level(CompileOptLevel level) {
    compile_opt_level_.store(level);
}

CompileOptLevel Pipeline::compile_opt_level() const {
    return compile_opt_level_.load();
}

void Pipeline::cancel() {
    cancel_requested_ = true;
    int pid = running_game_pid_.load();
    if (pid > 0) {
#if !defined(_WIN32)
        kill(pid, SIGTERM);
#else
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (hProc) {
            TerminateProcess(hProc, 1);
            CloseHandle(hProc);
        }
#endif
    }
}

PipelineStage Pipeline::stage() const {
    return stage_.load();
}

float Pipeline::progress() const {
    return progress_.load();
}

std::string Pipeline::current_step_description() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return step_desc_;
}

std::string Pipeline::error_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_msg_;
}

std::vector<CompileLogEntry> Pipeline::compile_log() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return compile_log_;
}

void Pipeline::append_log(LogLevel level, const std::string& msg) {
    const auto now = std::chrono::steady_clock::now();
    const int total_s = std::max(
        0,
        static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - pipeline_start_time_).count()));
    char tbuf[16];
    std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", total_s / 60, total_s % 60);
    std::lock_guard<std::mutex> lock(mutex_);
    compile_log_.push_back(CompileLogEntry{tbuf, level, msg});
}

std::string Pipeline::last_game_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_game_error_;
}

void Pipeline::clear_last_game_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_game_error_.clear();
}

std::filesystem::path Pipeline::output_binary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_binary_;
}

std::filesystem::path Pipeline::output_binary_windows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return output_binary_windows_;
}

std::filesystem::path Pipeline::workspace_dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workspace_dir_;
}

std::filesystem::path Pipeline::config_file_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return toml_path_;
}

const RecompStats& Pipeline::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool Pipeline::is_game_running() const {
    return running_game_pid_.load() > 0;
}

std::vector<FallbackEvent> Pipeline::learned_fallbacks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return captured_fallbacks_;
}

size_t Pipeline::pending_learn_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return unapplied_fallbacks_.size();
}

bool Pipeline::start_pipeline(const RomInfo& rom) {
    if (stage_.load() == PipelineStage::Analyzing ||
        stage_.load() == PipelineStage::Disassembling || stage_.load() == PipelineStage::Emitting ||
        stage_.load() == PipelineStage::CompilingNative) {
        return false;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    cancel_requested_ = false;
    stage_ = PipelineStage::Analyzing;
    progress_ = 0.05f;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool is_new_rom = (active_rom_.sha1 != rom.sha1);
        active_rom_ = rom;
        if (is_new_rom) {
            captured_fallbacks_.clear();
            unapplied_fallbacks_.clear();
        }
        step_desc_ = "Inicializando entorno de trabajo y analizando ROM...";
        error_msg_.clear();
    }

    worker_thread_ = std::thread(&Pipeline::run_worker, this, rom);
    return true;
}

bool Pipeline::generate_initial_toml(const RomInfo& rom, const std::filesystem::path& toml_path) {
    const auto repo_root = find_repo_root();

    // Check if an existing curated TOML config already exists in nes/games, snes/games, gb/games,
    // or gba/games
    std::string platform_subdir = "nes";
    if (rom.platform == RomPlatform::SNES) {
        platform_subdir = "snes";
    } else if (rom.platform == RomPlatform::GameBoy || rom.platform == RomPlatform::GameBoyColor) {
        platform_subdir = "gb";
    } else if (rom.platform == RomPlatform::GameBoyAdvance) {
        platform_subdir = "gba";
    } else if (rom.platform == RomPlatform::MasterSystem || rom.platform == RomPlatform::GameGear) {
        platform_subdir = "sms";
    }
    const auto games_dir = repo_root / platform_subdir / "games";
    if (std::filesystem::exists(games_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(games_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".toml") {
                try {
                    auto cfg = dragon::load_game_config(entry.path());
                    if (cfg.sha1 == rom.sha1) {
                        std::ifstream in(entry.path());
                        std::string content((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
                        if (requires_interpreter_only(rom)) {
                            content = ensure_interpreter_only(content);
                        }
                        std::ofstream out(toml_path);
                        out << content << "\n";
                        return true;
                    }
                } catch (...) {
                    // Ignore non-matching or parsing errors
                }
            }
        }
    }

    // Check if user persistent knowledge backup already exists for this ROM
    const auto user_game_toml = get_user_knowledge_dir() / "games" / (rom.sha1 + ".toml");
    if (std::filesystem::exists(user_game_toml)) {
        try {
            auto cfg = dragon::load_game_config(user_game_toml);
            if (cfg.sha1 == rom.sha1) {
                std::ifstream in(user_game_toml);
                std::string content((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
                if (requires_interpreter_only(rom)) {
                    content = ensure_interpreter_only(content);
                }
                std::ofstream out(toml_path);
                out << content << "\n";
                return true;
            }
        } catch (...) {
        }
    }

    std::ofstream out(toml_path);
    if (!out.is_open()) {
        return false;
    }

    out << "# DragonRecomp Auto-Generated Game Configuration\n\n";

    const std::string toml_rom_path = to_toml_path(rom.file_path);
    const std::string toml_rom_name = to_toml_string(rom.clean_name);

    if (rom.platform == RomPlatform::NES) {
        out << "[game]\n"
            << "name     = \"" << toml_rom_name << "\"\n"
            << "platform = \"nes\"\n"
            << "rom      = \"" << toml_rom_path << "\"\n"
            << "sha1     = \"" << rom.sha1 << "\"\n"
            << "mapper   = " << rom.mapper << "\n";

        // Check if Super Mario Bros. jump engine applies (only for known SMB1 SHA-1)
        if (rom.sha1 == "ea343f4e445a9050d4b4fbac2c77d0693b1d0922") {
            out << "jump_engine = [0x8E04]\n";
        }

        out << "\n[output]\n"
            << "dir         = \"generated\"\n"
            << "split_every = 200\n\n";
    } else if (rom.platform == RomPlatform::SNES) {
        out << "[game]\n"
            << "name     = \"" << toml_rom_name << "\"\n"
            << "platform = \"snes\"\n"
            << "rom      = \"" << toml_rom_path << "\"\n"
            << "sha1     = \"" << rom.sha1 << "\"\n"
            << "map      = \"" << rom.map_mode_str << "\"\n";

        if (requires_interpreter_only(rom)) {
            out << "interpreter_only = true\n";
        }

        out << "\n[output]\n"
            << "dir         = \"generated\"\n"
            << "split_every = 200\n\n";

        // Only inject known entry points for Super Mario Bros SNES if matching SHA-1
        if (rom.sha1 == "d685be2360730c97e4b57406e62b13919cbada58") {
            out << "[[entry_points]]\n"
                << "addr = 0xFFB0\n"
                << "bank = 0x00\n"
                << "name = \"Reset_Boot\"\n\n"
                << "[[entry_points]]\n"
                << "addr = 0xF000\n"
                << "bank = 0x00\n"
                << "name = \"NMI_Handler\"\n\n"
                << "[[entry_points]]\n"
                << "addr = 0x800D\n"
                << "bank = 0xA0\n"
                << "name = \"Boot_Init\"\n\n"
                << "[[entry_points]]\n"
                << "addr = 0x8000\n"
                << "bank = 0xA1\n"
                << "name = \"Game_Reset_Loop\"\n\n"
                << "[[entry_points]]\n"
                << "addr = 0x8082\n"
                << "bank = 0xA1\n"
                << "name = \"Game_NMI_Body\"\n\n"
                << "[[entry_points]]\n"
                << "addr = 0x8118\n"
                << "bank = 0xA2\n"
                << "name = \"OAM_Copy_A2\"\n\n";
        }
    } else if (rom.platform == RomPlatform::GameBoy || rom.platform == RomPlatform::GameBoyColor) {
        out << "[game]\n"
            << "name     = \"" << toml_rom_name << "\"\n"
            << "platform = \"" << (rom.platform == RomPlatform::GameBoyColor ? "gbc" : "gb")
            << "\"\n"
            << "rom      = \"" << toml_rom_path << "\"\n"
            << "sha1     = \"" << rom.sha1 << "\"\n"
            << "cartridge_type = \"" << rom.mbc_type_str << "\"\n\n"
            << "[output]\n"
            << "dir         = \"generated\"\n"
            << "split_every = 200\n\n"
            << "[[entry_points]]\n"
            << "addr = 0x0100\n"
            << "bank = 0x00\n"
            << "name = \"Reset_Boot\"\n\n"
            << "[[entry_points]]\n"
            << "addr = 0x0040\n"
            << "bank = 0x00\n"
            << "name = \"VBlank_Handler\"\n\n"
            << "[[entry_points]]\n"
            << "addr = 0x0048\n"
            << "bank = 0x00\n"
            << "name = \"STAT_Handler\"\n\n"
            << "[[entry_points]]\n"
            << "addr = 0x0050\n"
            << "bank = 0x00\n"
            << "name = \"Timer_Handler\"\n\n"
            << "[[entry_points]]\n"
            << "addr = 0x0060\n"
            << "bank = 0x00\n"
            << "name = \"Joypad_Handler\"\n\n";
    } else if (rom.platform == RomPlatform::GameBoyAdvance) {
        out << "[game]\n"
            << "name      = \"" << toml_rom_name << "\"\n"
            << "platform  = \"gba\"\n"
            << "rom       = \"" << toml_rom_path << "\"\n"
            << "sha1      = \"" << rom.sha1 << "\"\n"
            << "save_type = \"" << rom.save_type_str << "\"\n";

        if (requires_interpreter_only(rom)) {
            out << "interpreter_only = true\n";
        }

        out << "\n[output]\n"
            << "dir         = \"generated\"\n"
            << "split_every = 200\n\n"
            << "[[entry_points]]\n"
            << "addr  = 0x08000000\n"
            << "thumb = false\n"
            << "name  = \"ROM_Entry\"\n\n";
    } else if (rom.platform == RomPlatform::MasterSystem || rom.platform == RomPlatform::GameGear) {
        out << "[game]\n"
            << "name     = \"" << toml_rom_name << "\"\n"
            << "platform = \"" << (rom.platform == RomPlatform::GameGear ? "gg" : "sms") << "\"\n"
            << "rom      = \"" << toml_rom_path << "\"\n"
            << "sha1     = \"" << rom.sha1 << "\"\n\n"
            << "[output]\n"
            << "dir         = \"generated\"\n"
            << "split_every = 200\n\n"
            << "[[entry_points]]\n"
            << "addr = 0x0000\n"
            << "bank = 0x00\n"
            << "name = \"Reset\"\n\n"
            << "[[entry_points]]\n"
            << "addr = 0x0038\n"
            << "bank = 0x00\n"
            << "name = \"VDP_IRQ\"\n\n"
            << "[[entry_points]]\n"
            << "addr = 0x0066\n"
            << "bank = 0x00\n"
            << "name = \"Pause_NMI\"\n\n";
    }

    return true;
}

bool Pipeline::compile_c_to_binary(const RomInfo& rom, const std::filesystem::path& gen_dir,
                                   const std::filesystem::path& out_bin) {
    const auto repo_root = find_repo_root();
    const auto build_dir = find_build_root();

    std::string runtime_dir = "nes";
    std::string runtime_lib_name = "libnes_runtime.a";
    std::string main_cpp_name = "nes_recomp_game_main.cpp";
    if (rom.platform == RomPlatform::SNES) {
        runtime_dir = "snes";
        runtime_lib_name = "libsnes_runtime.a";
        main_cpp_name = "snes_recomp_game_main.cpp";
    } else if (rom.platform == RomPlatform::GameBoy || rom.platform == RomPlatform::GameBoyColor) {
        runtime_dir = "gb";
        runtime_lib_name = "libgb_runtime.a";
        main_cpp_name = "gb_recomp_game_main.cpp";
    } else if (rom.platform == RomPlatform::GameBoyAdvance) {
        runtime_dir = "gba";
        runtime_lib_name = "libgba_runtime.a";
        main_cpp_name = "gba_recomp_game_main.cpp";
    } else if (rom.platform == RomPlatform::MasterSystem || rom.platform == RomPlatform::GameGear) {
        runtime_dir = "sms";
        runtime_lib_name = "libsms_runtime.a";
        main_cpp_name = "sms_recomp_game_main.cpp";
    }
    const auto main_cpp = repo_root / runtime_dir / "runtime" / "src" / main_cpp_name;
    const auto runtime_inc = repo_root / runtime_dir / "runtime" / "include";
    const auto runtime_archive = build_dir / runtime_dir / "runtime" / runtime_lib_name;
    const auto common_archive = build_dir / "common" / "libdragon_common.a";
    const auto common_inc = repo_root / "common" / "include";

    // 1. Compile each C99 file generated by the recompiler to an object file.
    //    Commands run via run_process (explicit argv, no shell) so paths with
    //    spaces/quotes/metacharacters are passed verbatim.
    std::vector<std::filesystem::path> c_files;
    for (const auto& entry : std::filesystem::directory_iterator(gen_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".c") {
            c_files.push_back(entry.path());
        }
    }
    std::sort(c_files.begin(), c_files.end());

    if (c_files.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg_ = "No se encontraron archivos C generados en " + gen_dir.string();
        return false;
    }

    const auto opt_to_flag = [](CompileOptLevel lvl) -> std::string {
        switch (lvl) {
            case CompileOptLevel::None:
                return "-O0";
            case CompileOptLevel::Fast:
                return "-O1";
            case CompileOptLevel::Balanced:
                return "-O2";
            case CompileOptLevel::Aggressive:
                return "-O3";
        }
        return "-O1";
    };
    const std::string c_opt_flag = opt_to_flag(compile_opt_level_.load());
    const std::string cc_bin = find_c_compiler();
    const std::string cxx_bin = find_cxx_compiler();
    std::vector<std::filesystem::path> obj_files;
    obj_files.reserve(c_files.size());
    for (const auto& c_path : c_files) {
        auto obj_path = c_path;
        obj_path.replace_extension(".o");
        obj_files.push_back(obj_path);
    }

    std::atomic<size_t> next_idx{0};
    std::atomic<size_t> completed_count{0};
    std::atomic<bool> compile_failed{false};
    std::string thread_error_msg;
    std::mutex error_mutex;

    const unsigned int configured_threads = std::max(1U, max_compile_threads_.load());
    const unsigned int thread_count =
        std::max(1U, std::min(configured_threads, static_cast<unsigned int>(c_files.size())));

    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (unsigned int t = 0; t < thread_count; ++t) {
        workers.emplace_back([&]() {
            while (!compile_failed.load() && !cancel_requested_.load()) {
                const size_t idx = next_idx.fetch_add(1);
                if (idx >= c_files.size()) {
                    break;
                }
                const auto& c_path = c_files[idx];
                const auto& obj_path = obj_files[idx];

                if (!is_object_up_to_date(c_path, obj_path, gen_dir)) {
                    const std::vector<std::string> cc_argv = {
                        cc_bin,
                        c_opt_flag,
                        "-std=c99",
                        "-c",
                        c_path.string(),
                        "-o",
                        obj_path.string(),
                        "-I" + gen_dir.string(),
                        "-I" + common_inc.string(),
                        "-I" + runtime_inc.string(),
                    };

                    std::string spawn_err;
                    const ProcResult r = run_process(cc_argv, &spawn_err);
                    if (!r.spawned) {
                        compile_failed.store(true);
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (thread_error_msg.empty()) {
#if defined(_WIN32)
                            thread_error_msg = "No se pudo ejecutar Clang/GCC (" + cc_bin +
                                               "): " + spawn_err +
                                               "\nAsegurate de que LLVM/Clang o MinGW este "
                                               "instalado y en el PATH.";
#else
                            thread_error_msg =
                                "No se pudo ejecutar Clang/GCC (" + cc_bin + "): " + spawn_err +
                                "\nAsegurate de que Clang o GCC este instalado en tu sistema.";
#endif
                        }
                        break;
                    }
                    if (r.exit_code != 0) {
                        compile_failed.store(true);
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (thread_error_msg.empty()) {
                            thread_error_msg = "Fallo compilando C99 (" +
                                               c_path.filename().string() + "):\n" + r.output;
                        }
                        break;
                    }
                }

                const size_t done = completed_count.fetch_add(1) + 1;
                if (c_files.size() > 1U) {
                    progress_ = 0.60f + 0.30f * (static_cast<float>(done) /
                                                 static_cast<float>(c_files.size()));
                    std::lock_guard<std::mutex> lock(mutex_);
                    step_desc_ = "Compilando modulo C99 (" + std::to_string(done) + "/" +
                                 std::to_string(c_files.size()) + ", " +
                                 std::to_string(thread_count) + " nucleos)...";
                }
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }

    if (cancel_requested_.load()) {
        return false;
    }

    if (compile_failed.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg_ = thread_error_msg;
        return false;
    }

    // 2. Compile game runner C++20 source. The -D string-literal macros are
    //    passed as single argv elements with proper C-literal escaping.
    const auto main_obj = gen_dir / "game_main.o";
    const std::vector<std::string> cpp_argv = {
        cxx_bin,
        c_opt_flag,
        "-std=c++20",
        "-c",
        main_cpp.string(),
        "-o",
        main_obj.string(),
        "-I" + gen_dir.string(),
        "-I" + common_inc.string(),
        "-I" + runtime_inc.string(),
        "-DDRAGON_GAME_TITLE=" + c_str_literal(rom.clean_name),
        "-DDRAGON_DEFAULT_ROM_PATH=" + c_str_literal(rom.file_path.generic_string()),
        "-DDRAGON_DEFAULT_ROM_SHA1=" + c_str_literal(rom.sha1),
    };

    std::string spawn_err;
    const ProcResult r_cpp = run_process(cpp_argv, &spawn_err);
    if (!r_cpp.spawned) {
        std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
        error_msg_ = "No se pudo ejecutar Clang++ (" + cxx_bin + "): " + spawn_err +
                     "\nAsegurate de que LLVM/Clang este instalado y en el PATH (descarga desde "
                     "https://releases.llvm.org).";
#else
        error_msg_ = "No se pudo ejecutar Clang++/G++ (" + cxx_bin + "): " + spawn_err +
                     "\nAsegurate de que Clang++ o G++ este instalado en tu sistema.";
#endif
        return false;
    }
    if (r_cpp.exit_code != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg_ = "Fallo compilando runner C++:\n" + r_cpp.output;
        return false;
    }

    const bool use_lld = has_fast_lld_linker(cxx_bin);
#if defined(_WIN32)
    // 3. Link all object files into the final native PE (.exe).
    // -mwindows ensures the executable runs as a pure GUI application without opening a terminal
    // window.
    std::vector<std::string> link_argv = {cxx_bin, c_opt_flag, "-mwindows"};
    if (use_lld) {
        link_argv.push_back("-fuse-ld=lld");
    }
    for (const auto& obj : obj_files) {
        link_argv.push_back(obj.string());
    }
    link_argv.push_back(main_obj.string());
    link_argv.push_back(runtime_archive.string());
    link_argv.push_back(common_archive.string());
    link_argv.push_back("-lSDL3");
    link_argv.push_back("-o");
    link_argv.push_back(out_bin.string());
#else
    // 3. Link all object files into the final native ELF.
    std::vector<std::string> link_argv = {cxx_bin, c_opt_flag};
    if (use_lld) {
        link_argv.push_back("-fuse-ld=lld");
    }
    for (const auto& obj : obj_files) {
        link_argv.push_back(obj.string());
    }
    link_argv.push_back(main_obj.string());
    link_argv.push_back("-Wl,--start-group");
    link_argv.push_back(runtime_archive.string());
    link_argv.push_back(common_archive.string());
    link_argv.push_back("-Wl,--end-group");
    link_argv.push_back("-Wl,-rpath,$ORIGIN/lib:$ORIGIN");
    link_argv.push_back("-lSDL3");
    link_argv.push_back("-lm");
    link_argv.push_back("-lpthread");
    link_argv.push_back("-o");
    link_argv.push_back(out_bin.string());
#endif

    const ProcResult r_link = run_process(link_argv, &spawn_err);
    if (!r_link.spawned) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg_ = "No se pudo ejecutar el enlazador: " + spawn_err;
        return false;
    }
    if (r_link.exit_code != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg_ = "Fallo enlazando binario nativo:\n" + r_link.output;
        return false;
    }

#if defined(_WIN32)
    // Ensure SDL3.dll is present next to the output binary so Windows can load it
    const auto target_sdl_dll = out_bin.parent_path() / "SDL3.dll";
    if (!std::filesystem::exists(target_sdl_dll)) {
        const std::vector<std::filesystem::path> sdl_candidates = {
            "C:\\Users\\devel\\tools\\SDL3-3.4.20\\x86_64-w64-mingw32\\bin\\SDL3.dll",
            find_repo_root() / "build" / "SDL3.dll",
            get_executable_dir() / "SDL3.dll",
        };
        for (const auto& candidate : sdl_candidates) {
            if (std::filesystem::exists(candidate)) {
                std::error_code ec;
                std::filesystem::copy_file(candidate, target_sdl_dll,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                break;
            }
        }
    }

    if (!std::filesystem::exists(out_bin) && out_bin.extension() != ".exe") {
        auto out_exe = out_bin;
        out_exe.replace_extension(".exe");
        if (std::filesystem::exists(out_exe)) {
            std::error_code ec;
            std::filesystem::copy_file(out_exe, out_bin,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            return true;
        }
    }
#endif
    return std::filesystem::exists(out_bin);
}

bool Pipeline::compile_windows_binary(const RomInfo& rom, const std::filesystem::path& gen_dir,
                                      const std::filesystem::path& out_bin) {
#if defined(_WIN32)
    return compile_c_to_binary(rom, gen_dir, out_bin);
#else
    // Cross-compilation attempt using mingw-w64 on Linux
    std::string test_err;
    const ProcResult check = run_process({"which", "x86_64-w64-mingw32-g++"}, &test_err);
    if (!check.spawned || check.exit_code != 0) {
        return false;
    }

    const auto repo_root = find_repo_root();
    std::string runtime_dir = "nes";
    std::string main_cpp_name = "nes_recomp_game_main.cpp";
    if (rom.platform == RomPlatform::SNES) {
        runtime_dir = "snes";
        main_cpp_name = "snes_recomp_game_main.cpp";
    } else if (rom.platform == RomPlatform::GameBoy || rom.platform == RomPlatform::GameBoyColor) {
        runtime_dir = "gb";
        main_cpp_name = "gb_recomp_game_main.cpp";
    } else if (rom.platform == RomPlatform::GameBoyAdvance) {
        runtime_dir = "gba";
        main_cpp_name = "gba_recomp_game_main.cpp";
    } else if (rom.platform == RomPlatform::MasterSystem || rom.platform == RomPlatform::GameGear) {
        runtime_dir = "sms";
        main_cpp_name = "sms_recomp_game_main.cpp";
    }
    const auto main_cpp = repo_root / runtime_dir / "runtime" / "src" / main_cpp_name;
    const auto runtime_inc = repo_root / runtime_dir / "runtime" / "include";
    const auto common_inc = repo_root / "common" / "include";

    std::vector<std::filesystem::path> obj_files;
    for (const auto& entry : std::filesystem::directory_iterator(gen_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".c") {
            auto obj_path = entry.path();
            obj_path.replace_extension(".obj");

            const std::vector<std::string> cc_argv = {
                "x86_64-w64-mingw32-gcc",
                "-O2",
                "-std=c99",
                "-c",
                entry.path().string(),
                "-o",
                obj_path.string(),
                "-I" + gen_dir.string(),
                "-I" + common_inc.string(),
                "-I" + runtime_inc.string(),
            };

            std::string spawn_err;
            const ProcResult r = run_process(cc_argv, &spawn_err);
            if (!r.spawned || r.exit_code != 0) {
                return false;
            }
            obj_files.push_back(obj_path);
        }
    }

    const auto main_obj = gen_dir / "game_main.obj";
    const std::vector<std::string> cpp_argv = {
        "x86_64-w64-mingw32-g++",
        "-O2",
        "-std=c++20",
        "-c",
        main_cpp.string(),
        "-o",
        main_obj.string(),
        "-I" + gen_dir.string(),
        "-I" + common_inc.string(),
        "-I" + runtime_inc.string(),
        "-DDRAGON_GAME_TITLE=" + c_str_literal(rom.clean_name),
        "-DDRAGON_DEFAULT_ROM_PATH=" + c_str_literal(rom.file_path.generic_string()),
        "-DDRAGON_DEFAULT_ROM_SHA1=" + c_str_literal(rom.sha1),
    };

    std::string spawn_err;
    const ProcResult r_cpp = run_process(cpp_argv, &spawn_err);
    if (!r_cpp.spawned || r_cpp.exit_code != 0) {
        return false;
    }

    // game_main.cpp resolves the ROM via dragon::resolve_rom_path (see
    // */runtime/src/*_recomp_game_main.cpp) instead of trusting the baked-in
    // DRAGON_DEFAULT_ROM_PATH -- cross-compile its dependencies (sha1, file
    // I/O, the SHA-1 ROM search) directly, since this path does not link the
    // prebuilt native libdragon_common.a (that one is built for the host
    // triple, not mingw).
    const std::filesystem::path common_src = repo_root / "common" / "src";
    std::vector<std::filesystem::path> common_objs;
    for (const char* src_name : {"rom_locator.cpp", "sha1.cpp", "file_io.cpp"}) {
        const auto src_path = common_src / src_name;
        auto obj_path = gen_dir / src_name;
        obj_path.replace_extension(".obj");
        const std::vector<std::string> common_argv = {
            "x86_64-w64-mingw32-g++", "-O2", "-std=c++20",      "-c",
            src_path.string(),        "-o",  obj_path.string(), "-I" + common_inc.string(),
        };
        const ProcResult r_common = run_process(common_argv, &spawn_err);
        if (!r_common.spawned || r_common.exit_code != 0) {
            return false;
        }
        common_objs.push_back(obj_path);
    }

    std::vector<std::string> link_argv = {"x86_64-w64-mingw32-g++", "-O2", "-mwindows"};
    for (const auto& obj : obj_files) {
        link_argv.push_back(obj.string());
    }
    link_argv.push_back(main_obj.string());
    for (const auto& obj : common_objs) {
        link_argv.push_back(obj.string());
    }
    link_argv.push_back("-lSDL3");
    link_argv.push_back("-o");
    link_argv.push_back(out_bin.string());

    const ProcResult r_link = run_process(link_argv, &spawn_err);
    return (r_link.spawned && r_link.exit_code == 0 && std::filesystem::exists(out_bin));
#endif
}

void Pipeline::run_worker(RomInfo rom) {
    const auto start_time = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pipeline_start_time_ = start_time;
        compile_log_.clear();
    }
    append_log(LogLevel::Info, "ROM detectada: " + rom.platform_name());
    if (rom.platform == RomPlatform::SNES) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Checksum: $%04X (%s)", rom.checksum,
                      rom.fast_rom ? "FastROM" : "SlowROM");
        append_log(LogLevel::Message, buf);
    } else if (rom.platform == RomPlatform::NES) {
        append_log(LogLevel::Message, "Mapper " + std::to_string(rom.mapper) + " (iNES)");
    } else if (rom.platform == RomPlatform::GameBoyAdvance) {
        append_log(LogLevel::Message, "GBA " + (rom.game_code.empty() ? "AGB" : rom.game_code) +
                                          " (" + rom.save_type_str + ")");
    } else if (rom.platform == RomPlatform::MasterSystem || rom.platform == RomPlatform::GameGear) {
        append_log(LogLevel::Message,
                   std::string(rom.platform == RomPlatform::GameGear ? "Game Gear" : "Master System") +
                       " (" + std::to_string(rom.rom_size_kb) + " KB)");
    } else {
        append_log(LogLevel::Message,
                   rom.mbc_type_str + " (" + std::to_string(rom.rom_size_kb) + " KB)");
    }

    // 1. Prepare cache directory
    const auto cache_base = find_repo_root() / "build" / "studio_cache" / rom.sha1;
    try {
        std::filesystem::create_directories(cache_base);
    } catch (const std::exception& e) {
        stage_ = PipelineStage::Failed;
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg_ = "Error al crear directorio de trabajo: " + std::string(e.what());
        return;
    }

    workspace_dir_ = cache_base;
    toml_path_ = cache_base / "game.toml";
    const auto gen_dir = cache_base / "generated";
#if defined(_WIN32)
    const auto out_bin = cache_base / "game_binary.exe";
#else
    const auto out_bin = cache_base / "game_binary";
#endif

    bool need_gen = !std::filesystem::exists(toml_path_);
    if (!need_gen) {
        try {
            dragon::load_game_config(toml_path_);
        } catch (...) {
            need_gen = true;
        }
    }

    if (need_gen) {
        if (!generate_initial_toml(rom, toml_path_)) {
            stage_ = PipelineStage::Failed;
            std::lock_guard<std::mutex> lock(mutex_);
            error_msg_ = "Error al generar configuracion inicial TOML";
            return;
        }
    }

    if (cancel_requested_.load())
        return;

    // 2. Disassemble & Emit C99
    std::error_code ec_clean;
    std::filesystem::remove_all(gen_dir, ec_clean);
    std::filesystem::create_directories(gen_dir, ec_clean);

    stage_ = PipelineStage::Disassembling;
    progress_ = 0.25f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        step_desc_ = "Desensamblando vectores y analizando flujo de control...";
    }
    append_log(LogLevel::Message, "Iniciando desensamblado recursivo...");

    std::string tool_name = "nesrecomp";
    if (rom.platform == RomPlatform::SNES) {
        tool_name = "snesrecomp";
    } else if (rom.platform == RomPlatform::GameBoy || rom.platform == RomPlatform::GameBoyColor) {
        tool_name = "gbrecomp";
    } else if (rom.platform == RomPlatform::GameBoyAdvance) {
        tool_name = "gbarecomp";
    } else if (rom.platform == RomPlatform::MasterSystem || rom.platform == RomPlatform::GameGear) {
        tool_name = "smsrecomp";
    }
    const auto tool_path = find_tool(tool_name);

    std::vector<std::string> recomp_argv = {
        tool_path.string(), toml_path_.string(), "--rom", rom.file_path.string(), "--out",
        gen_dir.string(),
    };
    if (rom.platform != RomPlatform::GameBoyAdvance) {
        recomp_argv.push_back("--single-file");
    }

    stage_ = PipelineStage::Emitting;
    progress_ = 0.50f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        step_desc_ = "Generando codigo C99 optimizado (" + tool_name + ")...";
    }

    std::string recomp_spawn_err;
    const ProcResult recomp = run_process(recomp_argv, &recomp_spawn_err);
    if (!recomp.spawned) {
        stage_ = PipelineStage::Failed;
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg_ =
            "No se pudo ejecutar el recompilador (" + tool_path.string() + "): " + recomp_spawn_err;
        return;
    }

    size_t funcs_found = 0;
    size_t instrs_found = 0;
    {
        std::istringstream iss(recomp.output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("Functions:") != std::string::npos) {
                std::sscanf(line.c_str(), "%*s %zu", &funcs_found);
            } else if (line.find("Instructions:") != std::string::npos) {
                std::sscanf(line.c_str(), "%*s %zu", &instrs_found);
            }
        }
    }
    if (recomp.exit_code != 0) {
        stage_ = PipelineStage::Failed;
        std::lock_guard<std::mutex> lock(mutex_);
        error_msg_ = "Error durante el desensamblado:\n" + recomp.output;
        return;
    }

    if (cancel_requested_.load())
        return;

    // 3. Native C compilation
    stage_ = PipelineStage::CompilingNative;
    progress_ = 0.75f;
    {
        std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
        step_desc_ = "Compilando binario nativo Windows x86_64 (.exe)...";
#else
        step_desc_ = "Compilando binario nativo Linux x86_64 con Clang...";
#endif
    }
    append_log(LogLevel::Info, std::to_string(max_compile_threads_.load()) + " nucleos asignados");

    if (!compile_c_to_binary(rom, gen_dir, out_bin)) {
        stage_ = PipelineStage::Failed;
        return;
    }

#if defined(_WIN32)
    // On Windows, out_bin is already the native Windows PE (.exe) binary.
    // Also copy it with the clean game name for convenience and set output_binary_windows_.
    const auto win_bin = cache_base / (rom.clean_name + ".exe");
    std::error_code ec_cp;
    std::filesystem::copy_file(out_bin, win_bin, std::filesystem::copy_options::overwrite_existing,
                               ec_cp);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_binary_windows_ = std::filesystem::exists(win_bin) ? win_bin : out_bin;
    }
#else
    // Attempt Windows compilation if cross-compiler or native environment is ready
    const auto win_bin = cache_base / (rom.clean_name + ".exe");
    if (compile_windows_binary(rom, gen_dir, win_bin)) {
        std::lock_guard<std::mutex> lock(mutex_);
        output_binary_windows_ = win_bin;
    }
#endif

    const auto end_time = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    // 4. Success
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_binary_ = out_bin;
        stats_.functions = funcs_found;
        stats_.instructions = instrs_found;
        stats_.compilation_time_sec = elapsed;
        step_desc_ =
            "Recompilacion completada con exito en " + std::to_string(elapsed).substr(0, 4) + "s";
    }

    progress_ = 1.0f;
    stage_ = PipelineStage::Ready;
    append_log(LogLevel::Success, "Compilacion completada");
    append_log(LogLevel::Success,
               funcs_found == 0 ? "Modo interprete activo" : "Codigo 100% optimizado");

    if (relaunch_after_compile_.exchange(false)) {
        launch_game(last_fullscreen_, last_crt_, last_autosave_);
    }
}

void Pipeline::monitor_game_process(int pid, int stderr_fd, RomPlatform platform) {
    char buf[1024];
    std::string line_accum;
    std::string captured_err;
    std::regex fb_regex(
        R"(FALLBACK addr=\$([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4}) mode=([0-9A-Fa-f]{2}) from=\$([0-9A-Fa-f]{6}))");
    std::regex nes_fb_regex(
        R"(FALLBACK addr=\$([0-9A-Fa-f]{4}) bank=\$([0-9A-Fa-f]{2}) from=\$([0-9A-Fa-f]{4}))");
    std::regex gba_fb_regex(
        R"(FALLBACK addr=\$([0-9A-Fa-f]{8}) mode=([0-9A-Fa-f]{2}) from=\$([0-9A-Fa-f]{8}))");

    while (true) {
        ssize_t n = read(stderr_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            break;
        }
        buf[n] = '\0';
        captured_err += buf;
        line_accum += buf;

        size_t pos;
        while ((pos = line_accum.find('\n')) != std::string::npos) {
            std::string line = line_accum.substr(0, pos);
            line_accum.erase(0, pos + 1);

            std::smatch match;
            if (platform == RomPlatform::SNES && std::regex_search(line, match, fb_regex)) {
                FallbackEvent ev;
                ev.bank = static_cast<uint8_t>(std::stoul(match[1].str(), nullptr, 16));
                ev.addr = static_cast<uint16_t>(std::stoul(match[2].str(), nullptr, 16));
                ev.mode = static_cast<uint8_t>(std::stoul(match[3].str(), nullptr, 16));
                ev.from_addr = static_cast<uint32_t>(std::stoul(match[4].str(), nullptr, 16));

                std::lock_guard<std::mutex> lock(mutex_);
                captured_fallbacks_.push_back(ev);
                unapplied_fallbacks_.push_back(ev);
            } else if ((platform == RomPlatform::NES || platform == RomPlatform::GameBoy ||
                        platform == RomPlatform::GameBoyColor) &&
                       std::regex_search(line, match, nes_fb_regex)) {
                FallbackEvent ev;
                ev.addr = static_cast<uint16_t>(std::stoul(match[1].str(), nullptr, 16));
                ev.bank = static_cast<uint8_t>(std::stoul(match[2].str(), nullptr, 16));
                ev.from_addr = static_cast<uint32_t>(std::stoul(match[3].str(), nullptr, 16));

                std::lock_guard<std::mutex> lock(mutex_);
                captured_fallbacks_.push_back(ev);
                unapplied_fallbacks_.push_back(ev);
            } else if (platform == RomPlatform::GameBoyAdvance &&
                       std::regex_search(line, match, gba_fb_regex)) {
                FallbackEvent ev;
                ev.addr = static_cast<uint32_t>(std::stoul(match[1].str(), nullptr, 16));
                ev.mode = static_cast<uint8_t>(std::stoul(match[2].str(), nullptr, 16));
                ev.from_addr = static_cast<uint32_t>(std::stoul(match[3].str(), nullptr, 16));

                std::lock_guard<std::mutex> lock(mutex_);
                captured_fallbacks_.push_back(ev);
                unapplied_fallbacks_.push_back(ev);
            }
        }
    }
    close(stderr_fd);

#if !defined(_WIN32)
    int status = 0;
    waitpid(pid, &status, 0);
    running_game_pid_.store(-1);

    std::string clean_err;
    std::istringstream iss(captured_err);
    std::string line_str;
    while (std::getline(iss, line_str)) {
        while (!line_str.empty() && (line_str.back() == '\r' || line_str.back() == '\n')) {
            line_str.pop_back();
        }
        if (line_str.empty() || line_str.rfind("FALLBACK", 0) == 0 ||
            line_str.rfind("[DragonRecomp]", 0) == 0 || line_str.rfind("info:", 0) == 0) {
            continue;
        }
        clean_err += line_str + "\n";
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!clean_err.empty()) {
            last_game_error_ = clean_err;
        } else if (!captured_err.empty()) {
            last_game_error_ = captured_err;
        } else {
            last_game_error_ = "El juego termino con codigo " + std::to_string(WEXITSTATUS(status));
        }
    }
#else
    DWORD exit_code = 0;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                               static_cast<DWORD>(pid));
    if (hProc) {
        WaitForSingleObject(hProc, 5000);
        GetExitCodeProcess(hProc, &exit_code);
        CloseHandle(hProc);
    }
    running_game_pid_.store(-1);

    std::string clean_err;
    std::istringstream iss(captured_err);
    std::string line_str;
    while (std::getline(iss, line_str)) {
        while (!line_str.empty() && (line_str.back() == '\r' || line_str.back() == '\n')) {
            line_str.pop_back();
        }
        if (line_str.empty() || line_str.rfind("FALLBACK", 0) == 0 ||
            line_str.rfind("[DragonRecomp]", 0) == 0 || line_str.rfind("info:", 0) == 0) {
            continue;
        }
        clean_err += line_str + "\n";
    }

    if (exit_code != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!clean_err.empty()) {
            last_game_error_ = clean_err;
        } else {
            last_game_error_ = "El juego termino con codigo " + std::to_string(exit_code);
        }
    }
#endif
}

void Pipeline::stop_game() {
    pid_t pid = running_game_pid_.exchange(-1);
    if (pid > 0) {
#if !defined(_WIN32)
        kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            if (waitpid(pid, &status, WNOHANG) != 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, WNOHANG);
#else
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (hProc) {
            TerminateProcess(hProc, 1);
            WaitForSingleObject(hProc, 500);
            CloseHandle(hProc);
        }
#endif
    }
    if (game_monitor_thread_.joinable()) {
        game_monitor_thread_.join();
    }
}

bool Pipeline::launch_game(bool fullscreen, bool crt_filter, bool auto_save_state) {
    if (stage_.load() != PipelineStage::Ready) {
        return false;
    }
    if (running_game_pid_.load() > 0) {
        return false;
    }

    last_fullscreen_ = fullscreen;
    last_crt_ = crt_filter;
    last_autosave_ = auto_save_state;

    const auto bin = output_binary();
    if (!std::filesystem::exists(bin)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_game_error_.clear();
    }

    // Build argv and read config BEFORE fork() so the child performs only
    // async-signal-safe calls (no heap allocation, no std::filesystem) between
    // fork and execv.
    std::vector<std::string> arg_strings;
    arg_strings.push_back(bin.string());
    arg_strings.push_back("--rom");
    arg_strings.push_back(std::filesystem::absolute(active_rom_.file_path).string());
    if (fullscreen) {
        arg_strings.push_back("--fullscreen");
    }
    if (crt_filter) {
        arg_strings.push_back("--crt");
    }
    if (auto_save_state && !workspace_dir_.empty()) {
        arg_strings.push_back("--save-state");
        arg_strings.push_back((workspace_dir_ / "autosave.state").string());
    }
    try {
        if (std::filesystem::exists(toml_path_)) {
            auto cfg = dragon::load_game_config(toml_path_);
            if (!cfg.palette.empty()) {
                arg_strings.push_back("--palette");
                arg_strings.push_back(cfg.palette);
            }
        }
    } catch (...) {
    }

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(arg_strings.size() + 1);
    for (auto& s : arg_strings) {
        argv_ptrs.push_back(s.data());
    }
    argv_ptrs.push_back(nullptr);

    const RomPlatform platform = active_rom_.platform;

#if !defined(_WIN32)
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return false;
    }

    if (pid == 0) {
        // Child: async-signal-safe calls only.
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDERR_FILENO);
        close(pipe_fd[1]);
        execv(bin.c_str(), argv_ptrs.data());
        _exit(127);
    }

    // Parent process
    close(pipe_fd[1]);
    running_game_pid_.store(pid);

    if (game_monitor_thread_.joinable()) {
        game_monitor_thread_.join();
    }
    game_monitor_thread_ =
        std::thread(&Pipeline::monitor_game_process, this, pid, pipe_fd[0], platform);
    return true;
#else
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_read = NULL;
    HANDLE h_write = NULL;
    if (!CreatePipe(&h_read, &h_write, &sa, 0)) {
        return false;
    }
    SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdError = h_write;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::string cmdline = "\"" + bin.string() + "\"";
    for (size_t i = 1; i < arg_strings.size(); ++i) {
        cmdline += " \"" + arg_strings[i] + "\"";
    }
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    if (!CreateProcessA(NULL, cmd_buf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(h_read);
        CloseHandle(h_write);
        return false;
    }
    CloseHandle(h_write);
    running_game_pid_.store(static_cast<int>(pi.dwProcessId));

    if (game_monitor_thread_.joinable()) {
        game_monitor_thread_.join();
    }
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h_read), 0);
    game_monitor_thread_ = std::thread(&Pipeline::monitor_game_process, this,
                                       static_cast<int>(pi.dwProcessId), fd, platform);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#endif
}

bool Pipeline::optimize_and_recompile() {
    std::vector<FallbackEvent> to_apply;
    RomInfo current_rom;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (unapplied_fallbacks_.empty()) {
            return false;
        }
        to_apply = unapplied_fallbacks_;
        unapplied_fallbacks_.clear();
        current_rom = active_rom_;
    }

    // Structural dedup: parse the existing config and collect (bank, addr) of
    // already-defined entry points. A text search over the whole TOML (the
    // previous approach) matched the address in any table and ignored the bank,
    // dropping valid entries that shared a 16-bit address in a different bank.
    std::set<std::tuple<int, uint32_t, bool>> existing_eps;
    try {
        const auto cfg = dragon::load_game_config(toml_path_);
        for (const auto& ep : cfg.entry_points) {
            existing_eps.insert({ep.bank, ep.addr, ep.thumb});
        }
    } catch (...) {
        // Unparseable / missing config: treat as no existing entries.
    }

    auto already_present = [&](const FallbackEvent& ev) {
        const bool is_thumb = (ev.mode != 0U);
        for (const auto& [bank, addr, thumb] : existing_eps) {
            if (current_rom.platform == RomPlatform::GameBoyAdvance) {
                if (addr == ev.addr && thumb == is_thumb) {
                    return true;
                }
            } else {
                if (addr == ev.addr && (bank < 0 || bank == static_cast<int>(ev.bank))) {
                    return true;
                }
            }
        }
        return false;
    };

    // Deduplicate within this batch and filter out invalid/RAM entry points.
    std::set<std::tuple<uint32_t, uint8_t, uint8_t>> unique_eps;
    std::vector<FallbackEvent> valid_eps;

    std::vector<uint8_t> gba_rom_bytes;
    if (current_rom.platform == RomPlatform::GameBoyAdvance && !current_rom.file_path.empty()) {
        if (std::ifstream rf(current_rom.file_path, std::ios::binary); rf.is_open()) {
            gba_rom_bytes.assign((std::istreambuf_iterator<char>(rf)),
                                 std::istreambuf_iterator<char>());
        }
    }

    for (const auto& ev : to_apply) {
        if (current_rom.platform == RomPlatform::NES) {
            // NES entry points must be in ROM address space ($8000-$FFFF)
            if (ev.addr < 0x8000U) {
                continue;
            }
            // Bank index semantics depend on the mapper's bank size:
            //   mappers 1/2 report 16 KiB bank indices, mapper 4 reports 8 KiB,
            //   mappers 0/3 have no PRG banking (only bank 0). See
            //   Bus::current_prg_bank (nes/runtime/src/bus.cpp).
            uint32_t bank_kb = 0U;
            switch (current_rom.mapper) {
                case 1:
                case 2:
                    bank_kb = 16U;
                    break;
                case 4:
                    bank_kb = 8U;
                    break;
                default:
                    bank_kb = 0U;
                    break;
            }
            uint32_t total_banks;
            if (bank_kb == 0U) {
                total_banks = 1U;  // mappers 0/3: only bank 0 is valid
            } else if (current_rom.prg_size_kb > 0) {
                total_banks = std::max<uint32_t>(
                    1U, static_cast<uint32_t>(current_rom.prg_size_kb / bank_kb));
            } else {
                total_banks = 256U;  // unknown PRG size: stay permissive
            }
            if (ev.bank >= total_banks) {
                continue;
            }
        } else if (current_rom.platform == RomPlatform::GameBoy ||
                   current_rom.platform == RomPlatform::GameBoyColor) {
            // Game Boy entry points must be in ROM address space ($0000-$7FFF)
            if (ev.addr >= 0x8000U) {
                continue;
            }
        } else if (current_rom.platform == RomPlatform::MasterSystem ||
                   current_rom.platform == RomPlatform::GameGear) {
            // SMS/GG entry points must be in ROM address space ($0000-$BFFF)
            if (ev.addr >= 0xC000U) {
                continue;
            }
        } else if (current_rom.platform == RomPlatform::GameBoyAdvance) {
            // GBA entry points must be in Game Pak ROM address space ($08000000-$0DFFFFFF)
            if (ev.addr < 0x08000000U || ev.addr >= 0x0E000000U) {
                continue;
            }
            const uint32_t off = ev.addr - 0x08000000U;
            if (current_rom.file_size > 0 && off >= current_rom.file_size) {
                continue;
            }
            if (!gba_rom_bytes.empty() && off + 3 < gba_rom_bytes.size()) {
                const bool is_thumb = (ev.mode != 0U);
                if (is_thumb) {
                    const uint16_t hw = static_cast<uint16_t>(gba_rom_bytes[off]) |
                                        (static_cast<uint16_t>(gba_rom_bytes[off + 1]) << 8);
                    if ((hw & 0xFF00U) == 0xBD00U || (hw & 0xFF00U) == 0xBC00U ||
                        (hw & 0xFF80U) == 0x4700U || (hw & 0xFF87U) == 0x4687U) {
                        continue;
                    }
                } else {
                    const uint32_t w = static_cast<uint32_t>(gba_rom_bytes[off]) |
                                       (static_cast<uint32_t>(gba_rom_bytes[off + 1]) << 8) |
                                       (static_cast<uint32_t>(gba_rom_bytes[off + 2]) << 16) |
                                       (static_cast<uint32_t>(gba_rom_bytes[off + 3]) << 24);
                    if ((w & 0x0FFF8000U) == 0x08BD8000U || (w & 0x0FFFFFF0U) == 0x012FFF10U) {
                        continue;
                    }
                }
            }
        } else {
            // SNES entry points: skip low RAM ($0000-$1FFF in bank $00-$3F/$80-$BF) or WRAM
            // ($7E-$7F)
            if (ev.bank >= 0x7EU) {
                continue;
            }
            if ((ev.bank < 0x40U || (ev.bank >= 0x80U && ev.bank < 0xC0U)) && ev.addr < 0x2000U) {
                continue;
            }
        }

        auto key = std::make_tuple(ev.addr, ev.bank, ev.mode);
        if (unique_eps.insert(key).second && !already_present(ev)) {
            valid_eps.push_back(ev);
        }
    }

    if (valid_eps.empty()) {
        // Nothing new or valid to recompile
        return true;
    }

    // Append newly learned entry points to TOML
    std::ofstream out(toml_path_, std::ios::app);
    if (!out.is_open()) {
        return false;
    }

    // Emit only addr/bank/name — the config schema's [[entry_points]] does NOT
    // accept a 'mode' key (the loader throws on unknown keys, which previously
    // made every SNES recompile fail and discarded the learned entries). The
    // recompiler re-derives the M/X width by static tracing from each seed, as
    // it already does for every hand-written entry point.
    out << "\n# --- Auto-Learned Entry Points from Gameplay ---\n";
    for (const auto& ev : valid_eps) {
        char buf[160];
        if (current_rom.platform == RomPlatform::GameBoyAdvance) {
            const bool is_thumb = (ev.mode != 0U);
            std::snprintf(
                buf, sizeof(buf),
                "[[entry_points]]\naddr = 0x%08X\nthumb = %s\nname = \"Learned_%08X_%s\"\n\n",
                ev.addr, is_thumb ? "true" : "false", ev.addr, is_thumb ? "thumb" : "arm");
        } else {
            std::snprintf(
                buf, sizeof(buf),
                "[[entry_points]]\naddr = 0x%04X\nbank = 0x%02X\nname = \"Learned_%02X_%04X\"\n\n",
                ev.addr, ev.bank, ev.bank, ev.addr);
        }
        out << buf;
    }
    out.close();

    // Backup to user persistent knowledge directory
    try {
        const auto user_game_toml =
            get_user_knowledge_dir() / "games" / (current_rom.sha1 + ".toml");
        std::filesystem::create_directories(user_game_toml.parent_path());
        std::filesystem::copy_file(toml_path_, user_game_toml,
                                   std::filesystem::copy_options::overwrite_existing);
    } catch (...) {
    }

    // Trigger recompile with cleaned config
    return start_pipeline(active_rom_);
}

DiagnosticResult Pipeline::diagnose_and_repair(DiagnosticSymptom symptom) {
    DiagnosticResult res;
    RomInfo current_rom;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_rom = active_rom_;
    }

    if (!current_rom.is_valid()) {
        res.success = false;
        res.summary = "No hay ninguna ROM activa para diagnosticar.";
        return res;
    }

    // Honest diagnostics: report only the action actually taken and whether it
    // actually started. Many symptoms have no safe automatic repair; for those
    // we say so plainly instead of claiming a non-existent hardware fix.
    auto pipeline_busy = [this]() {
        const auto s = stage_.load();
        return s == PipelineStage::Analyzing || s == PipelineStage::Disassembling ||
               s == PipelineStage::Emitting || s == PipelineStage::CompilingNative;
    };

    const bool was_running = is_game_running();
    if (was_running) {
        stop_game();
    }

    const auto cache_base = find_repo_root() / "build" / "studio_cache" / current_rom.sha1;
    const auto toml_path = cache_base / "game.toml";

    switch (symptom) {
        case DiagnosticSymptom::FreezeOnBoot:
        case DiagnosticSymptom::FreezeInGame: {
            if (current_rom.platform == RomPlatform::GameBoyAdvance &&
                symptom == DiagnosticSymptom::FreezeOnBoot) {
                const size_t pruned = sanitize_learned_entry_points(toml_path, true);
                if (pruned > 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    captured_fallbacks_.clear();
                    unapplied_fallbacks_.clear();
                    try {
                        const auto user_game_toml =
                            get_user_knowledge_dir() / "games" / (current_rom.sha1 + ".toml");
                        std::filesystem::copy_file(
                            toml_path, user_game_toml,
                            std::filesystem::copy_options::overwrite_existing);
                    } catch (...) {
                    }
                    std::error_code ec;
                    std::filesystem::remove_all(cache_base / "generated", ec);
                    relaunch_after_compile_ = true;
                    start_pipeline(current_rom);
                    res.success = pipeline_busy();
                    res.summary =
                        "Se purgaron " + std::to_string(pruned) +
                        " rutinas aprendidas sospechosas y se inicio recompilacion limpia.";
                    res.details =
                        "Si el juego continua congelandose al arrancar, vuelve a intentar reparar "
                        "para activar el modo interprete exacto.";
                    break;
                }
            }

            // A freeze in a game that otherwise boots usually means the
            // recompiled code diverged from cycle-exact behaviour — typically an
            // interrupt handler that uses a stack trick / depends on cycle-exact
            // interrupt timing a block recompiler cannot reproduce. Fall back to
            // the cycle-accurate interpreter for this title by setting
            // interpreter_only in its config, then recompile from scratch.
            bool changed = false;
            bool already = false;
            if ((current_rom.platform == RomPlatform::NES ||
                 current_rom.platform == RomPlatform::SNES ||
                 current_rom.platform == RomPlatform::GameBoy ||
                 current_rom.platform == RomPlatform::GameBoyColor ||
                 current_rom.platform == RomPlatform::GameBoyAdvance) &&
                std::filesystem::exists(toml_path)) {
                std::string content;
                if (std::ifstream in(toml_path); in.is_open()) {
                    content.assign((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
                }
                if (content.find("interpreter_only") != std::string::npos) {
                    already = true;
                } else if (auto pos = content.find("[game]"); pos != std::string::npos) {
                    if (auto eol = content.find('\n', pos); eol != std::string::npos) {
                        content.insert(eol + 1, "interpreter_only = true\n");
                        std::ofstream out(toml_path);
                        out << content;
                        changed = true;
                    }
                }
            }
            if (changed) {
                std::error_code ec;
                std::filesystem::remove_all(cache_base / "generated", ec);
                relaunch_after_compile_ = true;
                start_pipeline(current_rom);
                res.success = pipeline_busy();
                res.summary = res.success
                                  ? "Juego cambiado a modo interprete (preciso a ciclo) y "
                                    "recompilando."
                                  : "No se pudo iniciar la recompilacion (pipeline ocupado).";
                res.details =
                    "Algunos juegos dependen de un temporizado exacto de "
                    "interrupciones (p. ej. trucos de pila en el NMI) que la "
                    "recompilacion no reproduce; el interprete los ejecuta con "
                    "exactitud.";
            } else if (already) {
                res.success = false;
                res.summary =
                    "El juego ya esta en modo interprete; el congelamiento no es por "
                    "la recompilacion.";
                res.details = "Si persiste, reporta el titulo como incidencia.";
            } else {
                res.success = false;
                res.summary = "No se pudo ajustar la configuracion del juego.";
            }
            break;
        }

        case DiagnosticSymptom::GrayScreen:
        case DiagnosticSymptom::NoSound:
        case DiagnosticSymptom::DistortedSound: {
            const bool purge_all_gba = (current_rom.platform == RomPlatform::GameBoyAdvance &&
                                        (symptom == DiagnosticSymptom::DistortedSound ||
                                         symptom == DiagnosticSymptom::NoSound));
            if (purge_all_gba) {
                std::lock_guard<std::mutex> lock(mutex_);
                captured_fallbacks_.clear();
                unapplied_fallbacks_.clear();
            }
            const size_t pruned = sanitize_learned_entry_points(toml_path, purge_all_gba);
            if (purge_all_gba && pruned > 0) {
                try {
                    const auto user_game_toml =
                        get_user_knowledge_dir() / "games" / (current_rom.sha1 + ".toml");
                    std::filesystem::copy_file(toml_path, user_game_toml,
                                               std::filesystem::copy_options::overwrite_existing);
                } catch (...) {
                }
            }
            const size_t pending_before = pending_learn_count();
            if (pending_before > 0 && !purge_all_gba) {
                relaunch_after_compile_ = true;
                optimize_and_recompile();
                if (pipeline_busy()) {
                    res.success = true;
                    res.summary = "Se integraron " + std::to_string(pending_before) +
                                  " rutina(s) aprendida(s) y se inicio la recompilacion.";
                    res.details =
                        "Las subrutinas descubiertas durante la partida se compilan "
                        "ahora como funciones nativas.";
                } else {
                    res.success = false;
                    res.summary =
                        "Las rutinas aprendidas ya estaban integradas; no habia nada "
                        "nuevo que recompilar.";
                }
            } else if (pruned > 0) {
                // Si habia puntos de entrada aprendidos corruptos o fragmentados en la
                // configuracion, purgarlos y recompilar limpiamente desde cero.
                std::error_code ec;
                std::filesystem::remove_all(cache_base / "generated", ec);
                relaunch_after_compile_ = true;
                start_pipeline(current_rom);
                res.success = pipeline_busy();
                res.summary =
                    res.success
                        ? "Se purgaron " + std::to_string(pruned) +
                              " punto(s) aprendidos corruptos del motor de sonido y se inicio la "
                              "recompilacion limpia."
                        : "No se pudo iniciar la recompilacion (pipeline ocupado).";
                res.details = "El motor de audio se recompila intacto sin funciones fragmentadas.";
            } else {
                res.success = false;
                res.summary = "No hay una reparacion automatica para este sintoma.";
                res.details =
                    "Juega unos segundos para que el auto-aprendizaje capture las "
                    "rutinas no compiladas y luego pulsa 'Optimizar y Recompilar'.";
            }
            break;
        }

        case DiagnosticSymptom::WrongColors: {
            bool changed = false;
            if ((current_rom.platform == RomPlatform::GameBoy ||
                 current_rom.platform == RomPlatform::GameBoyColor) &&
                std::filesystem::exists(toml_path)) {
                std::string content;
                if (std::ifstream in(toml_path); in.is_open()) {
                    content.assign((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
                }
                if (content.find("classic_palette = true") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("classic_palette = true"),
                                                 "classic_palette = false");
                    res.summary = "Paleta cambiada a escala de grises moderna.";
                    changed = true;
                } else if (content.find("classic_palette = false") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("classic_palette = false"),
                                                 "classic_palette = true");
                    res.summary = "Paleta cambiada a DMG verde clasico.";
                    changed = true;
                } else if (auto pos = content.find("[output]"); pos != std::string::npos) {
                    if (auto eol = content.find('\n', pos); eol != std::string::npos) {
                        content.insert(eol + 1, "classic_palette = true\n");
                        res.summary = "Paleta cambiada a DMG verde clasico.";
                        changed = true;
                    }
                }
                if (changed) {
                    std::ofstream out(toml_path);
                    out << content;
                    std::error_code ec;
                    std::filesystem::remove_all(cache_base / "generated", ec);
                    relaunch_after_compile_ = true;
                    start_pipeline(current_rom);
                    res.success = pipeline_busy();
                    res.details = "Paleta DMG alternada en la configuracion del juego.";
                }
            } else if (current_rom.platform == RomPlatform::NES &&
                       std::filesystem::exists(toml_path)) {
                std::string content;
                if (std::ifstream in(toml_path); in.is_open()) {
                    content.assign((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
                }
                if (content.find("palette = \"vs0002\"") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("palette = \"vs0002\""),
                                                 "palette = \"vs0001\"");
                    res.summary = "Paleta cambiada a RP2C04-0001 (Vs. System).";
                } else if (content.find("palette = \"vs0001\"") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("palette = \"vs0001\""),
                                                 "palette = \"vs0003\"");
                    res.summary = "Paleta cambiada a RP2C04-0003 (Vs. System).";
                } else if (content.find("palette = \"vs0003\"") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("palette = \"vs0003\""),
                                                 "palette = \"vs0004\"");
                    res.summary = "Paleta cambiada a RP2C04-0004 (Vs. System).";
                } else if (content.find("palette = \"vs0004\"") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("palette = \"vs0004\""),
                                                 "palette = \"vs2c03\"");
                    res.summary = "Paleta cambiada a RP2C03B RGB.";
                } else if (content.find("palette = \"vs2c03\"") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("palette = \"vs2c03\""),
                                                 "palette = \"smooth\"");
                    res.summary = "Paleta cambiada a Smooth FBX.";
                } else if (content.find("palette = \"smooth\"") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("palette = \"smooth\""),
                                                 "palette = \"2c02\"");
                    res.summary = "Paleta restaurada a NTSC 2C02 estandar.";
                } else if (content.find("palette = \"2c02\"") != std::string::npos) {
                    content = std::regex_replace(content, std::regex("palette = \"2c02\""),
                                                 "palette = \"vs0002\"");
                    res.summary = "Paleta cambiada a RP2C04-0002 (Vs. Battle City / Castlevania).";
                } else {
                    const std::string pal_choice = (current_rom.mapper == 99) ? "vs0001" : "smooth";
                    auto pos = content.find("[output]");
                    if (pos != std::string::npos) {
                        content.insert(pos, "palette = \"" + pal_choice + "\"\n\n");
                    } else {
                        content += "\npalette = \"" + pal_choice + "\"\n";
                    }
                    res.summary = "Paleta establecida a " + pal_choice + ".";
                }
                std::ofstream out(toml_path);
                out << content;
                changed = true;
            }
            if (changed) {
                relaunch_after_compile_ = true;
                start_pipeline(current_rom);
                res.success = pipeline_busy();
                if (!res.success) {
                    res.summary += " (no se pudo recompilar: pipeline ocupado)";
                }
                res.details = "Se roto el perfil de paleta PPU y se recompilo.";
            } else {
                res.success = false;
                res.summary = "No hay un ajuste de paleta aplicable para esta ROM.";
            }
            break;
        }

        case DiagnosticSymptom::CrashOnLaunch: {
            if (current_rom.platform == RomPlatform::GameBoyAdvance) {
                sanitize_learned_entry_points(toml_path, true);
                std::lock_guard<std::mutex> lock(mutex_);
                captured_fallbacks_.clear();
                unapplied_fallbacks_.clear();
            }
            std::error_code ec;
            std::filesystem::remove_all(cache_base / "generated", ec);
            relaunch_after_compile_ = true;
            start_pipeline(current_rom);
            res.success = pipeline_busy();
            res.summary = res.success
                              ? "Recompilacion limpia iniciada desde cero (intermedios y rutinas "
                                "dudosas purgados)."
                              : "No se pudo iniciar la recompilacion (pipeline ocupado).";
            break;
        }

        case DiagnosticSymptom::ControlsUnresponsive: {
            res.success = false;
            if (current_rom.mapper == 99) {
                res.summary =
                    "En modo arcade, inserta una moneda con C, 5 o Select (no es un fallo).";
            } else {
                res.summary = "No hay una reparacion automatica para este sintoma.";
                res.details =
                    "Verifica el mapeo de teclado/mando; la lectura de $4016/$4017 es estandar.";
            }
            break;
        }

        case DiagnosticSymptom::BlueScreen:
        case DiagnosticSymptom::CorruptGraphics:
        case DiagnosticSymptom::AbnormalSpeed: {
            res.success = false;
            res.summary = "No hay una reparacion automatica para este sintoma.";
            res.details =
                "Este sintoma no tiene un ajuste automatico seguro; revisa la "
                "configuracion del juego o reportalo como incidencia.";
            break;
        }
    }

    // If no recompilation was started, relaunch the game we stopped so the user
    // is not left with a closed window.
    if (!pipeline_busy()) {
        relaunch_after_compile_ = false;
        if (was_running) {
            launch_game(last_fullscreen_, last_crt_);
        }
    }

    // Persist an HONEST record of the attempted action (not a claimed repair).
    try {
        const auto user_game_toml =
            get_user_knowledge_dir() / "games" / (current_rom.sha1 + ".toml");
        std::filesystem::create_directories(user_game_toml.parent_path());
        if (std::filesystem::exists(toml_path)) {
            std::filesystem::copy_file(toml_path, user_game_toml,
                                       std::filesystem::copy_options::overwrite_existing);
        }
        std::string fb_name = "nes_feedback.toml";
        if (current_rom.platform == RomPlatform::SNES) {
            fb_name = "snes_feedback.toml";
        } else if (current_rom.platform == RomPlatform::GameBoy ||
                   current_rom.platform == RomPlatform::GameBoyColor) {
            fb_name = "gb_feedback.toml";
        } else if (current_rom.platform == RomPlatform::GameBoyAdvance) {
            fb_name = "gba_feedback.toml";
        } else if (current_rom.platform == RomPlatform::MasterSystem ||
                   current_rom.platform == RomPlatform::GameGear) {
            fb_name = "sms_feedback.toml";
        }
        const auto console_toml = get_user_knowledge_dir() / fb_name;
        std::ofstream fb(console_toml, std::ios::app);
        if (fb.is_open()) {
            fb << "\n# Diagnostico para " << current_rom.clean_name
               << " (SHA-1: " << current_rom.sha1 << ")\n"
               << "accion = \"" << res.summary << "\"\n"
               << "exito  = " << (res.success ? "true" : "false") << "\n";
        }
    } catch (...) {
    }

    return res;
}

}  // namespace dragon::studio
