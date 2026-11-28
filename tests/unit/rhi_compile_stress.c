/* rhi_compile_stress - variant-explicit compile jobs on worker threads
 * against the live mod registry, its per-tick writers and live environment
 * edits.
 *
 * Three roles run at once, as the renderer's threads do:
 *   - the display owner (main): under short registry read holds it makes
 *     compile jobs (rhi_compile_job_new) for every mod postfx pass and
 *     material slot that holds no pipeline and for a built-in pipeline in
 *     each reachable variant, writes environment variables the way the
 *     overlay's live env rows do (OPENPETE_SHADER_DIR among them, which job
 *     making reads), installs results into their slots after re-resolving
 *     the slot and comparing its generation, discards stale results as
 *     Vulkan objects, and retires swept and installed pipelines through
 *     rhi_retire_pipeline across begun frames;
 *   - N compile workers: rhi_compile_job_run on jobs from a locked queue,
 *     destroying the variant jobs themselves (rhi_compile_job_discard);
 *   - the guest thread: the per-tick writers (material_set_params,
 *     postfx_set_uniforms), material_enable, and a hot reload (remove the
 *     mod's shaders, passes and materials, register them again) every 256
 *     iterations, each call timed: its duration is the write-lock wait
 *     plus the call's own work.
 *
 * The run is made three times: with no compiles (jobs discarded unrun),
 * with the harness's own workers, and on the RHI compile pool
 * (rhi_compile_pool_submit), whose install callbacks the display owner runs
 * under its read hold (rhi_rebuild_poll drains them) and where the display
 * owner also requests a live variant switch every 16 cycles, and every 32 a
 * second one the next cycle (rhi_rebuild_request), keeps 8 built-in pipelines registered (each new one
 * retires the oldest, so members leave rebuilds in flight). In every leg
 * the environment writes run inside rhi_env_write_guard, and every compile
 * run (rhi_compile_job_run) and the guest's file commits
 * (op_atomic_file_set_env_guard) inside its read side. Reported: each writer's maximum
 * and mean call time in each leg, the display owner's registry read holds,
 * the compile and install counts (engagement), the pool leg's rebuild
 * counts, seam violations, and, on Linux, the getenv and fopen calls made on
 * worker threads (harness workers and the pool's openpete-compile threads;
 * the harness interposes both and attributes each by its caller: this
 * executable, which links the RHI, or the Vulkan driver's shared library),
 * and in each leg the registry lock acquisitions and the pipeline creations
 * by thread role, and the creations made holding the registry lock (the
 * census: the link wraps SDL's read-write lock calls, and volk's
 * vkCreateGraphicsPipelines pointer is routed through the harness). The
 * times are reported, not judged: they follow the host's scheduling.
 * Exit 0 when each compiling leg ran jobs, installed results, created
 * pipelines on compile workers, made no getenv or fopen call from this
 * executable's code on a worker, took the registry lock on no thread but
 * the display owner and the guest, and created no pipeline holding it (so
 * a writer waits only on the display owner's holds, and those hold no
 * compile); the pool leg swapped
 * rebuilds, superseded one, discarded stale results, lost a member to
 * retirement in flight and failed none; no seam violation was seen; a link
 * and unlink made off the display owner each counted one seam violation (the
 * negative control); the launcher's precompile recorded one built-in
 * description and skipped one given SPIR-V words, ran nothing while the
 * pool was paused, compiled every key it queued once resumed, and copies of
 * one create info submitted together waited for each other (the ahead
 * control); and a pool stopped with jobs queued and in flight
 * accounted every job as run or cancelled (the stop control);
 * 1 otherwise; 77 when no Vulkan device can be created.
 *
 * Usage: rhi_compile_stress [workers] [seconds] [shader_dir|-] [pool|boot]
 * workers is the harness worker count and the pool size
 * (OPENPETE_COMPILE_POOL). shader_dir, when given, is a directory of built
 * .spv files the display owner points OPENPETE_SHADER_DIR at on alternate
 * cycles. "pool" skips the harness-worker leg. "boot" runs only the boot
 * control (boot_control) on a pool sized without OPENPETE_COMPILE_POOL:
 * one worker per logical core until rhi_compile_pool_first_tick, background
 * tasks never above the steady count, then at most the steady count of any
 * task running at once, every queued task completing. Under ThreadSanitizer
 * (target rhi_compile_stress_tsan) a Linux kernel with high mmap
 * randomization needs `setarch "$(uname -m)" -R`. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* RTLD_NEXT */
#endif
#include "rhi.h"
#include "rhi_embedded_shaders.h"
#include "openpete_atomic_file.h"
#include "openpete_mods.h"
#include "openpete_native_render.h"
#include "mod_glsl.h"
#include "volk.h"

#include <SDL3/SDL.h>

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* ---- interposed environment and file reads (Linux) ------------------- */

static _Thread_local int t_worker;   /* set on compile worker threads */
/* Calls on worker threads whose caller is in this executable (openpete
 * code, the RHI and SDL are linked in statically) and calls from a shared
 * library (the Vulkan driver inside vkCreateGraphicsPipelines). */
static atomic_uint s_worker_getenv, s_worker_fopen, s_driver_getenv, s_driver_fopen;

#if defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
extern char **environ;

/** @brief Return whether a code address lies in this executable rather than a shared library.
 *
 * @param addr The address.
 * @return true for the executable. */
static bool in_exe(const void *addr)
{
    static const char *exe;
    Dl_info self, at;
    if (!exe && dladdr((const void *)&in_exe, &self)) exe = self.dli_fname;
    return dladdr(addr, &at) && exe && strcmp(at.dli_fname, exe) == 0;
}

/* The calling thread is an RHI compile-pool worker: 0 unknown, 1 yes, 2 no. */
static _Thread_local int t_pool_thread;
/* Set while secure_getenv forwards to getenv, so the call counts once. */
static _Thread_local int t_forwarding;

/** @brief Return whether the calling thread compiles: a harness worker, or an RHI compile-pool worker (named openpete-compile).
 *
 * @return true on a compile worker. */
static bool worker_thread(void)
{
    if (t_forwarding) return false;
    if (t_worker) return true;
    if (!t_pool_thread) {
        char name[16] = {0};
        pthread_getname_np(pthread_self(), name, sizeof name);
        t_pool_thread = strncmp(name, "openpete-compil", 15) == 0 ? 1 : 2;
    }
    return t_pool_thread == 1;
}

/** @brief Count one worker-thread call, as the executable's or the driver's by its caller.
 *
 * @param caller The call's return address.
 * @param own Counter for a caller in the executable.
 * @param driver Counter for a caller in a shared library. */
static void count_worker_call(const void *caller, atomic_uint *own, atomic_uint *driver)
{
    if (worker_thread()) atomic_fetch_add(in_exe(caller) ? own : driver, 1u);
}

/** @brief Look a variable up in environ, counting calls made on a compile worker.
 *
 * @param name Variable name.
 * @return Its value, or NULL. */
