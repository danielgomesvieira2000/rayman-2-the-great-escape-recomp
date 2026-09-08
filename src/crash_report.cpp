// Turn a silent crash into a located one.
//
// The port died with exit code 0xC0000005 and no output, on a thread that is
// not main(). Without a debugger installed that is almost no information: the
// exit code says "access violation" and nothing about where.
//
// This installs an unhandled-exception filter that prints the exception, the
// faulting address, a stack walk resolved to module + offset, and -- for an
// access violation -- what the OS thinks of the address that faulted. Module
// and offset answers "whose code is this"; VirtualQuery answers "what is
// actually mapped there", which is the question that reasoning from the source
// cannot settle.
//
// Build with -DCMAKE_BUILD_TYPE=RelWithDebInfo and resolve the offsets with:
//     llvm-symbolizer --obj=rayman2-recomp.exe --relative-address --demangle <rva>
//
// It deliberately does NOT use dbghelp/SymFromAddr: that pulls in a dependency
// to print names we can already recover offline, and dbghelp is not thread-safe,
// which is a poor property for something that only runs on a thread already in
// trouble.
//
// The one thing this cannot catch is fail-fast (0xC0000409), which Windows
// raises without running exception filters. That is the code an exception
// crossing a noexcept boundary produces, and it is why the frontend's throws
// were so opaque earlier in this phase.

#ifdef _WIN32

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <windows.h>

#include "librecomp/overlays.hpp"

#include "debug_report.h"

namespace {

void describe_address(void* addr, char* out, size_t out_size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &module)
        && module != nullptr) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);

        const char* name = path;
        for (const char* p = path; *p != '\0'; ++p) {
            if (*p == '\\' || *p == '/') {
                name = p + 1;
            }
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        const uintptr_t off = reinterpret_cast<uintptr_t>(addr) - base;
        std::snprintf(out, out_size, "%s+0x%llx", name, static_cast<unsigned long long>(off));
    }
    else {
        std::snprintf(out, out_size, "<no module>");
    }
}

const char* describe_protect(DWORD p) {
    switch (p & 0xFF) {
        case PAGE_NOACCESS:          return "NOACCESS";
        case PAGE_READONLY:          return "READONLY";
        case PAGE_READWRITE:         return "READWRITE";
        case PAGE_WRITECOPY:         return "WRITECOPY";
        case PAGE_EXECUTE:           return "EXECUTE";
        case PAGE_EXECUTE_READ:      return "EXECUTE_READ";
        case PAGE_EXECUTE_READWRITE: return "EXECUTE_READWRITE";
        case 0:                      return "(none)";
        default:                     return "(other)";
    }
}

const char* describe_state(DWORD s) {
    switch (s) {
        case MEM_COMMIT:  return "COMMIT";
        case MEM_RESERVE: return "RESERVE";
        case MEM_FREE:    return "FREE";
        default:          return "(other)";
    }
}

// Ask the OS what it thinks of the address that just faulted.
//
// librecomp reserves 4 GB for RDRAM as PAGE_NOACCESS and then flips the first
// 512 MB to PAGE_READWRITE, so a fault a few bytes into that region means one
// of those two steps did not do what the source says. This reports which:
// whether the page is committed at all, what protection it actually carries,
// and how the reservation is carved up from its base -- where a missing or
// short VirtualProtect would show itself.
void report_memory(void* addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) {
        rayman2::report::crash_line("  VirtualQuery failed (error %lu)",
                     static_cast<unsigned long>(GetLastError()));
        return;
    }

    const uintptr_t alloc_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    const uintptr_t here = reinterpret_cast<uintptr_t>(addr);

    rayman2::report::crash_line("  VirtualQuery: state=%s protect=%s alloc_protect=%s",
                 describe_state(mbi.State),
                 describe_protect(mbi.Protect),
                 describe_protect(mbi.AllocationProtect));
    rayman2::report::crash_line("    region 0x%llx + 0x%llx bytes",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
                 static_cast<unsigned long long>(mbi.RegionSize));
    if (alloc_base != 0) {
        rayman2::report::crash_line("    allocation base 0x%llx, offset into it 0x%llx",
                     static_cast<unsigned long long>(alloc_base),
                     static_cast<unsigned long long>(here - alloc_base));

        rayman2::report::crash_line("    regions from that base:");
        uint8_t* cursor = static_cast<uint8_t*>(mbi.AllocationBase);
        for (int i = 0; i < 6; ++i) {
            MEMORY_BASIC_INFORMATION r{};
            if (VirtualQuery(cursor, &r, sizeof(r)) == 0 || r.AllocationBase != mbi.AllocationBase) {
                break;
            }
            rayman2::report::crash_line("      0x%llx  size 0x%-12llx %-8s %s",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(r.BaseAddress)),
                         static_cast<unsigned long long>(r.RegionSize),
                         describe_state(r.State),
                         describe_protect(r.Protect));
            cursor = static_cast<uint8_t*>(r.BaseAddress) + r.RegionSize;
        }
    }
}

