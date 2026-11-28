// core/diag/crash.c - crash report for SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS
// (POSIX) and unhandled SEH exceptions (Windows).
//
// The report is formatted into a static buffer and written with write()
// (WriteFile on Windows) straight to the original stderr and the log file,
// bypassing the stderr pipe the log mirror drains: a process dying from a
// crash does not wait for the pump thread. Before the report the handler
// waits up to 250 ms for the pump to write what the pipe holds
// (openpete_log_file_barrier), so the log lines before the fault precede
// the report. On POSIX the handler calls only write, read (the stack-word
// probe), clock_gettime, sched_yield,
// raise, backtrace and backtrace_symbols_fd (libgcc is loaded at install),
// plus lock-free reads of the guest symbol table, the mod override frame
// stack and the breadcrumb slots; it runs on a per-thread alternate stack (sigaltstack) so a stack
// overflow, on a thread or on the game fiber the thread hosts, still gets a
// report. Recompiled code runs on a libco fiber, so the host RIP from the
// signal context is the fault site that `addr2line -e <exe> <hex>`
// resolves; the backtrace is printed last because it walks the faulting
// stack and can itself fault.

#define _GNU_SOURCE
#include "openpete_psx.h"
#include "openpete_crash.h"
#include "openpete_error_ids.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_mods.h"   /* crash attribution: name the offending mod */
#include "openpete_paths.h"

#include <SDL3/SDL_atomic.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

/** @brief Recurse with a 4 KiB frame the compiler cannot elide, until the stack is exhausted.
 *
 * @param depth Recursion depth so far.
 * @return Never returns normally; the sum keeps each frame live. */
static int crash_test_recurse(int depth) {
    volatile char pad[4096];
    pad[0] = (char)depth;
    pad[sizeof pad - 1] = (char)(depth >> 8);
    return crash_test_recurse(depth + 1) + pad[0] + pad[sizeof pad - 1];
}

void openpete_crash_test(void) {
    const char* what = getenv("OPENPETE_CRASH_TEST");
    if (!what || !*what) return;
    fprintf(stderr, "openpete: OPENPETE_CRASH_TEST=%s - faulting on purpose\n", what);
    fflush(stderr);
    if (strcmp(what, "segv") == 0) {
        volatile int* volatile p = NULL;
        *p = 1;
    } else if (strcmp(what, "overflow") == 0) {
        (void)crash_test_recurse(0);
    }
}

/* --- report buffer ---------------------------------------------------------
 * Formatting without stdio: fixed buffer, no allocation, no locale. One
 * crash formats at a time; a second fault inside the handler is not
 * reported (the signal is blocked, or the filter is not re-entered). */

static char   s_rep[8192];
static size_t s_rep_len;

/** @brief Append a NUL-terminated string to the report, truncating at the buffer's end. */
static void rep_str(const char* s) {
    while (*s && s_rep_len < sizeof s_rep) s_rep[s_rep_len++] = *s++;
}

/** @brief Append an unsigned value in hexadecimal with a 0x prefix and at least `digits` digits. */
static void rep_hex(uint64_t v, int digits) {
    char tmp[19];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    } while ((v || n < digits) && n < 16);
    rep_str("0x");
    while (n > 0 && s_rep_len < sizeof s_rep) s_rep[s_rep_len++] = tmp[--n];
}

/** @brief Append a signed value in decimal. */
static void rep_dec(int64_t v) {
    char tmp[21];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u);
    if (v < 0) rep_str("-");
    while (n > 0 && s_rep_len < sizeof s_rep) s_rep[s_rep_len++] = tmp[--n];
}

/** @brief Append "  <label> = <symbol>+0x<off>" or the raw address marked "(no symbol)", then a newline.
 *
 * The symbol table is read without locks or allocation. */
static void rep_guest_addr(const char* label, uint32_t addr) {
    uint32_t off = 0u;
    const char* fn = openpete_mods_symbol_for_addr(addr, &off);
    rep_str("  "); rep_str(label); rep_str(" = ");
    if (fn) { rep_str(fn); rep_str("+"); rep_hex(off, 1); }
    else    { rep_hex(addr, 8); rep_str(" (no symbol)"); }
    rep_str("\n");
}

static CPUState* g_diag_cpu = NULL;