char *getenv(const char *name)
{
    count_worker_call(__builtin_return_address(0), &s_worker_getenv, &s_driver_getenv);
    const size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

/** @brief getenv under the secure name, counted the same way.
 *
 * @param name Variable name.
 * @return Its value, or NULL. */
char *secure_getenv(const char *name)
{
    count_worker_call(__builtin_return_address(0), &s_worker_getenv, &s_driver_getenv);
    t_forwarding = 1;
    char *v = getenv(name);
    t_forwarding = 0;
    return v;
}

/** @brief Open a file through the next fopen, counting calls made on a compile worker.
 *
 * @param path File path.
 * @param mode fopen mode.
 * @return The stream, or NULL. */
FILE *fopen(const char *path, const char *mode)
{
    typedef FILE *(*fopen_fn)(const char *, const char *);
    static fopen_fn real;
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    count_worker_call(__builtin_return_address(0), &s_worker_fopen, &s_driver_fopen);
    return real ? real(path, mode) : NULL;
}
#endif

/* ---- registry lock and pipeline creation census ---------------------- */

/* Thread roles: the display owner (main), the guest thread, and every other
 * thread (the harness's compile workers, the RHI pool's openpete-compile
 * threads, the driver's own threads). */
enum { ROLE_OTHER, ROLE_DISPLAY, ROLE_GUEST, ROLE_COUNT };
static _Thread_local int t_role;
/* The registry lock's read or write depth on this thread. */
static _Thread_local int t_reg_depth;
/* Set while main takes the registry lock once to learn its address. */
static _Thread_local int t_reg_capture;
static SDL_RWLock *s_reg_rwlock;

/* One leg's census: registry lock acquisitions by role, and
 * vkCreateGraphicsPipelines calls by role and with the registry lock held. */
static struct {
    atomic_uint reg_taken[ROLE_COUNT];
    atomic_uint creates[ROLE_COUNT];
    atomic_uint creates_under_registry;
} s_census;

/** @brief Count a registry lock acquisition by the calling thread's role.
 *
 * The link maps SDL_LockRWLockForReading and SDL_LockRWLockForWriting to
 * the wrappers below (-Wl,--wrap), so every acquisition mod_shader.c makes
 * passes here; the registry's lock is told apart from every other SDL
 * read-write lock by the address main captured.
 * @param lock The lock being taken. */
static void reg_census_take(SDL_RWLock *lock)
{
    if (t_reg_capture) s_reg_rwlock = lock;
    if (!lock || lock != s_reg_rwlock) return;
    t_reg_depth++;
    atomic_fetch_add(&s_census.reg_taken[t_role], 1u);
}

void SDLCALL __real_SDL_LockRWLockForReading(SDL_RWLock *rwlock);
void SDLCALL __real_SDL_LockRWLockForWriting(SDL_RWLock *rwlock);
void SDLCALL __real_SDL_UnlockRWLock(SDL_RWLock *rwlock);

/** @brief Take a read-write lock for reading, counting a registry acquisition.
 *
 * @param rwlock The lock. */
void SDLCALL __wrap_SDL_LockRWLockForReading(SDL_RWLock *rwlock)
{
    __real_SDL_LockRWLockForReading(rwlock);
    reg_census_take(rwlock);
}

/** @brief Take a read-write lock for writing, counting a registry acquisition.
 *
 * @param rwlock The lock. */
void SDLCALL __wrap_SDL_LockRWLockForWriting(SDL_RWLock *rwlock)
{
    __real_SDL_LockRWLockForWriting(rwlock);
    reg_census_take(rwlock);
}

/** @brief Release a read-write lock, ending the thread's registry hold when it is the registry's.
 *
 * @param rwlock The lock. */
void SDLCALL __wrap_SDL_UnlockRWLock(SDL_RWLock *rwlock)
{
    if (rwlock && rwlock == s_reg_rwlock && t_reg_depth > 0) t_reg_depth--;
    __real_SDL_UnlockRWLock(rwlock);
}

static PFN_vkCreateGraphicsPipelines s_real_create_pipelines;

/** @brief vkCreateGraphicsPipelines through the census: count the call by role and when the caller holds the registry lock, then create.
 *
 * Installed in volk's function pointer after the device is created, so
 * every pipeline the RHI creates (compile runs, variant probes, library
 * links) passes here.
 * @param device Device.
 * @param cache Pipeline cache.
 * @param n Create info count.
 * @param infos Create infos.
 * @param alloc Allocator.
 * @param out Receives the pipelines.
 * @return The driver's result. */
static VKAPI_ATTR VkResult VKAPI_CALL census_create_pipelines(
    VkDevice device, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *infos,
    const VkAllocationCallbacks *alloc, VkPipeline *out)
{
    atomic_fetch_add(&s_census.creates[t_role], 1u);
    if (t_reg_depth) atomic_fetch_add(&s_census.creates_under_registry, 1u);
    return s_real_create_pipelines(device, cache, n, infos, alloc, out);
}

/** @brief Start the census on the main thread: learn the registry lock's address, then route pipeline creation through the census.
 *
 * Runs after the device exists and before any leg starts a thread; main
 * has already made the registry's first, unsynchronized lock use. */
static void census_start(void)
{
    t_role = ROLE_DISPLAY;
    t_reg_capture = 1;
    openpete_mods_registry_read_lock();
    t_reg_capture = 0;
    openpete_mods_registry_read_unlock();
    s_real_create_pipelines = vkCreateGraphicsPipelines;
    vkCreateGraphicsPipelines = census_create_pipelines;
}

/* ---- stubs for the mod loader and renderer symbols mod_shader.c calls -- */

OPENPETE_THREAD_LOCAL int openpete_mods_in_present;
static char s_mod_dir[512];
static const char *const k_mod_id = "stress";

/** @brief Return the stress mod's id.
 *
 * @param self Unused.
 * @return "stress". */
const char *openpete_mods_id_of(openpete_mod_t *self) { (void)self; return k_mod_id; }
/** @brief Return the stress mod's directory.
 *
 * @param self Unused.
 * @return The directory. */
const char *openpete_mods_dir_of(openpete_mod_t *self) { (void)self; return s_mod_dir; }
/** @brief Return the staged SDK directory.
 *
 * @return "". */
const char *openpete_mods_sdk_dir(void) { return ""; }
/** @brief Report every mod enabled.
 *
 * @param id Unused.
 * @return 1. */
int openpete_mods_is_enabled(const char *id) { (void)id; return 1; }
/** @brief Return a mod's load index.
 *
 * @param self Unused.
 * @return 0. */
int openpete_mods_load_index_of(openpete_mod_t *self) { (void)self; return 0; }
/** @brief Return a mod's api pin.
 *
 * @param self Unused.
 * @return 13. */
int openpete_mods_api_pin_of(openpete_mod_t *self) { (void)self; return 13; }
/** @brief Return the running game's id.
 *
 * @return "spyro-1". */
const char *game_get_id(void) { return "spyro-1"; }
/** @brief Return the toggle epoch.
 *
 * @return 0. */
unsigned openpete_mods_toggle_epoch(void) { return 0; }
/** @brief Open no guarded stretch: the test installs no guest-byte guard.
 *
 * @param mod_id Unused. */
void openpete_mods_guard_enter(const char *mod_id) { (void)mod_id; }
/** @brief Close no guarded stretch. */
void openpete_mods_guard_leave(void) {}
/** @brief Open no dispatch site.
 * @param ctx the site's kind.
 * @return the enclosing site, always NONE here. */
openpete_mods_site_t openpete_mods_site_open(openpete_mods_ctx ctx) {
    (void)ctx;
    const openpete_mods_site_t none = {OPENPETE_MODS_CTX_NONE, 0, 0};
    return none;
}
/** @brief Close no dispatch site.
 * @param prev the value the open returned. */
void openpete_mods_site_close(openpete_mods_site_t prev) { (void)prev; }
/** @brief Accept the material tag-split arming.
 *
 * @param on Unused. */
void openpete_native_set_tag_split(int on) { (void)on; }

/** @brief Map a shader path's extension to its stage.
 *
 * @param path Path.
 * @return OPENPETE_MOD_GLSL_STAGE_FRAG for .frag, VERT for .vert, else -1. */
int openpete_mod_glsl_stage_from_path(const char *path)
{
    const size_t n = strlen(path);
    if (n > 5 && strcmp(path + n - 5, ".frag") == 0) return OPENPETE_MOD_GLSL_STAGE_FRAG;
    if (n > 5 && strcmp(path + n - 5, ".vert") == 0) return OPENPETE_MOD_GLSL_STAGE_VERT;
    return -1;
}

/** @brief Return a fixed source text for any path, so the registry takes the compile route.
 *
 * @param path Unused.
 * @param len_out Receives the length.
 * @return A malloc'd copy of the text. */
char *openpete_mod_glsl_read_file(const char *path, size_t *len_out)
{
    (void)path;
    static const char k_src[] = "// stress\n";
    char *s = malloc(sizeof k_src);
    if (s) memcpy(s, k_src, sizeof k_src);
    if (len_out) *len_out = sizeof k_src - 1;
    return s;
}

/** @brief Fill an include context with empty directories.
 *
 * @param inc The context.
 * @param shader_path Unused.
 * @param sdk_dir Unused. */
void openpete_mod_glsl_include_init(openpete_mod_glsl_include_t *inc, const char *shader_path,
                                    const char *sdk_dir)
{
    (void)shader_path; (void)sdk_dir;
    memset(inc, 0, sizeof *inc);
}

/** @brief Report no terrain profile.
 *
 * @param src Unused.
 * @param stage Unused.
 * @return 0. */
int openpete_mod_glsl_has_terrain_profile(const char *src, int stage)
{
    (void)src; (void)stage;
    return 0;
}

static const uint32_t *s_frag_words;   /* the postfx fragment every mod shader compiles to */
static size_t          s_frag_n;

/** @brief Compile any source to a copy of the raster-quantize postfx fragment.
 *
 * @param src Unused.
 * @param stage Unused.
 * @param inc Unused.
 * @param preamble Unused.
 * @param log_path Unused.
 * @param out_words Receives a malloc'd copy.
 * @return The word count. */
size_t openpete_mod_glsl_compile(const char *src, int stage, const openpete_mod_glsl_include_t *inc,
                                 const char *preamble, const char *log_path, uint32_t **out_words)
{
    (void)src; (void)stage; (void)inc; (void)preamble; (void)log_path;
    *out_words = malloc(s_frag_n * 4);
    if (!*out_words) return 0;
    memcpy(*out_words, s_frag_words, s_frag_n * 4);
    return s_frag_n;
}
/** @brief Preprocess nothing: the stress mod's module names no op_material, so no composition member is prepared.
 *
 * @return NULL. */
char *openpete_mod_glsl_preprocess(const char *src, int stage, const openpete_mod_glsl_include_t *inc,
                                   const char *preamble, const char *log_path, size_t *len_out)
{
    (void)src; (void)stage; (void)inc; (void)preamble; (void)log_path; (void)len_out;
    return NULL;
}

/* ---- the job queue ----------------------------------------------------- */

typedef enum { JOB_BUILTIN, JOB_VARIANT, JOB_POSTFX, JOB_MATERIAL } job_kind;

typedef struct qjob {
    struct qjob     *next;
    rhi_compile_job *job;
    job_kind         kind;
    int              index;   /* postfx pass or material index */
} qjob;

static struct {
    SDL_Mutex     *mx;
    SDL_Condition *cv;
    qjob          *todo, *done;
    bool           stop;
} s_q;

/** @brief Push a job onto a list under the queue lock and wake a worker.
 *
 * @param list The list head.
 * @param j The job. */
static void q_push(qjob **list, qjob *j)
{
    SDL_LockMutex(s_q.mx);
    j->next = *list;
    *list = j;
    SDL_SignalCondition(s_q.cv);
    SDL_UnlockMutex(s_q.mx);
}

/* Counts of one run. */
typedef struct {
    atomic_uint ran, ran_ok, worker_discards;
    unsigned enqueued, installed, stale, slot_taken, builtin_retired, swept_retired,
             unrun_discards, reloads;
    uint64_t max_ns[4], sum_ns[4];
    unsigned calls[4];
    uint64_t ctl_max_ns;              /* the guest's unlocked control section */
    uint64_t hold_max_ns, hold_sum_ns; /* the display owner's registry read holds, wall */
    uint64_t hold_cpu_max_ns;          /* the same holds in the thread's CPU time */
    unsigned holds;
    /* Pool leg: live rebuild requests and what their swaps reported. */
    unsigned rb_requests, rb_swaps, rb_rebuilt, rb_failed, rb_late, rb_stale, rb_retired,
             rb_superseding, env_writes;
    /* Pool leg: slots set pending, pending results a switch made stale and
     * recompiled, pending slots a sweep dropped. */
    unsigned pending_set, pending_requeued, pending_swept;
} run_stats;

/* Where a leg's jobs go. */
typedef enum { MODE_NONE, MODE_QUEUE, MODE_POOL } leg_mode;

static rhi_device *s_dev;
static run_stats   s_st;
static atomic_bool s_guest_stop;

/** @brief Run jobs from the queue until stopped: variant jobs are destroyed here, the rest go to the done list.
 *
 * @param arg Unused.
 * @return 0. */
static int SDLCALL worker_main(void *arg)
{
    (void)arg;
    t_worker = 1;
    for (;;) {
        SDL_LockMutex(s_q.mx);
        while (!s_q.todo && !s_q.stop) SDL_WaitCondition(s_q.cv, s_q.mx);
        qjob *j = s_q.todo;
        if (j) s_q.todo = j->next;
        SDL_UnlockMutex(s_q.mx);
        if (!j) return 0;
        const int r = rhi_compile_job_run(s_dev, j->job, 0);
        atomic_fetch_add(&s_st.ran, 1u);
        if (r == 0) atomic_fetch_add(&s_st.ran_ok, 1u);
        if (j->kind == JOB_VARIANT) {
            rhi_compile_job_discard(s_dev, j->job);
            atomic_fetch_add(&s_st.worker_discards, 1u);
            free(j);
            continue;
        }
        q_push(&s_q.done, j);
    }
}

/* ---- the guest thread -------------------------------------------------- */

static openpete_mod_t *const k_self = (openpete_mod_t *)&k_mod_id;
enum { W_PARAMS, W_UNIFORMS, W_ENABLE, W_RELOAD };
static const char *const k_writer[4] = { "material_set_params", "postfx_set_uniforms",
                                         "material_enable", "hot_reload" };

/** @brief Register the stress mod's shader, postfx pass (enabled) and material.
 *
 * @param fx Receives the pass handle.
 * @param mat Receives the material handle. */
static void mod_register(int *fx, int *mat)
{
    const int sh = openpete_mods_shader_register(k_self, "shaders/stress.frag");
    *fx = openpete_mods_postfx_register(k_self, sh, 0, 0);
    openpete_mods_postfx_enable(k_self, *fx, 1);
    openpete_mod_material_selector_t sel;
    memset(&sel, 0, sizeof sel);
    sel.struct_size = sizeof sel;
    sel.channel = -1; sel.ident = -1; sel.moby_class = -1; sel.texture = -1; sel.level = -1;
    *mat = openpete_mods_material_register(k_self, &sel, -1, sh, NULL);
}

/** @brief Time one writer call into the run's stats.
 *
 * @param w Writer index.
 * @param t0 SDL_GetTicksNS before the call. */
static void writer_time(int w, Uint64 t0)
{
    const uint64_t dt = SDL_GetTicksNS() - t0;
    if (dt > s_st.max_ns[w]) s_st.max_ns[w] = dt;
    s_st.sum_ns[w] += dt;
    s_st.calls[w]++;
}

/** @brief Run the per-tick writers, the enable toggle and a hot reload every 256 iterations until stopped.
 *
 * Swept material slots are never reused, so reloads stop short of the
 * registry's material table bound.
 *
 * @param arg Unused.
 * @return 0. */
static int SDLCALL guest_main(void *arg)
{
    (void)arg;
    int fx = -1, mat = -1;
    t_role = ROLE_GUEST;
    mod_register(&fx, &mat);
    unsigned char block[16] = {0};
    for (unsigned it = 0; !atomic_load(&s_guest_stop); it++) {
        block[0] = (unsigned char)it;
        /* Control: the same copy with no lock, so its maximum is the
         * scheduling jitter the writers' times carry too. */
        Uint64 t0 = SDL_GetTicksNS();
        static volatile unsigned char s_sink[16];
        memcpy((void *)s_sink, block, sizeof block);
        const uint64_t ctl = SDL_GetTicksNS() - t0;
        if (ctl > s_st.ctl_max_ns) s_st.ctl_max_ns = ctl;
        t0 = SDL_GetTicksNS();
        openpete_mods_material_set_params(k_self, mat, block, sizeof block);
        writer_time(W_PARAMS, t0);
        t0 = SDL_GetTicksNS();
        openpete_mods_postfx_set_uniforms(k_self, fx, block, sizeof block);
        writer_time(W_UNIFORMS, t0);
        t0 = SDL_GetTicksNS();
        openpete_mods_material_enable(k_self, mat, (int)(it & 1u));
        writer_time(W_ENABLE, t0);
        if ((it & 255u) == 255u && openpete_mods_material_enum_count() < 240) {
            t0 = SDL_GetTicksNS();
            openpete_mods_postfx_remove_owner(k_mod_id);
            openpete_mods_material_remove_owner(k_mod_id);
            openpete_mods_shader_remove_owner(k_mod_id);
            mod_register(&fx, &mat);
            writer_time(W_RELOAD, t0);
            s_st.reloads++;
        }
        SDL_DelayNS(200000);   /* 0.2 ms: well above the tick rate's write frequency */
    }
    return 0;
}

/* ---- the display owner --------------------------------------------------- */

/** @brief Return the calling thread's CPU time, ns; wall time where no thread clock exists.
 *
 * @return The time. */
static uint64_t thread_cpu_ns(void)
{
#if defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return SDL_GetTicksNS();
#endif
}

/* Postfx desc the precompile builds for a pass: fullscreen vertex stage,
 * the pass's fragment words. */
static const rhi_pipeline_desc k_postfx_desc = {
    .vert_spv = "postfx_fs.vert.spv", .blend = RHI_BLEND_NONE, .postfx = true,
};
/* A depth-tested scene pipeline whose stages declare AFFINE_UV and NOTEX
 * and that the wireframe view selects: every view is a distinct create. */
static const rhi_pipeline_desc k_scene_desc = {
    .vert_spv = "quad.vert.spv", .frag_spv = "quad_key.frag.spv",
    .blend = RHI_BLEND_NONE, .depth_test = true,
    .uses_vram3 = true, .uses_vram4 = true, .uses_vram5 = true,
};

/* Slots with a job in flight, by kind and index. */
static unsigned char s_pending[2][1024];

static void pool_mod_install(rhi_device *dev, rhi_compile_job *job, int rc, void *tag);
static void pool_builtin_install(rhi_device *dev, rhi_compile_job *job, int rc, void *tag);

/** @brief Hand a made job to the leg's sink: discard it unrun, push it on the harness queue, or submit it to the RHI compile pool.
 *
 * @param mode The leg's mode.
 * @param job The job.
 * @param kind Its kind.
 * @param index Postfx pass or material index (mod kinds).
 * @return true when the job is in flight. */
static bool hand_off(leg_mode mode, rhi_compile_job *job, job_kind kind, int index)
{
    s_st.enqueued++;
    if (mode == MODE_NONE) {
        rhi_compile_job_discard(s_dev, job);
        s_st.unrun_discards++;
        return false;
    }
    if (mode == MODE_POOL) {
        if (kind == JOB_VARIANT)
            rhi_compile_pool_submit(s_dev, job, RHI_COMPILE_VARIANT | RHI_COMPILE_PROBE, NULL, NULL);
        else if (kind == JOB_BUILTIN)
            rhi_compile_pool_submit(s_dev, job, 0, pool_builtin_install, NULL);
        else
            rhi_compile_pool_submit(s_dev, job, 0, pool_mod_install,
                                    (void *)(uintptr_t)(((unsigned)kind << 16) | (unsigned)index));
        return true;
    }
    qjob *q = calloc(1, sizeof *q);
    *q = (qjob){ .job = job, .kind = kind, .index = index };
    q_push(&s_q.todo, q);
    return true;
}

/** @brief Enqueue a job for every live pass and material slot that holds no pipeline and has none in flight.
 *
 * Runs under the registry read lock, as the compose's enqueue does.
 * @param mode Where the jobs go (hand_off). */
static void enqueue_mod_slots(leg_mode mode)
{
    const rhi_pipe_variant var = rhi_pipe_variant_current();
    const uint32_t epoch = rhi_pipe_variant_epoch();
    for (int i = 0; i < openpete_mods_postfx_enum_count() && i < 1024; i++) {
        openpete_postfx_pass_t p;
        if (openpete_mods_postfx_enum_at(i, &p) != 0 || *p.pipeline_slot || s_pending[0][i]) continue;
        rhi_compile_job *job = rhi_compile_job_new(s_dev, &k_postfx_desc, NULL, 0, p.frag_spv,
                                                   p.frag_words, var, p.generation, epoch);
        if (job && hand_off(mode, job, JOB_POSTFX, i)) {
            s_pending[0][i] = 1;
            if (mode == MODE_POOL) { *p.pipeline_slot = OPENPETE_POSTFX_PIPE_PENDING; s_st.pending_set++; }
        }
    }
    for (int i = 0; i < openpete_mods_material_enum_count() && i < 1024; i++) {
        openpete_mod_material_gpu_t g;
        int contract = 0, terrain = 0;
        if (openpete_mods_material_enum_at(i, &contract, &terrain, &g) != 0 ||
            *g.pipeline_slot[0] || s_pending[1][i])
            continue;
        /* The pool leg compiles the material slot from the scene stages,
         * which declare AFFINE_UV, so a variant switch can make its pending
         * result stale. */
        rhi_compile_job *job = mode == MODE_POOL
            ? rhi_compile_job_new(s_dev, &k_scene_desc, NULL, 0, NULL, 0, var, g.generation, epoch)
            : rhi_compile_job_new(s_dev, &k_postfx_desc, NULL, 0, g.frag_spv, g.frag_words, var,
                                  g.generation, epoch);
        if (job && hand_off(mode, job, JOB_MATERIAL, i)) {
            s_pending[1][i] = 1;
            if (mode == MODE_POOL) { *g.pipeline_slot[0] = OPENPETE_POSTFX_PIPE_PENDING; s_st.pending_set++; }
        }
    }
}

/** @brief Enqueue the built-in scene pipeline in one view and its five other views as variant jobs.
 *
 * @param cycle Cycle number; selects the installed view.
 * @param mode Where the jobs go (hand_off). */
static void enqueue_builtin(unsigned cycle, leg_mode mode)
{
    for (int v = 0; v < 6; v++) {
        const rhi_pipe_variant var = { .affine = (v & 1) != 0, .notex = v == 2 || v == 3,
                                       .wireframe = v >= 4, .truescope = true };
        rhi_compile_job *job = rhi_compile_job_new(s_dev, &k_scene_desc, NULL, 0, NULL, 0, var, 0,
                                                   rhi_pipe_variant_epoch());
        if (job) hand_off(mode, job, (unsigned)v == cycle % 6u ? JOB_BUILTIN : JOB_VARIANT, 0);
    }
}

/** @brief Install a mod slot's result: only when its slot, re-resolved by index, still holds no pipeline (or the pending value) and its generation equals the job's; otherwise destroy it as Vulkan objects.
 *
 * A pending slot's result whose create info a variant switch changed is
 * recompiled for the variant drawn now and stays pending.
 * @param kind JOB_POSTFX or JOB_MATERIAL.
 * @param index Pass or material index.
 * @param job The job, consumed. */
static void install_mod(job_kind kind, int index, rhi_compile_job *job)
{
    void **slot = NULL;
    unsigned gen = 0;
    openpete_postfx_pass_t pp;
    openpete_mod_material_gpu_t g;
    int contract = 0, terrain = 0;
    if (kind == JOB_POSTFX && openpete_mods_postfx_enum_at(index, &pp) == 0) {
        slot = pp.pipeline_slot; gen = pp.generation;
    } else if (kind == JOB_MATERIAL &&
               openpete_mods_material_enum_at(index, &contract, &terrain, &g) == 0) {
        slot = g.pipeline_slot[0]; gen = g.generation;
    }
    const bool pend = slot && *slot == OPENPETE_POSTFX_PIPE_PENDING;
    if (pend && gen == rhi_compile_job_generation(job) &&
        rhi_compile_job_variant_stale(s_dev, job)) {
        rhi_compile_job_retarget(s_dev, job);
        s_st.pending_requeued++;
        rhi_compile_pool_submit(s_dev, job, 0, pool_mod_install,
                                (void *)(uintptr_t)((unsigned)kind << 16 | (unsigned)index));
        return;
    }
    s_pending[kind == JOB_MATERIAL][index] = 0;
    if (!slot || gen != rhi_compile_job_generation(job)) {
        rhi_compile_job_discard(s_dev, job);
        s_st.stale++;
    } else if (*slot && !pend) {
        rhi_compile_job_discard(s_dev, job);
        s_st.slot_taken++;
    } else {
        rhi_pipeline *p = rhi_compile_job_install(s_dev, job);
        *slot = p;
        if (p) s_st.installed++;
    }
}

/** @brief Install or discard every finished job of the harness queue on the display owner.
 *
 * A mod slot's result goes through install_mod. A built-in result is
 * installed, then retired at once. Runs under the registry read lock. */
static void drain_done(void)
{
    SDL_LockMutex(s_q.mx);
    qjob *j = s_q.done;
    s_q.done = NULL;
    SDL_UnlockMutex(s_q.mx);
    while (j) {
        qjob *next = j->next;
        if (j->kind == JOB_BUILTIN) {
            rhi_pipeline *p = rhi_compile_job_install(s_dev, j->job);
            if (p) { s_st.installed++; rhi_retire_pipeline(s_dev, p); s_st.builtin_retired++; }
        } else {
            install_mod(j->kind, j->index, j->job);
        }
        free(j);
        j = next;
    }
}

static void drain_swept(void);

/* Built-in pipelines the pool leg keeps registered, oldest retired first,
 * so live rebuilds have members to rebuild and to lose while in flight. */
#define LIVE_BUILTINS 8
static rhi_pipeline *s_live[LIVE_BUILTINS];
static unsigned      s_live_next;

/** @brief Pool install callback of a mod slot's job: install_mod, or a discard for a cancelled job.
 *
 * @param dev Device.
 * @param job The job, consumed.
 * @param rc Its run's result, or RHI_POOL_CANCELLED.
 * @param tag Kind (bits 16 and up) and index (low 16 bits). */
static void pool_mod_install(rhi_device *dev, rhi_compile_job *job, int rc, void *tag)
{
    const unsigned v = (unsigned)(uintptr_t)tag;
    const job_kind kind = (job_kind)(v >> 16);
    const int index = (int)(v & 0xFFFFu);
    atomic_fetch_add(&s_st.ran, 1u);
    if (rc == 0) atomic_fetch_add(&s_st.ran_ok, 1u);
    if (rc == RHI_POOL_CANCELLED) {
        s_pending[kind == JOB_MATERIAL][index] = 0;
        rhi_compile_job_discard(dev, job);
        return;
    }
    install_mod(kind, index, job);
}

/** @brief Pool install callback of a built-in job: install it into the live ring, retiring the ring's oldest member.
 *
 * @param dev Device.
 * @param job The job, consumed.
 * @param rc Its run's result, or RHI_POOL_CANCELLED.
 * @param tag Unused. */
static void pool_builtin_install(rhi_device *dev, rhi_compile_job *job, int rc, void *tag)
{
    (void)tag;
    atomic_fetch_add(&s_st.ran, 1u);
    if (rc == 0) atomic_fetch_add(&s_st.ran_ok, 1u);
    if (rc == RHI_POOL_CANCELLED) { rhi_compile_job_discard(dev, job); return; }
    rhi_pipeline *p = rhi_compile_job_install(dev, job);
    if (!p) return;
    s_st.installed++;
    if (s_live[s_live_next]) {
        rhi_retire_pipeline(dev, s_live[s_live_next]);
        s_st.builtin_retired++;
    }
    s_live[s_live_next] = p;
    s_live_next = (s_live_next + 1u) % LIVE_BUILTINS;
}

/** @brief Take a finished live rebuild's counts into the run's stats.
 *
 * @param r The swap's counts. */
static void note_swap(const rhi_rebuild_stats *r)
{
    s_st.rb_swaps++;
    s_st.rb_rebuilt += r->rebuilt;
    s_st.rb_failed += r->failed;
    s_st.rb_late += r->late;
    s_st.rb_stale += r->stale;
    s_st.rb_retired += r->retired;
    s_st.rb_superseding += r->requests > 1u;
}

/** @brief Pool leg's display-owner step under the read hold: install staged results and swap a complete rebuild, enqueue slots and built-ins, and request variant switches.
 *
 * @param cycle Cycle number. */
static void pool_cycle(unsigned cycle)
{
    rhi_rebuild_stats r;
    if (rhi_rebuild_poll(s_dev, &r)) note_swap(&r);
    enqueue_mod_slots(MODE_POOL);
    if ((cycle & 7u) == 0) enqueue_builtin(cycle, MODE_POOL);
    /* Every 16 cycles a switch; every 32, a second one the next cycle,
     * which supersedes the first while its jobs are in flight. */
    if ((cycle & 15u) == 0 || (cycle & 31u) == 1) {
        const unsigned v = (cycle / 16u + (cycle & 1u) * 3u) % 6u;
        const rhi_pipe_variant var = { .affine = (v & 1u) != 0, .notex = v == 2 || v == 3,
                                       .wireframe = v >= 4, .truescope = true };
        rhi_rebuild_request(s_dev, var, false);
        s_st.rb_requests++;
    }
}

/** @brief Finish the pool leg: wait for the pool, swap the rebuild in flight, and retire the live ring. */
static void pool_settle(void)
{
    for (;;) {
        const bool frame = rhi_begin_frame(s_dev);
        openpete_mods_registry_read_lock();
        drain_swept();
        rhi_compile_pool_wait(s_dev, NULL, NULL);
        rhi_rebuild_stats r;
        if (rhi_rebuild_poll(s_dev, &r)) note_swap(&r);
        const bool pending = rhi_rebuild_pending(s_dev);
        openpete_mods_registry_read_unlock();
        if (frame) rhi_end_frame(s_dev);
        if (!pending) break;
    }
    for (unsigned i = 0; i < LIVE_BUILTINS; i++) {
        if (s_live[i]) rhi_retire_pipeline(s_dev, s_live[i]);
        s_live[i] = NULL;
    }
}

/** @brief Retire the pipelines the registry's sweeps retired.
 *
 * Runs under the registry read lock, as the compose's drain does. */
static void drain_swept(void)
{
    void *dead[64];
    int n;
    while ((n = openpete_mods_postfx_take_retired(dead, 64)) > 0)
        for (int i = 0; i < n; i++) {
            rhi_retire_pipeline(s_dev, (rhi_pipeline *)dead[i]);
            s_st.swept_retired++;
        }
}

static void env_write(unsigned cycle, const char *shader_dir);

/** @brief Write environment variables as the overlay's live env rows do, from the display owner, inside rhi_env_write_guard.
 *
 * The registry's live rows write inside the same guard once the renderer
 * installs it (knob_registry_set_env_guard).
 * @param cycle Cycle number.
 * @param shader_dir Directory for OPENPETE_SHADER_DIR on odd cycles, or NULL. */
static void env_edit(unsigned cycle, const char *shader_dir)
{
    rhi_env_write_guard(1);
    s_st.env_writes++;
    env_write(cycle, shader_dir);
    rhi_env_write_guard(0);
}

/** @brief The environment writes env_edit makes.
 *
 * @param cycle Cycle number.
 * @param shader_dir Directory for OPENPETE_SHADER_DIR on odd cycles, or NULL. */
static void env_write(unsigned cycle, const char *shader_dir)
{
    char v[32];
    snprintf(v, sizeof v, "%u", cycle);
    SDL_setenv_unsafe("OPENPETE_STRESS_ROW", v, 1);
    if (cycle & 1u) SDL_unsetenv_unsafe("OPENPETE_STRESS_ALT");
    else SDL_setenv_unsafe("OPENPETE_STRESS_ALT", v, 1);
    if (!shader_dir) return;
    if (cycle & 1u) SDL_setenv_unsafe("OPENPETE_SHADER_DIR", shader_dir, 1);
    else SDL_unsetenv_unsafe("OPENPETE_SHADER_DIR");
}

/** @brief Run one timed leg: guest thread, the compile sink, and the display owner's cycle.
 *
 * @param mode MODE_NONE discards every job unrun, MODE_QUEUE runs them on
 *             the harness's own workers, MODE_POOL on the RHI compile pool
 *             with live rebuild requests.
 * @param workers Harness worker count (MODE_QUEUE).
 * @param seconds Duration.
 * @param shader_dir See env_write. */
static void run_leg(leg_mode mode, int workers, int seconds, const char *shader_dir)
{
    memset(&s_st, 0, sizeof s_st);
    memset(&s_census, 0, sizeof s_census);
    memset(s_pending, 0, sizeof s_pending);
    atomic_store(&s_guest_stop, false);
    s_q.stop = false;
    SDL_Thread *guest = SDL_CreateThread(guest_main, "stress-guest", NULL);
    SDL_Thread *w[64];
    const int nw = mode != MODE_QUEUE ? 0 : workers < 64 ? workers : 64;
    for (int i = 0; i < nw; i++) w[i] = SDL_CreateThread(worker_main, "stress-worker", NULL);
    const Uint64 end = SDL_GetTicksNS() + (Uint64)seconds * 1000000000ull;
    for (unsigned cycle = 0; SDL_GetTicksNS() < end; cycle++) {
        const bool frame = rhi_begin_frame(s_dev);
        openpete_mods_registry_read_lock();
        const Uint64 h0 = SDL_GetTicksNS();
        const uint64_t c0 = thread_cpu_ns();
        drain_swept();
        if (mode == MODE_POOL) {
            pool_cycle(cycle);
        } else {
            drain_done();
            enqueue_mod_slots(mode);
            if ((cycle & 7u) == 0) enqueue_builtin(cycle, mode);
        }
        const uint64_t hold = SDL_GetTicksNS() - h0;
        const uint64_t hold_cpu = thread_cpu_ns() - c0;
        openpete_mods_registry_read_unlock();
        if (hold_cpu > s_st.hold_cpu_max_ns) s_st.hold_cpu_max_ns = hold_cpu;
        if (hold > s_st.hold_max_ns) s_st.hold_max_ns = hold;
        s_st.hold_sum_ns += hold;
        s_st.holds++;
        if (frame) rhi_end_frame(s_dev);
        env_edit(cycle, shader_dir);
        SDL_DelayNS(1000000);
    }
    atomic_store(&s_guest_stop, true);
    SDL_WaitThread(guest, NULL);
    if (mode == MODE_POOL) { pool_settle(); return; }
    SDL_LockMutex(s_q.mx);
    s_q.stop = true;
    SDL_BroadcastCondition(s_q.cv);
    SDL_UnlockMutex(s_q.mx);
    for (int i = 0; i < nw; i++) SDL_WaitThread(w[i], NULL);
    for (qjob *j = s_q.todo; j;) { qjob *n = j->next; rhi_compile_job_discard(s_dev, j->job); free(j); j = n; }
    s_q.todo = NULL;
    openpete_mods_registry_read_lock();
    drain_done();
    drain_swept();
    openpete_mods_registry_read_unlock();
}

/** @brief Print one leg's counts and writer times.
 *
 * @param name Leg name. */
static void report(const char *name)
{
    printf("leg=%s enqueued=%u ran=%u ran_ok=%u installed=%u stale_discarded=%u slot_taken=%u "
           "worker_discarded=%u unrun_discarded=%u builtin_retired=%u swept_retired=%u reloads=%u\n",
           name, s_st.enqueued, atomic_load(&s_st.ran), atomic_load(&s_st.ran_ok), s_st.installed,
           s_st.stale, s_st.slot_taken, atomic_load(&s_st.worker_discards), s_st.unrun_discards,
           s_st.builtin_retired, s_st.swept_retired, s_st.reloads);
    for (int w = 0; w < 4; w++)
        printf("leg=%s writer=%s calls=%u max_ms=%.3f mean_ms=%.4f\n", name, k_writer[w],
               s_st.calls[w], (double)s_st.max_ns[w] / 1e6,
               s_st.calls[w] ? (double)s_st.sum_ns[w] / 1e6 / s_st.calls[w] : 0.0);
    printf("leg=%s read_holds=%u hold_max_ms=%.3f hold_cpu_max_ms=%.3f hold_mean_ms=%.4f "
           "control_max_ms=%.3f\n", name, s_st.holds, (double)s_st.hold_max_ns / 1e6,
           (double)s_st.hold_cpu_max_ns / 1e6,
           s_st.holds ? (double)s_st.hold_sum_ns / 1e6 / s_st.holds : 0.0,
           (double)s_st.ctl_max_ns / 1e6);
    printf("leg=%s registry_taken display=%u guest=%u other=%u pipeline_creates display=%u "
           "guest=%u other=%u under_registry=%u\n", name,
           atomic_load(&s_census.reg_taken[ROLE_DISPLAY]), atomic_load(&s_census.reg_taken[ROLE_GUEST]),
           atomic_load(&s_census.reg_taken[ROLE_OTHER]), atomic_load(&s_census.creates[ROLE_DISPLAY]),
           atomic_load(&s_census.creates[ROLE_GUEST]), atomic_load(&s_census.creates[ROLE_OTHER]),
           atomic_load(&s_census.creates_under_registry));
}

/** @brief Judge a compiling leg: engagement, and which threads took the registry lock and created pipelines under it.
 *
 * A writer waits only on the display owner's read holds when no other
 * thread takes the registry lock and no hold contains a pipeline creation
 * (a compile). Both are counted, not timed: a wait's length depends on
 * how the host schedules the threads, a count does not. The leg is
 * engaged when its jobs compiled and installed, compile workers created
 * pipelines, and the display owner and the guest each took the lock.
 * The writer and hold times report() prints are not judged.
 * @param leg Leg name.
 * @return 1 on a failure, else 0. */
static int judge_leg(const char *leg)
{
    int fail = 0;
    const unsigned other = atomic_load(&s_census.reg_taken[ROLE_OTHER]);
    const unsigned locked = atomic_load(&s_census.creates_under_registry);
    const struct { const char *what; unsigned n; } need[] = {
        { "job compiled", atomic_load(&s_st.ran_ok) }, { "result installed", s_st.installed },
        { "pipeline created on a compile worker", atomic_load(&s_census.creates[ROLE_OTHER]) },
        { "registry hold by the display owner", atomic_load(&s_census.reg_taken[ROLE_DISPLAY]) },
        { "registry write by the guest", atomic_load(&s_census.reg_taken[ROLE_GUEST]) },
    };
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) {
        if (need[i].n) continue;
        printf("verdict: leg=%s VACUOUS (no %s)\n", leg, need[i].what);
        fail = 1;
    }
    if (other) {
        printf("verdict: leg=%s LOCKED registry lock taken %u times on a thread other than the "
               "display owner and the guest\n", leg, other);
        fail = 1;
    }
    if (locked) {
        printf("verdict: leg=%s LOCKED %u pipeline creations made holding the registry lock\n",
               leg, locked);
        fail = 1;
    }
    return fail;
}

