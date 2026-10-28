/* Resistance port (v132): F9 audio recording (cellAudio.c). */
extern "C" volatile long g_audio_rec_blocks, g_audio_rec_seq;
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" unsigned long ppu_host_tid_by_name(const char* name);
extern "C" unsigned long spurs_lane_host_tid(unsigned lane);
extern "C" unsigned long rsx_render_host_tid(void);
extern "C" unsigned long rsx_fifo_host_tid(void);
extern "C" int rsx_perf_render_load(void);   /* v126: last PERF window's render_frame %, -1 none */
extern "C" uint32_t cellGcm_get_flip_mode(void);
extern "C" unsigned long spu_raw_host_tid(unsigned n);   /* v127: spu_raw.c */
extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;

/* Windows-only from here to the end of hang_watchdog. Snapshotting every
 * OTHER thread and reading its instruction pointer needs tlhelp32 plus
 * SuspendThread/GetThreadContext; POSIX has no equivalent that does not
 * amount to writing a debugger. The vblank ticker above, which is what the
 * frame loop actually depends on, is deliberately outside this. */
#ifdef _WIN32
#include <tlhelp32.h>
/* When the boot wedges, snapshot every other thread's instruction pointer as a
 * module RVA (symbolize with llvm-symbolizer) so a guest spin/wait is pinned to
 * an exact lifted function -- the HLE breadcrumb only covers HLE calls. */
/* Snapshot every other thread's RIP. For threads in the boot module (lifted
 * guest code) print the RVA (symbolizable) + a couple of stack-return RVAs;
 * for threads parked in a DLL (OS waits / FMOD) print the module name so they
 * are not mistaken for guest spins. Called twice so the caller can diff which
 * guest thread is genuinely parked (same RIP) vs. still progressing. */
/* Defined with the debug console below; writes to stderr and, when the
 * console is servicing a command, also to its response file. */
static void dbg_printf(const char* fmt, ...);
extern "C" void spu_dbg_dump_all(void (*out)(const char*));
extern "C" void spu_dbg_dump_ls(unsigned which, unsigned lsa, unsigned len, void (*out)(const char*));
static void dbg_out_str(const char* s) { dbg_printf("%s", s); }
extern "C" volatile int g_rfom_trace_left;
extern "C" volatile unsigned long long g_pad_inj_until;
extern "C" volatile unsigned int g_pad_inj_btn, g_pad_inj_sticks;
extern "C" unsigned long long ps3_ms_now(void);
/* While dump_threads holds a thread suspended, dbg_printf formats into this
 * buffer instead of a stream: the suspended thread may own stderr's CRT lock,
 * and writing then deadlocks the watchdog -- and with it every thread that
 * logs. GH3 froze solid (0% CPU, log silent mid-boot) exactly that way. */
static __declspec(thread) char   s_dbg_capbuf[16384];
static __declspec(thread) char*  s_dbg_cap = NULL;
static __declspec(thread) size_t s_dbg_capn = 0;

extern "C" const char* g_hle_inflight[];   /* ppu_hle.cpp; 64 entries */
extern "C" void ppu_report_guest_lrs(void); /* ppu_loader.cpp */

static void dump_threads(const char* label, HMODULE self)
{
    dbg_printf( "[WATCHDOG] %s; last HLE call = 0x%08X (%s)\n",
            label, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");

    /* Which HLE each guest thread is INSIDE right now.
     *
     * ppu_hle.cpp has maintained this array for a while and nothing ever read
     * it. It is the only way to see a thread parked in an HLE wait:
     * sys_lwmutex_lock and sys_lwcond_wait are sysPrxForUser handlers, not lv2
     * syscalls, so PS3_SCBLOCK_PROF cannot see them block, the usleep
     * histogram cannot see them (they do not usleep), and the sampler just
     * reports ntdll. A thread stuck and invisible to all three is exactly the
     * case this answers. */
    { int any = 0;
      for (unsigned t = 0; t < 64u; t++) {
          const char* n = g_hle_inflight[t];
          if (!n) continue;
          if (!any) { dbg_printf("[WATCHDOG] guest threads inside an HLE:\n"); any = 1; }
          dbg_printf("[WATCHDOG]   guest-tid %-3u in %s\n", t, n);
      }
      if (!any) dbg_printf("[WATCHDOG] no guest thread is inside an HLE\n"); }
    dbg_printf("[WATCHDOG] guest threads, by guest lr:\n");
    ppu_report_guest_lrs();
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            s_dbg_capn = 0; s_dbg_capbuf[0] = 0; s_dbg_cap = s_dbg_capbuf;
            SuspendThread(th);
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &ctx)) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m);
                if (m == self) {
                    dbg_printf( "[WATCHDOG]   tid %5lu BOOT rip rva=0x%llX\n",
                            (unsigned long)te.th32ThreadID,
                            (unsigned long long)((char*)ctx.Rip - (char*)self));
                } else {
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    dbg_printf( "[WATCHDOG]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                }
                /* Scan the suspended thread's stack for boot-module return
                 * addresses to reconstruct the lifted call chain — done for ALL
                 * threads (even when RIP is parked in ntdll inside a CriticalSection
                 * call), since that's exactly where the busy-spin's lwmutex churn
                 * lands the main thread. Map RVAs -> func_ names via flow.map.
                 * (some false positives expected — these are stack-scan hits.) */
                {
                    uint64_t* sp = (uint64_t*)ctx.Rsp;
                    /* Bound the scan to the committed stack region so we never read
                     * past the guard page. If the query fails or the region is not
                     * committed+readable (thread mid-create, guard page, garbage
                     * Rsp), skip the scan entirely -- a diagnostic AV here gets
                     * caught by the crash filter and kills the whole run. */
                    MEMORY_BASIC_INFORMATION mbi;
                    uint64_t region_end = (uint64_t)sp;
                    if (VirtualQuery((LPCVOID)sp, &mbi, sizeof mbi) &&
                        mbi.State == MEM_COMMIT &&
                        !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                        region_end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
                    int maxk = (int)((region_end - (uint64_t)sp) / 8);
                    if (maxk > 0x200000 / 8) maxk = 0x200000 / 8;   /* lifted SPU frames are big */
                    int found = 0;
                    for (int k = 0; k < maxk && found < 40; k++) {
                        uint64_t v = sp[k];
                        if (v < (uint64_t)self) continue;
                        HMODULE mm = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)v, &mm);
                        if (mm == self) {
                            /* Resolve to the guest function containing this return
                             * address: the function_table entry with the largest host
                             * pointer <= it. A raw RVA needs symbols a release build
                             * does not have, and "ret rva=0x519F66" says nothing about
                             * where a hung title actually is. Same mapping the
                             * unresolved-call dump uses. */
                            uint32_t bestGuest = 0;
                            uintptr_t bestHost = 0, nextHost = 0, maxHost = 0;
                            for (uint64_t q = 0; q < function_table_count; q++) {
                                uintptr_t h = (uintptr_t)function_table[q].func;
                                if (h > maxHost) maxHost = h;
                                if (h <= (uintptr_t)v && h > bestHost) {
                                    bestHost = h; bestGuest = function_table[q].addr;
                                }
                            }
                            for (uint64_t q = 0; q < function_table_count; q++) {
                                uintptr_t h = (uintptr_t)function_table[q].func;
                                if (h > bestHost && (nextHost == 0 || h < nextHost)) nextHost = h;
                            }
                            /* The highest-addressed lifted function has no next, so its
                             * extent is unknown -- and EVERY runtime/CRT address above it
                             * was attributed to it. Refuse to guess there. A confident
                             * wrong frame is worse than no frame. */
                            if (bestHost == maxHost ||
                                (uintptr_t)v - bestHost > 0x4000u) bestGuest = 0;
                            dbg_printf( bestGuest
                                    ? "[WATCHDOG]       tid %5lu ret rva=0x%llX  func_%08X+0x%llX\n"
                                    : "[WATCHDOG]       tid %5lu ret rva=0x%llX\n",
                                    (unsigned long)te.th32ThreadID,
                                    (unsigned long long)(v - (uint64_t)self),
                                    bestGuest, (unsigned long long)((uintptr_t)v - bestHost));
                            found++;
                        }
                    }
                }
            }
            ResumeThread(th);
            s_dbg_cap = NULL;
            if (s_dbg_capn) dbg_printf("%s", s_dbg_capbuf);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    fflush(stderr);
}