const char* describe_code(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        // A C++ throw is delivered as an SEH exception with this code, so an
        // uncaught one -- including one crossing a noexcept boundary -- arrives
        // here before std::terminate gets a chance. Naming it matters: this is
        // the code behind the silent frontend failures earlier in the project,
        // and "exception (0xE06D7363)" tells a reader nothing at all.
        case 0xE06D7363:                      return "CPP_EXCEPTION (an uncaught C++ exception)";
        default:                              return "exception";
    }
}

// Tell the player, and put a clock on saying so.
//
// Until now a fatal error closed the window and left nothing behind but a file
// the player has no reason to look for. A dialog is the only thing that turns
// "it crashed" into "it crashed and here is the file to send".
//
// It is shown on its own thread with a bounded wait, because a modal box is a
// hang if nobody can see it -- a fullscreen window on the wrong monitor, or a
// message pump that is already dead. Waiting forever for a dialog that may not
// be on screen would trade the silent close for a frozen process, which is
// worse. Thirty seconds is long enough to read it and short enough not to
// matter if it was never visible.
void show_fatal_dialog(const wchar_t* what) {
    const std::wstring path = rayman2::report::path().wstring();

    std::wstring body = what;
    if (path.empty()) {
        body += L"(no report file could be opened this session)";
    }
    else {
        body += path;
    }

    struct Params { const wchar_t* body; };
    Params params{ body.c_str() };

    HANDLE thread = CreateThread(nullptr, 0, [](LPVOID arg) -> DWORD {
        const Params* p = static_cast<const Params*>(arg);
        MessageBoxW(nullptr, p->body, L"Rayman 2: Recompiled",
                    MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        return 0;
    }, &params, 0, nullptr);

    if (thread != nullptr) {
        WaitForSingleObject(thread, 30 * 1000);
        CloseHandle(thread);
    }
}

// The game's own recent indirect calls, into the crash block.
//
// The native stack walk says which C functions are on the stack; in a release
// build that is a list of module offsets, and even symbolised it names
// generated functions whose bodies are a transliteration of MIPS. This says
// something the stack cannot: which addresses IN THE GAME were being called,
// in order, up to the moment it died.
//
// They are game addresses, not native ones, so they resolve against
// recomp/symbol_addrs.txt and the split disassembly rather than through
// llvm-symbolizer -- which is the difference between a reader needing a
// matching debug build and a reader needing the repository.
//
// Only meaningful on a thread that runs recompiled code. A fault on the
// renderer or the event pump leaves this empty, and an empty section is left
// out rather than printed as a row of nothing.
void report_lookup_history(bool say_when_empty) {
    recomp::overlays::LookupHistoryEntry entries[recomp::overlays::lookup_history_capacity];
    const size_t count = recomp::overlays::get_lookup_history(
        entries, recomp::overlays::lookup_history_capacity);

    if (count == 0) {
        if (say_when_empty) {
            rayman2::report::crash_line("  this thread had made no indirect calls at all, which is itself");
            rayman2::report::crash_line("  worth knowing: the very first one it made is the one that failed.");
        }
        return;
    }

    rayman2::report::crash_line("  the last %zu addresses this thread called indirectly, oldest first.", count);
    rayman2::report::crash_line("  These are GAME addresses, not native ones: llvm-symbolizer resolves");
    rayman2::report::crash_line("  the frames further down and knows nothing about these. To name one,");
    rayman2::report::crash_line("  grep the split disassembly -- `grep -rn \"glabel func_800A6590\" asm/`");
    rayman2::report::crash_line("  -- or recomp/symbol_addrs.txt, which carries the ones with real names.");
    rayman2::report::crash_line("  A count means the same address called in a row, collapsed to one entry.");

    // Four to a row. `line` cannot overflow: a cell is at most two spaces, ten
    // hex digits, " x" and ten decimal digits -- 24 characters -- and four of
    // those is 96.
    char line[256];
    size_t used = 0;
    line[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        char cell[48];
        if (entries[i].repeats > 1) {
            std::snprintf(cell, sizeof(cell), "  0x%08X x%-6u",
                          entries[i].address, entries[i].repeats);
        }
        else {
            std::snprintf(cell, sizeof(cell), "  0x%08X        ", entries[i].address);
        }
        std::snprintf(line + used, sizeof(line) - used, "%s", cell);
        used += std::strlen(cell);

        if ((i % 4) == 3 || i + 1 == count) {
            while (used > 0 && line[used - 1] == ' ') {
                line[--used] = '\0';
            }
            rayman2::report::crash_line("  %s", line);
            used = 0;
            line[0] = '\0';
        }
    }
}

// Walk the native stack into the crash block. Shared by every path here.
void report_stack() {
    void* frames[32] = {};
    const USHORT captured = CaptureStackBackTrace(0, 32, frames, nullptr);
    rayman2::report::crash_line("  stack (%u frames):", static_cast<unsigned>(captured));
    for (USHORT i = 0; i < captured; ++i) {
        char frame_where[MAX_PATH + 64] = {};
        describe_address(frames[i], frame_where, sizeof(frame_where));
        rayman2::report::crash_line("    %2u  %p  %s", static_cast<unsigned>(i),
                                    frames[i], frame_where);
    }
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    char where[MAX_PATH + 64] = {};
    describe_address(rec->ExceptionAddress, where, sizeof(where));

    rayman2::report::crash_begin(describe_code(rec->ExceptionCode));
    rayman2::report::crash_line("%s (0x%08lX) at %p  %s",
                 describe_code(rec->ExceptionCode),
                 static_cast<unsigned long>(rec->ExceptionCode),
                 rec->ExceptionAddress, where);
    rayman2::report::crash_line("  thread: %lu",
                 static_cast<unsigned long>(GetCurrentThreadId()));

    if (rec->ExceptionCode == 0xE06D7363) {
        rayman2::report::crash_line("  a C++ exception nobody caught. If it was thrown out of a");
        rayman2::report::crash_line("  noexcept function the process is terminated on the spot, which");
        rayman2::report::crash_line("  is why there may be no message beyond this block.");
    }

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR op = rec->ExceptionInformation[0];
        const ULONG_PTR at = rec->ExceptionInformation[1];
        const char* verb = (op == 0) ? "reading" : (op == 1) ? "writing" : "executing";
        rayman2::report::crash_line("  %s address 0x%llx%s",
                     verb, static_cast<unsigned long long>(at),
                     at < 0x10000 ? "   (a null or near-null pointer)" : "");
        report_memory(reinterpret_cast<void*>(at));
    }

    // Cheap and often decisive: an access violation on a game thread is usually
    // the recompiled game dereferencing something, and this says what it had
    // been calling. On a renderer or event-pump thread it is empty and skipped.
    report_lookup_history(false);
    report_stack();
    rayman2::report::crash_end();
    rayman2::report::end_session(false);

    return EXCEPTION_EXECUTE_HANDLER;   // terminate, but having said something
}

