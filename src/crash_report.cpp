// Turn a silent crash into a located one.
//
// The port currently dies with exit code 0xC0000005 and no output, on a thread
// that is not main(). Without a debugger installed that is almost no
// information: the exit code says "access violation" and nothing about where.
//
// This installs an unhandled-exception filter that prints the exception, the
// faulting address, and a stack walk resolved to module + offset. Module and
// offset is enough to answer the question that matters first -- is the fault in
// our code, in RT64, or in the recompiled game -- and an offset can be turned
// into a line with the .pdb the build already produces.
//
// It deliberately does NOT use dbghelp/SymFromAddr: that pulls in a dependency
// to print names we do not need yet, and dbghelp is not thread-safe, which is a
// poor property for something that only ever runs on a thread that is already
// in trouble.
//
// Note the one thing this cannot catch: fail-fast (0xC0000409), which Windows
// raises without running exception filters. That is the code an exception
// crossing a noexcept boundary produces, and it is why the frontend's throws
// were so opaque earlier in this phase.

#ifdef _WIN32

#include <cstdio>
#include <cstdint>

#include <windows.h>

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

const char* describe_code(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        default:                              return "exception";
    }
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    char where[MAX_PATH + 64] = {};
    describe_address(rec->ExceptionAddress, where, sizeof(where));

    std::fprintf(stderr, "\n[rayman2] CRASH: %s (0x%08lX) at %p  %s\n",
                 describe_code(rec->ExceptionCode),
                 static_cast<unsigned long>(rec->ExceptionCode),
                 rec->ExceptionAddress, where);
    std::fprintf(stderr, "[rayman2]   thread: %lu\n",
                 static_cast<unsigned long>(GetCurrentThreadId()));

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR op = rec->ExceptionInformation[0];
        const ULONG_PTR at = rec->ExceptionInformation[1];
        const char* verb = (op == 0) ? "reading" : (op == 1) ? "writing" : "executing";
        std::fprintf(stderr, "[rayman2]   %s address 0x%llx%s\n",
                     verb, static_cast<unsigned long long>(at),
                     at < 0x10000 ? "   (a null or near-null pointer)" : "");
    }

    void* frames[32] = {};
    const USHORT captured = CaptureStackBackTrace(0, 32, frames, nullptr);
    std::fprintf(stderr, "[rayman2]   stack (%u frames):\n", static_cast<unsigned>(captured));
    for (USHORT i = 0; i < captured; ++i) {
        char frame_where[MAX_PATH + 64] = {};
        describe_address(frames[i], frame_where, sizeof(frame_where));
        std::fprintf(stderr, "[rayman2]     %2u  %p  %s\n", static_cast<unsigned>(i),
                     frames[i], frame_where);
    }
    std::fflush(stderr);

    return EXCEPTION_EXECUTE_HANDLER;   // terminate, but having said something
}

} // namespace

void rayman2_install_crash_reporter() {
    SetUnhandledExceptionFilter(on_unhandled_exception);
}

#else

void rayman2_install_crash_reporter() {}

#endif