/** @brief Register the CPUState whose guest registers the crash report prints.
 *
 * @param cpu The game's CPUState; NULL prints "(no CPUState registered)". */
void openpete_psx_set_diag_cpu(CPUState* cpu) { g_diag_cpu = cpu; }

/** @brief Return the CPUState registered with openpete_psx_set_diag_cpu.
 *
 * @return The registered CPUState, or NULL before one is registered. */
CPUState* openpete_psx_diag_cpu(void) { return g_diag_cpu; }

/** @brief Append the guest registers of the registered CPUState and the mod whose override or bus handler was executing. */
static void rep_guest_state(void) {
    const CPUState* c = g_diag_cpu;
    if (c) {
        rep_str("  pc="); rep_hex(c->pc, 8); rep_str(" ra="); rep_hex(c->ra, 8);
        rep_str(" sp="); rep_hex(c->sp, 8); rep_str(" gp="); rep_hex(c->gp, 8);
        rep_str("\n  v0="); rep_hex(c->v0, 8); rep_str(" v1="); rep_hex(c->v1, 8);
        rep_str(" a0="); rep_hex(c->a0, 8); rep_str(" a1="); rep_hex(c->a1, 8);
        rep_str("\n");
        rep_guest_addr("guest pc", c->pc);
        rep_guest_addr("guest ra", c->ra);
    } else {
        rep_str("  (no CPUState registered)\n");
    }
    openpete_mods_attribution_t at;
    if (!openpete_mods_crash_attribution(&at)) return;
    if (at.kind == OPENPETE_MODS_FRAME_BUS) {
        rep_str("  MOD ATTRIBUTION: inside bus handler of bus '");
        rep_str(at.bus ? at.bus : "?");
        rep_str("' attached by mod '"); rep_str(at.owner); rep_str("'\n");
    } else {
        rep_str("  MOD ATTRIBUTION: inside override of "); rep_hex(at.addr, 8);
        rep_str(" registered by mod '"); rep_str(at.owner); rep_str("'\n");
    }
}

/* --- breadcrumbs ------------------------------------------------------------
 * One slot per thread that has a call in progress (openpete_crash_crumb_set).
 * The handler reads the slots without a lock: a slot is printed only while
 * its state is LIVE, the faulting thread's own slot cannot change under the
 * handler, and another thread's text can be torn only when that thread
 * rewrites it during the report. */

#define CRUMB_SLOTS 32

enum { CRUMB_FREE = 0, CRUMB_WRITING, CRUMB_LIVE };

typedef struct {
    SDL_AtomicInt state;   /* CRUMB_*; the SDL atomic store is a full barrier */
    uint64_t      tid;     /* the owning thread (crumb_tid)                   */
    char          text[OPENPETE_CRASH_CRUMB_MAX];
} crumb_slot;

static crumb_slot s_crumbs[CRUMB_SLOTS];
static _Thread_local int t_crumb = -1;   /* this thread's slot, or -1 */