/** @brief Report a leg's getenv and fopen calls on compile workers; a call from this executable's code fails.
 *
 * @param leg Leg name.
 * @return 1 on a failure, else 0. */
static int judge_worker_calls(const char *leg)
{
    const unsigned genv = atomic_load(&s_worker_getenv), gfo = atomic_load(&s_worker_fopen);
    printf("leg=%s worker_getenv=%u worker_fopen=%u driver_getenv=%u driver_fopen=%u%s\n", leg,
           genv, gfo, atomic_load(&s_driver_getenv), atomic_load(&s_driver_fopen),
#if defined(__linux__)
           ""
#else
           " (not interposed on this platform)"
#endif
    );
    return genv || gfo;
}

/** @brief Judge the pool leg's engagement: pool runs, staged installs, rebuild swaps, superseded and retired-in-flight members, guarded environment writes.
 *
 * @param p0 The pool's counts before the leg.
 * @return 1 when a path the leg exists to drive was not engaged, else 0. */
static int judge_pool(const rhi_compile_pool_stats *p0)
{
    rhi_compile_pool_stats p1;
    rhi_compile_pool_stats_get(s_dev, &p1);
    printf("leg=pool pool_workers=%u submitted=%u run=%u staged=%u variants_created=%u "
           "variant_probe_hits=%u variant_failed=%u queue_max=%u run_ms=%.1f\n",
           p1.workers, p1.submitted - p0->submitted, p1.run - p0->run, p1.staged - p0->staged,
           p1.variant_created - p0->variant_created,
           p1.variant_probe_hits - p0->variant_probe_hits, p1.variant_failed - p0->variant_failed,
           p1.max_queued, p1.run_ms - p0->run_ms);
    printf("leg=pool rebuild_requests=%u swaps=%u superseding_swaps=%u rebuilt=%u failed=%u "
           "late=%u stale_results=%u retired_in_flight=%u guarded_env_writes=%u\n",
           s_st.rb_requests, s_st.rb_swaps, s_st.rb_superseding, s_st.rb_rebuilt, s_st.rb_failed,
           s_st.rb_late, s_st.rb_stale, s_st.rb_retired, s_st.env_writes);
    printf("leg=pool pending_set=%u pending_requeued=%u stale_discarded=%u\n", s_st.pending_set,
           s_st.pending_requeued, s_st.stale);
    const struct { const char *what; unsigned n; } need[] = {
        { "pool runs", p1.run - p0->run }, { "staged results", p1.staged - p0->staged },
        { "variant jobs", p1.variant_created + p1.variant_probe_hits - p0->variant_created -
                          p0->variant_probe_hits },
        { "rebuild swaps", s_st.rb_swaps }, { "superseding swaps", s_st.rb_superseding },
        { "stale results", s_st.rb_stale }, { "members retired in flight", s_st.rb_retired },
        { "guarded env writes", s_st.env_writes },
        { "pending slots", s_st.pending_set },
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) {
        if (need[i].n) continue;
        printf("verdict: leg=pool VACUOUS (no %s)\n", need[i].what);
        fail = 1;
    }
    return fail || p1.variant_failed != p0->variant_failed || s_st.rb_failed;
}