/* ── Debug console ───────────────────────────────────────────────────────────
 * PS3_DEBUG=<path> opens a file-based command channel, so a title that is
 * already running can be asked what it is doing without a rebuild.
 *
 * Write one command into <path>; the console executes it, appends the answer
 * to <path>.out, and truncates <path> ready for the next one.
 *
 *     echo threads    > dbg.txt     # symbolised stack of every guest thread
 *     echo "mem 10200 64" > dbg.txt # hexdump of guest memory
 *     echo stat       > dbg.txt     # flips, HLE breadcrumb, uptime
 *
 * A file and not a socket on purpose: no listening port inside a game
 * process, nothing for a firewall to prompt about, and it drives from a shell
 * script exactly the way PAD_FILE already does.
 *
 * Read-only by design apart from poke32. Changing a diagnostic knob while the
 * title runs is NOT possible here: the large majority are read once at first
 * use and cached in a function-local static (see docs/DIAGNOSTICS.md), so they
 * are launch-time settings. `knobs` reports which ones this run was started
 * with, which is the useful half of that.
 */
static FILE* s_dbg_out = NULL;

static void dbg_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_dbg_cap) {             /* a thread is suspended: format only, no stream lock */
        int n = vsnprintf(s_dbg_cap + s_dbg_capn, sizeof s_dbg_capbuf - s_dbg_capn, fmt, ap);
        if (n > 0) s_dbg_capn += (size_t)n;
        if (s_dbg_capn >= sizeof s_dbg_capbuf) s_dbg_capn = sizeof s_dbg_capbuf - 1;
        va_end(ap);
        return;
    }
    if (s_dbg_out) { va_list c; va_copy(c, ap); vfprintf(s_dbg_out, fmt, c); va_end(c); }
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void dbg_mem(uint32_t ea, uint32_t len)
{
    if (!vm_base) { dbg_printf("  vm_base not live yet%c", 10); return; }
    if (len == 0 || len > 4096) len = 64;
    for (uint32_t off = 0; off < len; off += 16) {
        char asc[17];
        dbg_printf("  0x%08X ", ea + off);
        for (uint32_t i = 0; i < 16; i++) {
            if (off + i < len) {
                uint8_t b = vm_base[(size_t)(ea + off + i)];
                dbg_printf("%02X ", b);
                asc[i] = (b >= 32 && b < 127) ? (char)b : '.';
            } else { dbg_printf("   "); asc[i] = ' '; }
        }
        asc[16] = 0;
        dbg_printf(" |%s|%c", asc, 10);
    }
}

/* Resistance port (v134): memory scanners for finding game variables.
 *   fsnap <lo> <hi>    remember every big-endian float in [lo,hi] in guest RAM
 *   fkeep lt|gt|eq|ne  keep the remembered floats that are now < > == != their
 *                      remembered value (then remember the current values)
 *   flist [n]          print up to n (default 40) survivors
 *   projscan           find 4x4 perspective projection matrices (m00, m11,
 *                      zeros, -1 in the w column) and, near each, floats whose
 *                      value is the field of view that produced m11 */
#include <vector>
#include <cmath>
static std::vector<std::pair<uint32_t, float>> s_fscan;
static float be_f32_at(uint32_t ea)
{
    uint32_t v; memcpy(&v, vm_base + ea, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f; memcpy(&f, &v, 4); return f;
}
template <class F> static void guest_ram_walk(F fn)   /* fn(ea_begin, ea_end) per committed region */
{
    for (uintptr_t p = 0x10000; p < 0x40000000u && vm_base; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + p, &mi, sizeof mi)) break;
        uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
        if (end > 0x40000000u) end = 0x40000000u;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            fn((uint32_t)(p & ~3u), (uint32_t)end);
        p = end;
    }
}
static void dbg_fsnap(float lo, float hi)
{
    s_fscan.clear();
    guest_ram_walk([&](uint32_t b, uint32_t e) {
        for (uint32_t q = b; q + 4 <= e; q += 4) {
            const float f = be_f32_at(q);
            if (f >= lo && f <= hi) s_fscan.push_back({q, f});
        } });
    dbg_printf("  %zu floats in [%g, %g]%c", s_fscan.size(), lo, hi, 10);
}
static void dbg_fkeep(const char* op)
{
    size_t k = 0;
    for (auto& c : s_fscan) {
        const float f = be_f32_at(c.first);
        bool keep = false;
        if (!strcmp(op, "lt")) keep = f < c.second - 1e-6f * fabsf(c.second);
        else if (!strcmp(op, "gt")) keep = f > c.second + 1e-6f * fabsf(c.second);
        else if (!strcmp(op, "eq")) keep = f == c.second;
        else if (!strcmp(op, "ne")) keep = f != c.second;
        if (keep) s_fscan[k++] = {c.first, f};
    }
    s_fscan.resize(k);
    dbg_printf("  fkeep %s: %zu left%c", op, k, 10);
}
static void dbg_flist(unsigned n)
{
    if (!n) n = 40;
    for (size_t i = 0; i < s_fscan.size() && i < n; i++)
        dbg_printf("  0x%08X  %.6g (was %.6g)%c", s_fscan[i].first, be_f32_at(s_fscan[i].first), s_fscan[i].second, 10);
}
static void dbg_projscan(void)
{
    int found = 0;
    guest_ram_walk([&](uint32_t b, uint32_t e) {
        for (uint32_t q = b; q + 64 <= e && found < 24; q += 4) {
            const float m00 = be_f32_at(q);
            if (!(m00 > 0.2f && m00 < 6.0f)) continue;
            const float m11 = be_f32_at(q + 20);
            if (!(m11 > 0.2f && m11 < 6.0f)) continue;
            const float r = m11 / m00;
            if (!(r > 1.2f && r < 2.5f)) continue;           /* 4:3 .. 21:9 */
            if (be_f32_at(q + 4) != 0.0f || be_f32_at(q + 8) != 0.0f || be_f32_at(q + 12) != 0.0f ||
                be_f32_at(q + 16) != 0.0f || be_f32_at(q + 24) != 0.0f || be_f32_at(q + 28) != 0.0f ||
                be_f32_at(q + 32) != 0.0f || be_f32_at(q + 36) != 0.0f) continue;
            const float w1 = be_f32_at(q + 44), w2 = be_f32_at(q + 56);   /* -1 in row-major or column-major */
            if (!(fabsf(fabsf(w1) - 1.0f) < 1e-4f || fabsf(fabsf(w2) - 1.0f) < 1e-4f)) continue;
            found++;
            const double fovy = 2.0 * atan(1.0 / m11), fovx = 2.0 * atan(1.0 / m00);
            dbg_printf("  proj @0x%08X m00=%.5f m11=%.5f aspect=%.4f -> vertical FOV %.2f deg, horizontal %.2f deg%c",
                       q, m00, m11, r, fovy * 57.29578, fovx * 57.29578, 10);
            /* nearby floats equal to that FOV in radians / degrees / half-angles */
            const uint32_t lo = q > 0x800 ? q - 0x800 : 0, hi = q + 0x800;
            for (uint32_t s = lo; s + 4 <= hi && s + 4 <= e; s += 4) {
                if (s < b) continue;
                const float f = be_f32_at(s);
                const double cand[6] = { fovy, fovx, fovy * 57.29578, fovx * 57.29578, fovy * 0.5, fovx * 0.5 };
                static const char* nm[6] = { "vFOV rad", "hFOV rad", "vFOV deg", "hFOV deg", "half vFOV rad", "half hFOV rad" };
                for (int c = 0; c < 6; c++)
                    if (f != 0.0f && fabs(f - cand[c]) < fabs(cand[c]) * 2e-3)
                        dbg_printf("      0x%08X (%+d) = %.5f  <- %s%c", s, (int)(s - q), f, nm[c], 10);
            }
        } });
    dbg_printf("  %d projection matrices%c", found, 10);
}