// The case the exception filter cannot see.
//
// An exception crossing a noexcept boundary does not reach an unhandled
// exception filter: the runtime calls terminate and then fail-fasts, which
// Windows raises without running filters. That is exit code 0xC0000409, and it
// is what made the frontend's config-modal throw so opaque -- a window that
// appeared and vanished, no message, no non-zero exit worth reading.
//
// std::terminate IS called first, so this catches it. Rethrowing inside the
// handler is how the exception's own message is recovered; there is no other
// way to reach it from here.
void on_terminate() {
    rayman2::report::crash_begin("TERMINATE (uncaught exception or noexcept violation)");

    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        }
        catch (const std::exception& e) {
            rayman2::report::crash_line("exception: %s", e.what());
        }
        catch (...) {
            rayman2::report::crash_line("exception: not derived from std::exception");
        }
    }
    else {
        rayman2::report::crash_line("no active exception -- terminate was called directly");
    }

    rayman2::report::crash_line("  thread: %lu",
                                static_cast<unsigned long>(GetCurrentThreadId()));

    report_lookup_history(false);
    report_stack();

    rayman2::report::crash_end();
    rayman2::report::end_session(false);

    std::abort();
}

// The recompiled game called through a pointer that resolves to no function.
//
// librecomp's own handling of this printed one line and called std::exit, and
// both halves of that were wrong for this program. The line goes to stderr,
// which src/debug_report.cpp has replaced with a pipe drained by another
// thread, so a line printed moments before the process dies is still sitting in
// the pipe when it goes. And std::exit, called here on a game thread, runs the
// atexit table and every static destructor underneath a live renderer, audio
// device, window and mirror pump -- which is a second crash, in teardown, and
// that is the one that reaches the crash block. The player then sends a report
// about an access violation in the C runtime's exit path and the actual fault
// is a lost line in a pipe. See docs/issues/003.
//
// So: say what happened, straight to the file, then leave without touching the
// exit path at all.
void on_lookup_failure(int32_t addr) {
    // One thread wins. A second game thread arriving here while the first is
    // still writing would deadlock on the crash mutex rather than say anything
    // useful, and the process is going away regardless.
    static std::atomic<bool> reporting{false};
    if (reporting.exchange(true)) {
        Sleep(INFINITE);
    }

    rayman2::report::crash_begin("NULL_CALL (the game called through a bad function pointer)");
    rayman2::report::crash_line("the recompiled game called address 0x%08X, which is not a function",
                                static_cast<unsigned>(addr));
    if (addr == 0) {
        rayman2::report::crash_line("  0x00000000 means the pointer it called through was simply zero:");
        rayman2::report::crash_line("  a callback, jump-table slot or object field that nothing had");
        rayman2::report::crash_line("  filled in yet, or that something else overwrote.");
    }
    rayman2::report::crash_line("  thread: %lu",
                                static_cast<unsigned long>(GetCurrentThreadId()));
    report_lookup_history(true);
    rayman2::report::crash_line("  the frames below the runtime ones are the recompiled game's own");
    rayman2::report::crash_line("  call chain -- they are what says WHERE in the game this happened.");
    report_stack();
    rayman2::report::crash_end();
    rayman2::report::end_session(false);

    show_fatal_dialog(
        L"Rayman 2: Recompiled has hit an internal error and has to close.\n\n"
        L"The game called through a function pointer that does not point at "
        L"anything. This is a bug in the port, not in your ROM or your setup, "
        L"and it does not always happen -- launching again may well work.\n\n"
        L"A report describing it has been written to:\n");

    // NOT std::exit, and not abort() either: both run teardown that other
    // threads are still inside. The report is written and closed by now, so
    // there is nothing left worth unwinding for.
    TerminateProcess(GetCurrentProcess(), 3);
}

} // namespace