/** @brief A result compiled for the shown variant turns stale when a swap shows another variant that changes its create info, and is not stale after a retarget.
 *
 * Runs the scene pipeline's job for the shown variant, swaps a synchronous
 * rebuild to the variant with the other texture mapping, and reads
 * rhi_compile_job_variant_stale before and after rhi_compile_job_retarget.
 * Display owner, with no rebuild in flight.
 * @return 1 when a reading is wrong, else 0. */
static int stale_control(void)
{
    rhi_compile_job *job = rhi_compile_job_new(s_dev, &k_scene_desc, NULL, 0, NULL, 0,
                                               rhi_pipe_variant_current(), 0,
                                               rhi_pipe_variant_epoch());
    if (!job || rhi_compile_job_run(s_dev, job, 0) != 0) {
        rhi_compile_job_discard(s_dev, job);
        printf("stale_control: job failed\n");
        return 1;
    }
    const bool before = rhi_compile_job_variant_stale(s_dev, job);
    rhi_pipe_variant var = rhi_pipe_variant_current();
    var.affine = !var.affine;
    rhi_rebuild_request(s_dev, var, true);
    rhi_rebuild_stats r;
    rhi_rebuild_poll(s_dev, &r);
    const bool after = rhi_compile_job_variant_stale(s_dev, job);
    rhi_compile_job_retarget(s_dev, job);
    const bool retargeted = rhi_compile_job_variant_stale(s_dev, job);
    rhi_compile_job_discard(s_dev, job);
    const bool ok = !before && after && !retargeted;
    printf("stale_control: stale before switch %d, after %d, after retarget %d: %s\n", before,
           after, retargeted, ok ? "engaged" : "WRONG");
    return ok ? 0 : 1;
}