static void dbg_knobs(const char* prefix)
{
    /* Which diagnostics this run was actually started with. */
    LPCH env = GetEnvironmentStringsA();
    if (!env) return;
    int n = 0;
    for (LPCH p = env; *p; p += strlen(p) + 1) {
        const char* eq = strchr(p, '=');
        if (!eq || eq == p) continue;
        /* Only the runtime's own namespaces, not the whole shell environment. */
        static const char* known[] = {
            "PS3_", "SPU_", "SPURS_", "GCM_", "RSX_", "LD_", "YZ_", "YDKJ_",
            "LBP_", "FLOW_", "RD_", "TEX_", "FP_", "VP_", "PPU_", "PS1_",
            "CELLMARK_", "RTT_", "PAD_", "FS_", "WATCHDOG_", "SYNC_", NULL };
        int match = 0;
        for (int k = 0; known[k]; k++)
            if (strncmp(p, known[k], strlen(known[k])) == 0) { match = 1; break; }
        if (!match) continue;
        if (prefix && *prefix && strncmp(p, prefix, strlen(prefix)) != 0) continue;
        dbg_printf("  %s%c", p, 10);
        n++;
    }
    FreeEnvironmentStringsA(env);
    dbg_printf("  (%d set; docs/DIAGNOSTICS.md lists all of them)%c", n, 10);
}

/* "prof <host-tid> [sec]": sample ONE thread at ~1 kHz and unwind it through
 * .pdata (exact, unlike dump_threads' stack scan), then print the hottest
 * functions exclusive (leaf) and inclusive (anywhere on the stack), keyed by
 * function start. exe entries print as rva=; resolve against the link map.
 * Only lock-free work happens while the thread is suspended.
 * ponytail: fixed 4096-slot tables; a thread touching more functions drops the rest. */
#define PROF_SLOTS 4096
#define PROF_MAXT  8
/* v126: up to PROF_MAXT threads sampled together (one table set each), so a
 * frame-rate dip shows the main thread, the RSX render thread and the FIFO
 * thread over the SAME seconds -- sampled one after another they each saw a
 * different moment. */
static uint64_t s_prof_key[PROF_MAXT][6][PROF_SLOTS];   /* v140: [5] = exact leaf address */
static uint32_t s_prof_ct[PROF_MAXT][6][PROF_SLOTS];
static void prof_hit(int t, int tab, uint64_t key)
{
    for (uint32_t i = (uint32_t)((key >> 4) * 2654435761u) % PROF_SLOTS, n = 0; n < PROF_SLOTS;
         n++, i = (i + 1) % PROF_SLOTS) {
        if (s_prof_key[t][tab][i] == key) { s_prof_ct[t][tab][i]++; return; }
        if (!s_prof_key[t][tab][i]) { s_prof_key[t][tab][i] = key; s_prof_ct[t][tab][i] = 1; return; }
    }
}
/* v130: one sample of a suspended thread. Under SEH: unwinding a thread
 * stopped at an arbitrary instruction can read a garbage frame (lifted SPU
 * code longjmps and tail-calls), and an access violation here used to kill
 * the run with the target still suspended -- the game froze for ~7 s and
 * exited during an F11 profile. A faulting sample is dropped; the caller
 * always resumes the thread. */
static volatile LONG s_prof_faults;
static int prof_sample_body(int t, HANDLE th, uintptr_t exe)
{
            CONTEXT c; c.ContextFlags = CONTEXT_FULL;
            if (!GetThreadContext(th, &c)) return 0;
            {
                uint64_t seen[24]; int ns = 0, got = 0;
                uint64_t fr[3] = {0, 0, 0};   /* leaf, caller, caller's caller (function starts) */
                for (int d = 0; d < 24 && c.Rip; d++) {
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                    uint64_t key = rf ? base + rf->BeginAddress : c.Rip;
                    if (d < 3) fr[d] = key;
                    if (d == 2) {
                        /* call pairs, exe-relative (rva < 2^28): leaf<-caller and
                         * caller<-grandcaller -- who is spinning, and on behalf of whom */
                        const uint64_t r0 = fr[0] - exe, r1 = fr[1] - exe, r2 = fr[2] - exe;
                        if (r0 < (1ull << 28) && r1 < (1ull << 28)) prof_hit(t, 3, (r0 << 28) | r1 | (1ull << 60));
                        if (r1 < (1ull << 28) && r2 < (1ull << 28)) prof_hit(t, 4, (r1 << 28) | r2 | (1ull << 60));
                    }
                    if (d == 0) { prof_hit(t, 0, key); if (t <= 2) prof_hit(t, 5, c.Rip); }
                    /* first exe frame of a sample whose leaf is in a DLL: who called out */
                    if (!got && (uintptr_t)(key - exe) < 0x40000000u) { got = 1; if (d > 0) prof_hit(t, 2, key); }
                    int dup = 0; for (int k = 0; k < ns; k++) dup |= seen[k] == key;
                    if (!dup) { seen[ns++] = key; prof_hit(t, 1, key); }
                    if (!rf) break;                       /* leaf without unwind data: stop */
                    void* hd; DWORD64 ef;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, NULL);
                }
            }
            return 1;
}
static int prof_sample(int t, HANDLE th, uintptr_t exe)
{
    __try { return prof_sample_body(t, th, exe); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        s_prof_faults++;      /* reported after the run: no printing while a thread is suspended */
        return 0;
    }
}
static void dbg_prof_multi(const DWORD* tids, const char* const* names, int nt, unsigned sec)
{
    if (nt > PROF_MAXT) nt = PROF_MAXT;
    HANDLE h[PROF_MAXT] = {0};
    unsigned samples[PROF_MAXT] = {0};
    for (int t = 0; t < nt; t++) {
        /* Suspending ourselves never resumes: the console would hang for good. */
        if (!tids[t] || tids[t] == GetCurrentThreadId()) continue;
        h[t] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[t]);
        if (!h[t]) dbg_printf("  cannot open tid %lu%c", (unsigned long)tids[t], 10);
    }
    memset(s_prof_key, 0, sizeof s_prof_key); memset(s_prof_ct, 0, sizeof s_prof_ct);
    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
    const ULONGLONG end = GetTickCount64() + sec * 1000ull;
    while (GetTickCount64() < end) {
        for (int t = 0; t < nt; t++) {
            if (!h[t]) continue;
            if (SuspendThread(h[t]) == (DWORD)-1) { CloseHandle(h[t]); h[t] = NULL; continue; }
            if (prof_sample(t, h[t], exe)) samples[t]++;
            ResumeThread(h[t]);
        }
        Sleep(1);
    }
    if (s_prof_faults) { dbg_printf("  (%ld samples faulted while unwinding and were dropped)%c", (long)s_prof_faults, 10); s_prof_faults = 0; }
    for (int t = 0; t < nt; t++) {
        if (h[t]) CloseHandle(h[t]);
        if (!tids[t]) continue;
        if (names && names[t]) dbg_printf("  (%s)%c", names[t], 10);
        dbg_printf("  prof tid %lu: %u samples over %us%c", (unsigned long)tids[t], samples[t], sec, 10);
        for (int tab = 0; tab < 6; tab++) {
            static const char* tn[6] = { "exclusive", "inclusive", "exe caller of DLL leaf",
                                         "leaf <- caller", "caller <- its caller", "exact leaf address" };
            if (tab == 5) {
                /* v140: which instructions inside the hot functions (main,
                 * render, FIFO threads): resolve with the pdb to source lines */
                if (t > 2) continue;
                dbg_printf("  --- %s ---%c", tn[tab], 10);
                for (int rank = 0; rank < 80; rank++) {
                    uint32_t best = 0; int bi = -1;
                    for (int k = 0; k < PROF_SLOTS; k++) if (s_prof_ct[t][tab][k] > best) { best = s_prof_ct[t][tab][k]; bi = k; }
                    if (bi < 0) break;
                    const uint64_t key = s_prof_key[t][tab][bi];
                    if (key - exe < 0x40000000ull)
                        dbg_printf("  %5.1f%%  at=0x%llX%c", 100.0 * best / (samples[t] ? samples[t] : 1),
                                   (unsigned long long)(key - exe), 10);
                    s_prof_ct[t][tab][bi] = 0;
                }
                continue;
            }
            if (tab >= 3) {
                dbg_printf("  --- %s ---%c", tn[tab], 10);
                for (int rank = 0; rank < 40; rank++) {
                    uint32_t best = 0; int bi = -1;
                    for (int k = 0; k < PROF_SLOTS; k++) if (s_prof_ct[t][tab][k] > best) { best = s_prof_ct[t][tab][k]; bi = k; }
                    if (bi < 0) break;
                    const uint64_t key = s_prof_key[t][tab][bi] & ((1ull << 56) - 1);
                    dbg_printf("  %5.1f%%  rva=0x%llX <- rva=0x%llX%c", 100.0 * best / (samples[t] ? samples[t] : 1),
                               (unsigned long long)(key >> 28), (unsigned long long)(key & ((1ull << 28) - 1)), 10);
                    s_prof_ct[t][tab][bi] = 0;
                }
                continue;
            }
            dbg_printf("  --- %s ---%c", tn[tab], 10);
            for (int rank = 0; rank < 60; rank++) {
                uint32_t best = 0; int bi = -1;
                for (int k = 0; k < PROF_SLOTS; k++) if (s_prof_ct[t][tab][k] > best) { best = s_prof_ct[t][tab][k]; bi = k; }
                if (bi < 0) break;
                uint64_t key = s_prof_key[t][tab][bi];
                HMODULE m = NULL; char path[MAX_PATH] = "?";
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)key, &m);
                if (m) GetModuleFileNameA(m, path, sizeof path);
                const char* b = strrchr(path, 92); b = b ? b + 1 : path;
                if ((uintptr_t)m == exe) dbg_printf("  %5.1f%%  rva=0x%llX%c", 100.0 * best / (samples[t] ? samples[t] : 1),
                                                    (unsigned long long)(key - exe), 10);
                else dbg_printf("  %5.1f%%  %s+0x%llX%c", 100.0 * best / (samples[t] ? samples[t] : 1), b,
                                (unsigned long long)(key - (uintptr_t)m), 10);
                s_prof_ct[t][tab][bi] = 0;
            }
        }
    }
}
static void dbg_prof(DWORD tid, unsigned sec) { dbg_prof_multi(&tid, NULL, 1, sec); }