/** @brief Return the calling thread's id as the crash report prints it: the Win32 thread id, or pthread_self on POSIX. */
static uint64_t crumb_tid(void) {
#if defined(_WIN32)
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

void openpete_crash_crumb_set(const char* text) {
    int i = t_crumb;
    if (i < 0) {
        for (i = 0; i < CRUMB_SLOTS; i++)
            if (SDL_CompareAndSwapAtomicInt(&s_crumbs[i].state, CRUMB_FREE, CRUMB_WRITING)) break;
        if (i == CRUMB_SLOTS) return;
        s_crumbs[i].tid = crumb_tid();
        t_crumb = i;
    } else {
        SDL_SetAtomicInt(&s_crumbs[i].state, CRUMB_WRITING);
    }
    size_t n = strlen(text);
    if (n > OPENPETE_CRASH_CRUMB_MAX - 1) n = OPENPETE_CRASH_CRUMB_MAX - 1;
    memcpy(s_crumbs[i].text, text, n);
    s_crumbs[i].text[n] = '\0';
    SDL_SetAtomicInt(&s_crumbs[i].state, CRUMB_LIVE);
}

void openpete_crash_crumb_clear(void) {
    if (t_crumb < 0) return;
    SDL_SetAtomicInt(&s_crumbs[t_crumb].state, CRUMB_FREE);
    t_crumb = -1;
}

static void rep_flush(void);

/* openpete_crash_set_fatal_status's status, -1 when none is set. */
static SDL_AtomicInt s_fatal_status = {-1};

void openpete_crash_set_fatal_status(int code) { SDL_SetAtomicInt(&s_fatal_status, code); }

/** @brief Return the status set by openpete_crash_set_fatal_status, else @p code.
 * @param code The caller's exit code. */
static int fatal_status(int code) {
    const int set = SDL_GetAtomicInt(&s_fatal_status);
    return set >= 0 ? set : code;
}

/** @brief Append at most n bytes of s, stopping at a NUL. */
static void rep_strn(const char* s, size_t n) {
    for (size_t i = 0; i < n && s[i] && s_rep_len < sizeof s_rep; i++) s_rep[s_rep_len++] = s[i];
}

/** @brief Append and flush every live breadcrumb: the faulting thread's first, then each other thread's with its id. */
static void rep_crumbs(void) {
    const uint64_t self = crumb_tid();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < CRUMB_SLOTS; i++) {
            const crumb_slot* c = &s_crumbs[i];
            if (SDL_GetAtomicInt((SDL_AtomicInt*)&c->state) != CRUMB_LIVE) continue;
            if ((c->tid == self) != (pass == 0)) continue;
            if (pass == 0) {
                rep_str("  in progress on this thread: ");
            } else {
                rep_str("  in progress on thread ");
#if defined(_WIN32)
                rep_dec((int64_t)c->tid);
#else
                rep_hex(c->tid, 1);
#endif
                rep_str(": ");
            }
            rep_strn(c->text, sizeof c->text);
            rep_str("\n");
            rep_flush();
        }
    }
}

#if !defined(_WIN32)

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__GLIBC__) || defined(__APPLE__)
#  include <execinfo.h>
#  define OPENPETE_HAS_BACKTRACE 1
#endif

#if defined(__linux__)
#  include <ucontext.h>
#  define OPENPETE_HAS_UCONTEXT 1
#endif

/* Alternate signal stack size per thread: the handler's frames plus
 * backtrace's unwinder. */
#define CRASH_ALTSTACK_BYTES (64u * 1024u)

/* A pipe the handler copies candidate stack words through: write() of an
 * unmapped address fails with EFAULT instead of faulting, so the words at
 * the faulting RSP are read only when they are mapped. */
static int s_probe[2] = { -1, -1 };

/** @brief Write the formatted report to every crash sink with write(), then empty the buffer. */
static void rep_flush(void) {
    int fds[2];
    int n = openpete_log_file_crash_fds(fds);
    for (int i = 0; i < n; i++) {
        size_t off = 0;
        while (off < s_rep_len) {
            ssize_t w = write(fds[i], s_rep + off, s_rep_len - off);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                break;
            }
            off += (size_t)w;
        }
    }
    s_rep_len = 0;
}

/** @brief Read the 8-byte word at addr only when it is mapped, through the probe pipe.
 *
 * @param addr Address to read.
 * @param out Receives the word.
 * @return 1 when read, 0 when the address is unmapped or the probe is unavailable. */
static int probe_read_u64(uintptr_t addr, uint64_t* out) {
    if (s_probe[0] < 0) return 0;
    if (write(s_probe[1], (const void*)addr, sizeof *out) != (ssize_t)sizeof *out) return 0;
    return read(s_probe[0], out, sizeof *out) == (ssize_t)sizeof *out;
}

/** @brief Append the host registers from the signal context and the words at RSP that are mapped.
 *
 * @param ucontext The handler's context argument; may be NULL. */
static void rep_host_context(void* ucontext) {
#ifdef OPENPETE_HAS_UCONTEXT
    if (!ucontext) return;
    const ucontext_t* uc = (const ucontext_t*)ucontext;
    uint64_t rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    uint64_t rsp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
    uint64_t rbp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
    rep_str("  host RIP="); rep_hex(rip, 1); rep_str(" RSP="); rep_hex(rsp, 1);
    rep_str(" RBP="); rep_hex(rbp, 1); rep_str("\n");
    for (int i = 0; i < 8; i++) {
        uint64_t w = 0;
        rep_str("  [rsp+"); rep_dec(i * 8); rep_str("]=");
        if (probe_read_u64((uintptr_t)(rsp + (uint64_t)i * 8u), &w)) rep_hex(w, 1);
        else rep_str("(unmapped)");
        rep_str("\n");
    }
#else
    (void)ucontext;
#endif
}

