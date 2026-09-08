// The per-session debug report. See include/debug_report.h for what it is for.
//
// Three mechanisms, in order of how much they can be trusted when things go
// wrong:
//
//   1. Errors and crashes are formatted here and written straight to the file,
//      flushed on every line. Nothing else has to be working.
//   2. Everything the process prints to stdout or stderr is mirrored into the
//      file. This is what makes a report contain RT64's and librecomp's own
//      messages rather than only the ones this port remembered to record, and
//      it is the difference between "the game crashed" and "the game crashed
//      four seconds after the renderer reported a device loss".
//   3. The header records the build and the machine once, at the start.
//
// The mirror works by replacing file descriptors 1 and 2 with the write end of
// a pipe and pumping the read end on a thread, which is why (1) deliberately
// does not go through it: a crashing process may never drain that pipe again.

#include "debug_report.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <intrin.h>
#endif

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;
// Serialises whole crash blocks; see crash_begin.
std::mutex g_crash_mutex;
std::FILE* g_file = nullptr;
fs::path g_path;
std::chrono::steady_clock::time_point g_start;
std::time_t g_start_wall = 0;
std::string g_id;

bool g_header_open = true;                 // header lines still allowed
std::atomic<int> g_errors{0};
std::atomic<int> g_warnings{0};
std::atomic<int> g_crashes{0};

// The real console, saved before the mirror takes fds 1 and 2 over. Crash
// output goes here so it still reaches a terminal.
int g_real_stderr = -1;

#ifdef _WIN32
HANDLE g_pipe_read = nullptr;
HANDLE g_pipe_write = nullptr;
int g_saved_stdout = -1;
int g_saved_stderr = -1;
std::thread g_pump;
std::atomic<bool> g_pump_stop{false};
#endif

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string timestamp_since_start() {
    using namespace std::chrono;
    const auto elapsed = steady_clock::now() - g_start;
    const auto ms = duration_cast<milliseconds>(elapsed).count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(ms / 3600000),
                  static_cast<long long>((ms / 60000) % 60),
                  static_cast<long long>((ms / 1000) % 60),
                  static_cast<long long>(ms % 1000));
    return buf;
}

std::string wall_clock(std::time_t when) {
    char buf[64];
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &when);
#else
    localtime_r(&when, &tm);
#endif
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// A short session id. It goes in the filename so two reports from the same
// minute cannot collide, and in the header so a player can name the one they
// mean without reading a path out loud.
std::string make_session_id() {
    unsigned value = 0;
#ifdef _WIN32
    value = static_cast<unsigned>(GetCurrentProcessId());
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    value ^= static_cast<unsigned>(counter.QuadPart) * 2654435761u;
#else
    value = static_cast<unsigned>(std::time(nullptr)) * 2654435761u;
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", value);
    return buf;
}

// Caller holds g_mutex.
void write_line(const char* level, const char* category, const char* text) {
    if (g_file == nullptr) {
        return;
    }
    if (g_header_open) {
        std::fprintf(g_file,
                     "\nEVENTS\n"
                     "  Every line is:  [time since launch]  LEVEL  category: message\n"
                     "  LOG lines are what the port and its libraries printed, mirrored\n"
                     "  here as they happened; a mirrored line that names a failure is\n"
                     "  raised to ERROR, so the summary at the end cannot undercount.\n"
                     "  CRASH blocks are written by the port itself and survive\n"
                     "  anything short of the process being killed outright.\n"
                     "%s\n",
                     std::string(72, '-').c_str());
        g_header_open = false;
    }
    std::fprintf(g_file, "[%s] %-5s %s: %s\n",
                 timestamp_since_start().c_str(), level, category, text);
    std::fflush(g_file);
}

void write_event(const char* level, const char* category, const char* fmt, va_list args) {
    char text[2048];
    std::vsnprintf(text, sizeof(text), fmt, args);
    std::lock_guard<std::mutex> lock(g_mutex);
    write_line(level, category, text);
}

// ---------------------------------------------------------------------------
// The header: build and machine, recorded once
// ---------------------------------------------------------------------------