static DWORD s_dbg_main_tid = 0;   /* set by ps3_load_prx_modules (main guest thread) */
/* v126: main, RSX render, RSX FIFO and SPU 0's lane, sampled together.
 * v127: + both raw SPUs (the main thread feeds them through 8-deep command
 * rings and spins in func_00051184 when one is full -- 40% of its time in a
 * 17 fps York dip) and SPURS lanes 1-2. */
extern "C" volatile long g_fifo_mhist_on;
extern "C" void fifo_mhist_reset(void);
extern "C" int fifo_mhist_report(char* buf, int cap, int n, double secs);
/* v140: after an F11 profile, 3 s of FIFO method counts (cellGcmSys.c). */
static void dbg_fifo_mhist(unsigned sec)
{
    fifo_mhist_reset();
    InterlockedExchange(&g_fifo_mhist_on, 1);
    Sleep(sec * 1000);
    InterlockedExchange(&g_fifo_mhist_on, 0);
    Sleep(100);
    static char buf[12288];
    fifo_mhist_report(buf, (int)sizeof buf, 48, (double)sec);
    dbg_printf("%s", buf);
}
static void dbg_prof_frame_threads(unsigned sec)
{
    const DWORD t[8] = { s_dbg_main_tid, (DWORD)rsx_render_host_tid(), (DWORD)rsx_fifo_host_tid(),
                         (DWORD)spu_raw_host_tid(0), (DWORD)spu_raw_host_tid(1),
                         (DWORD)spurs_lane_host_tid(0), (DWORD)spurs_lane_host_tid(1),
                         (DWORD)spurs_lane_host_tid(2) };
    static const char* const nm[8] = { "main thread", "RSX render thread", "RSX FIFO thread (vblank clock)",
                                       "raw SPU 0", "raw SPU 1",
                                       "SPURS lane 0", "SPURS lane 1", "SPURS lane 2" };
    dbg_prof_multi(t, nm, 8, sec);
}

/* Resistance port: give the main guest thread a physical core to itself.
 * The main thread is the frame-rate limiter (lifted game code), and this
 * process runs far more busy threads than the CPU has: SPURS lanes and raw
 * SPUs poll, the FIFO thread spin-waits. Any of them landing on the main
 * thread's SMT sibling takes a large share of that core's throughput. Every
 * second: main -> logical CPU RFOM_MAIN_CPU (default 2), every other thread
 * of the process -> all CPUs except that one and its sibling (CPU ^ 1).
 * RFOM_MAIN_CORE=0 disables. */
static DWORD WINAPI main_core_keeper(LPVOID)
{
    const char* e = getenv("RFOM_MAIN_CPU");
    const int cpu = e ? atoi(e) : 2;   /* not CPU 0: Windows routes most interrupts there */
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return 0;
    const DWORD_PTR mine = (DWORD_PTR)1 << cpu;
    const DWORD_PTR pair = mine | ((DWORD_PTR)1 << (cpu ^ 1));
    /* v181: the RSX FIFO thread is the next limiter (F11: ~98% busy in York's
     * slow stretch) -- it gets the next core to itself. RFOM_FIFO_CPU=-1 off. */
    const char* fe = getenv("RFOM_FIFO_CPU");
    const int fcpu = fe ? atoi(fe) : 4;
    DWORD_PTR fmine = 0, fpair = 0;
    if (fcpu >= 0 && fcpu < 64 && (fcpu >> 1) != (cpu >> 1)) {
        fmine = (DWORD_PTR)1 << fcpu; fpair = fmine | ((DWORD_PTR)1 << (fcpu ^ 1));
        if (!(proc & fmine)) fmine = fpair = 0;
    }
    const DWORD_PTR rest = proc & ~pair & ~fpair;
    if (!(proc & mine) || !rest) return 0;
    fprintf(stderr, "[affinity] main thread %lu -> CPU %d, RSX FIFO thread -> CPU %d, other threads -> mask 0x%llX%c",
            (unsigned long)s_dbg_main_tid, cpu, fmine ? fcpu : -1, (unsigned long long)rest, 10);
    DWORD fifo_done = 0;
    static DWORD done[512]; int nd = 0;
    const DWORD pid = GetCurrentProcessId();
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te; te.dwSize = sizeof te;
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                const DWORD ftid = fmine ? (DWORD)rsx_fifo_host_tid() : 0;
                if (ftid && te.th32ThreadID == ftid) {
                    if (fifo_done != ftid) {
                        HANDLE fh = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, ftid);
                        if (fh) { SetThreadAffinityMask(fh, fmine); CloseHandle(fh); fifo_done = ftid;
                                  fprintf(stderr, "[affinity] RSX FIFO thread %lu pinned to CPU %d%c", (unsigned long)ftid, fcpu, 10); }
                    }
                    continue;
                }
                int seen = 0;
                for (int i = 0; i < nd; i++) if (done[i] == te.th32ThreadID) { seen = 1; break; }
                if (seen) continue;
                HANDLE th = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!th) continue;
                SetThreadAffinityMask(th, te.th32ThreadID == s_dbg_main_tid ? mine : rest);
                CloseHandle(th);
                if (nd < 512) done[nd++] = te.th32ThreadID;
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        Sleep(1000);
    }
}
/* v147: F6 = 10 s "pad watch". Hardware data breakpoints (debug registers)
 * on the main thread over the right-stick bytes of the guest's pad buffer
 * (port 0 GetData buffer +12, and the game's previous-frame copy +128 past
 * it); every instruction that touches them is counted by host address and
 * written to logs\padwatch.txt as module RVAs -- which lifted functions read
 * the look stick, i.e. where the camera turns it into yaw/pitch. */