/** @brief Wait until the pool has run a number of jobs since a snapshot, or a second passes.
 *
 * @param p0 The snapshot.
 * @param n The jobs to wait for.
 * @return The jobs run since the snapshot. */
static unsigned pool_ran_since(const rhi_compile_pool_stats *p0, unsigned n)
{
    rhi_compile_pool_stats p;
    for (int i = 0; i < 100; i++) {
        rhi_compile_pool_stats_get(s_dev, &p);
        if (p.run - p0->run >= n) break;
        SDL_Delay(10);
    }
    rhi_compile_pool_stats_get(s_dev, &p);
    return p.run - p0->run;
}

/* Wake calls of the ahead walk (ahead_wake). */
static atomic_uint s_ahead_wakes;

/** @brief Count one wake call of the ahead walk: a walk job returned on a compile worker. */
static void ahead_wake(void) { atomic_fetch_add(&s_ahead_wakes, 1u); }

/** @brief The launcher's precompile on the pool: description mode records without creating, a paused pool runs nothing until resumed, the ahead walk compiles every recorded key with one wake call per returned job, and copies of one create info submitted at once wait for each other.
 *
 * @return 1 when a step misbehaves or is not engaged, else 0. */
static int ahead_control(void)
{
    void *slot[2] = { NULL, NULL };
    rhi_pipeline_describe_begin(s_dev);
    rhi_create_pipeline_slot(s_dev, &k_scene_desc, NULL, 0, NULL, 0, &slot[0], NULL);
    rhi_create_pipeline_slot(s_dev, &k_postfx_desc, NULL, 0, s_frag_words, s_frag_n, &slot[1], NULL);
    const unsigned described = rhi_pipeline_describe_end(s_dev);
    const bool marked = slot[0] && slot[1];

    rhi_compile_pool_stats p0;
    rhi_compile_pool_stats_get(s_dev, &p0);
    rhi_compile_pool_pause(s_dev, true);
    atomic_store(&s_ahead_wakes, 0u);
    const int keys = rhi_precompile_ahead_start(s_dev, false, ahead_wake);
    (void)rhi_precompile_walk_step(s_dev);
    const unsigned paused_ran = pool_ran_since(&p0, 1);
    rhi_compile_pool_pause(s_dev, false);
    for (int i = 0; i < 500 && rhi_precompile_walk_step(s_dev) == RHI_WALK_RUNNING; i++) SDL_Delay(10);
    unsigned done = 0, total = 0;
    rhi_precompile_stats st;
    const rhi_walk_state ws = rhi_precompile_walk_get(s_dev, &done, &total, &st);
    /* A worker calls the wake after it releases the pool lock, so the last
     * call may follow the step that saw its job return. */
    for (int i = 0; i < 100 && keys > 0 && atomic_load(&s_ahead_wakes) < (unsigned)keys; i++) SDL_Delay(10);
    const unsigned wakes = atomic_load(&s_ahead_wakes);

    /* Copies of one create info submitted together: the workers that take a
     * copy while another runs it wait. */
    rhi_compile_pool_stats q0;
    rhi_compile_pool_stats_get(s_dev, &q0);
    const unsigned copies = 4;
    rhi_pipe_variant var = rhi_pipe_variant_current();
    var.notex = !var.notex;
    for (unsigned i = 0; i < copies; i++)
        rhi_compile_pool_submit(s_dev, rhi_compile_job_new(s_dev, &k_scene_desc, NULL, 0, NULL, 0,
                                                           var, 0, 0),
                                RHI_COMPILE_VARIANT, NULL, NULL);
    rhi_compile_pool_wait(s_dev, NULL, NULL);
    rhi_compile_pool_stats q1;
    rhi_compile_pool_stats_get(s_dev, &q1);
    const unsigned waits = q1.same_key_waits - q0.same_key_waits;

    const bool ok = described == 1 && marked && keys > 0 && paused_ran == 0 &&
                    ws == RHI_WALK_DONE && st.created + st.probe_hits == (unsigned)keys &&
                    wakes == (unsigned)keys && waits >= 1 && q1.variant_failed == q0.variant_failed;
    printf("ahead_control: described %u (marked %d), keys %d, run while paused %u, walk %s "
           "created %u probe hits %u of %u, wakes %u, same-key waits %u of %u copies: %s\n", described,
           marked, keys, paused_ran, ws == RHI_WALK_DONE ? "done" : "NOT DONE", st.created,
           st.probe_hits, total, wakes, waits, copies, ok ? "engaged" : "NOT ENGAGED");
    return ok ? 0 : 1;
}

