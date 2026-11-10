/* Resistance port: crash and hang reports (ported from the Resistance 2 port's
 * r2_crash.inc).
 *
 * A crash (an exception nobody handles; the guest-VM demand-paging handler
 * runs first and is unaffected) writes logs\crash\RFOM_<time>_<exe>_crash.dmp
 * plus a one-line summary in logs\crash\crashes.txt, then lets the process
 * die as before. A hang -- the flip counter does not advance for RFOM_HANG_S
 * seconds (default 90, longer than any level load) once the game has started
 * flipping -- writes RFOM_<time>_<exe>_hang.dmp once per stall and keeps
 * running (FREEZE_RELAUNCH, debugtools.inc, still decides about restarts).
 * Time spent paused on focus loss does not count. The dumps hold thread stacks
 * and the memory they point at, not the 4 GB guest space, so they stay small.
 *   RFOM_CRASH_DUMPS=0     both off
 *   RFOM_CRASH_DUMPS=full  full-memory dumps (several GB)
 * Both executables (ResistanceFoM.exe front end, ResistanceFoMGame.exe game)
 * include this, so the exe name is part of every file name and line. */
#ifdef _WIN32
#include <dbghelp.h>
#include <time.h>
#pragma comment(lib, "dbghelp.lib")

extern "C" volatile long rsx_focus_paused;     /* rsx_d3d12_backend.c: paused on focus loss */

static char s_crash_dir[MAX_PATH];
static char s_crash_exe[64] = "game";
static int  s_crash_full = 0;
static volatile LONG s_crash_busy = 0;

static void rfom_crash_path(char* out, size_t n, const char* kind)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &t);
    snprintf(out, n, "%s\\RFOM_%04d%02d%02d_%02d%02d%02d_%s_%s.dmp", s_crash_dir, tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, s_crash_exe, kind);
}

static int rfom_write_dump(const char* path, EXCEPTION_POINTERS* ep)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
    const MINIDUMP_TYPE type = s_crash_full
        ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithThreadInfo | MiniDumpWithHandleData)
        : (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                          MiniDumpScanMemory | MiniDumpWithUnloadedModules);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                                      ep ? &mei : NULL, NULL, NULL);
    CloseHandle(f);
    return ok != FALSE;
}

static void rfom_crash_note(const char* line)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\crashes.txt", s_crash_dir);
    if (FILE* f = fopen(p, "a")) {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_s(&tmv, &t);
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                tmv.tm_hour, tmv.tm_min, tmv.tm_sec, s_crash_exe);
        fputs(line, f);
        fclose(f);
    }
    fputs(line, stderr);
    fflush(stderr);
}

static LONG WINAPI rfom_crash_filter(EXCEPTION_POINTERS* ep)
{
    if (InterlockedExchange(&s_crash_busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    char path[MAX_PATH];
    rfom_crash_path(path, sizeof path, "crash");
    const ULONG_PTR addr = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    char mname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod)
        GetModuleFileNameA(mod, mname, sizeof mname);
    const char* base = strrchr(mname, '\\');
    const int ok = rfom_write_dump(path, ep);
    char line[1024];
    snprintf(line, sizeof line, "[crash] code 0x%08lX at %s+0x%llX (thread %lu), flip %u -> %s\n",
             ep->ExceptionRecord->ExceptionCode, base ? base + 1 : mname,
             (unsigned long long)(addr - (ULONG_PTR)mod), GetCurrentThreadId(), cellGcm_flip_request_count(),
             ok ? path : "dump FAILED");
    rfom_crash_note(line);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI rfom_hang_watch(LPVOID)
{
    int limit_s = 90;
    if (const char* e = getenv("RFOM_HANG_S")) { const int v = atoi(e); if (v >= 10) limit_s = v; }
    unsigned last = cellGcm_flip_request_count();
    ULONGLONG since = GetTickCount64();
    int reported = 0;
    for (;;) {
        Sleep(1000);
        const unsigned now = cellGcm_flip_request_count();
        /* not counting before the game flips steadily (boot), nor while paused */
        if (now != last || now < 200 || rsx_focus_paused) {
            last = now; since = GetTickCount64(); reported = 0; continue;
        }
        if (reported || GetTickCount64() - since < (ULONGLONG)limit_s * 1000ull) continue;
        reported = 1;
        char path[MAX_PATH], line[1024];
        rfom_crash_path(path, sizeof path, "hang");
        const int ok = rfom_write_dump(path, NULL);
        snprintf(line, sizeof line, "[hang] no frame for %d s (flip %u) -> %s\n", limit_s, now,
                 ok ? path : "dump FAILED");
        rfom_crash_note(line);
    }
    return 0;
}

static void rfom_crash_init(void)
{
    const char* e = getenv("RFOM_CRASH_DUMPS");
    if (e && *e == '0') return;
    s_crash_full = e && !_stricmp(e, "full");
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    if (const char* b = strrchr(exe, '\\')) {
        snprintf(s_crash_exe, sizeof s_crash_exe, "%s", b + 1);
        if (char* dot = strrchr(s_crash_exe, '.')) *dot = 0;
    }
    if (char* s = strrchr(exe, '\\')) *s = 0;
    /* <port>\build\ or <port>\build_game\ -> <port>\logs\crash */
    snprintf(s_crash_dir, sizeof s_crash_dir, "%s\\..\\logs", exe);
    CreateDirectoryA(s_crash_dir, NULL);
    strncat(s_crash_dir, "\\crash", sizeof s_crash_dir - strlen(s_crash_dir) - 1);
    CreateDirectoryA(s_crash_dir, NULL);
    SetUnhandledExceptionFilter(rfom_crash_filter);
    CloseHandle(CreateThread(NULL, 64 * 1024, rfom_hang_watch, NULL, 0, NULL));
    fprintf(stderr, "[crash] reporter on: dumps and crashes.txt in %s%s\n", s_crash_dir,
            s_crash_full ? " (full memory)" : "");
}
#else
static void rfom_crash_init(void) {}
#endif