extern "C" volatile u32 g_pad_data_ea0;
extern "C" volatile uint32_t rfom_pad_out_ea = 0;   /* set by func_000D4BC4 (game) */
extern "C" volatile uint32_t rfom_pad_slot_ea = 0, rfom_pad_obj_ea = 0;
/* v147g: func_002B12A0 runs for more than one controller object; remember the
 * latest slot of each (up to 4) and watch one of each. */
static volatile uint32_t s_pw_obj[4], s_pw_slot[4]; static volatile LONG s_pw_nobj;
extern "C" void rfom_pad_slot_note(uint32_t obj, uint32_t slot)
{
    rfom_pad_slot_ea = slot; rfom_pad_obj_ea = obj;
    LONG n = s_pw_nobj;
    for (LONG i = 0; i < n; i++) if (s_pw_obj[i] == obj) { s_pw_slot[i] = slot; return; }
    if (n < 4) { s_pw_obj[n] = obj; s_pw_slot[n] = slot; s_pw_nobj = n + 1; }
}
static volatile LONG s_pw_on = 0;
static struct { DWORD64 rip; LONG n; DWORD dr; DWORD tid; } s_pw_hits[128];
static volatile LONG s_pw_n = 0;
static LONG WINAPI pw_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !s_pw_on) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const DWORD64 st = c->Dr6 & 0xF;
    if (!st) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD64 rip = (DWORD64)ep->ExceptionRecord->ExceptionAddress;
    LONG n = s_pw_n; int found = 0;
    for (LONG i = 0; i < n && i < 128; i++)
        if (s_pw_hits[i].rip == rip) { s_pw_hits[i].n++; s_pw_hits[i].dr |= (DWORD)st; found = 1; break; }
    if (!found && n < 128) { s_pw_hits[n].rip = rip; s_pw_hits[n].n = 1; s_pw_hits[n].dr = (DWORD)st; s_pw_hits[n].tid = GetCurrentThreadId(); s_pw_n = n + 1; }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static int pw_set(DWORD tid, const DWORD64* a, int on)
{
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) return 0;
    SuspendThread(th);
    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    int ok = 0;
    if (GetThreadContext(th, &c)) {
        c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0; c.Dr7 = 0;
        if (on) {
            DWORD64* dr[4] = { &c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3 };
            for (int k = 0; k < 4; k++) {
                if (!a[k]) continue;
                /* 4 bytes when 4-aligned, else the 8 around it */
                const DWORD64 len = (a[k] & 3) ? 2 : 3;
                *dr[k] = (a[k] & 3) ? (a[k] & ~7ull) : a[k];
                c.Dr7 |= (1ull << (2 * k)) | (3ull << (16 + 4 * k)) | (len << (18 + 4 * k));
            }
        }
        c.Dr6 = 0;
        ok = SetThreadContext(th, &c) ? 1 : 0;
    }
    ResumeThread(th);
    CloseHandle(th);
    return ok;
}
extern "C" volatile uint32_t g_rfom_opts_ea;   /* cellPad.c */
static void pad_watch_run(void)
{
    const u32 ea = g_pad_data_ea0;
    if (!ea || !vm_base || !s_dbg_main_tid) { fprintf(stderr, "[padwatch] no pad buffer yet%c", 10); return; }
    static PVOID h = NULL;
    if (!h) h = AddVectoredExceptionHandler(1, pw_veh);
    s_pw_n = 0;
    /* v147b: watch the game's converted stick floats (right X/Y) once known */
    /* v147c: one slot of the input history ring (64 slots, one per frame):
     * every time the ring comes round to it (~1 s) its readers hit. */
    /* v147d: the slot's remapped axes +0x10..+0x1C (func_002B12A0 writes
     * them from the control-scheme table at obj+0x1AC0) -- the values the
     * camera actually consumes. */
    const u32 po = rfom_pad_slot_ea;
    DWORD64 a[4] = { 0, 0, 0, 0 };
    /* v158: the player's own controller object (its layout table is the
     * options block the mouse look found), LEFT stick: remapped +0x10/+0x14
     * and raw +0x40/+0x44 -- to find what reads it during a Menial grab. */
    u32 pslot = 0;
    for (LONG i = 0; i < s_pw_nobj; i++)
        if (g_rfom_opts_ea && vm_read32(s_pw_obj[i] + 0x1AC0) == g_rfom_opts_ea) pslot = s_pw_slot[i];
    if (!pslot && s_pw_nobj) pslot = s_pw_slot[0];
    if (pslot) { a[0] = (DWORD64)(vm_base + pslot + 0x10); a[1] = (DWORD64)(vm_base + pslot + 0x14);
                 a[2] = (DWORD64)(vm_base + pslot + 0x40); a[3] = (DWORD64)(vm_base + pslot + 0x44); }
    else { a[0] = (DWORD64)(vm_base + ea + 12); a[1] = (DWORD64)(vm_base + ea + 12 + 128); }
    s_pw_on = 1;
    /* v147f: every thread of the process -- debug registers are per thread,
     * and the code that reads the look axes is not on the main thread. */
    static DWORD tids[512]; int nt = 0;
    { HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 te; te.dwSize = sizeof te;
      if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
          do { if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId() && nt < 512)
                   tids[nt++] = te.th32ThreadID; } while (Thread32Next(snap, &te));
      }
      if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap); }
    int ok = 0;
    for (int i = 0; i < nt; i++) ok += pw_set(tids[i], a, 1);
    fprintf(stderr, "[padwatch] F6: watching %s 0x%08X for 10 s (%d of %d threads armed)%c", po ? "input history slot" : "pad buffer", po ? po : ea, ok, nt, 10);
    Sleep(10000);
    { const DWORD64 z[4] = { 0, 0, 0, 0 }; for (int i = 0; i < nt; i++) pw_set(tids[i], z, 0); }
    Sleep(50);
    s_pw_on = 0;
    const DWORD64 mb = (DWORD64)GetModuleHandleA(NULL);
    static int s_pw_run = 0; char pwn[64]; snprintf(pwn, sizeof pwn, "logs\\padwatch_%d.txt", ++s_pw_run);
    FILE* f = fopen(pwn, "w");
    if (f) {
        fprintf(f, "# pad buffer 0x%08X, history slot 0x%08X (object 0x%08X), module base 0x%llX, %ld objects:", ea, po, rfom_pad_obj_ea, (unsigned long long)mb, (long)s_pw_nobj);
        for (LONG i = 0; i < s_pw_nobj; i++) fprintf(f, " obj 0x%08X slot 0x%08X (dr%ld);", s_pw_obj[i], s_pw_slot[i], (long)i);
        fputc(10, f);
        for (LONG i = 0; i < s_pw_n && i < 128; i++)
            fprintf(f, "rva=0x%llX hits=%ld dr=%lu tid=%lu\n", (unsigned long long)(s_pw_hits[i].rip - mb), s_pw_hits[i].n, s_pw_hits[i].dr, s_pw_hits[i].tid);
        fclose(f);
    }
    fprintf(stderr, "[padwatch] done: %ld instruction sites -> %s%c", (long)s_pw_n, pwn, 10);
}
static DWORD WINAPI pad_watch_thread(LPVOID) { pad_watch_run(); return 0; }
static DWORD WINAPI debug_console(LPVOID param)
{
    const char* path = (const char*)param;
    char outpath[1024];
    snprintf(outpath, sizeof outpath, "%s.out", path);

    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&debug_console, &self);
    const DWORD t0 = GetTickCount();
    fprintf(stderr, "[dbg] console on '%s' -- write a command, read '%s'%c",
            path, outpath, 10);

    int f11_was = 0, f9_was = 0, f8_was = 0, f6_was = 0;
    for (int it = 0;; it++) {
        Sleep(20);
        /* Resistance port (v132): F9 = record the next 20 s of the game's
         * 8-channel sound mix to logs\audiorec_N.wav (cellAudio.c). */
        { const int down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
          if (down && !f9_was && g_audio_rec_blocks <= 0) {
              InterlockedIncrement(&g_audio_rec_seq);
              InterlockedExchange(&g_audio_rec_blocks, 20 * 48000 / 256);
              fprintf(stderr, "[dbg] F9: recording 20 s of game audio%c", 10);
          }
          f9_was = down; }
        /* v142: F8 = a 20 s frame trace on its own (logs\ftrace_reqN.txt), no
         * sampling profiler: the profiler suspends threads ~1000 times a second
         * and its own start-up made 100 ms frames, which is exactly what a
         * stutter hunt must not contain. */
        /* v163i: F5 = dump the game's data/bss (guest 0x10000000..0x10500000) to
         * logs\memdump_N.bin -- diff two dumps taken on different settings. */
        { static int f5_was = 0; static int f5_n = 0;
          const int down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
          if (down && !f5_was) {
              extern uint8_t* vm_base;
              char fp[1024]; snprintf(fp, sizeof fp, "%s", path);
              char* sl = strrchr(fp, 92);
              if (sl && vm_base) { snprintf(sl + 1, 64, "memdump_%llu.bin", (unsigned long long)GetTickCount64() / 1000ull); (void)f5_n;
                  FILE* ff = fopen(fp, "wb");
                  /* page by page: uncommitted pages (demand-committed VM) read as zeros --
                   * a single fwrite of the range failed on the first hole */
                  if (ff) { static uint8_t pg[4096];
                      for (uint32_t o = 0; o < 0x500000u; o += 4096u) {
                          MEMORY_BASIC_INFORMATION mi; const uint8_t* src = vm_base + 0x10000000u + o;
                          if (VirtualQuery(src, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
                              !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) memcpy(pg, src, 4096);
                          else memset(pg, 0, 4096);
                          fwrite(pg, 1, 4096, ff); }
                      fclose(ff); }
                  fprintf(stderr, "[dbg] F5: %s%c", fp, 10); }
          }
          f5_was = down; }
        /* v147: F6 = pad watch (see pad_watch_run) */
        { const int down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
          if (down && !f6_was) { HANDLE t = CreateThread(NULL, 0, pad_watch_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
          f6_was = down; }
        /* v147: the trace moved F8 -> F7 -- F8 is the mouse release key
         * (controls.txt mouse_release_key), so pressing it let go of the mouse. */
        { const int down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
          if (down && !f8_was) {
              char fp[1024]; snprintf(fp, sizeof fp, "%s", path);
              char* sl = strrchr(fp, 92);
              if (sl) { strcpy(sl + 1, "ftrace.now");
                        FILE* ff = fopen(fp, "wb"); if (ff) { fputs("20", ff); fclose(ff); } }
              fprintf(stderr, "[dbg] F7: 20 s frame trace%c", 10);
          }
          f8_was = down; }
        /* F11 edge: the "pressed since last call" bit (& 1) is shared with
         * every other caller in the session (the launcher process polls it
         * too), so it can be eaten; the key-down bit at 20 ms is not. */
        { const SHORT ks = GetAsyncKeyState(VK_F11);
          const int down = (ks & 0x8000) != 0;
          const int edge = down && !f11_was;
          f11_was = down;
          if (edge && s_dbg_main_tid) {
              s_dbg_out = fopen(outpath, "ab");
              dbg_printf("%c[dbg] > F11: prof main %lu 6 + render, FIFO, raw SPUs, SPURS lanes together%c", 10, (unsigned long)s_dbg_main_tid, 10);
              fprintf(stderr, "[dbg] F11: profiling main/render/FIFO/raw SPUs/SPURS lanes for 6 s%c", 10);
              /* ...and a 10 s frame trace alongside (logs\ftrace.now). */
              { char fp[1024]; snprintf(fp, sizeof fp, "%s", path);
                char* sl = strrchr(fp, 92);
                if (sl) { strcpy(sl + 1, "ftrace.now");
                          FILE* ff = fopen(fp, "wb"); if (ff) { fputs("10", ff); fclose(ff); } } }
              dbg_prof_frame_threads(6);
              dbg_fifo_mhist(3);
              if (s_dbg_out) { fclose(s_dbg_out); s_dbg_out = NULL; }
              fprintf(stderr, "[dbg] F11 profile written%c", 10);
              f11_was = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
              continue;
          } }
        /* Resistance port: automatic F11. Once a second, the guest flip
         * rate; two seconds in a row under RFOM_AUTOPROF_FPS (default 26) but
         * above 8 (not a load screen or pause) -> profile the main thread and
         * take a 10 s frame trace, at most twice per run. RFOM_AUTOPROF=0 off. */
        if (it % 50 == 0 && s_dbg_main_tid) {
            static int ap_on = -1, ap_fps = 26, ap_done = 0, ap_low = 0, ap_hot = 0, ap_hot_done = 0;
            static unsigned ap_last = 0;
            if (ap_on < 0) { const char* e = getenv("RFOM_AUTOPROF"); ap_on = e ? atoi(e) : 0;   /* v142: off by default -- the profile itself stalls the game */
                             /* v126: game.self only -- the front end's menus run below
                              * the gameplay target and would use up both profiles */
                             { const char* bp = getenv("PS3_BOOT_PATH");
                               if (!bp || !strstr(bp, "game.self")) ap_on = 0; }
                             /* v126: 3/4 of the frame-rate target (RFOM_FPS, default 60) */
                             const char* r = getenv("RFOM_FPS");
                             int tgt = (r && *r) ? atoi(r) : 60;
                             if (r && (*r == 'o' || *r == 'O')) tgt = 30;
                             if (tgt <= 0) tgt = 60; if (tgt > 120) tgt = 120; if (tgt < 30) tgt = 30;
                             ap_fps = tgt * 3 / 4;
                             const char* f = getenv("RFOM_AUTOPROF_FPS"); if (f) ap_fps = atoi(f); }
            const unsigned fc = cellGcm_flip_request_count();
            const unsigned d = fc - ap_last; ap_last = fc;
            /* movies set VSYNC flips (SetFlipMode 2; gameplay uses HSYNC, 1)
             * and play at their own 30 fps: not a dip */
            if (ap_on && ap_done < 2 && d > 8 && d < (unsigned)ap_fps && cellGcm_get_flip_mode() != 2)
                ap_low++; else ap_low = 0;
            /* v126: or the RSX render thread pinned (render_frame >= 85% of
             * the PERF window three seconds running), once per run */
            { const int ld = rsx_perf_render_load();
              if (ap_on && !ap_hot_done && d > 8 && ld >= 85) ap_hot++; else ap_hot = 0; }
            if (ap_low >= 2 || ap_hot >= 3) {
                if (ap_hot >= 3) ap_hot_done = 1; else ap_done++;
                ap_low = 0; ap_hot = 0;
                s_dbg_out = fopen(outpath, "ab");
                dbg_printf("%c[dbg] > F11: prof main %lu 6 (auto, %u fps, render load %d%%) + render, FIFO, raw SPUs, SPURS lanes together%c",
                           10, (unsigned long)s_dbg_main_tid, d, rsx_perf_render_load(), 10);
                fprintf(stderr, "[dbg] auto-profile: %u fps -- profiling main/render/FIFO/raw SPUs/SPURS lanes for 6 s%c", d, 10);
                { char fp[1024]; snprintf(fp, sizeof fp, "%s", path);
                  char* sl = strrchr(fp, 92);
                  if (sl) { strcpy(sl + 1, "ftrace.now");
                            FILE* ff = fopen(fp, "wb"); if (ff) { fputs("10", ff); fclose(ff); } } }
                /* v124: the dips are on the RSX side now -- the render thread
                 * (D3D12 recording) and the FIFO thread (decode + prepass) first,
                 * then the main thread */
                dbg_prof_frame_threads(6);
                if (s_dbg_out) { fclose(s_dbg_out); s_dbg_out = NULL; }
                fprintf(stderr, "[dbg] auto-profile written%c", 10);
                ap_last = cellGcm_flip_request_count();
                continue;
            }
        }
        if (it % 5) continue;   /* the command file every 100 ms */
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char cmd[512] = {0};
        if (!fgets(cmd, sizeof cmd, f)) { fclose(f); continue; }
        fclose(f);
        char* nl = strpbrk(cmd, "\r\n"); if (nl) *nl = 0;
        if (!cmd[0]) continue;
        FILE* t = fopen(path, "wb"); if (t) fclose(t);   /* consume it */

        s_dbg_out = fopen(outpath, "ab");
        dbg_printf("%c[dbg] > %s%c", 10, cmd, 10);

        char verb[64] = {0};
        unsigned a = 0, b = 0, c = 0;
        sscanf(cmd, "%63s", verb);

        if (!strcmp(verb, "help")) {
            dbg_printf("  threads          stacks of every guest thread, symbolised%c", 10);
            dbg_printf("  prof <tid> [s]   sample one host thread ~1kHz, hottest functions%c", 10);
            dbg_printf("  hle              last HLE call the runtime dispatched%c", 10);
            dbg_printf("  stat             flips, HLE breadcrumb, uptime%c", 10);
            dbg_printf("  mem <hex> [len]  hexdump guest memory%c", 10);
            dbg_printf("  poke32 <hex> <v> write a guest u32%c", 10);
            dbg_printf("  knobs [prefix]   diagnostics this run was started with%c", 10);
        } else if (!strcmp(verb, "threads")) {
            dump_threads("console", self);
        } else if (!strcmp(verb, "find32") && sscanf(cmd, "%*s %x", &a) == 1) {
            /* find32 <hex>: every aligned big-endian u32 equal to <hex> in committed
             * guest memory below 0xC0000000 (first 4096 hits). */
            const uint32_t be = ((a & 0xFF) << 24) | ((a & 0xFF00) << 8) | ((a >> 8) & 0xFF00) | (a >> 24);
            int hits = 0;
            for (uintptr_t p = 0; p < 0xC0000000u && hits < 4096 && vm_base; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + p, &mi, sizeof mi)) break;
                const uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    for (uintptr_t q = p & ~3u; q + 4 <= end && q < 0xC0000000u && hits < 4096; q += 4)
                        if (*(const uint32_t*)(vm_base + q) == be) { dbg_printf("  0x%08X%c", (unsigned)q, 10); hits++; }
                p = end;
            }
            dbg_printf("  %d hit(s)%c", hits, 10);
        } else if (!strcmp(verb, "qbset") && sscanf(cmd, "%*s %x %x", &a, &b) == 2) {
            /* qbset <name-crc> <value>: set every integer QB struct member with
             * that name. A member is 16 bytes: xx 01|81 00 00 | name | value | next,
             * big-endian (GH3: bot_play, unlocked). Prints how many it set. */
            const uint32_t be_name = ((a & 0xFF) << 24) | ((a & 0xFF00) << 8) | ((a >> 8) & 0xFF00) | (a >> 24);
            const uint32_t be_val  = ((b & 0xFF) << 24) | ((b & 0xFF00) << 8) | ((b >> 8) & 0xFF00) | (b >> 24);
            unsigned n = 0;
            for (uintptr_t p = 0; p < 0xC0000000u && vm_base; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + p, &mi, sizeof mi)) break;
                const uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
                if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
                    for (uintptr_t q = (p + 4) & ~3u; q + 8 <= end; q += 4)
                        if (*(const uint32_t*)(vm_base + q) == be_name) {
                            /* type word: any flags byte, then 0x01 or 0x81 (integer), 00 00;
                             * only 0/1 values change -- an unlocked tag is a flag. */
                            const uint8_t* t = vm_base + q - 4;
                            const uint32_t v = *(const uint32_t*)(vm_base + q + 4);
                            if ((t[1] == 0x01 || t[1] == 0x81) && !t[2] && !t[3] &&
                                (v == 0 || v == 0x01000000u)) {
                                *(uint32_t*)(vm_base + q + 4) = be_val; n++;
                            }
                        }
                p = end;
            }
            dbg_printf("  qbset 0x%08X = %u: %u member(s)%c", a, b, n, 10);
        } else if (!strcmp(verb, "profname")) {
            /* profname <guest-thread-name-prefix> [s] */
            char nm[64] = {0}; unsigned sec = 5;
            sscanf(cmd, "%*s %63s %u", nm, &sec);
            const unsigned long ht = ppu_host_tid_by_name(nm);
            dbg_printf("  '%s' -> host tid %lu%c", nm, ht, 10);
            if (ht) dbg_prof((DWORD)ht, sec ? sec : 5);
        } else if (!strcmp(verb, "profrsx")) {
            /* profrsx [s]: the RSX render thread, then the FIFO thread */
            sscanf(cmd, "%*s %u", &a); if (!a) a = 4;
            const unsigned long rt = rsx_render_host_tid(), ft = rsx_fifo_host_tid();
            dbg_printf("  render tid %lu, FIFO tid %lu%c", rt, ft, 10);
            if (rt) dbg_prof((DWORD)rt, a);
            if (ft) dbg_prof((DWORD)ft, a);
        } else if (!strcmp(verb, "proflane") && sscanf(cmd, "%*s %u %u", &a, &b) >= 1) {
            /* proflane <SPURS lane> [s]: 0-2 = SPU 0-2, 3+ = extra lanes */
            const unsigned long ht = spurs_lane_host_tid(a);
            dbg_printf("  lane %u -> host tid %lu%c", a, ht, 10);
            if (ht) dbg_prof((DWORD)ht, b ? b : 5);
        } else if (!strcmp(verb, "prof") && sscanf(cmd, "%*s %u %u", &a, &b) >= 1) {
            dbg_prof((DWORD)a, b ? b : 5);
        } else if (!strcmp(verb, "mtrace") && sscanf(cmd, "%*s %u", &a) == 1) {
            g_rfom_trace_left = (int)a;
            dbg_printf("  tracing the next %u FIFO methods into the run log%c", a, 10);
        } else if (!strcmp(verb, "pad")) {
            /* pad <hexbuttons> <lx> <ly> <rx> <ry> <ms>: hold on port 0 (sticks 0..255, 128 = centre) */
            unsigned bt = 0, lx = 128, ly = 128, rx = 128, ry = 128, ms = 500;
            sscanf(cmd, "%*s %x %u %u %u %u %u", &bt, &lx, &ly, &rx, &ry, &ms);
            g_pad_inj_btn = bt; g_pad_inj_sticks = ((lx & 255) << 24) | ((ly & 255) << 16) | ((rx & 255) << 8) | (ry & 255);
            g_pad_inj_until = ps3_ms_now() + ms;
            dbg_printf("  pad btn=0x%04X l=(%u,%u) r=(%u,%u) for %u ms%c", bt, lx, ly, rx, ry, ms, 10);
        } else if (!strcmp(verb, "spu")) {
            spu_dbg_dump_all(dbg_out_str);
        } else if (!strcmp(verb, "spuls") && sscanf(cmd, "%*s %u %x %u", &a, &b, &c) >= 2) {
            spu_dbg_dump_ls(a, b, c, dbg_out_str);
        } else if (!strcmp(verb, "hle")) {
            dbg_printf("  last HLE = 0x%08X (%s)%c", g_last_hle_nid,
                       g_last_hle_name ? g_last_hle_name : "", 10);
        } else if (!strcmp(verb, "stat")) {
            dbg_printf("  uptime   %.1f s%c", (GetTickCount() - t0) / 1000.0, 10);
            dbg_printf("  flips    %u%c", cellGcm_flip_request_count(), 10);
            dbg_printf("  last HLE 0x%08X (%s)%c", g_last_hle_nid,
                       g_last_hle_name ? g_last_hle_name : "", 10);
            dbg_printf("  vm_base  %s%c", vm_base ? "live" : "not mapped", 10);
        } else if (!strcmp(verb, "mem") && sscanf(cmd, "%*s %x %u", &a, &b) >= 1) {
            dbg_mem(a, b);
        } else if (!strcmp(verb, "poke32") && sscanf(cmd, "%*s %x %x", &a, &b) == 2) {
            if (vm_base) {
                uint32_t be = ((b & 0xFF) << 24) | ((b & 0xFF00) << 8) |
                              ((b >> 8) & 0xFF00) | ((b >> 24) & 0xFF);
                memcpy(vm_base + a, &be, 4);
                dbg_printf("  [0x%08X] = 0x%08X%c", a, b, 10);
            } else dbg_printf("  vm_base not live yet%c", 10);
        } else if (!strcmp(verb, "fsnap")) {
            float lo = 0, hi = 0;
            if (sscanf(cmd, "%*s %f %f", &lo, &hi) == 2) dbg_fsnap(lo, hi);
            else dbg_printf("  fsnap <lo> <hi>%c", 10);
        } else if (!strcmp(verb, "fkeep")) {
            char op[16] = {0}; sscanf(cmd, "%*s %15s", op); dbg_fkeep(op);
        } else if (!strcmp(verb, "flist")) {
            sscanf(cmd, "%*s %u", &a); dbg_flist(a);
        } else if (!strcmp(verb, "projscan")) {
            dbg_projscan();
        } else if (!strcmp(verb, "pokef") && sscanf(cmd, "%*s %x", &a) == 1) {
            float f = 0; sscanf(cmd, "%*s %*x %f", &f);
            uint32_t v; memcpy(&v, &f, 4);
            v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
            if (vm_base) { memcpy(vm_base + a, &v, 4); dbg_printf("  [0x%08X] = %g%c", a, f, 10); }
        } else if (!strcmp(verb, "knobs")) {
            char pfx[64] = {0};
            sscanf(cmd, "%*s %63s", pfx);
            dbg_knobs(pfx);
        } else {
            dbg_printf("  ? '%s' -- try 'help'%c", verb, 10);
        }

        if (s_dbg_out) { fclose(s_dbg_out); s_dbg_out = NULL; }
    }
    return 0;
}