/** @brief Stop the pool with jobs queued and in flight: every job must be run or cancelled, none lost.
 *
 * @return 1 when the counts do not add up or nothing was cancelled, else 0. */
static int stop_control(void)
{
    rhi_compile_pool_stats p0;
    rhi_compile_pool_stats_get(s_dev, &p0);
    for (unsigned c = 0; c < 8; c++) enqueue_builtin(c, MODE_POOL);
    rhi_compile_pool_stop(s_dev);
    rhi_compile_pool_stats p1;
    rhi_compile_pool_stats_get(s_dev, &p1);
    const unsigned sub = p1.submitted - p0.submitted, ran = p1.run - p0.run;
    const unsigned cancelled = p1.cancelled - p0.cancelled;
    const unsigned staged = p1.staged - p0.staged;
    /* Every job ends as a variant run on a worker (run, not staged), or
     * cancelled: never run, or run and staged but not installed before the
     * stop (nothing drains in between, so every staged job is cancelled). */
    const bool whole = ran - staged + cancelled == sub;
    printf("stop_control: submitted=%u run=%u staged=%u cancelled=%u: %s\n", sub, ran, staged,
           cancelled, whole && cancelled ? "engaged" : "NOT ENGAGED");
    for (unsigned i = 0; i < LIVE_BUILTINS; i++) {
        if (s_live[i]) rhi_retire_pipeline(s_dev, s_live[i]);
        s_live[i] = NULL;
    }
    return !(whole && cancelled);
}