#ifdef _WIN32
std::string registry_string(const char* value) {
    char buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        return buf;
    }
    return {};
}

std::string cpu_brand() {
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004u) {
        return "unknown";
    }
    for (unsigned leaf = 0; leaf < 3; leaf++) {
        __cpuid(regs, 0x80000002 + leaf);
        std::memcpy(brand + leaf * 16, regs, 16);
    }
    // Intel pads the brand string with leading spaces.
    const char* start = brand;
    while (*start == ' ') {
        start++;
    }
    return start;
}
#endif

// When this BINARY was linked, read out of its own PE header.
//
// __DATE__ and __TIME__ were the obvious thing and are a trap: they are baked
// into this translation unit's object file, so they stop moving as soon as this
// file stops changing, while the executable around it goes on being relinked.
// A report from a fresh build then claims an old timestamp, and anyone
// symbolising a crash against "the build named in the report" is working from
// the wrong binary and gets confident, plausible, wrong function names. That
// happened, and cost a correct diagnosis.
//
// The PE header's TimeDateStamp is written by the linker, so it moves whenever
// the executable does.
std::string build_stamp() {
#ifdef _WIN32
    const HMODULE module = GetModuleHandleW(nullptr);
    if (module != nullptr) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                const std::time_t linked = static_cast<std::time_t>(nt->FileHeader.TimeDateStamp);
                return wall_clock(linked) + " (from the executable's PE header)";
            }
        }
    }
#endif
    return std::string(__DATE__) + " " + __TIME__ + " (this file's compile time -- may be older than the build)";
}

void write_header(const std::string& port_version) {
    std::fprintf(g_file,
        "================================================================\n"
        " Rayman 2: The Great Escape - Recompiled\n"
        " Session debug report\n"
        "================================================================\n"
        "\n"
        "WHAT THIS FILE IS\n"
        "  One file per play session, written as the session happens. It\n"
        "  records everything the port printed, plus every error and every\n"
        "  crash it detected.\n"
        "\n"
        "  If something went wrong, send this whole file rather than a quote\n"
        "  from it -- the blocks below usually answer the questions that would\n"
        "  otherwise have to be asked, and the last few lines before an ERROR\n"
        "  are often the ones that matter. It is written to be read equally\n"
        "  well by a person and by an AI assistant; you can hand it to either.\n"
        "\n"
        "  The SUMMARY at the end says whether the session finished cleanly.\n"
        "  If the file has no SUMMARY, the port was killed or crashed hard\n"
        "  before it could write one, and that is itself worth reporting.\n"
        "\n"
        "  Nothing here identifies you. It holds your hardware, your Windows\n"
        "  version, and the paths the port itself uses.\n");

    std::fprintf(g_file,
        "\nSESSION\n"
        "  id                 %s\n"
        "  started            %s (local time)\n"
        "  report file        %s\n",
        g_id.c_str(), wall_clock(g_start_wall).c_str(), g_path.string().c_str());

    std::fprintf(g_file,
        "\nBUILD\n"
        "  port version       %s\n"
        "  frontend           %s\n"
        "  configuration      %s\n"
        "  linked             %s\n",
        port_version.c_str(),
#ifdef RAYMAN2_ENABLE_FRONTEND
        "enabled (launcher and config menus)",
#else
        "disabled (headless build)",
#endif
#ifdef NDEBUG
        "release",
#else
        "debug",
#endif
        build_stamp().c_str());

#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);

    SYSTEM_INFO sysinfo{};
    GetNativeSystemInfo(&sysinfo);

    const std::string product = registry_string("ProductName");
    const std::string display = registry_string("DisplayVersion");
    const std::string build   = registry_string("CurrentBuild");

    std::fprintf(g_file,
        "\nSYSTEM\n"
        "  os                 %s %s (build %s)\n"
        "  cpu                %s\n"
        "  logical cores      %lu\n"
        "  physical memory    %llu MB\n",
        product.empty() ? "Windows" : product.c_str(),
        display.c_str(), build.c_str(),
        cpu_brand().c_str(),
        static_cast<unsigned long>(sysinfo.dwNumberOfProcessors),
        static_cast<unsigned long long>(memory.ullTotalPhys / (1024 * 1024)));
#endif

    // Everything add_context() records lands here, under a heading of its own,
    // rather than trailing the machine description and reading as part of it.
    std::fprintf(g_file, "\nGAME\n");
    std::fflush(g_file);
}