/* Resistance port: FREEZE_RELAUNCH=<secs>. If the frame counter stops for
 * <secs> after the game has been running, write every thread's stack and the
 * SPU register files to logs\freeze_<n>.txt, start a fresh copy of this
 * program with the same command line, environment and console, and end this
 * one -- so a freeze costs a restart, not a person at the keyboard. */
static DWORD WINAPI freeze_relauncher(LPVOID)
{
    const char* e = getenv("FREEZE_RELAUNCH");
    const unsigned secs = e ? (unsigned)atoi(e) : 0;
    if (!secs) return 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&freeze_relauncher, &self);
    unsigned last = 0, still = 0;
    for (;;) {
        Sleep(1000);
        const unsigned f = cellGcm_flip_request_count();
        if (f != last || f < 200) { last = f; still = 0; continue; }
        if (++still < secs) continue;
        static int n = 0;
        { char fn[64]; snprintf(fn, sizeof fn, "logs\\freeze_%lu_%d.txt", (unsigned long)GetTickCount(), n++);
          s_dbg_out = fopen(fn, "wb");
          dbg_printf("[freeze] frame counter stuck at %u for %u s%c", f, secs, 10);
          dump_threads("freeze", self);
          spu_dbg_dump_all(dbg_out_str);
          if (s_dbg_out) { fclose(s_dbg_out); s_dbg_out = NULL; }
          fprintf(stderr, "[freeze] stuck at flip %u -- state in %s, relaunching%c", f, fn, 10); fflush(stderr); }
        STARTUPINFOA si; PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof si); si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        ZeroMemory(&pi, sizeof pi);
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        char* cl = _strdup(GetCommandLineA());
        if (CreateProcessA(exe, cl, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        TerminateProcess(GetCurrentProcess(), 3);
    }
}