/* The boot control's host tasks: how many run now (all, and background
 * ones), the most seen at a task's start before the first tick and inside
 * the measured window after it, and how many started and finished. */
static struct {
    atomic_uint active, bg_active, started, bg_started, finished, post_starts;
    atomic_uint pre_max, bg_pre_max, post_max;
    atomic_bool measuring;   /* the window after the first tick and its settle */
} s_boot;

/** @brief Raise an atomic maximum to a value.
 *
 * @param m The maximum.
 * @param v The value. */
static void atomic_max_u(atomic_uint *m, unsigned v)
{
    unsigned cur = atomic_load(m);
    while (v > cur && !atomic_compare_exchange_weak(m, &cur, v)) {}
}

/** @brief One boot-control task: count itself running, note the running counts at its start in the current window, hold for 15 ms, and count itself finished.
 *
 * @param tag Non-NULL for a background task. */
static void boot_task(void *tag)
{
    const bool bg = tag != NULL;
    const unsigned a = atomic_fetch_add(&s_boot.active, 1u) + 1u;
    const unsigned g = bg ? atomic_fetch_add(&s_boot.bg_active, 1u) + 1u : 0u;
    atomic_fetch_add(&s_boot.started, 1u);
    atomic_fetch_add(&s_boot.bg_started, bg);
    if (atomic_load(&s_boot.measuring)) {
        atomic_max_u(&s_boot.post_max, a);
        atomic_fetch_add(&s_boot.post_starts, 1u);
    } else {
        atomic_max_u(&s_boot.pre_max, a);
        atomic_max_u(&s_boot.bg_pre_max, g);
    }
    SDL_DelayNS(15000000);
    atomic_fetch_sub(&s_boot.bg_active, bg);
    atomic_fetch_sub(&s_boot.active, 1u);
    atomic_fetch_add(&s_boot.finished, 1u);
}

/** @brief The pool's two phases: without OPENPETE_COMPILE_POOL it starts one worker per logical core, the workers beyond the steady count run only tasks that are not background, so before and at the first tick no more than the steady count of background tasks run; after rhi_compile_pool_first_tick no more than the steady count of any task run, and every task completes.
 *
 * Background tasks (rhi_compile_pool_submit_background_task) are queued
 * first, then boot tasks, as the background walk runs ahead of a late
 * bring-up batch. The first tick is marked once the pool has run every
 * worker at once, with background tasks still queued. Tasks starting in the
 * 120 ms after the mark (8 task lengths) are not measured: a task taken
 * before the mark may start its body just after it. The verdict counts the
 * tasks running at each measured task's start. Runs in a process whose
 * first tick has not been marked, on a pool not yet started.
 * @return 0 pass, 1 fail, 77 on a host with one logical core (no step-down exists). */