// ---------------------------------------------------------------------------
// The stdout/stderr mirror
// ---------------------------------------------------------------------------

// Decide what level a mirrored line is.
//
// Most of what the port and its libraries print is progress, and belongs at
// LOG. But some of it is a genuine failure that only ever existed as a printed
// string -- a renderer that could not create a resource, a file that would not
// open -- and if those are filed as ordinary output then the summary at the end
// of the report says "0 errors" while the log plainly contains one. A report
// that undercounts is worse than one that occasionally over-counts, because the
// summary is the first thing anyone reads.
//
// The test is deliberately narrow. It matches whole words, so "Driver Version"
// and "no errors" do not trip it, and the line is recorded verbatim either way
// -- the level changes what a reader is drawn to, never what was said.
const char* classify(const std::string& line) {
    static const char* const failures[] = { "error", "failed", "failure", "fatal",
                                            "exception", "assert", "cannot", "unable" };
    static const char* const cautions[] = { "warning", "warn" };

    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto contains_word = [&lower](const char* word) {
        const size_t len = std::strlen(word);
        for (size_t at = lower.find(word); at != std::string::npos;
             at = lower.find(word, at + 1)) {
            const bool left = (at == 0) || !std::isalpha(static_cast<unsigned char>(lower[at - 1]));
            const size_t end = at + len;
            const bool right = (end >= lower.size())
                            || !std::isalpha(static_cast<unsigned char>(lower[end]));
            if (left && right) {
                return true;
            }
        }
        return false;
    };

    for (const char* word : failures) {
        if (contains_word(word)) {
            g_errors.fetch_add(1);
            return "ERROR";
        }
    }
    for (const char* word : cautions) {
        if (contains_word(word)) {
            g_warnings.fetch_add(1);
            return "WARN";
        }
    }
    return "LOG";
}

#ifdef _WIN32
void pump_thread() {
    std::string partial;
    char buf[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(g_pipe_read, buf, sizeof(buf), &read, nullptr) || read == 0) {
            break;
        }
        // Echo to the real console first, so a player watching a terminal sees
        // output at the same time the file does.
        if (g_saved_stdout >= 0) {
            _write(g_saved_stdout, buf, static_cast<unsigned>(read));
        }

        partial.append(buf, read);
        size_t start = 0;
        for (;;) {
            const size_t nl = partial.find('\n', start);
            if (nl == std::string::npos) {
                break;
            }
            std::string line = partial.substr(start, nl - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                std::lock_guard<std::mutex> lock(g_mutex);
                write_line(classify(line), "out", line.c_str());
            }
            start = nl + 1;
        }
        partial.erase(0, start);

        // A library that prints a long line without a newline should not be
        // able to grow this without bound.
        if (partial.size() > 64 * 1024) {
            std::lock_guard<std::mutex> lock(g_mutex);
            write_line(classify(partial), "out", partial.c_str());
            partial.clear();
        }
    }
    if (!partial.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        write_line(classify(partial), "out", partial.c_str());
    }
}