/** @brief Name a caught signal. */
static const char* sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGBUS:  return "SIGBUS";
        default:      return "?";
    }
}

/** @brief Write the crash report to the original stderr and the log file, then re-raise the signal under its default disposition.
 *
 * Runs on the thread's alternate stack. The report holds the signal, the
 * crash's error reference (OP-E-CRASH), the fault address and si_code (named as a game-fiber stack overflow when the
 * address is in the fiber's guard page), the host RIP/RSP/RBP and the
 * mapped words at RSP, the guest registers with their symbols, the mod
 * whose override chain was executing, the breadcrumbs of the calls in
 * progress (rep_crumbs), and last the host backtrace
 * (backtrace and backtrace_symbols_fd; libgcc is loaded at install), which is skipped on a
 * stack overflow because it walks the exhausted stack. Only
 * async-signal-safe calls and lock-free table reads are made.
 * @param sig Signal number.
 * @param info Signal info; may be NULL.
 * @param ucontext Host ucontext_t at the fault; may be NULL. */
static void on_signal(int sig, siginfo_t* info, void* ucontext) {
    int overflow = info && openpete_fiber_guard_contains((uintptr_t)info->si_addr);
    (void)openpete_log_file_barrier(250);
    s_rep_len = 0;
    rep_str("\n=== openpete crash: "); rep_str(sig_name(sig)); rep_str(" ===\n");
    rep_str("  "); rep_str(openpete_error_ref(OP_E_CRASH)); rep_str("\n");
    if (info) {
        rep_str("  fault addr="); rep_hex((uint64_t)(uintptr_t)info->si_addr, 1);
        rep_str(" code="); rep_dec(info->si_code); rep_str("\n");
    }
    if (overflow) rep_str("  game fiber stack overflow (fault in the fiber's guard page)\n");
    rep_host_context(ucontext);
    rep_guest_state();
    rep_flush();
    rep_crumbs();

#ifdef OPENPETE_HAS_BACKTRACE
    if (!overflow) {
        void* frames[32];
        int n = backtrace(frames, 32);
        rep_str("  host backtrace ("); rep_dec(n); rep_str(" frames):\n");
        rep_flush();
        int fds[2];
        int nf = openpete_log_file_crash_fds(fds);
        for (int i = 0; i < nf; i++) backtrace_symbols_fd(frames, n, fds[i]);
    }
#endif

    /* SA_RESETHAND restored the default disposition on entry; the raised
     * signal is delivered when the handler returns. */
    raise(sig);
}

void openpete_crash_thread_attach(void) {
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_sp) return;
    void* mem = mmap(NULL, CRASH_ALTSTACK_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = mem;
    ss.ss_size = CRASH_ALTSTACK_BYTES;
    if (sigaltstack(&ss, NULL) != 0) munmap(mem, CRASH_ALTSTACK_BYTES);
}

void openpete_psx_install_crash_handler(void) {
#ifdef OPENPETE_HAS_BACKTRACE
    /* backtrace() loads libgcc_s on first use, which allocates; do it here
     * so the handler's call finds it loaded. */
    void* warm[1];
    (void)backtrace(warm, 1);
#endif
    if (s_probe[0] < 0 && pipe(s_probe) != 0) s_probe[0] = s_probe[1] = -1;
    openpete_crash_thread_attach();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
#ifdef SIGBUS
    sigaction(SIGBUS,  &sa, NULL);
#endif
    OPL_INFO_F("boot", "crash handler installed (SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS, "
               "alternate stack)");
}

void openpete_crash_terminate_now(int code) {
    _Exit(fatal_status(code));
}

#else  /* _WIN32 - Win32 SEH unhandled-exception filter */

#include <windows.h>
#include <dbghelp.h>
#include <io.h>

/* The minidump path, resolved at install so the filter formats nothing. */
static wchar_t s_dump_path[1100];