static int boot_control(void)
{
    const int cores = SDL_GetNumLogicalCPUCores();
    const unsigned full = (unsigned)(cores > 32 ? 32 : cores);
    const unsigned steady = (unsigned)(cores / 2 > 0 ? cores / 2 : 1);
    if (full <= steady) {
        printf("boot_control: %d logical core(s): no step-down to test - SKIP\n", cores);
        return 77;
    }
    const unsigned nbg = 12u * full, nboot = 4u * full, n = nbg + nboot;
    for (unsigned i = 0; i < nbg; i++) rhi_compile_pool_submit_background_task(s_dev, boot_task, &s_boot);
    for (unsigned i = 0; i < nboot; i++) rhi_compile_pool_submit_task(s_dev, boot_task, NULL);
    for (int i = 0; i < 200 && atomic_load(&s_boot.pre_max) < full; i++) SDL_Delay(10);
    rhi_compile_pool_first_tick();
    const unsigned bg_queued_at_tick = nbg - atomic_load(&s_boot.bg_started);
    const unsigned bg_running_at_tick = atomic_load(&s_boot.bg_active);
    SDL_Delay(120);
    atomic_store(&s_boot.measuring, true);
    rhi_compile_pool_wait(s_dev, NULL, NULL);
    rhi_compile_pool_stats st;
    rhi_compile_pool_stats_get(s_dev, &st);
    const unsigned pre = atomic_load(&s_boot.pre_max), bg_pre = atomic_load(&s_boot.bg_pre_max);
    const unsigned post = atomic_load(&s_boot.post_max), post_starts = atomic_load(&s_boot.post_starts);
    const unsigned fin = atomic_load(&s_boot.finished);
    const bool engaged = pre == full && bg_pre == steady && bg_queued_at_tick > 0 &&
                         post_starts > steady && post == steady;
    const bool held = bg_pre <= steady && bg_running_at_tick <= steady && post <= steady &&
                      fin == n && st.tasks_run == n && st.workers == full && st.workers_steady == steady;
    printf("boot_control: workers %u (want %u), steady %u (want %u); before the first tick at most "
           "%u running, %u of them background; at it %u background running, %u background queued; "
           "after it %u measured starts, at most %u running; %u of %u tasks finished (pool counted "
           "%u): %s\n",
           st.workers, full, st.workers_steady, steady, pre, bg_pre, bg_running_at_tick,
           bg_queued_at_tick, post_starts, post, fin, n, st.tasks_run,
           !held ? "FAIL" : engaged ? "engaged" : "NOT ENGAGED");
    return held && engaged ? 0 : 1;
}

/** @brief Run the boot control alone on a fresh device whose pool is sized without OPENPETE_COMPILE_POOL, and print the verdict.
 *
 * @return 0 pass, 1 fail, 77 no Vulkan device or one logical core. */
static int boot_main(void)
{
    SDL_unsetenv_unsafe("OPENPETE_COMPILE_POOL");
    s_dev = rhi_create_headless("rhi_compile_stress", 64, 64, NULL);
    if (!s_dev) { fprintf(stderr, "rhi_compile_stress: no Vulkan device - SKIP\n"); return 77; }
    const int rc = boot_control();
    rhi_destroy(s_dev);
    static const char *const k_verdict[] = { "PASS", "FAIL" };
    printf("verdict: %s\n", rc == 77 ? "SKIP" : k_verdict[rc != 0]);
    return rc;
}

/** @brief Copy the embedded raster-quantize fragment's SPIR-V as the words every stress mod shader compiles to.
 *
 * @return true when the blob was found. */
static bool load_frag_words(void)
{
    const void *b = NULL;
    size_t n = 0;
    for (unsigned i = 0; i < g_rhi_embedded_shader_count; i++)
        if (strcmp(g_rhi_embedded_shaders[i].name, "raster_quantize.frag.spv") == 0) {
            b = g_rhi_embedded_shaders[i].data;
            n = g_rhi_embedded_shaders[i].size;
        }
    if (!b || n < 4 || (n & 3u)) return false;
    uint32_t *w = malloc(n);
    if (!w) return false;
    memcpy(w, b, n);
    s_frag_words = w;
    s_frag_n = n / 4;
    return true;
}

/** @brief Install and retire one pipeline off the display owner: the seam check's negative control.
 *
 * @param arg A compiled job.
 * @return 0. */
static int SDLCALL seam_control_main(void *arg)
{
    rhi_pipeline *p = rhi_compile_job_install(s_dev, (rhi_compile_job *)arg);
    if (p) rhi_retire_pipeline(s_dev, p);
    return 0;
}

/** @brief Run the no-compile leg, the harness-worker leg and the compile-pool leg, and judge the compiling legs.
 *
 * @param argc Argument count.
 * @param argv workers, seconds, shader_dir ("-" for none), legs ("pool"
 *             skips the harness-worker leg) (all optional).
 * @return 0 pass, 1 fail, 77 no Vulkan device. */
int main(int argc, char **argv)
{
    const int workers = argc > 1 ? atoi(argv[1]) : 4;
    const int seconds = argc > 2 ? atoi(argv[2]) : 5;
    const char *shader_dir = argc > 3 && strcmp(argv[3], "-") != 0 ? argv[3] : NULL;
    const char *legs = argc > 4 ? argv[4] : NULL;
    if (legs && strcmp(legs, "boot") == 0) return boot_main();
    SDL_setenv_unsafe("MESA_SHADER_CACHE_DISABLE", "true", 1);   /* no driver file IO on workers */
    op_atomic_file_set_env_guard(rhi_env_read_guard);   /* the guest's file commits read the environment */
    /* Beside the executable (the build tree), so a run from any working
     * directory leaves the source tree untouched. */
    const char *base = SDL_GetBasePath();
    snprintf(s_mod_dir, sizeof s_mod_dir, "%s%s", base ? base : "", "rhi_compile_stress.d");
    SDL_CreateDirectory(s_mod_dir);
    s_q.mx = SDL_CreateMutex();
    s_q.cv = SDL_CreateCondition();
#if defined(__linux__)
    (void)in_exe((const void *)&main);   /* resolve the executable's name before any thread */
#endif
    /* The registry creates its lock on first use, unsynchronized: take it
     * once here, as mod init does on the boot thread, before any thread. */
    openpete_mods_registry_read_lock();
    openpete_mods_registry_read_unlock();
    s_dev = rhi_create_headless("rhi_compile_stress", 64, 64, NULL);
    if (!s_dev) { fprintf(stderr, "rhi_compile_stress: no Vulkan device - SKIP\n"); return 77; }
    if (!load_frag_words()) { fprintf(stderr, "rhi_compile_stress: no raster_quantize.frag.spv\n"); return 1; }
    census_start();

    const bool p1_leg = !legs || strcmp(legs, "pool") != 0;
    SDL_setenv_unsafe("OPENPETE_COMPILE_POOL", argc > 1 ? argv[1] : "4", 1);   /* read at the pool's start */
    run_leg(MODE_NONE, 0, seconds, shader_dir);
    report("no_compile");
    int fail = 0;
    if (p1_leg) {
        atomic_store(&s_worker_getenv, 0u);
        atomic_store(&s_worker_fopen, 0u);
        run_leg(MODE_QUEUE, workers, seconds, shader_dir);
        report("compile");
        fail |= judge_leg("compile");
        fail |= judge_worker_calls("compile");
    }
    atomic_store(&s_worker_getenv, 0u);
    atomic_store(&s_worker_fopen, 0u);
    atomic_store(&s_driver_getenv, 0u);
    atomic_store(&s_driver_fopen, 0u);
    rhi_compile_pool_stats p0;
    rhi_compile_pool_stats_get(s_dev, &p0);
    run_leg(MODE_POOL, workers, seconds, shader_dir);
    report("pool");
    fail |= judge_leg("pool");
    fail |= judge_pool(&p0);
    fail |= judge_worker_calls("pool");
    const unsigned seams = rhi_seam_violations();
    printf("workers=%d seconds=%d seam_violations=%u\n", workers, seconds, seams);
    if (seams) fail = 1;

    /* Negative control: a registry link and unlink from a thread that does
     * not own the display must each count one seam violation. */
    rhi_compile_job *cj = rhi_compile_job_new(s_dev, &k_postfx_desc, NULL, 0, s_frag_words,
                                              s_frag_n, rhi_pipe_variant_current(), 0, 0);
    if (cj && rhi_compile_job_run(s_dev, cj, 0) == 0) {
        SDL_WaitThread(SDL_CreateThread(seam_control_main, "stress-seam", cj), NULL);
    } else {
        rhi_compile_job_discard(s_dev, cj);
    }
    const unsigned seam_hits = rhi_seam_violations() - seams;
    printf("seam_control: off-owner link+unlink counted %u violation(s) (want 2): %s\n", seam_hits,
           seam_hits == 2 ? "engaged" : "NOT ENGAGED");
    if (seam_hits != 2) fail = 1;
    fail |= stale_control();
    fail |= ahead_control();
    fail |= stop_control();

    openpete_mods_postfx_retire_all();
    openpete_mods_material_retire_all();
    rhi_wait_idle(s_dev);
    openpete_mods_registry_read_lock();
    drain_swept();
    openpete_mods_registry_read_unlock();
    rhi_destroy(s_dev);
    printf("verdict: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