/* v126: movie-end watchdog. York's black screen after a cutscene looked like
 * a movie that reached its end without the title moving on (the screen still
 * flips, so FREEZE_RELAUNCH never fires). If 20 s pass after the decoder's
 * EndSeq with no StartSeq/Close/Open and the display still in movie mode
 * (the title sets VSYNC flips for movies, HSYNC for gameplay), write every
 * thread's stack and the SPU
 * state to logs\moviehang_<tick>.txt once, so the next report says where it
 * waits. RFOM_MOVIE_WATCHDOG=0 turns it off; =<secs> changes the delay. */
extern "C" unsigned long long cellVdec_endseq_pending_ms(void);
static DWORD WINAPI movie_watchdog(LPVOID)
{
    const char* e = getenv("RFOM_MOVIE_WATCHDOG");
    const unsigned secs = e ? (unsigned)atoi(e) : 20u;
    if (!secs) return 0;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&movie_watchdog, &self);
    unsigned long long done_for = 0;
    for (;;) {
        Sleep(1000);
        const unsigned long long t = cellVdec_endseq_pending_ms();
        if (!t || t == done_for) continue;
        const unsigned long long now = ps3_ms_now();
        if (now < t || now - t < (unsigned long long)secs * 1000ull) continue;
        /* a movie that ended properly hands the screen back to gameplay
         * (SetFlipMode 1); York's hang stayed in movie mode (2) */
        if (cellGcm_get_flip_mode() != 2) { done_for = t; continue; }
        done_for = t;
        char fn[64]; snprintf(fn, sizeof fn, "logs\\moviehang_%lu.txt", (unsigned long)GetTickCount());
        s_dbg_out = fopen(fn, "wb");
        dbg_printf("[moviehang] decoder EndSeq %llu ms ago and nothing since; flips %u, flip mode %u%c",
                   now - t, cellGcm_flip_request_count(), (unsigned)cellGcm_get_flip_mode(), 10);
        dump_threads("moviehang", self);
        spu_dbg_dump_all(dbg_out_str);
        if (s_dbg_out) { fclose(s_dbg_out); s_dbg_out = NULL; }
        fprintf(stderr, "[moviehang] movie ended %u s ago and the game has not moved on -- state in %s%c",
                secs, fn, 10); fflush(stderr);
    }
}

static DWORD WINAPI hang_watchdog(LPVOID)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hang_watchdog, &self);
    /* ponytail: two fixed samples cover a boot wedge, but a hang that happens
     * minutes in (e.g. after a level load) needs them moved. WATCHDOG_AT="a,b"
     * gives the two sample times in seconds. */
    int t1 = 8, t2 = 15;
    { const char* e = getenv("WATCHDOG_AT"); int a = 0, b = 0;
      if (e && sscanf(e, "%d,%d", &a, &b) == 2 && a > 0 && b > a) { t1 = a; t2 = b; } }
    char lbl[64];
    Sleep((DWORD)t1 * 1000);
    snprintf(lbl, sizeof lbl, "%ds sample", t1); dump_threads(lbl, self);
    Sleep((DWORD)(t2 - t1) * 1000);
    snprintf(lbl, sizeof lbl, "%ds sample", t2); dump_threads(lbl, self);
    return 0;
}
#endif