/* Thread that installed the handler - i.e. main. Lets the banner say whether
 * the fault was on main or a worker without the reader having to guess. */
static DWORD g_main_tid;

/** @brief Write the formatted report to every crash sink with WriteFile and flush the log file, then empty the buffer.
 *
 * The sinks are the handles behind the log mirror's descriptors: stderr is
 * a pipe the pump thread drains, and the process ends when the filter
 * returns, so the report goes to the file handle directly. */
static void rep_flush(void) {
    int fds[2];
    int n = openpete_log_file_crash_fds(fds);
    for (int i = 0; i < n; i++) {
        HANDLE h = (HANDLE)_get_osfhandle(fds[i]);
        if (h == INVALID_HANDLE_VALUE || !h) continue;
        DWORD put = 0;
        WriteFile(h, s_rep, (DWORD)s_rep_len, &put, NULL);
        if (n > 1 && i == 1) FlushFileBuffers(h);
    }
    s_rep_len = 0;
}

static const char* seh_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:    return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:      return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:    return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_BREAKPOINT:          return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_INT_OVERFLOW:        return "EXCEPTION_INT_OVERFLOW";
        case 0xC0000409u:                   return "STATUS_STACK_BUFFER_OVERRUN";
        default:                            return "?";
    }
}

/* What the minidump writer thread needs from the faulting thread. */
typedef struct {
    EXCEPTION_POINTERS* ep;
    DWORD               tid;
    BOOL                ok;
} dump_job_t;

/** @brief Thread body: write a MiniDumpNormal of the process for the faulting thread's exception to s_dump_path.
 *
 * Runs on its own thread because the faulting thread's stack may be
 * exhausted (EXCEPTION_STACK_OVERFLOW) and MiniDumpWriteDump needs stack.
 * @param arg The dump_job_t.
 * @return 0. */
static DWORD WINAPI dump_thread(LPVOID arg) {
    OP_ROLE_BIND(crashdump);
    dump_job_t* job = (dump_job_t*)arg;
    HANDLE f = CreateFileW(s_dump_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = job->tid;
    mei.ExceptionPointers = job->ep;
    mei.ClientPointers = FALSE;
    job->ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                MiniDumpNormal, &mei, NULL, NULL);
    CloseHandle(f);
    return 0;
}

/** @brief Append the exception banner, the fault address and the host registers of the exception record.
 *
 * The banner is followed by the crash's error reference (OP-E-CRASH).
 * @param ep The exception's pointers. */
static void rep_seh_context(const EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    rep_str("\n=== openpete crash: "); rep_str(seh_name(code)); rep_str(" (");
    rep_hex(code, 8); rep_str(") thread="); rep_dec((int64_t)GetCurrentThreadId());
    rep_str(GetCurrentThreadId() == g_main_tid ? " (main) ===\n" : " ===\n");
    rep_str("  "); rep_str(openpete_error_ref(OP_E_CRASH)); rep_str("\n");
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        rep_str("  fault addr="); rep_hex((uint64_t)ep->ExceptionRecord->ExceptionInformation[1], 1);
        rep_str(kind == 0 ? " kind=read\n" : kind == 1 ? " kind=write\n" : kind == 8 ? " kind=DEP\n" : " kind=?\n");
    }
    const CONTEXT* cx = ep->ContextRecord;
    rep_str("  host RIP="); rep_hex((uint64_t)cx->Rip, 1); rep_str(" RSP="); rep_hex((uint64_t)cx->Rsp, 1);
    rep_str(" RBP="); rep_hex((uint64_t)cx->Rbp, 1); rep_str("\n");
}

/** @brief Unhandled-exception filter: write the report to the log file and stderr, write a minidump, and continue the search so the process terminates.
 *
 * It first waits up to 250 ms for the log pump to write what the stderr
 * pipe holds (openpete_log_file_barrier); the
 * report holds the exception context, the guest state and the breadcrumbs
 * of the calls in progress (rep_crumbs).
 * @param ep The exception.
 * @return EXCEPTION_CONTINUE_SEARCH. */