// Replace fds 1 and 2 with a pipe and pump it into the report.
//
// Deliberately best-effort: every failure path leaves the process printing
// normally and simply without a mirror, because a debug report is not worth
// losing the program's output over. RAYMAN2_NO_MIRROR=1 disables it outright,
// which is the escape hatch if it is ever suspected of causing trouble during
// a playtest.
void start_mirror() {
    if (std::getenv("RAYMAN2_NO_MIRROR") != nullptr) {
        return;
    }

    // A generous buffer: the writer blocks when the pipe is full, and the
    // writer here is the game.
    if (!CreatePipe(&g_pipe_read, &g_pipe_write, nullptr, 1 << 20)) {
        return;
    }

    g_saved_stdout = _dup(_fileno(stdout));
    g_saved_stderr = _dup(_fileno(stderr));

    const int pipe_fd = _open_osfhandle(reinterpret_cast<intptr_t>(g_pipe_write), _O_WRONLY);
    if (pipe_fd < 0) {
        CloseHandle(g_pipe_read);
        CloseHandle(g_pipe_write);
        g_pipe_read = g_pipe_write = nullptr;
        return;
    }

    _dup2(pipe_fd, _fileno(stdout));
    _dup2(pipe_fd, _fileno(stderr));
    _close(pipe_fd);

    // Unbuffered, or the mirror only sees output in 4 KB bursts and the
    // timestamps stop meaning anything.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    g_pump = std::thread(pump_thread);
}

void stop_mirror() {
    if (g_pipe_read == nullptr) {
        return;
    }
    // Restoring the descriptors closes the last write handle to the pipe, which
    // is what lets the pump's ReadFile return zero and the thread finish.
    if (g_saved_stdout >= 0) {
        _dup2(g_saved_stdout, _fileno(stdout));
    }
    if (g_saved_stderr >= 0) {
        _dup2(g_saved_stderr, _fileno(stderr));
    }
    if (g_pump.joinable()) {
        g_pump.join();
    }
    CloseHandle(g_pipe_read);
    g_pipe_read = nullptr;
    g_pipe_write = nullptr;
}
#else
void start_mirror() {}
void stop_mirror() {}
#endif

// ---------------------------------------------------------------------------
// Housekeeping
// ---------------------------------------------------------------------------

// Keep the newest reports and delete the rest. A playtester should never have
// to think about this folder, and an unbounded one eventually becomes a reason
// not to look in it.
constexpr int kKeepReports = 50;

void prune(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> reports;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!ec && entry.is_regular_file(ec) && entry.path().extension() == ".txt") {
            reports.push_back(entry.path());
        }
    }
    if (static_cast<int>(reports.size()) <= kKeepReports) {
        return;
    }
    // The filenames start with the timestamp, so lexical order is chronological.
    std::sort(reports.begin(), reports.end());
    for (size_t i = 0; i + kKeepReports < reports.size(); i++) {
        fs::remove(reports[i], ec);
    }
}

bool directory_is_writable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path probe = dir / ".write-test";
    std::FILE* f = std::fopen(probe.string().c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    std::fclose(f);
    fs::remove(probe, ec);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

namespace rayman2::report {

void begin_session(const fs::path& preferred_dir, const fs::path& fallback_dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        return;
    }

    g_start = std::chrono::steady_clock::now();
    g_start_wall = std::time(nullptr);
    g_id = make_session_id();

    // Next to the executable first, because that is where a player will look.
    // An install under Program Files is not writable, so fall back to the same
    // per-user folder the rest of the configuration lives in.
    fs::path dir = preferred_dir / "debug-report";
    if (!directory_is_writable(dir)) {
        dir = fallback_dir / "debug-report";
        if (!directory_is_writable(dir)) {
            return;
        }
    }

    char stamp[32];
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &g_start_wall);
#else
    localtime_r(&g_start_wall, &tm);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", &tm);

    g_path = dir / (std::string(stamp) + "-" + g_id + ".txt");
    g_file = std::fopen(g_path.string().c_str(), "w");
    if (g_file == nullptr) {
        return;
    }

    write_header("0.2.0-alpha");
    prune(dir);

#ifdef _WIN32
    g_real_stderr = _dup(_fileno(stderr));
#endif
    start_mirror();
}