void rayman2_install_crash_reporter() {
    SetUnhandledExceptionFilter(on_unhandled_exception);
    std::set_terminate(on_terminate);
    recomp::overlays::set_lookup_failure_handler(on_lookup_failure);
}

#else

// Everything above is Windows-only: the exception filter, VirtualQuery and the
// stack walk have no portable equivalent worth writing here. The lookup failure
// does, and it is the half that matters -- it is a decision the port makes, not
// something the OS tells it -- so it is installed on every platform.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "librecomp/overlays.hpp"

#include "debug_report.h"

namespace {

void on_lookup_failure(int32_t addr) {
    static std::atomic<bool> reporting{false};
    if (reporting.exchange(true)) {
        for (;;) {
        }
    }

    rayman2::report::crash_begin("NULL_CALL (the game called through a bad function pointer)");
    rayman2::report::crash_line("the recompiled game called address 0x%08X, which is not a function",
                                static_cast<unsigned>(addr));
    rayman2::report::crash_end();
    rayman2::report::end_session(false);

    // _Exit, not exit: no atexit table, no static destructors, nothing run
    // underneath the threads that are still using them. See the Windows
    // version above for why that distinction is the whole point.
    std::_Exit(3);
}

} // namespace

void rayman2_install_crash_reporter() {
    recomp::overlays::set_lookup_failure_handler(on_lookup_failure);
}

#endif

// ---------------------------------------------------------------------------
// Proving the call history, from inside the running game
// ---------------------------------------------------------------------------
//
// RAYMAN2_SELFTEST=nullcall fires before the game has started, so the crash
// block it produces has an empty call history -- which proves the reporting and
// nothing about the thing the reporting is for. This fires from a hook that
// runs on a game thread, with a few thousand real indirect calls behind it, and
// produces the report a real occurrence would produce.
//
// It is called from the guPerspective hook in src/draw_distance.cpp because
// that is a game thread doing game work: by the time the camera is building a
// projection the history is full of the game's own addresses. The cost when the
// variable is unset is one already-initialised static bool.

#include <cstdlib>
#include <cstring>

#include "librecomp/overlays.hpp"

extern "C" recomp_func_t* get_function(int32_t addr);

extern "C" void rayman2_selftest_live_null_call() {
    static const bool armed = []() {
        const char* value = std::getenv("RAYMAN2_SELFTEST");
        return value != nullptr && std::strcmp(value, "nullcall-live") == 0;
    }();
    if (armed) {
        get_function(0);
    }
}