static LONG WINAPI on_seh(EXCEPTION_POINTERS* ep) {
    (void)openpete_log_file_barrier(250);
    s_rep_len = 0;
    rep_seh_context(ep);
    rep_guest_state();
    rep_flush();
    rep_crumbs();

    if (s_dump_path[0]) {
        dump_job_t job = { ep, GetCurrentThreadId(), FALSE };
        HANDLE th = CreateThread(NULL, 256 * 1024, dump_thread, &job, 0, NULL);
        if (th) {
            WaitForSingleObject(th, 30000);
            CloseHandle(th);
        }
        char path[1100];
        if (WideCharToMultiByte(CP_ACP, 0, s_dump_path, -1, path, (int)sizeof path, NULL, NULL) <= 0)
            path[0] = '\0';
        rep_str(job.ok ? "  minidump: " : "  minidump FAILED: ");
        rep_str(path);
        rep_str("\n");
        rep_flush();
    }
    /* Hand back to Windows so the OS still terminates the process.
     * EXECUTE_HANDLER would swallow the exception. */
    return EXCEPTION_CONTINUE_SEARCH;
}

void openpete_crash_thread_attach(void) {
    /* Reserve stack for the filter when this thread overflows. */
    ULONG guarantee = 64u * 1024u;
    SetThreadStackGuarantee(&guarantee);
}

/** @brief Write the minidump's directory: the open log file's directory, else the logs state directory, created.
 *
 * An explicit log file (OPENPETE_LOG_FILE, --log-file) keeps the dump
 * beside it; the logs state directory resolves under a sandbox's root.
 * @param dir Receives the directory.
 * @param cap Capacity of @p dir.
 * @return 0, or -1 when neither resolves. */
static int crash_dump_dir(char* dir, size_t cap) {
    const char* log = openpete_log_file_path();
    if (log && *log) {
        const char* slash = strrchr(log, '\\');
        const char* fwd = strrchr(log, '/');
        if (fwd && (!slash || fwd > slash)) slash = fwd;
        if (slash && (size_t)(slash - log) < cap) {
            snprintf(dir, cap, "%.*s", (int)(slash - log), log);
            return 0;
        }
    }
    return openpete_paths_state_dir("logs", dir, cap, 1);
}

void openpete_psx_install_crash_handler(void) {
    g_main_tid = GetCurrentThreadId();
    char dir[1024];
    if (crash_dump_dir(dir, sizeof dir) == 0) {
        char path[1100];
        snprintf(path, sizeof path, "%s\\openpete-crash.dmp", dir);
        if (!MultiByteToWideChar(CP_ACP, 0, path, -1, s_dump_path,
                                 (int)(sizeof s_dump_path / sizeof s_dump_path[0])))
            s_dump_path[0] = 0;
    }
    openpete_crash_thread_attach();
    SetUnhandledExceptionFilter(on_seh);
    OPL_INFO_F("boot", "crash handler installed (Win32 SEH unhandled-exception filter, "
               "minidump to the logs directory)");
}

void openpete_crash_terminate_now(int code) {
    code = fatal_status(code);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    _Exit(code);
}

#endif  /* !_WIN32 */

/* The registered exit flushes (openpete_crash_set_exit_flush) as atomic
 * pointers, since the stall monitor thread reads them, the reason of the
 * running fatal exit, and 1 once a fatal exit has claimed the single run. */
static void* s_exit_flush[OP_EXIT_FLUSH_SLOTS];
static void* s_exit_reason;
static SDL_AtomicInt s_exit_flush_ran;

void openpete_crash_set_exit_flush(op_exit_flush_slot_t slot, void (*fn)(void)) {
    if ((unsigned)slot >= OP_EXIT_FLUSH_SLOTS) return;
    SDL_SetAtomicPointer(&s_exit_flush[slot], (void*)fn);
}

void openpete_crash_run_exit_flush(const char* reason) {
    if (!SDL_CompareAndSwapAtomicInt(&s_exit_flush_ran, 0, 1)) return;
    SDL_SetAtomicPointer(&s_exit_reason, (void*)reason);
    for (int i = 0; i < OP_EXIT_FLUSH_SLOTS; i++) {
        void (*fn)(void) = (void (*)(void))SDL_GetAtomicPointer(&s_exit_flush[i]);
        if (fn) fn();
    }
}

const char* openpete_crash_exit_reason(void) {
    return (const char*)SDL_GetAtomicPointer(&s_exit_reason);
}