void end_session(bool clean) {
    stop_mirror();

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file == nullptr) {
        return;
    }

    const std::time_t now = std::time(nullptr);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - g_start).count();

    std::fprintf(g_file,
        "\n%s\n"
        "SUMMARY\n"
        "  ended              %s (local time)\n"
        "  ran for            %lldm %llds\n"
        "  errors             %d\n"
        "  warnings           %d\n"
        "  crashes            %d\n"
        "  finished           %s\n",
        std::string(72, '-').c_str(),
        wall_clock(now).c_str(),
        static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60),
        g_errors.load(), g_warnings.load(), g_crashes.load(),
        clean ? "cleanly" : "after a crash");

    if (g_errors.load() == 0 && g_crashes.load() == 0) {
        std::fprintf(g_file,
            "\n  Nothing went wrong in this session. If you are reporting a\n"
            "  problem that happened anyway -- something looked wrong, sounded\n"
            "  wrong, or did not respond -- say what you were doing and roughly\n"
            "  when, and the LOG lines above will still be worth reading.\n");
    }

    std::fclose(g_file);
    g_file = nullptr;
}

fs::path path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path;
}

void add_context(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file == nullptr) {
        return;
    }
    if (g_header_open) {
        std::fprintf(g_file, "  %-18s %s\n", key.c_str(), value.c_str());
        std::fflush(g_file);
    }
    else {
        write_line("INFO", key.c_str(), value.c_str());
    }
}

void info(const char* category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write_event("INFO", category, fmt, args);
    va_end(args);
}

void warn(const char* category, const char* fmt, ...) {
    g_warnings.fetch_add(1);
    va_list args;
    va_start(args, fmt);
    write_event("WARN", category, fmt, args);
    va_end(args);
}

void error(const char* category, const char* fmt, ...) {
    g_errors.fetch_add(1);
    va_list args;
    va_start(args, fmt);
    write_event("ERROR", category, fmt, args);
    va_end(args);
}

void crash_begin(const char* kind) {
    g_crashes.fetch_add(1);

    // Hold this for the whole block, not just this line.
    //
    // Two threads faulting at once produced a report whose two crashes were
    // shuffled line by line into each other -- stack frames from one appearing
    // in the middle of the other's -- which is close to unreadable and exactly
    // when readability matters most. The lock is released in crash_end.
    g_crash_mutex.lock();

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        write_line("CRASH", "crash", "================ BEGIN CRASH REPORT ================");
        std::fprintf(g_file, "  kind: %s\n", kind);
        std::fflush(g_file);
    }
    char line[256];
    const int n = std::snprintf(line, sizeof(line), "\n[rayman2] CRASH (%s)\n", kind);
#ifdef _WIN32
    if (g_real_stderr >= 0) {
        _write(g_real_stderr, line, n);
    }
#else
    (void)n;
#endif
}

void crash_line(const char* fmt, ...) {
    char text[2048];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fprintf(g_file, "  %s\n", text);
        std::fflush(g_file);
    }
#ifdef _WIN32
    if (g_real_stderr >= 0 && n > 0) {
        _write(g_real_stderr, "  ", 2);
        _write(g_real_stderr, text, static_cast<unsigned>(n));
        _write(g_real_stderr, "\n", 1);
    }
#else
    (void)n;
#endif
}

void crash_end() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file != nullptr) {
    std::fprintf(g_file,
        "  \n"
        "  What to do with this: send the whole file. The lines above the\n"
        "  BEGIN marker are what the game was doing in the seconds before it\n"
        "  died, and they are usually more informative than the crash itself.\n"
        "  \n"
        "  The addresses read as module+offset. To turn one into a function\n"
        "  name, against the exact build named in the BUILD block above:\n"
        "  \n"
        "      llvm-symbolizer --obj=rayman2-recomp.exe --relative-address\n"
        "                      --demangle <offset>\n"
        "  \n"
        "  That needs a build made with debug information\n"
        "  (-DCMAKE_BUILD_TYPE=RelWithDebInfo); a plain release build resolves\n"
        "  to the nearest exported symbol only.\n");
            write_line("CRASH", "crash", "================= END CRASH REPORT =================");
        }
    }
    g_crash_mutex.unlock();
}

int error_count() { return g_errors.load(); }
int crash_count() { return g_crashes.load(); }

} // namespace rayman2::report
