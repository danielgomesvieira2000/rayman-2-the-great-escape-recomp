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
        std::fprintf(stderr, "[rayman2]   VirtualQuery failed (error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return;
    }

    const uintptr_t alloc_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    const uintptr_t here = reinterpret_cast<uintptr_t>(addr);

    std::fprintf(stderr, "[rayman2]   VirtualQuery: state=%s protect=%s alloc_protect=%s\n",
                 describe_state(mbi.State),
                 describe_protect(mbi.Protect),
                 describe_protect(mbi.AllocationProtect));
    std::fprintf(stderr, "[rayman2]     region 0x%llx + 0x%llx bytes\n",
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
                 static_cast<unsigned long long>(mbi.RegionSize));
    if (alloc_base != 0) {
        std::fprintf(stderr, "[rayman2]     allocation base 0x%llx, offset into it 0x%llx\n",
                     static_cast<unsigned long long>(alloc_base),
                     static_cast<unsigned long long>(here - alloc_base));

        std::fprintf(stderr, "[rayman2]     regions from that base:\n");
        uint8_t* cursor = static_cast<uint8_t*>(mbi.AllocationBase);
        for (int i = 0; i < 6; ++i) {
            MEMORY_BASIC_INFORMATION r{};
            if (VirtualQuery(cursor, &r, sizeof(r)) == 0 || r.AllocationBase != mbi.AllocationBase) {
                break;
            }
            std::fprintf(stderr, "[rayman2]       0x%llx  size 0x%-12llx %-8s %s\n",
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
        report_memory(reinterpret_cast<void*>(at));
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
