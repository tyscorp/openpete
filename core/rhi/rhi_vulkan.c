/*
 * openpete RHI - Vulkan backend.
 *
 * Brings up an instance/device/swapchain, an offscreen color target the
 * size of the swapchain, and per-frame: render into the offscreen target,
 * blit it onto the acquired swapchain image, present. The VRAM image,
 * pipelines, vertex buffers, descriptor sets, and readback sit behind the
 * same rhi.h seam.
 *
 * Sync model: imageAvailable + inFlight fence per frame-in-flight;
 * renderFinished semaphore per swapchain image; an imagesInFlight fence
 * map to avoid reusing a swapchain image still in flight.
 */

#include "rhi.h"
#include "rhi_embedded_shaders.h" /* g_rhi_embedded_shaders[] - generated */

#include "volk.h"          /* dynamic Vulkan loader; pulls in vulkan.h with
                            * VK_NO_PROTOTYPES. Must precede SDL_vulkan.h. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>

#include "rhi_gpu_audit.h" /* must follow volk.h: redirects the vk* globals */
#include "rhi_memtype.h"
#include "rhi_pipe_census.h"
#include "rhi_pcache_file.h"
#include "../include/openpete_thread_roles.h"

/* Acquire budget, and the back-off after one expiry.
 *
 * The budget has two bounds. It must stay well under the pacer's slip
 * threshold (4 sim periods, ~67 ms), or every expiry becomes a reported sim
 * slip. It must also exceed one display refresh period: under FIFO,
 * presenting at any rate below the refresh means the next image becomes
 * available at the next repaint, and a budget shorter than that expires on
 * nearly every present. Each expiry opens the back-off below, which
 * suppresses the next present, so a budget under one refresh period shows
 * almost no images while the present counter keeps advancing.
 *
 * The budget is therefore derived from the refresh at swapchain creation
 * (dev->acquire_budget_ns) rather than fixed.
 *
 * The back-off is what stops a sustained stall (a live resize holds images
 * for as long as the user drags) from costing the budget on every iteration
 * and throttling the sim. After one expiry, presents are skipped outright
 * for a short window with no blocking, so the sim runs at full speed and
 * presentation resumes once images flow again. */
/* Used only when the refresh cannot be read: assume the slowest display
 * expected (60 Hz) rather than a fast one, because a budget shorter than the
 * refresh period expires, backs off, and reports presents that never reached
 * the screen. */
#ifndef RHI_ACQUIRE_TIMEOUT_NS
#define RHI_ACQUIRE_TIMEOUT_NS  (25ull * 1000ull * 1000ull)   /* ~1.5 x 60 Hz */
#endif
/* Floor for the derived budget: only has to exceed the driver's own acquire
 * latency. */
#ifndef RHI_ACQUIRE_BUDGET_MIN_NS
#define RHI_ACQUIRE_BUDGET_MIN_NS (4ull * 1000ull * 1000ull)   /* 4 ms */
#endif
/* Must match the renderer's own resize settle window closely enough that the
 * swapchain and the render targets rebuild in the same gesture. */
#ifndef RHI_RESIZE_SETTLE_NS
#define RHI_RESIZE_SETTLE_NS    (150ull * 1000ull * 1000ull)  /* 150 ms */
#endif
/* Number of stage images in the staged-present ring. */
#ifndef RHI_STAGE_RING
#define RHI_STAGE_RING 3
#endif
/* Bound for the waits a dedicated present thread makes on a slot fence. Long
 * enough for a swapchain rebuild plus the slowest display's refresh (a 60 Hz
 * panel under FIFO with the queue full is ~50 ms of legitimate waiting);
 * short enough that a dead compositor returns control. */
#ifndef RHI_BLOCKING_FENCE_TIMEOUT_NS
#define RHI_BLOCKING_FENCE_TIMEOUT_NS (500ull * 1000ull * 1000ull)
#endif
/* Bound on the wait for the fence of the submission that last used an
 * acquired swapchain image. By then the slot poll has passed, so the fence is
 * normally signalled; the bound keeps a driver hiccup from parking the
 * presenting thread indefinitely. */
#ifndef RHI_IMAGE_FENCE_TIMEOUT_NS
#define RHI_IMAGE_FENCE_TIMEOUT_NS (32ull * 1000ull * 1000ull)   /* 32 ms */
#endif
/* Hard cap on the derived budget: an expiry must never be able to become a
 * reported sim slip (the pacer clamps at 4 sim periods, ~67 ms). */
#ifndef RHI_ACQUIRE_BUDGET_MAX_NS
#define RHI_ACQUIRE_BUDGET_MAX_NS (40ull * 1000ull * 1000ull)  /* 40 ms */
#endif
/* Shortest wait that counts as having observed a flip. Below this the image
 * was already on screen and the return time is when the wait was asked, not
 * when the display changed. */
#ifndef RHI_FLIP_OBSERVED_NS
#define RHI_FLIP_OBSERVED_NS    (20ull * 1000ull)              /* 20 us */
#endif
/* Back-off after an expiry, and the interval between swapchain rebuild
 * attempts. It must not sit near a multiple of the present period (33.4 ms):
 * a window there suppresses every other present in a stable alternating
 * pattern instead of decaying. One VBL is short enough to recover within a
 * tick and long enough to break a stall loop. */
#ifndef RHI_ACQUIRE_BACKOFF_NS
#define RHI_ACQUIRE_BACKOFF_NS  (16ull * 1000ull * 1000ull)   /* ~1 VBL */
#endif

/* Swapchain pipeline depth. Overridable from the build so the depth/refresh
 * trade can be swept without editing the source; every per-frame ring below
 * is sized from it. Note the readback and grid rings lag by exactly this many
 * presents, so a capture taken at depth N lands N presents behind. */
#ifndef RHI_FRAMES_IN_FLIGHT
#define RHI_FRAMES_IN_FLIGHT 2
#endif
#define RHI_MAX_SC_IMAGES    8

#define RHI_LOG(...)  fprintf(stderr, "[rhi/vk] " __VA_ARGS__)

/* Where a device-creation knob's value comes from (rhi_set_knob_source);
 * NULL reads the process environment. */
static const char *(*s_knob_source)(const char *name);

void rhi_set_knob_source(const char *(*fn)(const char *name))
{
    s_knob_source = fn;
}

void rhi_loader_env_prepare(const char *(*user_value)(const char *name))
{
#ifdef _WIN32
    static const char kVar[] = "DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1";
    if (getenv(kVar)) return;   /* the process environment wins, 0 included */
    const char *v = user_value ? user_value(kVar) : NULL;
    _putenv_s(kVar, (v && *v) ? v : "1");
#else
    (void)user_value;
#endif
}

/** @brief Read a device-creation knob through the installed source, else the environment.
 *
 * @param name The env var.
 * @return Its value, or NULL when unset. */
static const char *rhi_knob(const char *name)
{
    return s_knob_source ? s_knob_source(name) : getenv(name);
}

/* Heap-balloon block size, the per-spec block cap (32 GiB of blocks) and the
 * block-list length. The cap is per spec so a spec filling a second heap
 * cannot be starved by the blocks an earlier spec already holds. */
#define RHI_BALLOON_BLOCK_MIB  64u
#define RHI_BALLOON_SPEC_CAP   512
#define RHI_BALLOON_MAX_BLOCKS 1024

/* The runtime half of the display-seam fence (the build-time half is
 * scripts/fences/seam_guard.py). The display's frame/present/swapchain
 * state is externally synchronised and owned by exactly one thread at a
 * time; ownership moves only through rhi_display_claim, called by the
 * owner-to-be at each handoff. The frame begin and end, staged-present,
 * present-wait, ring-growth and pipeline-rebuild entry points verify the
 * calling thread. A violation logs (first few in full, then counted) so a
 * shipped build never dies for it; OPENPETE_SEAM_GUARD_FATAL=1 aborts at
 * the first, which names the interleaving that a driver would otherwise turn
 * into a silent process kill minutes later. */
static _Atomic(SDL_ThreadID) s_display_owner;   /* read by compile jobs on any thread */
static unsigned     s_seam_violations;
void rhi_display_claim(rhi_device *dev)
{
    (void)dev;
    atomic_store(&s_display_owner, SDL_GetCurrentThreadID());
}
/** @brief Count and log a display-seam violation when the calling thread is not the display owner.
 *
 * Does nothing before the first rhi_display_claim. The first eight
 * violations log; OPENPETE_SEAM_GUARD_FATAL set to a value not starting
 * with 0 aborts at the first (read once).
 * @param where Entry point name for the log line. */
static void seam_check(const char *where)
{
    const SDL_ThreadID owner = atomic_load(&s_display_owner);
    if (!owner) return;                           /* pre-claim boot window */
    const SDL_ThreadID me = SDL_GetCurrentThreadID();
    if (me == owner) return;
    static int fatal = -1;
    if (fatal < 0) { const char *e = getenv("OPENPETE_SEAM_GUARD_FATAL"); fatal = (e && *e && *e != '0'); }
    if (s_seam_violations < 8)
        RHI_LOG("SEAM: %s on thread %" PRIu64 " but the display is owned by %" PRIu64 "\n",
                where, (uint64_t)me, (uint64_t)owner);
    s_seam_violations++;
    if (fatal) abort();
}
unsigned rhi_seam_violations(void) { return s_seam_violations; }

/* Handler invoked on an unrecoverable VK_ERROR_DEVICE_LOST (registered by the
 * core layer, which owns the shutdown flag) instead of spinning failed frames
 * forever. A callback keeps openpete_rhi free of core-runtime symbols: the
 * link goes core -> rhi, not back. */
static void (*s_device_lost_cb)(void) = NULL;
void rhi_set_device_lost_handler(void (*fn)(void)) { s_device_lost_cb = fn; }
static const char *s_last_gpu_zone;   /* set by rhi_gpu_zone_begin */
/* Set once the device is lost or a submit failed; every later frame and
 * present entry point refuses, so no caller waits on a fence whose
 * submission never happened. */
static bool s_device_lost;

/** @brief Record device loss, log it once, and run the registered handler once.
 *
 * Also reached from a failed queue submit: the frame's fence then has no
 * pending signal and the acquire semaphore no pending wait, a state no later
 * frame can recover, so it is handled as a loss.
 * @param where Name of the call that reported the loss. */
static void rhi_signal_device_lost(const char *where) {
    if (s_device_lost) return;
    s_device_lost = true;
    RHI_LOG("FATAL: device lost on %s - requesting shutdown\n", where);
    if (s_last_gpu_zone)
        RHI_LOG("device-lost suspect: last GPU zone begun = \"%s\" "
                "(a runaway mod shader TDRs here)\n", s_last_gpu_zone);
    else
        RHI_LOG("re-run with OPENPETE_GPU_TIMERS=1 to attribute the hang "
                "to a named pass/mod\n");
    if (s_device_lost_cb) s_device_lost_cb();
}

#define VK_CHECK(expr)                                                       \
    do {                                                                     \
        VkResult _vr = (expr);                                               \
        if (_vr != VK_SUCCESS) {                                             \
            RHI_LOG("%s failed: VkResult=%d (%s:%d)\n", #expr, (int)_vr,     \
                    __FILE__, __LINE__);                                     \
        }                                                                    \
    } while (0)

/* Resource census, printed every RHI_CENSUS_PERIOD begun frames while
 * OPENPETE_RHI_CENSUS is set: live sampled textures with their device and
 * host staging bytes, vkQueueWaitIdle calls, and pipeline compiles with
 * their wall time. The pipeline counters are atomic: a compile job counts
 * its create on any thread. The rest are touched only by the display
 * owner. */
#define RHI_CENSUS_PERIOD 60
static struct {
    int      on;             /* -1 until the env is read */
    int64_t  tex_live;       /* create_sampled_image textures not yet destroyed */
    int64_t  tex_dev_bytes;  /* their device-local image memory */
    int64_t  tex_host_bytes; /* host staging held for texture uploads (the upload ring) */
    uint64_t queue_idles;    /* vkQueueWaitIdle calls */
    _Atomic uint64_t pipe_builds;    /* vkCreateGraphicsPipelines calls */
    _Atomic uint64_t pipe_ns;        /* wall time inside them */
    uint64_t frames;         /* frames begun */
    uint64_t idles_mark;     /* queue_idles at the previous report */
} s_census = { .on = -1 };

/** @brief Return whether the resource census is on.
 *
 * On when OPENPETE_RHI_CENSUS is set, non-empty and not starting with '0';
 * read once.
 * @return 1 when on, else 0. */
static int census_on(void)
{
    if (s_census.on < 0) {
        const char *e = getenv("OPENPETE_RHI_CENSUS");
        s_census.on = (e && *e && *e != '0');
    }
    return s_census.on;
}

/** @brief Count one begun frame and print the census line every RHI_CENSUS_PERIOD frames. */
static void census_frame(void)
{
    s_census.frames++;
    if (!census_on() || s_census.frames % RHI_CENSUS_PERIOD) return;
    RHI_LOG("census: frames=%llu tex=%lld dev=%.1fMiB host=%.1fMiB "
            "queue_idle=%llu (+%llu) pipe_builds=%llu pipe_ms=%.1f\n",
            (unsigned long long)s_census.frames, (long long)s_census.tex_live,
            (double)s_census.tex_dev_bytes / 1048576.0,
            (double)s_census.tex_host_bytes / 1048576.0,
            (unsigned long long)s_census.queue_idles,
            (unsigned long long)(s_census.queue_idles - s_census.idles_mark),
            (unsigned long long)atomic_load(&s_census.pipe_builds),
            (double)atomic_load(&s_census.pipe_ns) / 1e6);
    s_census.idles_mark = s_census.queue_idles;
}

/** @brief Drain the graphics queue, counted by the census.
 *
 * @param q Queue to drain.
 * @return The vkQueueWaitIdle result. */
static VkResult queue_wait_idle(VkQueue q)
{
    s_census.queue_idles++;
    return vkQueueWaitIdle(q);
}

/** @brief Return whether OPENPETE_VK_FAULT schedules an injected failure for this queue submit.
 *
 * OPENPETE_VK_FAULT=submit:N makes the N-th fenced vkQueueSubmit of the
 * process (a frame or present submit; N >= 1, "submit" alone means 1)
 * return VK_ERROR_DEVICE_LOST without submitting, which exercises the
 * submit-failure path on a working device. Unfenced one-shot submits are
 * not counted.
 * @return 1 for the scheduled submit, else 0. */
static int fault_submit_due(void)
{
    static long long at = -1, n = 0;
    if (at < 0) {
        const char *e = getenv("OPENPETE_VK_FAULT");
        at = 0;
        if (e && !strncmp(e, "submit", 6)) at = (e[6] == ':') ? atoll(e + 7) : 1;
        if (at > 0) RHI_LOG("fault injection: queue submit %lld fails\n", at);
    }
    return at > 0 && ++n == at;
}

/** @brief Return whether OPENPETE_VK_FAULT=enumerate makes physical-device enumeration list no device.
 *
 * Exercises a missing or broken Vulkan driver on a working one: every
 * device creation of the process then fails at the device pick.
 * @return 1 when the fault is armed, else 0. */
static int fault_enumerate_due(void)
{
    static int armed = -1;
    if (armed < 0) {
        const char *e = getenv("OPENPETE_VK_FAULT");
        armed = (e && !strcmp(e, "enumerate")) ? 1 : 0;
        if (armed) RHI_LOG("fault injection: physical-device enumeration lists no device\n");
    }
    return armed;
}

/** @brief Route a fence wait or status result to the device-lost handler when it reports loss.
 *
 * @param r The vkWaitForFences or vkGetFenceStatus result.
 * @param where Name of the wait, for the log.
 * @return @p r unchanged. */
static VkResult fence_result(VkResult r, const char *where)
{
    if (r == VK_ERROR_DEVICE_LOST) rhi_signal_device_lost(where);
    return r;
}

/* Internal color format: the offscreen target, the stage images, every
 * render target and every render pass's color attachment. It is fixed, so
 * pipelines, readback sizes and the readback swizzle never depend on what a
 * surface offers; the present blit converts to the swapchain format. */
#define RHI_RT_FORMAT VK_FORMAT_B8G8R8A8_UNORM
/* Bytes per RHI_RT_FORMAT texel: every color readback's staging size. The
 * readback swizzle handles 4-byte BGRA or RGBA texels only. */
#define RHI_RT_TEXEL_BYTES 4u
_Static_assert(RHI_RT_FORMAT == VK_FORMAT_B8G8R8A8_UNORM ||
               RHI_RT_FORMAT == VK_FORMAT_R8G8B8A8_UNORM,
               "color readbacks and their swizzle assume 4-byte BGRA or RGBA texels");

/* Descriptor pools a device may create; each holds RHI_DESC_POOL_SETS sets
 * (4096 sampler and 64 dynamic-UBO descriptors). */
#define RHI_DESC_POOLS_MAX 16
#define RHI_DESC_POOL_SETS 4160

/* One host-visible chunk of the device upload ring (stage_acquire). */
struct rhi_stage_chunk {
    rhi_buffer            *buf;
    size_t                 cap, used;
    struct rhi_stage_chunk *next;
};

static void record_pending_inits(rhi_device *dev);

/* A live rebuild in flight (rhi_rebuild_request to rhi_rebuild_poll): the
 * set of registered pipelines the requested variant changes, one entry per
 * member (struct rebuild_entry), and the timing of the requests. A member
 * pipeline carries the set's generation and its entry index. */
struct rebuild_set {
    uint32_t              gen;        /* generation of the set; jobs carry it */
    bool                  active;     /* requested and not yet swapped        */
    rhi_pipe_variant      target;     /* the latest requested variant         */
    struct rebuild_entry *e;          /* members, by index                    */
    unsigned              n, cap;
    unsigned              waiting;    /* members with a job outstanding       */
    unsigned              requests, stale, retired;
    unsigned              joined;     /* members registered after the first request */
    uint64_t              t0_ns;      /* wall clock at the first request      */
    uint64_t              owner_ns;   /* display-owner time in the requests   */
    uint64_t              serial0;    /* frame serial at the first request    */
    unsigned              hit0, miss0, unk0; /* rebuild tally at the first request */
};

/* One variant a variant walk visits: its pipeline's registry serial (a
 * pipeline is looked up when its job is built, never held by pointer), or,
 * on the ahead walk, the index of its recorded description and the
 * SLOT_CONTEST value it compiles for; the variant and its create-info key. */
typedef struct {
    uint64_t         serial;
    int              ahead;     /* description index (rhi_precompile_ahead_start), -1 for a registry item */
    unsigned         rank;      /* ahead walk: view order * RHI_AHEAD_GROUPS + group */
    VkBool32         slot_contest;
    rhi_pipe_variant var;
    uint64_t         key;
} walk_item;

/* Ranks of the ahead walk: each walked view (PIPE_WALK_VIEWS) by each group. */
#define AHEAD_RANKS (2u * RHI_AHEAD_GROUPS)

/* The ahead walk's front ranks: the first texture mapping's launcher screens
 * (group 0) and terrain (group 1). Every front rank returns before the walk
 * submits an item of a later rank, and each front rank after the first has
 * its library parts queued once the rank before it returned. */
#define AHEAD_FRONT_RANKS 2u

/* A description recorded in description mode (rhi_pipeline_describe_begin),
 * its .spv names owned, and its stages' facts once the ahead walk resolved
 * them. */
typedef struct {
    rhi_pipeline_desc desc;
    char             *vert, *frag, *whole;
    unsigned          group;    /* rhi_pipeline_describe_group at the record */
} described_desc;

/* A variant walk (rhi_precompile_variants, rhi_precompile_walk_start): the
 * enumerated variants, the next to submit, the keys a live rebuild request
 * covered, and the pool's variant outcomes before the first submit, from
 * which the returned count follows (no other submitter queues variant
 * jobs). */
struct precompile_walk {
    rhi_walk_state         state;
    bool                   background; /* logs and arms the save at the end; one job out at a time unless ahead */
    bool                   ahead;      /* the ahead walk: recorded descriptions, up to limit jobs per step */
    unsigned               limit;      /* jobs out at most */
    walk_item             *items;
    unsigned               n, next;
    unsigned               submitted;
    unsigned               skipped;    /* covered or gone */
    uint64_t               serial_end; /* the last registry serial enumerated */
    uint64_t               out_key;    /* key of the last submitted job */
    bool                   out_dup;    /* a rebuild request already counted it as a duplicate */
    rhi_pc_keyset          covered;    /* keys live rebuild requests compiled */
    rhi_compile_pool_stats p0;         /* the pool's counts before the first submit */
    rhi_precompile_stats   st;
    uint64_t               t0_ns;
    unsigned               rank_n[AHEAD_RANKS];    /* ahead walk: items per rank */
    unsigned               rank_skip[AHEAD_RANKS]; /* ahead walk: items skipped per rank */
    unsigned               rank_logged;            /* ahead walk: ranks whose return was logged */
    unsigned               rank_parts;             /* ahead walk: front ranks whose library parts were queued */
    bool                   affine_first;           /* ahead walk: the affine view is walked first */
};

/* Link-time optimization of the pipelines linked from libraries
 * (rhi_pipeline_lto_step): each fast-linked pipeline the registry holds is
 * linked again with link-time optimization on the compile pool's low
 * queue, and the result replaces its objects at a later step, between
 * frames. Display owner only, except the counters a compile run adds to. */
struct lto_state {
    bool              owed;     /* a fast-linked pipeline was installed since the last queue pass */
    bool              drained;  /* every queued link returned and the drained line was logged */
    struct lto_ready *ready;    /* results waiting for their swap */
    unsigned          out;      /* links submitted and not yet returned */
    unsigned          queued, returned, swapped, stale, failed;
    unsigned          swapped_whole; /* of the swaps, results created whole from vert_spv_whole */
    unsigned          swaps_in_frame; /* swaps made while a frame recorded */
    _Atomic unsigned  direct;   /* pipelines a create made optimized at once (cached) */
    _Atomic unsigned  probe_miss; /* of the creates that tried, those a probe found uncached */
    uint64_t          serial_first, serial_last; /* frame serials of the first and last swap */
    uint64_t          t0_ns;    /* wall clock at the first queue pass */
};

/* A frame slot's list of per-bind sampler sets of one layout (the pair or
 * the trio): each bind in a frame takes the next set, or rebinds a set this
 * frame already wrote with the same views. */
typedef struct {
    VkDescriptorSet *sets;                /* grown by doubling             */
    VkImageView     (*views)[4];          /* each taken set's views        */
    uint32_t        cap;                  /* entries sets/views hold       */
    uint32_t        alloc;                /* sets allocated                */
    uint32_t        used;                 /* sets taken this frame         */
} rhi_view_sets;

struct rhi_device {
    struct SDL_Window *window;

    VkInstance               instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR             surface;
    VkPhysicalDevice         phys;
    VkDevice                 device;
    uint32_t                 gfx_family;
    VkPipelineCache          pcache;          /* every pipeline compiles through it */
    char                     pcache_path[1152]; /* file it loads from and saves to; "" = not persisted */
    uint64_t                 pcache_digest;   /* digest of the cache bytes at load or the last save */
    rhi_pc_keyset            pc_keys;         /* manifest: loaded keys plus every key created this run */
    size_t                   pcache_keys_saved; /* pc_keys.n at load or the last save */
    bool                     manifest_trusted; /* the driver is calibrated (pcache_manifest_calibrated) */
    unsigned                 pcache_unhit;    /* creates since load or the last save not reported a cache hit */
    SDL_Mutex               *pc_mx;           /* guards pc_keys and pcache_unhit: compile jobs write them on any thread */
    size_t                   pcache_saved_n;  /* cache byte count at load or the last save */
    /* Debounced first-use save (pcache_autosave): first-use creates not
     * reported a cache hit, counted on any thread; the count and frame
     * serial the display owner last saw; the in-flight save worker. */
    _Atomic unsigned         pcache_fu_misses;
    unsigned                 pcache_fu_seen;
    uint64_t                 pcache_fu_serial;
    bool                     pcache_fu_pending;
    struct pcache_save_job  *pcache_save;
    rhi_pcache_autosave_stats autosave;
    struct compile_pool     *cpool;      /* compile worker pool; NULL until the first submit */
    bool                     cpool_stopped; /* rhi_compile_pool_stop ran: submits run at once */
    rhi_compile_pool_stats   cpool_final;   /* the pool's counts at its stop */
    struct rebuild_set       rb;         /* live rebuild in flight */
    bool                     batch_open; /* rhi_pipeline_batch_begin .. _end */
    rhi_pipeline_batch_stats batch;
    uint64_t                 batch_t0;   /* wall clock at rhi_pipeline_batch_begin */
    struct precompile_walk   walk;       /* the variant walk (rhi_precompile_progress, rhi_precompile_walk_step) */
    bool                     describing; /* rhi_pipeline_describe_begin .. _end */
    described_desc          *described;  /* the descriptions recorded, owned */
    unsigned                 described_n, described_cap, described_words; /* words: skipped (SPIR-V-word stages) */
    unsigned                 describe_mark_n, describe_mark_words; /* the counts at the open describe_begin */
    unsigned                 describe_group; /* rhi_pipeline_describe_group */
    bool                     cpool_paused; /* rhi_compile_pool_pause: the pool starts paused */
    bool                     cpool_low;    /* rhi_compile_pool_low: the pool starts on its low workers */
    uint64_t                 pipe_serial; /* the last registry serial handed out (registry_link) */
    VkQueue                  gfx_queue;
    char                     device_name[256];
    char                     driver_version[64];
    bool                     device_software; /* picked device is a CPU rasterizer (llvmpipe) */
    char                     gpu_choice[128]; /* --gpu / OPENPETE_VK_DEVICE: an enumeration
                                          * index or a deviceName substring; "" = automatic */
    bool                     headless;   /* surfaceless: no swapchain / present */
    int                      vsync_pref; /* --vsync: 1 = FIFO (vsync ON), 0 = vsync
                                          * OFF, -1 = unset (env / default). Read by
                                          * choose_present_mode on (re)create. */
    char                     present_mode_name[16]; /* OPENPETE_PRESENT_MODE as
                                          * the knob source gave it at create, or
                                          * rhi_set_present_mode_name; "" = unset.
                                          * Read when vsync_pref is -1. */
    uint64_t                 fence_sheds; /* presents skipped rather than
                                           * blocking the caller on the
                                           * display's cadence */
    VkPresentModeKHR         present_mode_granted; /* what the swapchain was
                                          * actually built with -- the requested
                                          * mode is not evidence of anything. */

    /* Heap balloon (OPENPETE_RHI_BALLOON): device memory held, unmapped and
     * unbound, so the driver refuses later allocations in that heap. Freed
     * before the device is destroyed. */
    VkDeviceMemory           balloon_blocks[RHI_BALLOON_MAX_BLOCKS];
    int                      balloon_count;

    /* Swapchain */
    VkSwapchainKHR swapchain;
    VkFormat       sc_format;
    VkFormat       depth_format;  /* combined depth+stencil: D32_SFLOAT_S8
                                   * preferred, D24_UNORM_S8 fallback - picked at
                                   * device selection (pick_depth_format). */
    /* The depth convention per rhi_depth_class, set with the format
     * (depth_convention_init): the compare op a pipeline of the class
     * tests with, and the value a pass of the class clears depth to. */
    VkCompareOp    depth_cmp[RHI_DEPTH_CLASSES];
    float          depth_clear[RHI_DEPTH_CLASSES];
    VkBool32       depth_rev[RHI_DEPTH_CLASSES]; /* spec constant 0 (DEPTH_REVERSED) */
    float          cur_depth_clear;  /* clear value of the current pass (rhi_clear_depth) */
    _Atomic unsigned pipes_by_cmp[8]; /* pipelines compiled per VkCompareOp (engagement) */
    bool           has_depth_bias_clamp; /* depthBiasClamp feature enabled */
    /* Depth-bias factors set but not yet recorded (depth_bias_set): recorded
     * only while a depth_bias pipeline is bound, since setting a dynamic state
     * a bound pipeline holds static and rebinding that pipeline relies on
     * driver behaviour older conformance versions lack. */
    float          bias_pending[3];
    bool           bias_owed;      /* bias_pending awaits a depth_bias pipeline */
    bool           bias_bound;     /* the bound pipeline was created with depth_bias */
    bool           has_fill_non_solid;   /* fillModeNonSolid feature enabled */
    bool           has_creation_feedback; /* VK_EXT_pipeline_creation_feedback enabled */
    bool           has_cache_control;    /* pipelineCreationCacheControl enabled */
    bool           has_gpl;              /* graphicsPipelineLibrary enabled, with fast linking */
    bool           has_lto;              /* library-linked pipelines get a link-time-optimized link (lto_step) */
    struct lto_state lto;                /* their background links and swaps */
    unsigned       null_binds;           /* rhi_bind_pipeline calls given a pipeline with no objects */
    /* Graphics pipeline libraries (pipe_uses_library): every part library
     * created this run, keyed by its create info, guarded by gpl_mx; gpl_cv
     * is broadcast when an entry leaves the building state. Destroyed with
     * the device. */
    SDL_Mutex     *gpl_mx;
    SDL_Condition *gpl_cv;
    struct gpl_lib *gpl_head;
    VkExtent2D     sc_extent;
    /* Extent of the offscreen color and depth targets and the stage images:
     * off_pin while it is non-zero, else sc_extent (off_extent_sync). The
     * present blits fit it into sc_extent when the two differ. */
    VkExtent2D     off_extent;
    VkExtent2D     off_pin;       /* rhi_pin_offscreen_extent; 0x0 follows the swapchain */
    uint32_t       sc_count;
    VkImage        sc_images[RHI_MAX_SC_IMAGES];

    /* Offscreen color target (off_extent). SSAA supersampling lives in
     * the renderer as a separate render target that resolves into this. */
    VkRenderPass   off_rp;
    VkRenderPass   off_load_rp;   /* off_rp but color LOADs (SCENE postfx re-begin) */
    /* Render-to-texture pass: same attachment formats as off_rp (so off_rp-built
     * pipelines are compatible) but the color attachment ends in
     * SHADER_READ_ONLY_OPTIMAL so the result can be sampled the same frame. */
    VkRenderPass   rt_rp;
    /* Depth-sampled RT pass (the shadow-decal depth pre-pass): like rt_rp but the
     * DEPTH attachment is STORED and ends in SHADER_READ_ONLY_OPTIMAL too, so a
     * later pass in the same frame can sample the per-pixel depth. (rt_rp's depth
     * is storeOp=DONT_CARE - sampling it after the pass would read undefined
     * memory per spec, even where drivers happen to preserve it.) Attachment
     * formats match rt_rp/off_rp, so all existing pipelines are compatible. */
    VkRenderPass   rt_ds_rp;
    /* rt_rp with an R32G32B32A32_UINT colour attachment: the pass of every
     * RHI_COLOR_FORMAT_RGBA32_UINT target and the pass its pipelines are
     * built against. */
    VkRenderPass   rt_u32_rp;
    VkImage        off_image;
    VkDeviceMemory off_mem;
    VkImageView    off_view;
    VkFramebuffer  off_fb;

    /* Depth attachment for the offscreen target (native-renderer depth):
     * D32_SFLOAT, cleared to 1.0 each frame, store-discarded. Pipelines opt in
     * via rhi_pipeline_desc.depth_test; the 2D panels leave it off. */
    VkImage        off_depth_image;
    VkDeviceMemory off_depth_mem;
    VkImageView    off_depth_view;

    /* Commands + sync */
    VkCommandPool   pool;
    VkCommandBuffer cmd[RHI_FRAMES_IN_FLIGHT];
    VkSemaphore     img_avail[RHI_FRAMES_IN_FLIGHT];
    VkFence         in_flight[RHI_FRAMES_IN_FLIGHT];
    VkSemaphore     render_done[RHI_MAX_SC_IMAGES];
    VkFence         images_in_flight[RHI_MAX_SC_IMAGES]; /* not owned */

    /* Staged presents (rhi_end_frame_to_stage / rhi_present_stage): a ring
     * of swapchain-sized images a rendered frame is blitted into instead of
     * the swapchain, so the render can run ahead of the instant the frame is
     * meant to appear, and the present step is an acquire and a blit. All
     * synchronisation between the two is queue submission order plus image
     * barriers -- no semaphore crosses them, so a staged frame that is never
     * presented (superseded, or the display was taken back) leaves nothing
     * pending. Recreated with the swapchain; `stage_gen` tells a holder of a
     * stage index that its image is gone. */
    VkImage         stage_image[RHI_STAGE_RING];
    VkDeviceMemory  stage_mem[RHI_STAGE_RING];
    VkImageLayout   stage_layout[RHI_STAGE_RING];
    uint32_t        stage_gen;
    VkCommandBuffer pcmd[RHI_STAGE_RING];       /* the present step's own ring */
    VkFence         pfence[RHI_STAGE_RING];
    VkSemaphore     pimg_avail[RHI_STAGE_RING];
    uint32_t        pslot;
    bool            pacquired;      /* rhi_present_stage_acquire holds an image for the next submit */
    uint32_t        pacquired_gen;
    uint32_t        pacquired_index; /* WHICH image that acquire returned: image_index is clobbered
                                      * by any begin_frame in between (the inline path, after the
                                      * present thread hands the display back), and presenting an
                                      * image the held semaphore was not armed for faults the driver. */

    /* All live pipelines on this device (doubly linked via rhi_pipeline
     * prev/next): the sweep list of a live rebuild (rhi_rebuild_request). */
    struct rhi_pipeline *pipes_head;

    uint32_t frame;        /* 0..RHI_FRAMES_IN_FLIGHT-1 */
    uint32_t image_index;  /* acquired swapchain image this frame */
    bool     needs_recreate;
    /* Presents dropped because no swapchain image arrived in time - a live
     * resize is the expected cause. Counted so a silent frame drop is
     * visible rather than mysterious. */
    unsigned acquire_timeouts;
    /* Presents skipped outright because a previous acquire expired and the
     * back-off window is still open. Counted separately from the timeouts
     * themselves: one expiry can suppress several presents, and a caller that
     * cannot tell them apart cannot tell a display running at half rate from
     * one running at full rate. */
    uint64_t acquire_sheds;
    /* fence_sheds and acquire_sheds at the last rhi_take_sheds. */
    uint64_t fence_sheds_taken, acquire_sheds_taken;
    /* OPENPETE_ACQUIRE_FAULT window and period in ns (acquire_fault); 0 =
     * off. fault_read: the variable has been parsed. */
    Uint64   fault_off_ns, fault_period_ns;
    bool     fault_read;
    /* Acquire budget in ns, derived from the display refresh at swapchain
     * creation; the derivation is documented at RHI_ACQUIRE_TIMEOUT_NS. */
    Uint64   acquire_budget_ns;
    /* The presenting thread has nothing else to do: block on the slot fence
     * and the acquire instead of shedding. Set by a dedicated present thread;
     * clear while the thread that runs the sim presents. */
    bool     present_blocking;
    float    refresh_hz;          /* of the display the swapchain was built for (0 = unknown) */
    /* Set by display_change_watch when the window moves to another display;
     * consumed by ensure_swapchain on the display owner's thread. */
    atomic_bool display_changed;
    float    refresh_stub_hz;     /* harness: the refresh query's answer when above 0 */
    /* While set, presents are skipped without blocking (see the acquire
     * budget above). Cleared once the window passes. */
    Uint64   acquire_backoff_until_ns;
    /* Same idea for swapchain rebuilds, which are far more expensive. */
    Uint64   recreate_backoff_until_ns;
    /* Window size as last observed while waiting for a resize to settle. */
    int      last_seen_w, last_seen_h;
    /* Window pixel size when the swapchain was created; ensure_swapchain
     * rebuilds before a present once the window no longer has it. */
    int      sc_win_w, sc_win_h;
    Uint64   size_stable_since_ns;
    /* Display timing (VK_KHR_present_id + VK_KHR_present_wait), when the
     * driver offers both. Each present carries an id; waiting on an id blocks
     * until that present has been DISPLAYED, which is the only number here
     * that is not a submit time. */
    bool     has_present_timing;
    uint64_t present_id_next;      /* id the NEXT present will carry (from 1) */
    uint64_t present_id_submitted; /* id of the last present actually queued  */
    uint64_t present_id_seen;      /* newest id confirmed DISPLAYED           */
    /* Set when the caller knows the size change was DISCRETE - one the app
     * caused, arriving in a single step. The settle window exists to absorb a
     * drag's per-pixel walk; applying it to a fullscreen toggle just freezes
     * the display for the length of the window. */
    bool     resize_discrete;
    bool     frame_active;  /* between begin_frame and end_frame */
    /* The current frame holds an acquired swapchain image: the blit targets
     * it and end_frame presents it. Clear for headless and staged frames and
     * for a capture-mode frame the swapchain could not take. */
    bool     frame_acquired;
    /* Offline capture (rhi_set_capture_mode): the slot and image fences are
     * waited without bound, the acquire does not wait, and a frame the
     * swapchain cannot take renders and submits without a present. */
    bool     capture_mode;
    /* A zero surface extent has been logged since the last swapchain
     * creation (surface_extent); cleared by create_swapchain. */
    bool     sc_zero_logged;
    /* Swapchain census, logged by rhi_destroy: successful rebuilds,
     * vkCreateSwapchainKHR failures, surface rebuilds, presents the driver
     * accepted, and that present count at the last successful rebuild. */
    uint32_t sc_recreates;
    uint32_t sc_create_failures;
    uint32_t sc_surface_rebuilds;
    uint64_t sc_presents;
    uint64_t sc_presents_at_recreate;
    /* OPENPETE_VK_FAULT=window-held*: the window holds a binding to a
     * swapchain (fault_swapchain_create). */
    bool     fault_window_bound;
    uint32_t draw_calls;    /* rhi_draw invocations since begin_frame (perf census) */
    bool     pass_open;     /* a render pass is recording (offscreen or a render target) */
    /* Compute (rhi_compute_*): whether the device runs compute passes, decided
     * at creation by compute_caps_init, and why not; the facts it read, for
     * rhi_device_report; the open pass, its bound pipeline and set, and the
     * dispatches recorded since begin_frame. */
    bool        compute_ok;
    const char *compute_why;
    char        compute_facts[256];
    bool        compute_open;
    bool        compute_set_bound;
    struct rhi_compute_pipeline *compute_pipe;
    uint32_t    compute_dispatches;
    VkExtent2D cur_extent;  /* render area of the CURRENT pass (offscreen off_extent
                            * or the bound render target) - for rhi_clear_depth */

    float clear[4];

    /* VRAM-sampler infrastructure: one shared sampler + set layout +
     * pool; each rhi_texture owns a descriptor set allocated from the pool. */
    VkSampler             vram_sampler;
    VkSampler             linear_sampler;   /* LINEAR twin for HD replacement art */
    VkDescriptorSetLayout vram_set_layout;
    /* Descriptor pools, oldest first; desc_alloc appends one when every
     * pool is out of memory, up to RHI_DESC_POOLS_MAX. */
    VkDescriptorPool      desc_pools[RHI_DESC_POOLS_MAX];
    int                   desc_pool_n;
    VkPipelineLayout      cur_layout;  /* layout of the bound pipeline */
    VkShaderStageFlags    cur_pc_stages; /* its push-constant stage flags */
    bool                  cur_depth_rev; /* its depth class's convention (rhi_bound_depth_reversed) */

    /* Reusable host-visible staging buffer for rhi_read_offscreen (the blocking
     * screenshot path). Persistent so it doesn't vkAllocateMemory/free each call;
     * grown on demand. */
    rhi_buffer           *readback_buf;

    /* Async readback ring for --record (rhi_cycle_readback): one slot per frame
     * in flight. The copy into slot i rides cmd[i] under fence in_flight[i], so
     * begin_frame's existing wait on in_flight[frame] guarantees slot[frame]'s
     * previous copy has completed by the time that slot is cycled again - the
     * pixels are retrieved with NO added GPU stall (unlike rhi_read_offscreen's
     * vkDeviceWaitIdle), which is what lets recording run at real speed. */
    struct {
        rhi_buffer *staging;   /* host-visible TRANSFER_DST; grown on demand */
        bool        pending;   /* a copy was queued into cmd[i], not yet retrieved */
        int         w, h;      /* extent captured into staging */
        uint64_t    tag;       /* opaque caller token (the PTS) for this capture */
    } readback_ring[RHI_FRAMES_IN_FLIGHT];

    /* Second async ring for rhi_cycle_rt_readback (the occlusion-grid RT - a
     * few-KB color image read back every present). Same fence discipline as
     * readback_ring; single client (one RT per device), so no per-RT rings. */
    struct {
        rhi_buffer *staging;
        bool        pending;
        int         w, h;
    } grid_ring[RHI_FRAMES_IN_FLIGHT];

    /* Thumbnail ring (rhi_cycle_thumb): per frame slot a small image the
     * offscreen is blitted into and the host staging buffer it is copied to.
     * The blit and the copy ride cmd[i] under in_flight[i], the discipline
     * of readback_ring, so retrieving slot i after begin_frame's fence wait
     * adds no GPU wait. */
    struct {
        VkImage         image;
        VkDeviceMemory  mem;
        int             iw, ih;    /* the image's extent */
        rhi_buffer     *staging;
        bool            pending;
        int             w, h;      /* extent of the pending copy */
        uint64_t        tag;
    } thumb_ring[RHI_FRAMES_IN_FLIGHT];

    /* GPU per-pass timestamp zones (OPENPETE_GPU_TIMERS=1): one timestamp
     * query pool per frame in flight, 2 queries per zone. Slot i's results are
     * harvested in rhi_begin_frame right after slot i's fence wait - the same
     * discipline as readback_ring, so retrieval never blocks (results arrive
     * RHI_FRAMES_IN_FLIGHT presents late; fine for a diagnostic). Both zone
     * timestamps are BOTTOM_OF_PIPE: elapsed = previous work drained -> this
     * zone's work drained, which attributes correctly on a serial queue and
     * degrades to an approximation where the GPU overlaps passes. */
    /* Mod-shader extensions: 2-binding dynamic-UBO set layout (postfx
     * set 3), per-frame-slot descriptor sets for sampling off_image (the
     * COMPOSITE postfx input - lazily (re)written when off_gen moves, i.e.
     * after every offscreen recreate), and the UBO offset alignment. */
    VkDescriptorSetLayout ubo_set_layout;
    VkDescriptorSetLayout ubo1_set_layout;            /* one dynamic binding    */
    VkDescriptorSetLayout ssbo1_set_layout;           /* one dynamic storage binding */
    VkDescriptorSetLayout texssbo_set_layout;         /* sampler at 0 + dynamic storage at 1 */
    /* The 16-byte zero storage buffer every rhi_ubo set shows at binding 2
     * (rhi_ubo_data); created with the first rhi_ubo. */
    struct rhi_buffer *ubo_data_zero;
    VkDescriptorSet off_sampled_set[RHI_FRAMES_IN_FLIGHT];
    uint32_t        off_gen;                          /* bumped per create_offscreen */
    uint32_t        off_sampled_gen[RHI_FRAMES_IN_FLIGHT];
    VkDeviceSize    ubo_align;                        /* minUniformBufferOffsetAlignment */
    VkDeviceSize    ssbo_align;                       /* minStorageBufferOffsetAlignment */
    uint32_t        max_desc_sets;                    /* maxBoundDescriptorSets          */
    /* Two-sampler set layout (binding 0 = a colour texture, binding 1 = a
     * sampled depth image) for pipelines with set1_vram_depth. Its sets are
     * written per bind: each bind in a frame takes the next set of the frame
     * slot's list, grown on demand, and the list's cursor rewinds when the
     * slot's fence has retired (begin_frame_impl), so a set is never
     * rewritten while a recorded draw still reads it. */
    VkDescriptorSetLayout pair_set_layout;
    /* Device-wide upload ring for in-frame texture uploads: one chunk list
     * per frame slot, its cursors reset on the slot's first use in a new
     * frame (the slot's fence has retired it). A request no chunk fits
     * appends a chunk of max(RHI_UPLOAD_CHUNK, need) bytes; chunks live
     * until rhi_destroy, so a range handed out earlier in the frame stays
     * valid and two uploads in one frame never share bytes. */
    struct rhi_stage_chunk *up_ring[RHI_FRAMES_IN_FLIGHT];
    uint64_t        up_serial[RHI_FRAMES_IN_FLIGHT];
    struct rhi_texture *init_head;   /* textures awaiting their initial transition */
    /* Three-sampler set layout (bindings 0-2): the terrain key image's set
     * in the key-resolve and material layouts (key depth, drain records, key
     * owner attributes). Its sets are written per bind like the pair's. */
    VkDescriptorSetLayout trio_set_layout;
    /* Four-sampler set layout (bindings 0-3): the actor key set of the
     * key-resolve and material layouts (the flat actor key image's depth and
     * owner colour, the per-pixel actor key image's depth and owner colour).
     * Its sets are written per bind like the pair's. */
    VkDescriptorSetLayout quad_set_layout;
    /* The per-bind set lists of the pair (index 0), trio (index 1) and quad
     * (index 2) layouts, one per frame slot. */
    rhi_view_sets   vsets[3][RHI_FRAMES_IN_FLIGHT];

    bool        gpu_timers;                 /* env set AND queue has timestamps */
    float       gpu_ts_period;              /* ns per timestamp tick */
    VkQueryPool gpu_qp[RHI_FRAMES_IN_FLIGHT];
    const char *gpu_zone_name[RHI_FRAMES_IN_FLIGHT][RHI_GPU_ZONES_MAX];
    uint8_t     gpu_zone_depth[RHI_FRAMES_IN_FLIGHT][RHI_GPU_ZONES_MAX];
    int         gpu_zone_n[RHI_FRAMES_IN_FLIGHT];   /* zones begun this frame */
    int         gpu_zone_stack[RHI_GPU_ZONES_MAX];  /* open-zone index stack   */
    int         gpu_zone_sp;                        /* nesting depth           */
    uint64_t    gpu_zone_token[RHI_FRAMES_IN_FLIGHT];   /* caller tag @ submit  */
    rhi_gpu_zone_result gpu_results[RHI_GPU_ZONES_MAX];
    int         gpu_results_n;
    uint64_t    gpu_results_token;                  /* tag of harvested slot   */
    /* Storage a grown ring vertex buffer replaced (rhi_vertex_buffer_reserve),
     * kept until no submitted frame can still read it. */
    struct rhi_retired_buf *retired;
    struct rhi_texture *retired_tex;   /* rhi_destroy_texture_deferred, linked by retire_next */
    struct rhi_retired_pipe *retired_pipes;   /* pipelines replaced by a live rebuild */
};

/* One replaced buffer allocation and the frame serial current when it was
 * replaced: frames up to that serial may have recorded binds of it. */
/* A pipeline and layout replaced by a live rebuild, destroyed once
 * every frame that may have bound them has retired. */
struct rhi_retired_pipe {
    struct rhi_retired_pipe *next;
    VkPipeline       pipeline;
    VkPipelineLayout layout;
    uint64_t         serial;
};

struct rhi_retired_buf {
    struct rhi_retired_buf *next;
    VkBuffer       buffer;
    VkDeviceMemory memory;
    uint64_t       serial;
};
static void retired_reclaim(rhi_device *dev, bool all);

struct rhi_buffer {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void          *mapped;
    VkDeviceSize   size;
    /* Dynamic per-frame ring (rhi_create_vertex_buffer_dynamic): the buffer
     * holds RHI_FRAMES_IN_FLIGHT slots of `ring_stride` bytes; update writes
     * and bind reads the CURRENT frame's slot, so a per-present rewrite can
     * never race a previous in-flight frame's GPU read (the frame's fence was
     * waited in rhi_begin_frame before its slot is reused). 0 = single
     * (static) buffer. */
    VkDeviceSize   ring_stride;
    /* Generations within one frame slot (rhi_create_vertex_buffer_dynamic_gens):
     * a write that follows a bind in the same frame moves to the next
     * generation, so the draw already recorded against the previous data
     * keeps it; the GPU reads host-visible memory at execution, not at
     * record. Physical slot = frame * gens + gen. gens <= 1: one generation,
     * the write overwrites in place. */
    uint32_t       gens, gen_cur;
    /* Per ring slot, the end of the furthest byte range the CPU wrote
     * (rhi_update_buffer, rhi_update_buffer_range); NULL on a static buffer.
     * Bytes past it were never written, so a growth copies only below it. */
    VkDeviceSize  *slot_hwm;
    uint64_t       gen_serial;   /* frame serial gen_cur/gen_bound belong to */
    bool           gen_bound;    /* current generation referenced by a draw */
    bool           device_local; /* memory type carries DEVICE_LOCAL (device-mapped buffers) */
    uint64_t       uid;          /* nonzero, distinct per device-mapped buffer made: a
                                  * descriptor cached for a buffer keys on it, since a
                                  * freed buffer's address can be reused by the next */
    rhi_device    *dev;        /* for frame indexing in rhi_update_buffer */
};

/* Same-frame rewrite accounting (engagement evidence for the generation
 * mechanism): bumps = writes that moved to a fresh generation; overflows =
 * writes that had no generation left and overwrote bound data in place. */
static uint64_t s_ring_gen_bumps, s_ring_gen_overflows, s_dbg_gw, s_dbg_gb;
static uint64_t s_frame_serial;

/* Boot facts (rhi_boot_stats_get) on the rhi_set_clock clock, and whether a
 * frame has been submitted: the first submit ends the bring-up phase of the
 * pipeline-create instruments. */
static int64_t (*s_clock)(void);
static rhi_boot_stats s_boot;
static atomic_bool s_first_frame_done;   /* read by compile jobs on any thread */
static atomic_bool s_precompile_done;    /* rhi_pipe_precompile_done */
/* Per-thread create phase (rhi_pipe_phase_set) and rebuild marker. */
static _Thread_local rhi_pipe_phase t_pipe_phase;
static _Thread_local bool t_in_rebuild;

void rhi_set_clock(int64_t (*now_ns)(void)) { s_clock = now_ns; }
void rhi_pipe_phase_set(rhi_pipe_phase phase) { t_pipe_phase = phase; }

rhi_pipe_phase rhi_pipe_phase_get(void) { return t_pipe_phase; }

/** @brief Return the rhi_set_clock time, or 0 without a clock.
 *
 * @return ns on the installed clock. */
static int64_t boot_now(void) { return s_clock ? s_clock() : 0; }

/* Set while the launcher draws (rhi_boot_launcher_frames). */
static atomic_bool s_launcher_frames;

void rhi_boot_launcher_frames(bool on) { atomic_store(&s_launcher_frames, on); }

/** @brief Record the first frame submission: its time, and the end of the bring-up create phase.
 *
 * A launcher frame records nothing. */
static void boot_frame_submitted(void)
{
    if (atomic_load(&s_first_frame_done) || atomic_load(&s_launcher_frames)) return;
    atomic_store(&s_first_frame_done, true);
    s_boot.first_frame_ns = boot_now();
}

void rhi_boot_stats_get(rhi_boot_stats *out)
{
    *out = s_boot;
    out->bringup_creates = rhi_pc_bringup(&out->bringup_ns);
    unsigned creates = 0;
    rhi_pc_counts(RHI_PC_PHASE_BRINGUP, &creates, &out->bringup_known);
    rhi_pc_counts(RHI_PC_PHASE_PRECOMPILE, &out->precompile_creates, &out->precompile_known);
}
/** @brief Return whether OPENPETE_RING_GEN_DBG requests the ring-generation totals at rhi_destroy.
 *
 * The variable is read once; unset, empty or starting with 0 is off. */
static bool ring_gen_dbg(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("OPENPETE_RING_GEN_DBG"); v = (e && *e && *e != '0'); }
    return v;
}
/** @brief Return the slot index for the next write of a ring buffer inside the active frame.
 *
 * A single-generation buffer writes the frame's slot. A generation buffer
 * starts each frame at generation 0; a write after a bind in the same frame
 * moves to the next generation (counted in s_ring_gen_bumps), and with none
 * left overwrites the current one (counted in s_ring_gen_overflows, logged
 * once).
 * @param buf Ring buffer; its device must have a frame recording.
 * @return frame x gens + current generation. */
static uint32_t ring_write_slot(rhi_buffer *buf)
{
    rhi_device *dev = buf->dev;
    if (buf->gens <= 1) return (uint32_t)dev->frame;
    s_dbg_gw++;
    if (buf->gen_serial != s_frame_serial) {
        buf->gen_serial = s_frame_serial; buf->gen_cur = 0; buf->gen_bound = false;
    } else if (buf->gen_bound) {
        if (buf->gen_cur + 1 < buf->gens) { buf->gen_cur++; s_ring_gen_bumps++; }
        else {
            s_ring_gen_overflows++;
            static bool once; if (!once) { once = true;
                RHI_LOG("ring buffer: %u generations exhausted in frame %llu; overwriting bound data\n",
                        buf->gens, (unsigned long long)s_frame_serial); }
        }
        buf->gen_bound = false;
    }
    return (uint32_t)dev->frame * buf->gens + buf->gen_cur;
}
/** @brief Return the slot index a bind inside the active frame references, and mark the generation bound.
 *
 * The first touch in a new frame resets the generation to 0. Marking it
 * bound makes the next same-frame write move to a fresh generation.
 * @param buf Ring buffer; its device must have a frame recording. NULL
 *            returns 0 without a read.
 * @return frame x gens + current generation; the frame's slot for a
 *         single-generation buffer; 0 for a NULL buffer. */
static uint32_t ring_bind_slot(rhi_buffer *buf)
{
    if (!buf) return 0;
    rhi_device *dev = buf->dev;
    if (buf->gens <= 1) return (uint32_t)dev->frame;
    s_dbg_gb++;
    if (buf->gen_serial != s_frame_serial) {
        buf->gen_serial = s_frame_serial; buf->gen_cur = 0;
    }
    buf->gen_bound = true;
    return (uint32_t)dev->frame * buf->gens + buf->gen_cur;
}
uint64_t rhi_ring_gen_bumps(void) { return s_ring_gen_bumps; }
uint64_t rhi_ring_gen_overflows(void) { return s_ring_gen_overflows; }

/* Stage facts recorded when a compile job resolves its stages: the desc's
 * dedupe hash, each stage's SPIR-V content hash and declared constant ids,
 * and the same for the whole-create vertex stage (vert_spv_whole; wvs_hash
 * 0 when the desc names none). */
typedef struct {
    uint64_t desc_hash, vs_hash, fs_hash, wvs_hash;
    uint32_t vs_ids, fs_ids, wvs_ids;
} pipe_stage_info;

/* How a pipeline's objects were made: created whole from its stages, fast
 * linked from library parts, or linked from them with link-time
 * optimization. */
typedef enum { PIPE_LINK_WHOLE = 0, PIPE_LINK_FAST, PIPE_LINK_LTO } pipe_link;

/* The bare result of one pipeline create: no rhi_pipeline, no registry link. */
typedef struct {
    VkPipeline         pipeline;
    VkPipelineLayout   layout;
    VkShaderStageFlags pc_stages;   /* stage visibility of the push-constant block */
    uint64_t           key;         /* dedupe key of the create info (vert_spv's view) */
    pipe_link          link;        /* how the pipeline was made */
    bool               whole_stage; /* created whole from the desc's vert_spv_whole */
} pipe_compiled;

/* A link-time-optimized link waiting for its swap (lto_swap_ready): the
 * registry serial of the pipeline it replaces and its objects, whose key is
 * the create info they were linked for. */
struct lto_ready {
    struct lto_ready *next;
    uint64_t          serial;
    pipe_compiled     c;
};

struct rhi_pipeline {
    VkPipeline       pipeline;
    VkPipelineLayout layout;
    /* Rebuild sources (rhi_rebuild_request): the desc plus owned copies of
     * the shader identities - embedded/loose filenames, or in-memory SPIR-V
     * for mod shaders - so any pipeline can be re-baked with new vertex
     * spec-constant values without its owner's involvement. desc.vert_spv /
     * desc.frag_spv point at the owned name copies (NULL when words given). */
    rhi_pipeline_desc desc;
    char            *vert_name, *frag_name;   /* owned filename copies      */
    char            *whole_name;              /* owned desc.vert_spv_whole copy */
    uint32_t        *vert_words, *frag_words; /* owned SPIR-V word copies   */
    size_t           vert_n, frag_n;          /* word counts                */
    struct rhi_pipeline *prev, *next;         /* device-wide rebuild registry */
    uint64_t         swap_serial;  /* s_frame_serial when a rebuild installed the objects */
    bool             swap_idle;    /* that rebuild ran outside a recording frame */
    VkShaderStageFlags pc_stages; /* stage visibility of the push-constant
                                   * block (FRAGMENT; terrain: VERTEX too) -
                                   * rhi_push_constants must match the range */
    pipe_stage_info  info;         /* recorded when the creating job resolved its stages */
    uint64_t         vkey;         /* dedupe key of the installed objects' create info */
    uint32_t         rb_gen;       /* the rebuild set's gen while a member, else 0 */
    uint32_t         rb_idx;       /* its entry index in that set */
    uint64_t         reg_serial;   /* its registry serial: unique, rising with registration */
    pipe_link        link;         /* how the installed objects were made */
    bool             whole_stage;  /* they were created whole from desc.vert_spv_whole */
    bool             lto_out;      /* a link-time-optimized link of it is on the pool */
};

/* A compile job (rhi.h, variant-explicit compile jobs): every input of one
 * create, owned, and its result. The stage names and words are kept so an
 * installed pipeline can be rebuilt; vb/fb are the bytes the run reads. */
struct rhi_compile_job {
    rhi_pipeline_desc desc;          /* vert_spv / frag_spv point at the name copies */
    char            *vert_name, *frag_name;     /* owned .spv name copies (built-in stages) */
    char            *whole_name;                /* owned desc.vert_spv_whole copy            */
    uint32_t        *vert_words, *frag_words;   /* owned SPIR-V word copies (mod stages)    */
    size_t           vert_n, frag_n;            /* word counts                               */
    const void      *vb, *fb, *wb;   /* resolved stage bytes: the words, an embedded blob or a file
                                      * read; wb the whole-create vertex stage's, or NULL */
    size_t           vbn, fbn, wbn;  /* their byte counts                          */
    void            *vown, *fown, *wown; /* OPENPETE_SHADER_DIR file bytes read at the job's making */
    pipe_stage_info  info;
    rhi_pipe_variant variant;
    VkBool32         slot_contest;   /* SLOT_CONTEST at the job's making           */
    uint32_t         generation, epoch;
    bool             lto;            /* the optimized form, no probe: created whole from wb when
                                      * resolved, else linked from library parts with link-time
                                      * optimization */
    pipe_compiled    out;            /* the result; out.pipeline VK_NULL_HANDLE until a run succeeds */
};

/* Descriptor sets a texture keeps for rhi_bind_texture_storage pairs. The
 * terrain cache, the one caller, keeps at most OPTC_STORES_ALIVE stores
 * alive at once (the live one and its unfreed retirees); the slack covers a
 * freed store whose set the last RHI_FRAMES_IN_FLIGHT frame serials still
 * bound, which happens when the device-owning thread's service calls (one
 * per VBL headless) outpace the frame serials (one per present). */
#define RHI_TEX_STORE_SETS 8

struct rhi_texture {
    VkImage         image;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkDescriptorSet set;       /* points at (view, dev->vram_sampler) */
    VkDescriptorPool set_pool, depth_set_pool;   /* pools set and depth_set came from */
    bool            uploadable; /* a sampled image (create_sampled_image), not a render target */
    /* Created while a frame was recording: the UNDEFINED -> SHADER_READ_ONLY
     * transition is recorded into that frame's command buffer before its
     * next render pass or its end (record_pending_inits), unless an upload
     * transitions it first. Linked from dev->init_head. */
    struct rhi_texture *init_next;
    bool            init_pending;
    struct rhi_texture *retire_next;   /* dev->retired_tex link */
    uint64_t        retire_serial;     /* frame serial at retirement */
    uint32_t        width, height;
    uint32_t        bpp;       /* bytes per pixel (2 = R16_UINT VRAM, 4 = RGBA8) */
    VkImageLayout   layout;    /* current layout, for re-upload transitions */

    /* Render-target extras (is_rt only): a depth attachment + a framebuffer so
     * geometry can be rendered into `image` via rt_pass (rt_rp, or rt_ds_rp for a
     * depth-sampled target). */
    bool            is_rt;
    VkImage         rt_depth_image;
    VkDeviceMemory  rt_depth_mem;
    VkImageView     rt_depth_view;
    VkFramebuffer   rt_fb;
    VkRenderPass    rt_pass;   /* pass rhi_begin_render_target uses (NULL -> rt_rp) */
    /* Depth-sampled RT only (rhi_create_render_target_depth_sampled): a DEPTH-
     * aspect-only view (sampling a view with DEPTH|STENCIL aspects is illegal)
     * + a descriptor set (sample view, vram_sampler) so the depth can be bound
     * as a combined image sampler. NULL on a plain render target. */
    VkImageView     rt_depth_sample_view;
    VkDescriptorSet depth_set;
    /* rhi_bind_texture_storage: this image at binding 0 beside a storage
     * range at binding 1 (texssbo_set_layout), one set per (store uid,
     * range) pair, made at the pair's first bind; store_serial is the last
     * frame serial that bound the set. A replaced store takes another entry,
     * so the set the in-flight frames bound stays valid until they retire. */
    VkDescriptorSet  store_set[RHI_TEX_STORE_SETS];
    VkDescriptorPool store_pool[RHI_TEX_STORE_SETS];
    uint64_t         store_uid[RHI_TEX_STORE_SETS];
    uint32_t         store_range[RHI_TEX_STORE_SETS];
    uint64_t         store_serial[RHI_TEX_STORE_SETS];
    bool            census;    /* counted in s_census (create_sampled_image) */
    int64_t         census_dev_bytes;
    bool            depth_readable; /* depth image carries TRANSFER_SRC
                                     * (rhi_read_render_target_depth) */
    rhi_depth_class depth_class; /* the depth convention of its pass (SCENE default) */
};

/* ------------------------------------------------------------------ */

/** @brief Debug-utils messenger callback that logs warning and error messages from the validation layer.
 *
 * @param sev Message severity; only WARNING and ERROR are logged.
 * @param type Message type (unused).
 * @param data Callback data carrying the message text.
 * @param ud User data (unused).
 * @return VK_FALSE, so the call that triggered the message is not aborted. */
static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
         VkDebugUtilsMessageTypeFlagsEXT type,
         const VkDebugUtilsMessengerCallbackDataEXT *data, void *ud)
{
    (void)type; (void)ud;
    if (sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
        RHI_LOG("validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

/** @brief Return the index of the first memory type in `type_bits` that has all of `props`.
 *
 * Scans the device's memory types in index order, so the lowest matching index
 * wins; a type must be set in @p type_bits and carry every flag of @p props
 * (extra flags are allowed).
 * @param phys Physical device whose memory types are searched.
 * @param type_bits memoryTypeBits mask of a VkMemoryRequirements.
 * @param props Property flags the type must include.
 * @return The memory-type index, or UINT32_MAX when none matches; callers
 *         compare against UINT32_MAX. */
static uint32_t find_memtype(VkPhysicalDevice phys, uint32_t type_bits,
                             VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

/** @brief Return whether the Vulkan loader lists the instance layer `name`.
 *
 * Enumerates the instance layers into a temporary array and compares names
 * exactly.
 * @param name Layer name, such as VK_LAYER_KHRONOS_validation.
 * @return true when the layer is listed; false when it is not or the loader
 *         reports no layers. */
static bool layer_available(const char *name)
{
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    if (n == 0) return false;
    VkLayerProperties *props = calloc(n, sizeof(*props));
    vkEnumerateInstanceLayerProperties(&n, props);
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++) {
        if (strcmp(props[i].layerName, name) == 0) found = true;
    }
    free(props);
    return found;
}

/* Facts the device report states: the validation layer, the enabled device
 * extensions and the swapchain's colour space, recorded at creation. */
static bool s_validation_on;
static char s_dev_exts[1024];
static int  s_sc_colorspace = -1;

/** @brief Return whether to enable the Khronos validation layer and its debug-utils messenger.
 *
 * Opt-in: OPENPETE_VK_VALIDATION set to a value not starting with '0'
 * requests them, and only when the loader offers the layer (it ships with
 * the Vulkan SDK, not with drivers); a requested but missing layer logs.
 * Unset or 0 requests neither, so a machine with the SDK installed runs the
 * same instance as one without it. Read once, at instance creation.
 * @return true when the layer is requested and available. */
static bool want_validation_layer(void)
{
    const char *e = rhi_knob("OPENPETE_VK_VALIDATION");
    if (!(e && *e && *e != '0')) return false;
    if (layer_available("VK_LAYER_KHRONOS_validation")) return true;
    RHI_LOG("OPENPETE_VK_VALIDATION set but VK_LAYER_KHRONOS_validation is "
            "not installed; validation off\n");
    return false;
}

/* ------------------------------------------------------------------ */

/** @brief Create the Vulkan 1.2 instance with the surface extensions SDL requires, plus validation when requested.
 *
 * Windowed runs enable the extensions SDL_Vulkan_GetInstanceExtensions
 * reports; headless runs enable none. rhi_loader_env_prepare runs first,
 * before the loader scans implicit layers. When want_validation_layer() returns
 * true, the instance also enables VK_LAYER_KHRONOS_validation and
 * VK_EXT_debug_utils, and a messenger routing warnings and errors to
 * debug_cb is created on dev->debug.
 * @param dev Device whose headless flag is read and whose instance and
 *            debug messenger are written.
 * @param app_name Application name for VkApplicationInfo; NULL selects
 *                 "openpete".
 * @return true on success; false when SDL cannot report its extensions or
 *         vkCreateInstance fails. */
static bool create_instance(rhi_device *dev, const char *app_name)
{
    /* The loader reads a layer's disable variable when it scans implicit
     * layers, which want_validation_layer's layer query and vkCreateInstance
     * both do. A windowed caller already ran it before creating the window. */
    rhi_loader_env_prepare(NULL);

    /* Windowed: SDL supplies the platform surface extensions. Headless: none -
     * a surfaceless instance needs no surface extension at all. */
    Uint32 sdl_n = 0;
    char const *const *sdl_ext = NULL;
    if (!dev->headless) {
        sdl_ext = SDL_Vulkan_GetInstanceExtensions(&sdl_n);
        if (!sdl_ext) {
            RHI_LOG("SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
            return false;
        }
    }

    bool want_validation = want_validation_layer();
    s_validation_on = want_validation;

    /* Copy SDL's required extensions (none when headless), optionally append
     * debug utils. */
    uint32_t ext_n = sdl_n + (want_validation ? 1u : 0u);
    const char **exts = ext_n ? calloc(ext_n, sizeof(*exts)) : NULL;
    for (Uint32 i = 0; i < sdl_n; i++) exts[i] = sdl_ext[i];
    if (want_validation) exts[sdl_n] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = app_name ? app_name : "openpete",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "openpete-rhi",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = ext_n,
        .ppEnabledExtensionNames = exts,
        .enabledLayerCount = want_validation ? 1u : 0u,
        .ppEnabledLayerNames = want_validation ? layers : NULL,
    };
    VkResult r = vkCreateInstance(&ci, NULL, &dev->instance);
    free(exts);
    if (r != VK_SUCCESS) {
        RHI_LOG("vkCreateInstance failed: %d\n", (int)r);
        return false;
    }

    if (want_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT mk =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                dev->instance, "vkCreateDebugUtilsMessengerEXT");
        if (mk) {
            VkDebugUtilsMessengerCreateInfoEXT dci = {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity =
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType =
                    VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = debug_cb,
            };
            mk(dev->instance, &dci, NULL, &dev->debug);
        }
        RHI_LOG("validation layer enabled\n");
    }
    return true;
}

/** @brief Pick the stencil-capable depth format: D32_SFLOAT_S8_UINT when supported, else D24_UNORM_S8_UINT.
 *
 * The float format is preferred because the reversed scene depth convention
 * (depth_convention_init) needs a float mantissa. The Vulkan spec guarantees
 * at least one of the two supports DEPTH_STENCIL_ATTACHMENT with optimal
 * tiling. OPENPETE_RHI_DEPTH_D24=1 skips the float candidate so the D24 path
 * runs on a device that supports both.
 * @param phys Physical device whose format properties are queried.
 * @return The chosen format. */
static VkFormat pick_depth_format(VkPhysicalDevice phys)
{
    const VkFormat cand[2] = { VK_FORMAT_D32_SFLOAT_S8_UINT,
                               VK_FORMAT_D24_UNORM_S8_UINT };
    /* OPENPETE_RHI_DEPTH_D24=1 starts at the fallback, so the D24 selection
     * runs on a device that has both. */
    const char *force = rhi_knob("OPENPETE_RHI_DEPTH_D24");
    for (int i = (force && *force == '1') ? 1 : 0; i < 2; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, cand[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return cand[i];
    }
    return VK_FORMAT_D32_SFLOAT_S8_UINT;  /* unreachable per spec */
}

/** @brief Return a log name for a depth attachment format.
 *
 * @param f Format chosen by pick_depth_format.
 * @return "D32_SFLOAT_S8_UINT" or "D24_UNORM_S8_UINT"; "other" for any
 *         other format. The string is static. */
static const char *depth_format_name(VkFormat f)
{
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32_SFLOAT_S8_UINT"
         : f == VK_FORMAT_D24_UNORM_S8_UINT  ? "D24_UNORM_S8_UINT" : "other";
}

/** @brief Set the depth compare op, clear value and reversed flag of every depth class for dev->depth_format.
 *
 * FORWARD images store nearer as smaller: LESS_OR_EQUAL, clear 1. The SCENE
 * convention on the float format is reversed (the scene stages write
 * z_ndc = 1/vz, psx_viewz.glsl op_scene_clip): GREATER_OR_EQUAL, clear 0, so
 * the float's exponent tracks 1/vz and every distance keeps about 2^-24
 * relative precision. D24_UNORM has one absolute step everywhere, where 1/vz
 * would spend it near the camera, so the fallback keeps the forward
 * convention and z_ndc = 1 - 1/vz. dev->cur_depth_clear starts at the SCENE
 * clear value.
 * @param dev Device whose depth_format is read and whose depth_cmp,
 *            depth_clear, depth_rev and cur_depth_clear are written. */
static void depth_convention_init(rhi_device *dev)
{
    const bool rev = dev->depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (int c = 0; c < RHI_DEPTH_CLASSES; c++) {
        dev->depth_cmp[c]   = VK_COMPARE_OP_LESS_OR_EQUAL;
        dev->depth_clear[c] = 1.0f;
        dev->depth_rev[c]   = VK_FALSE;
    }
    if (rev) {
        dev->depth_cmp[RHI_DEPTH_SCENE]   = VK_COMPARE_OP_GREATER_OR_EQUAL;
        dev->depth_clear[RHI_DEPTH_SCENE] = 0.0f;
        dev->depth_rev[RHI_DEPTH_SCENE]   = VK_TRUE;
    }
    dev->cur_depth_clear = dev->depth_clear[RHI_DEPTH_SCENE];
    RHI_LOG("depth format %s\n", depth_format_name(dev->depth_format));
}

/** @brief List the instance's physical devices into a calloc'd array.
 *
 * A VK_INCOMPLETE result keeps the handles the loader wrote and is logged;
 * an error code or an empty list is logged with its VkResult.
 * @param instance The Vulkan instance.
 * @param out_n Receives the number of handles in the returned array.
 * @return The array, which the caller frees, or NULL when enumeration fails
 *         or lists no device. */
static VkPhysicalDevice *enumerate_physical_devices(VkInstance instance, uint32_t *out_n)
{
    uint32_t n = 0;
    VkResult r = vkEnumeratePhysicalDevices(instance, &n, NULL);
    if (fault_enumerate_due()) n = 0;
    VkPhysicalDevice *devs = (r >= 0 && n > 0) ? calloc(n, sizeof(*devs)) : NULL;
    if (devs) r = vkEnumeratePhysicalDevices(instance, &n, devs);
    if (r < 0 || n == 0) {
        RHI_LOG("no Vulkan physical devices (vkEnumeratePhysicalDevices: %d, count %u)\n",
                (int)r, n);
        free(devs);
        return NULL;
    }
    if (r != VK_SUCCESS)
        RHI_LOG("vkEnumeratePhysicalDevices returned %d; using the %u devices listed\n",
                (int)r, n);
    *out_n = n;
    return devs;
}

/** @brief Return whether an extension name is in a device's extension list.
 *
 * @param avail The list, or NULL.
 * @param n Its length.
 * @param name Extension name.
 * @return true when listed. */
static bool ext_offered(const VkExtensionProperties *avail, uint32_t n, const char *name)
{
    for (uint32_t i = 0; avail && i < n; i++)
        if (strcmp(avail[i].extensionName, name) == 0) return true;
    return false;
}

/* Feature, property and create-info structs of VK_EXT_extended_dynamic_state3
 * and VK_EXT_graphics_pipeline_library, which the pinned Vulkan headers
 * predate; layouts, structure types and flag bits from the Vulkan registry.
 * The first is queried for the pipeline feature report only; the second is
 * enabled with fast linking (pipeline_exts_select) for the library path. */
#define RHI_ST_EDS3_FEATURES   ((VkStructureType)1000455000)
#define RHI_ST_GPL_FEATURES    ((VkStructureType)1000320000)
#define RHI_ST_GPL_PROPS       ((VkStructureType)1000320001)
#define RHI_ST_GPL_CREATE_INFO ((VkStructureType)1000320002)
#define RHI_GPL_VERTEX_INPUT_INTERFACE   0x1u
#define RHI_GPL_PRE_RASTERIZATION        0x2u
#define RHI_GPL_FRAGMENT_SHADER          0x4u
#define RHI_GPL_FRAGMENT_OUTPUT          0x8u
#define RHI_PIPE_CREATE_LTO              0x00000400u   /* LINK_TIME_OPTIMIZATION_BIT_EXT */
#define RHI_PIPE_CREATE_RETAIN_LTO       0x00800000u   /* RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT */
typedef struct {
    VkStructureType sType;
    const void     *pNext;
    VkFlags         flags;   /* RHI_GPL_* */
} rhi_gpl_create_info;
typedef struct {
    VkStructureType sType;
    void           *pNext;
    VkBool32        f[31];   /* f[2] = extendedDynamicState3PolygonMode */
} rhi_eds3_features;
typedef struct {
    VkStructureType sType;
    void           *pNext;
    VkBool32        graphicsPipelineLibrary;
} rhi_gpl_features;
typedef struct {
    VkStructureType sType;
    void           *pNext;
    VkBool32        fastLinking;
    VkBool32        independentInterpolationDecoration;
} rhi_gpl_props;

/* What the pipeline feature report found on a physical device. */
typedef struct {
    bool cache_control;   /* pipelineCreationCacheControl */
    bool eds3_polygon;    /* extendedDynamicState3PolygonMode */
    bool gpl;             /* graphicsPipelineLibrary */
    bool gpl_fast_link;   /* graphicsPipelineLibraryFastLinking */
} rhi_pipe_features;

/** @brief Query the pipeline-related features of the offered extensions through vkGetPhysicalDeviceFeatures2.
 *
 * A struct is chained only for an extension the device lists; a device
 * below Vulkan 1.1 reports every feature absent.
 * @param phys Physical device.
 * @param cc The device lists VK_EXT_pipeline_creation_cache_control.
 * @param eds3 It lists VK_EXT_extended_dynamic_state3.
 * @param gpl It lists VK_EXT_graphics_pipeline_library.
 * @return The features found. */
static rhi_pipe_features query_pipe_features(VkPhysicalDevice phys, bool cc, bool eds3, bool gpl)
{
    rhi_pipe_features out = {0};
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.apiVersion < VK_API_VERSION_1_1) return out;
    VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT f_cc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT };
    rhi_eds3_features f_eds3 = { .sType = RHI_ST_EDS3_FEATURES };
    rhi_gpl_features f_gpl = { .sType = RHI_ST_GPL_FEATURES };
    rhi_gpl_props p_gpl = { .sType = RHI_ST_GPL_PROPS };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    void *chain = NULL;
    if (cc)   { f_cc.pNext = chain;   chain = &f_cc; }
    if (eds3) { f_eds3.pNext = chain; chain = &f_eds3; }
    if (gpl)  { f_gpl.pNext = chain;  chain = &f_gpl; p2.pNext = &p_gpl; }
    f2.pNext = chain;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    vkGetPhysicalDeviceProperties2(phys, &p2);
    out.cache_control = f_cc.pipelineCreationCacheControl == VK_TRUE;
    out.eds3_polygon = f_eds3.f[2] == VK_TRUE;
    out.gpl = f_gpl.graphicsPipelineLibrary == VK_TRUE;
    out.gpl_fast_link = p_gpl.fastLinking == VK_TRUE;
    return out;
}

/** @brief Select the pipeline extensions a device offers, record them, and log the pipeline feature report.
 *
 * VK_EXT_pipeline_creation_feedback is enabled when listed (it adds a
 * per-create report and nothing to any other call), and
 * VK_EXT_pipeline_creation_cache_control when listed with its feature (no
 * create passes its flags unless asked); OPENPETE_PIPE_EXTS starting with 0
 * declines both, for pricing them against a run without.
 * VK_EXT_graphics_pipeline_library and its dependency VK_KHR_pipeline_library
 * are enabled when the device lists both, has graphicsPipelineLibrary and
 * reports graphicsPipelineLibraryFastLinking: the terrain-cache pipelines
 * are then linked from libraries (pipe_uses_library).
 * OPENPETE_PIPELINE_LIBRARY starting with 0 declines the pair, so every
 * pipeline is created whole, for an A/B against the library path. With the
 * library path on, its pipelines are also linked with link-time
 * optimization (rhi_pipeline_lto_step) unless OPENPETE_PIPELINE_LTO starts
 * with 0, which keeps the fast links for an A/B. The report line also names
 * extendedDynamicState3PolygonMode, queried only.
 * @param dev Device whose phys is read and has_creation_feedback,
 *            has_cache_control, has_gpl and has_lto are written.
 * @param avail The device's extension list.
 * @param n Its length.
 * @param out Receives the names to enable (room for four).
 * @return How many names were written. */
static uint32_t pipeline_exts_select(rhi_device *dev, const VkExtensionProperties *avail,
                                     uint32_t n, const char **out)
{
    static const char k_fb[] = "VK_EXT_pipeline_creation_feedback";
    static const char k_cc[] = "VK_EXT_pipeline_creation_cache_control";
    static const char k_gpl[] = "VK_EXT_graphics_pipeline_library";
    static const char k_pl[] = "VK_KHR_pipeline_library";
    const bool cc_ext = ext_offered(avail, n, k_cc);
    const bool eds3 = ext_offered(avail, n, "VK_EXT_extended_dynamic_state3");
    const bool gpl = ext_offered(avail, n, k_gpl) && ext_offered(avail, n, k_pl);
    const rhi_pipe_features f = query_pipe_features(dev->phys, cc_ext, eds3, gpl);
    uint32_t k = 0;
    const char *x_env = rhi_knob("OPENPETE_PIPE_EXTS");
    const bool declined = x_env && *x_env == '0';
    const char *l_env = rhi_knob("OPENPETE_PIPELINE_LIBRARY");
    const bool gpl_declined = l_env && *l_env == '0';
    const bool fb_listed = ext_offered(avail, n, k_fb);
    dev->has_creation_feedback = fb_listed && !declined;
    dev->has_cache_control = cc_ext && f.cache_control && !declined;
    dev->has_gpl = gpl && f.gpl && f.gpl_fast_link && !gpl_declined;
    const char *o_env = rhi_knob("OPENPETE_PIPELINE_LTO");
    dev->has_lto = dev->has_gpl && !(o_env && *o_env == '0');
    if (dev->has_creation_feedback) out[k++] = k_fb;
    if (dev->has_cache_control) out[k++] = k_cc;
    if (dev->has_gpl) { out[k++] = k_pl; out[k++] = k_gpl; }
    RHI_LOG("pipeline features: creation_feedback=%s cache_control=%s "
            "eds3_polygon_mode=%s graphics_pipeline_library=%s gpl_fast_link=%s gpl_lto=%s\n",
            dev->has_creation_feedback ? "enabled" : fb_listed ? "declined" : "absent",
            dev->has_cache_control ? "enabled" : !cc_ext ? "absent"
                                   : declined ? "declined" : "listed-no-feature",
            eds3 ? (f.eds3_polygon ? "yes" : "no") : "absent",
            dev->has_gpl ? "enabled" : !gpl ? "absent" : !f.gpl ? "no"
                         : !f.gpl_fast_link ? "no-fast-link" : "declined",
            gpl ? (f.gpl_fast_link ? "yes" : "no") : "absent",
            dev->has_lto ? "enabled" : dev->has_gpl ? "declined" : "off");
    return k;
}

/* The feature structs of the pipeline extensions pipeline_exts_select
 * enabled, chained onto vkCreateDevice. */
typedef struct {
    VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT cc;
    rhi_gpl_features                                        gpl;
} pipe_feature_chain;

/** @brief Chain the feature structs of the enabled pipeline extensions in front of a device-create chain.
 *
 * pipelineCreationCacheControl when has_cache_control, graphicsPipelineLibrary
 * when has_gpl; the counterpart of pipeline_exts_select, which chose them.
 * @param dev Device whose has_cache_control and has_gpl are read.
 * @param c Storage for the structs, which must outlive vkCreateDevice.
 * @param next The rest of the chain, or NULL.
 * @return The chain's head: @p next when neither is enabled. */
static void *pipeline_features_chain(const rhi_device *dev, pipe_feature_chain *c, void *next)
{
    c->cc = (VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT,
        .pNext = next,
        .pipelineCreationCacheControl = VK_TRUE,
    };
    if (dev->has_cache_control) next = &c->cc;
    c->gpl = (rhi_gpl_features){
        .sType = RHI_ST_GPL_FEATURES,
        .pNext = next,
        .graphicsPipelineLibrary = VK_TRUE,
    };
    if (dev->has_gpl) next = &c->gpl;
    return next;
}

/** @brief Create the library cache's lock and condition on a device with the graphics pipeline library enabled.
 *
 * A failed creation turns the library path off (has_gpl and has_lto
 * false): the extension stays enabled and every pipeline is created whole.
 * @param dev Device, created; its has_gpl is read and may be cleared with
 *            has_lto, gpl_mx and gpl_cv are written. */
static void pipeline_library_locks(rhi_device *dev)
{
    if (!dev->has_gpl) return;
    dev->gpl_mx = SDL_CreateMutex();
    dev->gpl_cv = SDL_CreateCondition();
    if (dev->gpl_mx && dev->gpl_cv) return;
    RHI_LOG("pipeline libraries: lock creation failed; every pipeline is created whole\n");
    dev->has_gpl = false;
    dev->has_lto = false;
}

/** @brief Write a device's driver version, decoded the way its vendor packs driverVersion.
 *
 * NVIDIA packs 10.8.8.6 bits; every other vendor uses the Vulkan version
 * macros. A pacing baseline is invalidated by a driver update as surely as
 * by a different GPU, so the version is recorded decoded, not guessed.
 * @param props The device's properties.
 * @param buf Output buffer.
 * @param cap Its size. */
static void format_driver_version(const VkPhysicalDeviceProperties *props, char *buf, size_t cap)
{
    const uint32_t dv = props->driverVersion;
    if (props->vendorID == 0x10DE)
        snprintf(buf, cap, "%u.%u.%u.%u", (dv >> 22) & 0x3FF, (dv >> 14) & 0xFF,
                 (dv >> 6) & 0xFF, dv & 0x3F);
    else
        snprintf(buf, cap, "%u.%u.%u", VK_API_VERSION_MAJOR(dv),
                 VK_API_VERSION_MINOR(dv), VK_API_VERSION_PATCH(dv));
}

/** @brief Return the short name of a physical device type, as the device list logs it.
 *
 * @param t The type.
 * @return "discrete", "integrated", "virtual", "cpu" or "other". */
static const char *device_type_name(VkPhysicalDeviceType t)
{
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
        default:                                     return "other";
    }
}

/** @brief Rank a physical device type for the automatic pick: discrete 4, integrated 3, virtual 2, other 1, CPU 0.
 *
 * Real hardware is preferred over a software rasterizer (llvmpipe reports
 * as CPU).
 * @param t The type.
 * @return The rank; higher wins. */
static int device_type_score(VkPhysicalDeviceType t)
{
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
        default:                                     return 1;
    }
}

/** @brief Find a physical device's first queue family with graphics that can present to dev->surface.
 *
 * Headless has no surface, so a graphics family alone qualifies.
 * @param dev Device whose headless flag and surface are read.
 * @param pd The physical device.
 * @param has_graphics Set to whether any family has graphics; may be NULL.
 * @return The family index, or UINT32_MAX when none qualifies. */
static uint32_t usable_queue_family(const rhi_device *dev, VkPhysicalDevice pd, bool *has_graphics)
{
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *q = qn ? calloc(qn, sizeof(*q)) : NULL;
    if (q) vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, q);
    else qn = 0;
    uint32_t fam = UINT32_MAX;
    bool gfx = false;
    for (uint32_t j = 0; j < qn && fam == UINT32_MAX; j++) {
        if (!(q[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        gfx = true;
        VkBool32 present = VK_FALSE;
        if (!dev->headless)
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, j, dev->surface, &present);
        if (dev->headless || present) fam = j;
    }
    free(q);
    if (has_graphics) *has_graphics = gfx;
    return fam;
}

/** @brief Log every enumerated physical device: index, name, type, vendor and device id, decoded driver version.
 *
 * @param devs The devices, in enumeration order (the order --gpu indexes).
 * @param n Their count. */
static void log_physical_devices(const VkPhysicalDevice *devs, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        char drv[64];
        format_driver_version(&props, drv, sizeof drv);
        RHI_LOG("gpu %u: %s (%s, vendor 0x%04x device 0x%04x, driver %s)\n", i,
                props.deviceName, device_type_name(props.deviceType),
                props.vendorID, props.deviceID, drv);
    }
}

/** @brief Return the device a --gpu value names: an all-digit value is an enumeration index, any other the first device whose deviceName contains it, case-insensitively.
 *
 * An all-digit value is never matched against names, so "1" cannot select a
 * device whose name contains a 1.
 * @param devs The devices, in enumeration order.
 * @param n Their count.
 * @param choice The value; non-empty.
 * @return The index, or -1 when the index is out of range or no name matches. */
static int match_device_choice(const VkPhysicalDevice *devs, uint32_t n, const char *choice)
{
    if (strspn(choice, "0123456789") == strlen(choice)) {
        const unsigned long idx = strtoul(choice, NULL, 10);
        return idx < n ? (int)idx : -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (SDL_strcasestr(props.deviceName, choice)) return (int)i;
    }
    return -1;
}

/** @brief Return the device dev->gpu_choice names when it qualifies, else -1 with one log line naming why.
 *
 * A device qualifies with a graphics queue family that can present to
 * dev->surface (headless: graphics alone).
 * @param dev Device whose gpu_choice, headless flag and surface are read.
 * @param devs The devices, in enumeration order.
 * @param n Their count.
 * @param fam Set to the chosen device's queue family.
 * @return The index, or -1 when the choice is empty, matches nothing or
 *         names a device that does not qualify. */
static int chosen_by_request(const rhi_device *dev, const VkPhysicalDevice *devs, uint32_t n,
                             uint32_t *fam)
{
    if (!dev->gpu_choice[0]) return -1;
    const int i = match_device_choice(devs, n, dev->gpu_choice);
    if (i < 0) {
        RHI_LOG("gpu \"%s\": no enumerated device matches; choosing automatically\n",
                dev->gpu_choice);
        return -1;
    }
    bool gfx = false;
    *fam = usable_queue_family(dev, devs[i], &gfx);
    if (*fam != UINT32_MAX) return i;
    RHI_LOG("gpu \"%s\": device %d %s; choosing automatically\n", dev->gpu_choice, i,
            !gfx ? "has no graphics queue" : "cannot present to the window");
    return -1;
}

/** @brief Choose the physical device and its graphics queue family: the requested one when it qualifies, else the best-ranked qualifying one.
 *
 * Logs the choice and why (requested, or automatic). The automatic pick
 * ranks qualifying devices by device_type_score; the first of the highest
 * rank wins.
 * @param dev Device whose gpu_choice, headless flag and surface are read.
 * @param devs The devices, in enumeration order.
 * @param n Their count.
 * @param fam Set to the chosen device's queue family.
 * @return The index, or -1 when no device qualifies. */
static int choose_physical_device(const rhi_device *dev, const VkPhysicalDevice *devs, uint32_t n,
                                  uint32_t *fam)
{
    int best = chosen_by_request(dev, devs, n, fam);
    const char *why = "matched";
    if (best < 0) {
        int best_score = -1;
        why = "automatic";
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t f = usable_queue_family(dev, devs[i], NULL);
            if (f == UINT32_MAX) continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devs[i], &props);
            const int score = device_type_score(props.deviceType);
            if (score > best_score) { best = (int)i; best_score = score; *fam = f; }
        }
    }
    if (best >= 0) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[best], &props);
        RHI_LOG("gpu chosen: %d %s (%s%s%s%s)\n", best, props.deviceName, why,
                dev->gpu_choice[0] ? ", requested \"" : "", dev->gpu_choice,
                dev->gpu_choice[0] ? "\"" : "");
    }
    return best;
}

static void compute_caps_init(rhi_device *dev);

/** @brief Choose the physical device and queue family and create the logical device and its graphics queue.
 *
 * Every enumerated device is logged (log_physical_devices), then the choice
 * (choose_physical_device): dev->gpu_choice when it names a qualifying
 * device, else the automatic rank. The chosen device's name, decoded driver
 * version and software flag are recorded in dev. The depth format and depth convention
 * follow from the chosen device (pick_depth_format, depth_convention_init).
 *
 * Optional device features are enabled only where supported and recorded as
 * dev->has_depth_bias_clamp and has_fill_non_solid.
 * VK_KHR_present_id and VK_KHR_present_wait are enabled, with the swapchain
 * extension, only on a windowed device that has both and whose
 * OPENPETE_PRESENT_TIMING, OPENPETE_PACE_FLIP_LOG or OPENPETE_EAGER_LATTICE
 * asks for them (OPENPETE_PRESENT_TIMING=0 declines); dev->has_present_timing
 * records the outcome. pipeline_exts_select adds the pipeline extensions on
 * every device, headless included, and logs the pipeline feature report; a
 * headless device enables no other extension. With the graphics pipeline
 * library enabled, its feature is chained and its lock created. Then
 * compute_caps_init decides compute availability, enabling nothing.
 * @param dev Device whose instance, gpu_choice, headless flag and surface are
 *            read and whose phys, gfx_family, device, gfx_queue, depth and feature
 *            fields are written.
 * @return false when no physical device is listed, none qualifies, or
 *         vkCreateDevice fails (each logged); true otherwise. */
static bool pick_physical_and_device(rhi_device *dev)
{
    uint32_t n = 0;
    VkPhysicalDevice *devs = enumerate_physical_devices(dev->instance, &n);
    if (!devs) return false;
    log_physical_devices(devs, n);
    uint32_t chosen_family = UINT32_MAX;
    const int pick = choose_physical_device(dev, devs, n, &chosen_family);
    const VkPhysicalDevice chosen = pick >= 0 ? devs[pick] : VK_NULL_HANDLE;
    free(devs);
    if (chosen == VK_NULL_HANDLE) {
        RHI_LOG("no device with a graphics%s queue\n",
                dev->headless ? "" : "+present");
        return false;
    }
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(chosen, &props);
        snprintf(dev->device_name, sizeof(dev->device_name), "%s", props.deviceName);
        format_driver_version(&props, dev->driver_version, sizeof dev->driver_version);
        dev->device_software = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
    }
    dev->phys = chosen;
    dev->gfx_family = chosen_family;
    dev->depth_format = pick_depth_format(chosen);
    depth_convention_init(dev);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = chosen_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    /* Optional features (enable only what's supported; nothing here is
     * load-bearing): depthBiasClamp lets rhi_set_depth_bias bound the decal
     * pull - without it the clamp param is forced to 0 (spec
     * VUID-vkCmdSetDepthBias-depthBiasClamp-00790). */
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(dev->phys, &supported);
    VkPhysicalDeviceFeatures enable = {0};
    enable.depthBiasClamp = supported.depthBiasClamp;
    dev->has_depth_bias_clamp = (supported.depthBiasClamp == VK_TRUE);
    /* geometryShader: only for the terrain face-identity diagnostic
     * (OPENPETE_TERRAIN_FACEID), whose fragment stage reads gl_PrimitiveID
     * and so declares the Geometry capability. No other stage needs it. */
    {
        const char *fid = getenv("OPENPETE_TERRAIN_FACEID");
        if (fid && *fid && *fid != '0') enable.geometryShader = supported.geometryShader;
    }
    /* fillModeNonSolid: VK_POLYGON_MODE_LINE for the wireframe view
     * (rhi_set_wireframe); without it that view falls back to filled. */
    enable.fillModeNonSolid = supported.fillModeNonSolid;
    dev->has_fill_non_solid = (supported.fillModeNonSolid == VK_TRUE);
    /* Optional display-timing pair. present_id tags each present with a
     * monotonically increasing id; present_wait blocks until the present
     * carrying that id has actually been DISPLAYED. Together they are the only
     * portable way to learn a flip time - every timestamp the engine takes
     * itself is a SUBMIT time, and under FIFO the display quantises submits to
     * its own refresh, so submit-side spacing can look even while the flips do
     * not (and the reverse). Enabled only when both are present; nothing here
     * is load-bearing. */
    uint32_t dev_ext_n = 0;
    vkEnumerateDeviceExtensionProperties(dev->phys, NULL, &dev_ext_n, NULL);
    VkExtensionProperties *dev_avail = dev_ext_n
        ? calloc(dev_ext_n, sizeof(*dev_avail)) : NULL;
    if (dev_avail)
        vkEnumerateDeviceExtensionProperties(dev->phys, NULL, &dev_ext_n, dev_avail);
    const bool have_present_id = ext_offered(dev_avail, dev_ext_n, "VK_KHR_present_id");
    const bool have_present_wait = ext_offered(dev_avail, dev_ext_n, "VK_KHR_present_wait");

    /* OFF unless asked for, because enabling it is NOT free: chaining a
     * present id onto every present measured 34.5 / 31.0 / 33.4 late ticks per
     * 1000 against 11.7 / 12.9 / 15.2 with the pair declined - same leg, same
     * binary, complete separation, and the enabled arm fails the standing
     * gate's threshold of 15. An extension that changes what the driver does
     * with the present queue has to be priced before it is switched on for
     * everyone, not assumed inert because the app ignores it.
     *
     * OPENPETE_PACE_FLIP_LOG implies it: asking for flip times is asking to
     * pay for them, and the pacing numbers from such a run are not comparable
     * with a normal one. */
    const char *pt_env = rhi_knob("OPENPETE_PRESENT_TIMING");
    const char *fl_env = rhi_knob("OPENPETE_PACE_FLIP_LOG");
    const char *lt_env = rhi_knob("OPENPETE_EAGER_LATTICE");
    /* OPENPETE_EAGER_LATTICE implies it too: there the presenter is its own
     * thread, the sim thread never touches the queue, and the flips are the
     * pacing SOURCE (the present thread fits the display's vblank lattice
     * from them). OPENPETE_PRESENT_TIMING=0 declines it explicitly. */
    const bool pt_declined = pt_env && *pt_env == '0';
    const bool pt_want = !pt_declined &&
                         ((pt_env && *pt_env && *pt_env != '0') ||
                          (fl_env && *fl_env && *fl_env != '0') ||
                          (lt_env && *lt_env && *lt_env != '0'));
    RHI_LOG("display timing: present_id %s, present_wait %s -> %s\n",
            have_present_id ? "available" : "absent",
            have_present_wait ? "available" : "absent",
            (pt_want && have_present_id && have_present_wait)
                ? "ENABLED (costs pacing; measurement runs only)" : "off");
    dev->has_present_timing = pt_want && !dev->headless &&
                              have_present_id && have_present_wait;
    /* Swapchain first, the present-timing pair after it, then the pipeline
     * extensions; a headless device takes only the last. */
    const char *dev_ext[7] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                               "VK_KHR_present_id", "VK_KHR_present_wait" };
    uint32_t ext_n = dev->headless ? 0u : (dev->has_present_timing ? 3u : 1u);
    ext_n += pipeline_exts_select(dev, dev_avail, dev_ext_n, dev_ext + ext_n);
    free(dev_avail);
    VkPhysicalDevicePresentWaitFeaturesKHR feat_wait = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
        .presentWait = VK_TRUE,
    };
    VkPhysicalDevicePresentIdFeaturesKHR feat_id = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
        .pNext = &feat_wait,
        .presentId = VK_TRUE,
    };
    pipe_feature_chain pf;
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = pipeline_features_chain(dev, &pf, dev->has_present_timing ? &feat_id : NULL),
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = ext_n,
        .ppEnabledExtensionNames = ext_n ? dev_ext : NULL,
        .pEnabledFeatures = &enable,
    };
    VkResult r = vkCreateDevice(dev->phys, &dci, NULL, &dev->device);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateDevice failed: %d\n", (int)r); return false; }
    s_dev_exts[0] = '\0';
    for (uint32_t i = 0, at = 0; i < ext_n && at < sizeof s_dev_exts; i++)
        at += (uint32_t)snprintf(s_dev_exts + at, sizeof s_dev_exts - at, "%s%s", i ? " " : "",
                                 dev_ext[i]);
    vkGetDeviceQueue(dev->device, chosen_family, 0, &dev->gfx_queue);
    pipeline_library_locks(dev);
    compute_caps_init(dev);
    return true;
}

/** @brief Return the swapchain extent for the surface capabilities.
 *
 * The surface's currentExtent is used unless its width is UINT32_MAX (the
 * surface leaves the size to the application); then the window's size in
 * pixels is clamped to the surface's minimum and maximum image extent. A
 * window that reports a zero size yields a zero extent when the minimum is
 * zero, which create_swapchain treats as minimized.
 * @param dev Device whose window is measured.
 * @param caps Surface capabilities of dev->phys and dev->surface.
 * @return The extent in pixels. */
static VkExtent2D choose_extent(rhi_device *dev, const VkSurfaceCapabilitiesKHR *caps)
{
    if (caps->currentExtent.width != UINT32_MAX) return caps->currentExtent;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(dev->window, &w, &h);
    VkExtent2D e = { (uint32_t)w, (uint32_t)h };
    if (e.width  < caps->minImageExtent.width)  e.width  = caps->minImageExtent.width;
    if (e.width  > caps->maxImageExtent.width)  e.width  = caps->maxImageExtent.width;
    if (e.height < caps->minImageExtent.height) e.height = caps->minImageExtent.height;
    if (e.height > caps->maxImageExtent.height) e.height = caps->maxImageExtent.height;
    return e;
}

/** @brief Return whether the surface advertises present mode `want`.
 *
 * FIFO is always supported (spec), so callers query only the vsync-off
 * modes.
 * @param dev Device whose physical device and surface are queried.
 * @param want Present mode to look for.
 * @return true when the surface lists @p want. */
static bool surface_has_present_mode(rhi_device *dev, VkPresentModeKHR want)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev->phys, dev->surface, &n, NULL);
    VkPresentModeKHR *modes = calloc(n ? n : 1, sizeof(*modes));
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev->phys, dev->surface, &n, modes);
    bool ok = false;
    for (uint32_t i = 0; i < n; i++) if (modes[i] == want) { ok = true; break; }
    free(modes);
    return ok;
}

/** @brief Choose the swapchain present mode from --vsync, then OPENPETE_PRESENT_MODE, then FIFO.
 *
 * Precedence: the explicit --vsync flag (dev->vsync_pref 0|1) beats the
 * OPENPETE_PRESENT_MODE env, which beats the default FIFO (vsync ON, always
 * supported, locks present to the monitor refresh).
 *   --vsync 1        -> FIFO (vsync ON).
 *   --vsync 0        -> a vsync-OFF mode (immediate, else mailbox) for low latency
 *                       or uncapped-throughput benchmarking (FIFO otherwise hides
 *                       the real frame rate behind the refresh ceiling).
 *   --vsync unset    -> OPENPETE_PRESENT_MODE=immediate|mailbox if set, else FIFO.
 * The env spelling is the device's present_mode_name: the knob source's value
 * at rhi_create, or what rhi_set_present_mode_name set since.
 * A vsync-OFF request the surface lacks falls back to FIFO (with a log).
 * @param dev Device whose vsync_pref, present_mode_name and surface are read.
 * @return The present mode to create the swapchain with. */
static VkPresentModeKHR choose_present_mode(rhi_device *dev)
{
    if (dev->vsync_pref == 1)
        return VK_PRESENT_MODE_FIFO_KHR;               /* explicit vsync ON */

    if (dev->vsync_pref == 0) {                         /* explicit vsync OFF */
        if (surface_has_present_mode(dev, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            RHI_LOG("present mode: immediate (--vsync 0)\n");
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        if (surface_has_present_mode(dev, VK_PRESENT_MODE_MAILBOX_KHR)) {
            RHI_LOG("present mode: mailbox (--vsync 0; immediate unsupported)\n");
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        RHI_LOG("--vsync 0 but no vsync-OFF present mode supported; using FIFO (vsync)\n");
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    /* unset (-1): defer to the env spelling, else default FIFO (vsync ON). */
    const char *env = dev->present_mode_name[0] ? dev->present_mode_name : NULL;
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    if (env && !strcmp(env, "immediate")) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (env && !strcmp(env, "mailbox")) want = VK_PRESENT_MODE_MAILBOX_KHR;
    if (want == VK_PRESENT_MODE_FIFO_KHR) return want; /* always supported */
    if (!surface_has_present_mode(dev, want)) {
        RHI_LOG("present mode '%s' unsupported by surface; using FIFO (vsync)\n", env);
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    RHI_LOG("present mode: %s (vsync OFF)\n", env);
    return want;
}

/** @brief Choose the swapchain surface format the present blit can write.
 *
 * The shaders write display-encoded values into the RHI_RT_FORMAT offscreen
 * image and the present blits it onto the swapchain, so the swapchain takes
 * the first of B8G8R8A8_UNORM, R8G8B8A8_UNORM and A8B8G8R8_UNORM_PACK32 in
 * SRGB_NONLINEAR whose optimal tiling supports BLIT_DST; an _SRGB format
 * would encode those values a second time. Without one, the first
 * blit-capable format the surface lists is taken and logged.
 * @param dev Device whose physical device is queried.
 * @param fmts The surface's formats.
 * @param fn Number of @p fmts, at least 1.
 * @return The chosen format. */
static VkSurfaceFormatKHR pick_surface_format(rhi_device *dev, const VkSurfaceFormatKHR *fmts,
                                              uint32_t fn)
{
    static const VkFormat pref[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32,
    };
    int fallback = -1;
    for (size_t p = 0; p <= sizeof pref / sizeof pref[0]; p++) {
        for (uint32_t i = 0; i < fn; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(dev->phys, fmts[i].format, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) continue;
            if (p == sizeof pref / sizeof pref[0]) { fallback = (int)i; break; }
            if (fmts[i].format == pref[p] &&
                fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return fmts[i];
        }
    }
    const VkSurfaceFormatKHR f = fmts[fallback >= 0 ? fallback : 0];
    RHI_LOG("create_swapchain: no 8-bit UNORM SRGB_NONLINEAR format; using format %d "
            "colorspace %d%s\n", (int)f.format, (int)f.colorSpace,
            fallback >= 0 ? "" : " (no blit-capable format listed)");
    return f;
}

/** @brief Read the new swapchain's images into sc_images, refusing a swapchain with more images than the device tracks.
 *
 * On refusal or failure the swapchain is destroyed and the reason logged.
 * @param dev Device whose swapchain was just created.
 * @return true when sc_images and sc_count hold every image. */
static bool swapchain_fetch_images(rhi_device *dev)
{
    dev->sc_count = 0;
    vkGetSwapchainImagesKHR(dev->device, dev->swapchain, &dev->sc_count, NULL);
    /* The driver may create more images than minImageCount asked for, and
     * acquire can then return any index below its count; sc_images,
     * render_done and images_in_flight hold RHI_MAX_SC_IMAGES. */
    if (dev->sc_count > RHI_MAX_SC_IMAGES) {
        RHI_LOG("create_swapchain: the driver created %u images, more than the "
                "%d this device tracks; swapchain refused\n",
                dev->sc_count, RHI_MAX_SC_IMAGES);
        vkDestroySwapchainKHR(dev->device, dev->swapchain, NULL);
        dev->swapchain = VK_NULL_HANDLE;
        dev->sc_count = 0;
        return false;
    }
    if (vkGetSwapchainImagesKHR(dev->device, dev->swapchain, &dev->sc_count,
                                dev->sc_images) != VK_SUCCESS) {
        RHI_LOG("create_swapchain: vkGetSwapchainImagesKHR failed\n");
        vkDestroySwapchainKHR(dev->device, dev->swapchain, NULL);
        dev->swapchain = VK_NULL_HANDLE;
        dev->sc_count = 0;
        return false;
    }
    return true;
}

/** @brief Set off_extent to the pinned extent when one is held, else to the swapchain extent.
 *
 * Runs wherever sc_extent or off_pin is assigned, so the offscreen targets
 * built after it (create_offscreen, create_stages) take the extent the
 * renderer composes at.
 * @param dev Device whose off_extent is written. */
static void off_extent_sync(rhi_device *dev)
{
    dev->off_extent = (dev->off_pin.width && dev->off_pin.height) ? dev->off_pin : dev->sc_extent;
}

/** @brief Query the surface capabilities and the extent a swapchain would take, refusing a zero extent.
 *
 * A minimized window reports a zero extent, and no swapchain can be created
 * at one. The first refusal after a swapchain creation is logged with the
 * extent the device keeps rendering at; later refusals are silent until
 * create_swapchain succeeds and clears sc_zero_logged.
 * @param dev Windowed device.
 * @param caps Receives the surface capabilities.
 * @param extent Receives the chosen extent.
 * @return false when the extent is zero. */
static bool surface_extent(rhi_device *dev, VkSurfaceCapabilitiesKHR *caps, VkExtent2D *extent)
{
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->phys, dev->surface, caps);
    *extent = choose_extent(dev, caps);
    if (extent->width != 0 && extent->height != 0) return true;
    if (!dev->sc_zero_logged)
        RHI_LOG("swapchain: surface extent %ux%u (window minimized): rebuild deferred, "
                "%s at %ux%u\n", extent->width, extent->height,
                dev->capture_mode ? "capture frames render without a present"
                                  : "presents skipped",
                dev->off_extent.width, dev->off_extent.height);
    dev->sc_zero_logged = true;
    return false;
}

/** @brief Return the refresh rate of the display that holds the device's window: the harness stub when set, else SDL's current mode of that display.
 *
 * @param dev Windowed device.
 * @return The rate in Hz; 0 when unknown. */
static float window_refresh_hz(const rhi_device *dev)
{
    if (dev->refresh_stub_hz > 0.0f) return dev->refresh_stub_hz;
    if (!dev->window) return 0.0f;
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(dev->window));
    return dm ? dm->refresh_rate : 0.0f;
}

/** @brief SDL event watch that flags a display change of the device's window for the display owner.
 *
 * Runs on the thread that raises the event (the event pump), so it only
 * stores an atomic flag; ensure_swapchain consumes it on the presenting
 * thread, where the swapchain is rebuilt.
 * @param ud The rhi_device.
 * @param ev The event.
 * @return true, so the event is queued as usual. */
static bool SDLCALL display_change_watch(void *ud, SDL_Event *ev)
{
    rhi_device *dev = ud;
    if (ev->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED && dev->window &&
        ev->window.windowID == SDL_GetWindowID(dev->window))
        atomic_store(&dev->display_changed, true);
    return true;
}

/** @brief Consume a flagged display change: when the new display's refresh differs from the swapchain's, flag a discrete rebuild.
 *
 * The rebuild re-reads the refresh (window_refresh_hz), so the acquire
 * budget and the rate rhi_refresh_hz reports follow the window; FIFO's
 * present rate follows the display by itself.
 * @param dev Windowed device. */
static void consume_display_change(rhi_device *dev)
{
    if (!atomic_exchange(&dev->display_changed, false)) return;
    const float hz = window_refresh_hz(dev);
    RHI_LOG("display change: refresh %.3f -> %.3f Hz%s\n", (double)dev->refresh_hz, (double)hz,
            hz != dev->refresh_hz ? ", swapchain rebuild" : "");
    if (hz == dev->refresh_hz) return;
    dev->needs_recreate = true;
    dev->resize_discrete = true;
    dev->recreate_backoff_until_ns = 0;
    dev->acquire_backoff_until_ns = 0;
}

/** @brief Return the OPENPETE_VK_FAULT window-held mode: 0 off, 1 window-held, 2 window-held-surface.
 *
 * Both modes make the window stay bound to Vulkan from its first swapchain
 * creation, through vkDestroySwapchainKHR, until the VkSurfaceKHR is
 * destroyed; while bound, a vkCreateSwapchainKHR on it returns
 * VK_ERROR_NATIVE_WINDOW_IN_USE_KHR without creating anything. Mode 1
 * (window-held) also lets a creation that names a swapchain of the window as
 * oldSwapchain succeed and take the binding over; mode 2
 * (window-held-surface) refuses that creation too, so only a new surface
 * releases the window.
 * @return The mode. */
static int fault_window_held_mode(void)
{
    static OP_ONCE_INT cache = -1;
    int mode = op_once_int_get(&cache);
    if (mode >= 0) return mode;
    const char *e = getenv("OPENPETE_VK_FAULT");
    mode = 0;
    if (e && !strcmp(e, "window-held")) mode = 1;
    if (e && !strcmp(e, "window-held-surface")) mode = 2;
    if (mode) RHI_LOG("fault injection: the window stays bound after swapchain destruction "
                      "until %s\n", mode == 1 ? "an oldSwapchain handover or a new surface"
                                               : "a new surface");
    return op_once_int_fill(&cache, mode);
}

/** @brief Create a swapchain through vkCreateSwapchainKHR, refusing it as a driver that keeps the window bound would under OPENPETE_VK_FAULT=window-held*.
 *
 * Under the fault (fault_window_held_mode) a creation on a bound window
 * returns VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, except in mode 1 when
 * sci->oldSwapchain names a swapchain; a successful creation binds the
 * window. Without the fault this is vkCreateSwapchainKHR.
 * @param dev Device whose window binding is read and written.
 * @param sci The creation info.
 * @param out Receives the swapchain on success.
 * @return The creation result. */
static VkResult fault_swapchain_create(rhi_device *dev, const VkSwapchainCreateInfoKHR *sci,
                                       VkSwapchainKHR *out)
{
    const int held = fault_window_held_mode();
    const bool handover = held == 1 && sci->oldSwapchain != VK_NULL_HANDLE;
    if (held && dev->fault_window_bound && !handover) return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    const VkResult r = vkCreateSwapchainKHR(dev->device, sci, NULL, out);
    dev->fault_window_bound = dev->fault_window_bound || r == VK_SUCCESS;
    return r;
}

/** @brief Create the swapchain for the window's current size, format and present mode.
 *
 * A non-null @p old is retired by the creation, whether or not it succeeds,
 * and handed to the driver as oldSwapchain, so the window passes from the old
 * swapchain to the new one without a moment in which neither holds it; the
 * caller destroys @p old afterwards. dev->swapchain is written only on
 * success, and with it the present-id sequence restarts at 1.
 * @param dev Windowed device.
 * @param old The swapchain being replaced, or VK_NULL_HANDLE.
 * @return VK_SUCCESS; the vkCreateSwapchainKHR result when it fails;
 *         VK_NOT_READY when the window is minimized;
 *         VK_ERROR_INITIALIZATION_FAILED when the surface lists no format
 *         or the swapchain's images cannot be fetched. */
static VkResult create_swapchain(rhi_device *dev, VkSwapchainKHR old)
{
    VkSurfaceCapabilitiesKHR caps;
    VkExtent2D extent;
    if (!surface_extent(dev, &caps, &extent)) return VK_NOT_READY;
    SDL_GetWindowSizeInPixels(dev->window, &dev->sc_win_w, &dev->sc_win_h);
    /* The resize-settle check in ensure_swapchain compares against
     * last_seen_w/h: seeding them with the creation size makes a rebuild at
     * an unchanged size (an image-fence timeout, OUT_OF_DATE without a
     * resize) skip the settle, while a size change still reads as moving. */
    dev->last_seen_w = dev->sc_win_w;
    dev->last_seen_h = dev->sc_win_h;

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev->phys, dev->surface, &fn, NULL);
    if (fn == 0) {
        RHI_LOG("create_swapchain: surface reports zero formats\n");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkSurfaceFormatKHR *fmts = calloc(fn, sizeof(*fmts));
    if (!fmts) {
        RHI_LOG("create_swapchain: OOM for %u surface formats\n", fn);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev->phys, dev->surface, &fn, fmts);
    VkSurfaceFormatKHR pick = pick_surface_format(dev, fmts, fn);
    free(fmts);

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > RHI_MAX_SC_IMAGES) want = RHI_MAX_SC_IMAGES;

    s_sc_colorspace = (int)pick.colorSpace;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = dev->surface,
        .minImageCount = want,
        .imageFormat = pick.format,
        .imageColorSpace = pick.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        /* COLOR_ATTACHMENT for generality; TRANSFER_DST because we blit
         * the offscreen target onto it. */
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = (dev->present_mode_granted = choose_present_mode(dev)), /* FIFO (vsync) unless --vsync 0 / OPENPETE_PRESENT_MODE */
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = fault_swapchain_create(dev, &sci, &sc);
    if (r != VK_SUCCESS) {
        RHI_LOG("vkCreateSwapchainKHR failed: %d\n", (int)r);
        dev->sc_create_failures++;
        return r;
    }
    dev->swapchain = sc;
    /* Present ids belong to a swapchain: every new one starts its sequence at
     * 1 (0 is VK_KHR_present_id's "no id", which no vkWaitForPresentKHR can
     * confirm), and any id outstanding against a replaced one never completes. */
    dev->present_id_next = 1;
    dev->present_id_submitted = 0;
    dev->present_id_seen = 0;

    dev->sc_format = pick.format;
    dev->sc_extent = extent;
    off_extent_sync(dev);
    dev->sc_zero_logged = false;
    if (!swapchain_fetch_images(dev)) return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < RHI_MAX_SC_IMAGES; i++)
        dev->images_in_flight[i] = VK_NULL_HANDLE;

    /* Budget = 1.5 refresh periods, clamped to [RHI_ACQUIRE_BUDGET_MIN_NS,
     * RHI_ACQUIRE_BUDGET_MAX_NS] and RHI_ACQUIRE_TIMEOUT_NS when the refresh
     * is unknown.
     *
     * More than one period: presenting below the refresh rate means the image
     * arrives at a repaint boundary, and a budget of exactly one period
     * expires on any jitter at all. The cap keeps an expiry from becoming a
     * sim slip; the floor covers an absurd reported refresh.
     *
     * Only load-bearing under FIFO, since a non-blocking mode never waits, but
     * a mode change recreates the swapchain, so this re-runs either way. */
    {
        const float hz = window_refresh_hz(dev);
        dev->refresh_hz = hz;
        /* One period is what the display owes the present; the half period is
         * jitter margin. Scaling with the refresh rather than flooring at a
         * fixed value keeps both ends correct: a 60 Hz display gets 25 ms,
         * and a 480 Hz display gets 4 ms, so a genuine stall is abandoned
         * sooner where waiting costs the most. */
        Uint64 budget = (hz > 1.0f) ? (Uint64)(1.5f * 1e9f / hz)
                                    : RHI_ACQUIRE_TIMEOUT_NS;
        if (budget < RHI_ACQUIRE_BUDGET_MIN_NS) budget = RHI_ACQUIRE_BUDGET_MIN_NS;
        if (budget > RHI_ACQUIRE_BUDGET_MAX_NS) budget = RHI_ACQUIRE_BUDGET_MAX_NS;
        dev->acquire_budget_ns = budget;
        RHI_LOG("swapchain: %u image(s), acquire budget %.1f ms (refresh %.3f Hz)\n",
                dev->sc_count, (double)budget / 1e6, (double)hz);
    }
    return VK_SUCCESS;
}

/** @brief Create dev->surface on the device's window and check that the device's queue family presents to it.
 *
 * @param dev Windowed device whose surface is VK_NULL_HANDLE.
 * @return false when SDL cannot create the surface or the queue family
 *         cannot present to it; dev->surface is then VK_NULL_HANDLE. */
static bool surface_create(rhi_device *dev)
{
    if (!SDL_Vulkan_CreateSurface(dev->window, dev->instance, NULL, &dev->surface)) {
        RHI_LOG("surface: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        dev->surface = VK_NULL_HANDLE;
        return false;
    }
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(dev->phys, dev->gfx_family, dev->surface, &present);
    if (present) return true;
    RHI_LOG("surface: queue family %u cannot present to the new surface\n", dev->gfx_family);
    vkDestroySurfaceKHR(dev->instance, dev->surface, NULL);
    dev->surface = VK_NULL_HANDLE;
    return false;
}

/** @brief Replace dev->surface with a new VkSurfaceKHR on the same window, releasing a window the driver still holds for a destroyed swapchain.
 *
 * The caller has destroyed every swapchain of the surface.
 * @param dev Windowed device.
 * @return The surface_create result. */
static bool surface_rebuild(rhi_device *dev)
{
    RHI_LOG("swapchain: the window is in use (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) with no "
            "swapchain alive; rebuilding the surface\n");
    vkDestroySurfaceKHR(dev->instance, dev->surface, NULL);
    dev->surface = VK_NULL_HANDLE;
    dev->fault_window_bound = false;
    dev->sc_surface_rebuilds++;
    return surface_create(dev);
}

/* External dependencies every render pass of the device declares. A pipeline
 * is created against off_rp and drawn in rt_rp and rt_ds_rp too; render-pass
 * compatibility requires the passes to be identical apart from attachment
 * layouts and load/store ops, dependencies included, so each pass carries
 * the union of what any of them needs. */
#define RP_DEPS 3

/** @brief Fill the external subpass dependencies shared by every render pass of the device.
 *
 * In: the previous use's colour and depth-stencil writes (write after
 * write), the previous use's fragment-shader samples of a render target and
 * transfer reads of the offscreen image (write after read). Out: colour and
 * depth-stencil writes made visible to the present blit, the readback copy
 * and fragment-shader sampling of a render target.
 * @param deps Receives RP_DEPS dependencies. */
static void render_pass_deps(VkSubpassDependency deps[RP_DEPS])
{
    deps[0] = (VkSubpassDependency){
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };
    deps[1] = (VkSubpassDependency){
        .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
    };
    /* off_image is one target shared by every frame in flight: the next
     * frame's colour write waits for the previous frame's transfer reads of
     * it (the present blit and the async --record copy). */
    deps[2] = (VkSubpassDependency){
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
}

/** @brief Create the offscreen render passes off_rp (color cleared) and off_load_rp (color loaded).
 *
 * Both share one color and one combined depth-stencil attachment; the color
 * ends in TRANSFER_SRC for the present blit, depth and stencil clear and are
 * not stored. The external dependencies are render_pass_deps'.
 * @param dev Device whose formats are read and whose passes are written.
 * @return true on success, false when vkCreateRenderPass fails. */
static bool create_render_pass(rhi_device *dev)
{
    VkAttachmentDescription color = {
        .format = RHI_RT_FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        /* Leave it ready for the blit that reads it after the pass. */
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentDescription depth = {
        .format = dev->depth_format,      /* combined depth+stencil */
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, /* not read after the pass */
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,   /* stencil starts 0 every pass */
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference depth_ref = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
        .pDepthStencilAttachment = &depth_ref,
    };
    VkSubpassDependency deps[RP_DEPS];
    render_pass_deps(deps);
    VkAttachmentDescription atts[2] = { color, depth };
    VkRenderPassCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = atts,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = RP_DEPS,
        .pDependencies = deps,
    };
    VkResult r = vkCreateRenderPass(dev->device, &rci, NULL, &dev->off_rp);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateRenderPass failed: %d\n", (int)r); return false; }

    /* LOAD variant (SCENE postfx point): identical attachments so it
     * shares off_fb, but the color attachment LOADs its existing contents
     * (initialLayout COLOR_ATTACHMENT - the caller barriers off_image
     * TRANSFER_SRC -> COLOR before begin) instead of clearing, so HUD draws
     * land ON TOP of a scene that a SCENE-point chain already post-
     * processed. Depth still CLEARs (HUD self-clears depth anyway; the
     * scene pass discarded it). The dependencies are off_rp's, which
     * already order the colour attachment read a LOAD performs. */
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription atts_load[2] = { color, depth };
    rci.pAttachments = atts_load;
    r = vkCreateRenderPass(dev->device, &rci, NULL, &dev->off_load_rp);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateRenderPass (load) failed: %d\n", (int)r); return false; }
    return true;
}

/** @brief Create a render-to-texture render pass with a given colour format: dev->rt_rp (RHI_RT_FORMAT) or dev->rt_u32_rp.
 *
 * With RHI_RT_FORMAT the attachment formats match off_rp (RHI_RT_FORMAT
 * color, depth_format depth-stencil), so pipelines built against off_rp are
 * compatible with it. The color attachment ends in SHADER_READ_ONLY_OPTIMAL,
 * and the outgoing dependency (render_pass_deps) makes its writes visible to
 * fragment-shader reads, so a later pass in the same frame can sample the
 * image. Depth and stencil clear on load and are not stored.
 * @param dev Device whose depth format is read.
 * @param color_fmt The colour attachment's format.
 * @param out Receives the render pass.
 * @return true on success, false when vkCreateRenderPass fails. */
static bool create_rt_render_pass_fmt(rhi_device *dev, VkFormat color_fmt, VkRenderPass *out)
{
    VkAttachmentDescription color = {
        .format = color_fmt,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkAttachmentDescription depth = {
        .format = dev->depth_format,      /* combined depth+stencil */
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,   /* stencil starts 0 every pass */
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref,
        .pDepthStencilAttachment = &depth_ref,
    };
    VkSubpassDependency deps[RP_DEPS];
    render_pass_deps(deps);
    VkAttachmentDescription atts[2] = { color, depth };
    VkRenderPassCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = atts,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = RP_DEPS, .pDependencies = deps,
    };
    VkResult r = vkCreateRenderPass(dev->device, &rci, NULL, out);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateRenderPass(rt) failed: %d\n", (int)r); return false; }
    return true;
}

/** @brief Create dev->rt_ds_rp, the render pass for depth-sampled render targets.
 *
 * Used by the shadow-decal depth pre-pass. The attachment formats match rt_rp
 * (pipeline-compatible), but the depth attachment is stored and finishes in
 * SHADER_READ_ONLY_OPTIMAL so a later pass in the same frame can sample the
 * per-pixel depth. The incoming dependency (render_pass_deps) also waits on
 * fragment-shader reads of the previous use (write-after-read, since the
 * image is re-rendered every present); the outgoing one makes color and depth
 * writes visible to fragment-shader reads.
 * @param dev Device whose formats are read and whose rt_ds_rp is written.
 * @return true on success, false when vkCreateRenderPass fails. */
static bool create_rt_ds_render_pass(rhi_device *dev)
{
    VkAttachmentDescription color = {
        .format = RHI_RT_FORMAT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkAttachmentDescription depth = {
        .format = dev->depth_format,      /* combined depth+stencil */
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,        /* SAMPLED after the pass */
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkAttachmentReference ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref,
        .pDepthStencilAttachment = &depth_ref,
    };
    VkSubpassDependency deps[RP_DEPS];
    render_pass_deps(deps);
    VkAttachmentDescription atts[2] = { color, depth };
    VkRenderPassCreateInfo rci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = atts,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = RP_DEPS, .pDependencies = deps,
    };
    VkResult r = vkCreateRenderPass(dev->device, &rci, NULL, &dev->rt_ds_rp);
    if (r != VK_SUCCESS) { RHI_LOG("vkCreateRenderPass(rt-ds) failed: %d\n", (int)r); return false; }
    return true;
}

/** @brief Create the offscreen color image (RHI_RT_FORMAT), its depth-stencil image, their views and the framebuffer at the current extent.
 *
 * @param dev Device whose extent, formats and off_rp are read.
 * @return false when any step fails. */
static bool create_offscreen(rhi_device *dev)
{
    /* The offscreen target has off_extent. The 3D scene composites here and
     * rhi_blit_to_swapchain presents it, 1:1 when off_extent is the swapchain
     * extent. SSAA renders the scene into
     * a larger render target (renderer-owned) and resolves it into this. */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = RHI_RT_FORMAT,
        .extent = { dev->off_extent.width, dev->off_extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        /* SAMPLED: the COMPOSITE postfx point reads the composed frame
         * (rhi_offscreen_to_sampled); TRANSFER_DST: the chain's final
         * ping blits back in (rhi_blit_rt_to_offscreen). */
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, NULL, &dev->off_image) != VK_SUCCESS) {
        RHI_LOG("offscreen vkCreateImage failed\n");
        return false;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev->device, dev->off_image, &mr);
    uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { RHI_LOG("no device-local memory type\n"); return false; }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(dev->device, &mai, NULL, &dev->off_mem) != VK_SUCCESS) {
        RHI_LOG("offscreen vkAllocateMemory failed\n");
        return false;
    }
    vkBindImageMemory(dev->device, dev->off_image, dev->off_mem, 0);

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = dev->off_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = RHI_RT_FORMAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    if (vkCreateImageView(dev->device, &vci, NULL, &dev->off_view) != VK_SUCCESS) {
        RHI_LOG("offscreen vkCreateImageView failed\n");
        return false;
    }
    /* Depth attachment: combined depth+stencil, device-local. */
    VkImageCreateInfo dici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = dev->depth_format,
        .extent = { dev->off_extent.width, dev->off_extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &dici, NULL, &dev->off_depth_image) != VK_SUCCESS) {
        RHI_LOG("offscreen depth vkCreateImage failed\n");
        return false;
    }
    VkMemoryRequirements dmr;
    vkGetImageMemoryRequirements(dev->device, dev->off_depth_image, &dmr);
    uint32_t dmt = find_memtype(dev->phys, dmr.memoryTypeBits,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dmt == UINT32_MAX) { RHI_LOG("no device-local memory type (depth)\n"); return false; }
    VkMemoryAllocateInfo dmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = dmr.size,
        .memoryTypeIndex = dmt,
    };
    if (vkAllocateMemory(dev->device, &dmai, NULL, &dev->off_depth_mem) != VK_SUCCESS) {
        RHI_LOG("offscreen depth vkAllocateMemory failed\n");
        return false;
    }
    vkBindImageMemory(dev->device, dev->off_depth_image, dev->off_depth_mem, 0);
    VkImageViewCreateInfo dvci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = dev->off_depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = dev->depth_format,
        .subresourceRange = {
            /* attachment view of a combined format must cover both aspects */
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    if (vkCreateImageView(dev->device, &dvci, NULL, &dev->off_depth_view) != VK_SUCCESS) {
        RHI_LOG("offscreen depth vkCreateImageView failed\n");
        return false;
    }

    VkImageView fb_atts[2] = { dev->off_view, dev->off_depth_view };
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = dev->off_rp,
        .attachmentCount = 2,
        .pAttachments = fb_atts,
        .width = dev->off_extent.width,
        .height = dev->off_extent.height,
        .layers = 1,
    };
    if (vkCreateFramebuffer(dev->device, &fci, NULL, &dev->off_fb) != VK_SUCCESS) {
        RHI_LOG("offscreen vkCreateFramebuffer failed\n");
        return false;
    }
    /* New image/view => the per-slot off_sampled_set descriptors are stale;
     * rhi_bind_offscreen_tex rewrites a slot's set when its gen lags. */
    dev->off_gen++;
    return true;
}

/** @brief Create the command pool, the per-slot command buffers, semaphores and fences, and the staged-present set.
 *
 * The pool allows per-buffer reset. Per frame slot (RHI_FRAMES_IN_FLIGHT) it
 * makes a command buffer, an image-available semaphore and an in-flight fence
 * created signaled. Per swapchain image (dev->sc_count, so it runs after the
 * swapchain exists; zero when headless) it makes a render-done semaphore. Per
 * stage slot (RHI_STAGE_RING) it makes the present step's command buffer,
 * acquire semaphore and fence (signaled). A failed semaphore or fence
 * creation is only logged, leaving a null handle.
 * @param dev Device whose gfx_family and sc_count are read and whose pool,
 *            cmd, img_avail, in_flight, render_done, pcmd, pimg_avail and
 *            pfence are written.
 * @return false when the pool or a command-buffer allocation fails; true
 *         otherwise. */
static bool create_sync_and_commands(rhi_device *dev)
{
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = dev->gfx_family,
    };
    if (vkCreateCommandPool(dev->device, &pci, NULL, &dev->pool) != VK_SUCCESS) {
        RHI_LOG("vkCreateCommandPool failed\n");
        return false;
    }
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = RHI_FRAMES_IN_FLIGHT,
    };
    if (vkAllocateCommandBuffers(dev->device, &cbi, dev->cmd) != VK_SUCCESS) {
        RHI_LOG("vkAllocateCommandBuffers failed\n");
        return false;
    }
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    RHI_LOG("frames in flight = %d\n", RHI_FRAMES_IN_FLIGHT);
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->img_avail[i]));
        VK_CHECK(vkCreateFence(dev->device, &fence, NULL, &dev->in_flight[i]));
    }
    for (uint32_t i = 0; i < dev->sc_count; i++) {
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->render_done[i]));
    }
    /* The staged present step's own command buffers and sync. */
    VkCommandBufferAllocateInfo pcbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = RHI_STAGE_RING,
    };
    if (vkAllocateCommandBuffers(dev->device, &pcbi, dev->pcmd) != VK_SUCCESS) {
        RHI_LOG("vkAllocateCommandBuffers (present step) failed\n");
        return false;
    }
    for (int i = 0; i < RHI_STAGE_RING; i++) {
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->pimg_avail[i]));
        VK_CHECK(vkCreateFence(dev->device, &fence, NULL, &dev->pfence[i]));
    }
    return true;
}

/** @brief Create the per-slot timestamp query pools for GPU pass zones when OPENPETE_GPU_TIMERS or OPENPETE_BUGREPORT_GPU is set and supported.
 *
 * With both OPENPETE_GPU_TIMERS and OPENPETE_BUGREPORT_GPU unset or 0, or with a graphics queue reporting no
 * timestamp valid bits or a zero timestamp period, dev->gpu_timers stays false
 * and every zone call returns at its flag check; the unsupported case logs
 * one line. Otherwise one pool of RHI_GPU_ZONES_MAX * 2 queries is created per
 * frame in flight and dev->gpu_ts_period is recorded.
 * @param dev Device whose gpu_qp, gpu_ts_period and gpu_timers are written.
 * @return true in every case; a failed query-pool creation is only logged by
 *         VK_CHECK. */
static bool create_gpu_timers(rhi_device *dev)
{
    const char *e = rhi_knob("OPENPETE_GPU_TIMERS");
    const char *b = rhi_knob("OPENPETE_BUGREPORT_GPU");   /* the bug report's GPU_FRAME records */
    if (!(e && *e && *e != '0') && !(b && *b && *b != '0')) return true;   /* off: zero cost */

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev->phys, &props);
    uint32_t qn = 0, valid_bits = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->phys, &qn, NULL);
    if (qn > 0) {
        VkQueueFamilyProperties *q = calloc(qn, sizeof(*q));
        if (!q) return true;
        vkGetPhysicalDeviceQueueFamilyProperties(dev->phys, &qn, q);
        if (dev->gfx_family < qn) valid_bits = q[dev->gfx_family].timestampValidBits;
        free(q);
    }
    if (valid_bits == 0 || props.limits.timestampPeriod <= 0.0f) {
        RHI_LOG("OPENPETE_GPU_TIMERS: no timestamp support on the graphics queue - disabled\n");
        return true;
    }
    dev->gpu_ts_period = props.limits.timestampPeriod;

    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = RHI_GPU_ZONES_MAX * 2,
    };
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++)
        VK_CHECK(vkCreateQueryPool(dev->device, &qi, NULL, &dev->gpu_qp[i]));
    dev->gpu_timers = true;
    RHI_LOG("GPU pass timers on (%.3f ns/tick)\n", (double)dev->gpu_ts_period);
    return true;
}

/** @brief Read the finished timestamp pairs of the current slot into dev->gpu_results.
 *
 * Called by begin_frame_impl after the slot's in_flight fence has signalled,
 * so the queries are complete and the read does not block. Each zone's
 * duration and its start relative to the slot's earliest begin timestamp are
 * converted to microseconds with dev->gpu_ts_period. The slot's zone count
 * is reset; a failed query read leaves gpu_results_n at 0.
 * @param dev Device whose current slot is harvested. */
static void gpu_timers_harvest(rhi_device *dev)
{
    int slot = (int)dev->frame;
    int n = dev->gpu_zone_n[slot];
    dev->gpu_results_n = 0;
    dev->gpu_zone_n[slot] = 0;
    if (n <= 0) return;
    uint64_t ts[RHI_GPU_ZONES_MAX * 2];
    VkResult r = vkGetQueryPoolResults(dev->device, dev->gpu_qp[slot], 0,
                                       (uint32_t)(n * 2),
                                       sizeof(uint64_t) * (size_t)(n * 2), ts,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return;    /* shouldn't happen post-fence; drop the frame */
    /* Frame anchor = earliest begin timestamp, so start_us places every zone
     * on one per-present timeline the trace can lay out. */
    uint64_t t_min = ts[0];
    for (int i = 0; i < n; i++) if (ts[i * 2] < t_min) t_min = ts[i * 2];
    for (int i = 0; i < n; i++) {
        dev->gpu_results[i].name  = dev->gpu_zone_name[slot][i];
        dev->gpu_results[i].depth = dev->gpu_zone_depth[slot][i];
        dev->gpu_results[i].us =
            (double)(ts[i * 2 + 1] - ts[i * 2]) * (double)dev->gpu_ts_period / 1000.0;
        dev->gpu_results[i].start_us =
            (double)(ts[i * 2] - t_min) * (double)dev->gpu_ts_period / 1000.0;
    }
    dev->gpu_results_n = n;
    dev->gpu_results_token = dev->gpu_zone_token[slot];
}

int rhi_gpu_timers_on(const rhi_device *dev)
{
    return dev && dev->gpu_timers;
}

/* s_last_gpu_zone (declared beside rhi_signal_device_lost, which prints
 * it): the device-lost suspect - populated only when timers are on. */
void rhi_gpu_zone_begin(rhi_device *dev, const char *name)
{
    if (!dev || !dev->gpu_timers || !dev->frame_active) return;
    int slot = (int)dev->frame;
    if (dev->gpu_zone_n[slot] >= RHI_GPU_ZONES_MAX ||
        dev->gpu_zone_sp >= RHI_GPU_ZONES_MAX) return;
    int idx = dev->gpu_zone_n[slot]++;          /* reserve the query pair now  */
    s_last_gpu_zone = name;
    dev->gpu_zone_name[slot][idx]  = name;
    dev->gpu_zone_depth[slot][idx] = (uint8_t)dev->gpu_zone_sp;
    dev->gpu_zone_stack[dev->gpu_zone_sp++] = idx;
    vkCmdWriteTimestamp(dev->cmd[slot], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        dev->gpu_qp[slot], (uint32_t)(idx * 2));
}

void rhi_gpu_zone_end(rhi_device *dev)
{
    if (!dev || !dev->gpu_timers || !dev->frame_active || dev->gpu_zone_sp <= 0) return;
    int slot = (int)dev->frame;
    int idx = dev->gpu_zone_stack[--dev->gpu_zone_sp];
    vkCmdWriteTimestamp(dev->cmd[slot], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        dev->gpu_qp[slot], (uint32_t)(idx * 2 + 1));
}

int rhi_gpu_zone_results(rhi_device *dev, rhi_gpu_zone_result *out, int cap)
{
    if (!dev || !out || cap <= 0) return 0;
    int n = dev->gpu_results_n < cap ? dev->gpu_results_n : cap;
    memcpy(out, dev->gpu_results, (size_t)n * sizeof(*out));
    return n;
}

void rhi_gpu_frame_token(rhi_device *dev, uint64_t token)
{
    if (!dev || !dev->gpu_timers || !dev->frame_active) return;
    dev->gpu_zone_token[(int)dev->frame] = token;
}

uint64_t rhi_gpu_results_token(const rhi_device *dev)
{
    return dev ? dev->gpu_results_token : 0;
}

/** @brief Create one more descriptor pool of the device's fixed per-pool sizes.
 *
 * @param dev Device whose pool list grows.
 * @return false when the list is full or vkCreateDescriptorPool fails; both are logged. */
static bool desc_pool_add(rhi_device *dev)
{
    if (dev->desc_pool_n >= RHI_DESC_POOLS_MAX) {
        RHI_LOG("descriptor pools: all %d pools (%d sets each) are full; the "
                "allocation is refused\n", RHI_DESC_POOLS_MAX, RHI_DESC_POOL_SETS);
        return false;
    }
    VkDescriptorPoolSize ps[4] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 4096 },
        /* off_image sampled sets (per frame slot) + rhi_ubo pairs. */
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 64 },
        /* binding 2 of every rhi_ubo set and rhi_ubo_data set */
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 64 },
        /* the single binding of every rhi_create_ssbo, rhi_storage and
         * rhi_gpu_buffer set, and every binding of a compute set */
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .descriptorCount = 64 },
    };
    VkDescriptorPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = RHI_DESC_POOL_SETS, .poolSizeCount = 4, .pPoolSizes = ps,
    };
    VkDescriptorPool pool;
    if (vkCreateDescriptorPool(dev->device, &pi, NULL, &pool) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorPool failed\n");
        return false;
    }
    dev->desc_pools[dev->desc_pool_n++] = pool;
    if (dev->desc_pool_n > 1) RHI_LOG("descriptor pools: %d in use\n", dev->desc_pool_n);
    return true;
}

/** @brief Allocate one descriptor set of `layout`, adding a pool when every existing pool is out of memory.
 *
 * @param dev Device.
 * @param layout Set layout.
 * @param out Receives the set.
 * @param out_pool Receives the pool the set came from, which
 *                 vkFreeDescriptorSets needs; NULL when the set is never freed
 *                 individually.
 * @return true on success; a failure is logged. */
static bool desc_alloc(rhi_device *dev, VkDescriptorSetLayout layout,
                       VkDescriptorSet *out, VkDescriptorPool *out_pool)
{
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorSetCount = 1, .pSetLayouts = &layout,
    };
    for (int attempt = 0; attempt < 2; attempt++) {
        for (int i = dev->desc_pool_n - 1; i >= 0; i--) {
            ai.descriptorPool = dev->desc_pools[i];
            VkResult r = vkAllocateDescriptorSets(dev->device, &ai, out);
            if (r == VK_SUCCESS) { if (out_pool) *out_pool = dev->desc_pools[i]; return true; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
                RHI_LOG("vkAllocateDescriptorSets failed: %d\n", (int)r);
                return false;
            }
        }
        if (attempt == 0 && !desc_pool_add(dev)) break;
    }
    *out = VK_NULL_HANDLE;
    return false;
}

/** @brief Create the samplers, descriptor set layouts and descriptor pool every texture and UBO binding draws from.
 *
 * The VRAM sampler is NEAREST with clamp-to-edge addressing: the PSX sampler
 * shader does its own texelFetch indexing of the R16_UINT VRAM image, so no
 * GPU filtering applies. linear_sampler is the same with LINEAR filtering,
 * for high-res replacement art. Layouts: vram_set_layout (one
 * combined-image-sampler at binding 0, shared by every VRAM-sampling
 * pipeline), pair_set_layout (two at bindings 0 and 1), trio_set_layout
 * (three at bindings 0 to 2), quad_set_layout (four at bindings 0 to 3), ubo_set_layout (two
 * dynamic UBOs, vertex and fragment visible, and a fragment-visible storage
 * buffer at binding 2) and ubo1_set_layout (one dynamic UBO). Records ubo_align (minUniformBufferOffsetAlignment, 256 when
 * the device reports 0) and max_desc_sets. The pool holds 4096
 * combined-image-samplers, 64 dynamic UBOs and 64 storage buffers in at
 * most 4160 sets, with
 * FREE_DESCRIPTOR_SET, because every rhi_texture, including each texture-pack
 * replacement, holds one set for its lifetime.
 * @param dev Device whose samplers, layouts, limits and first descriptor pool are written.
 * @return true on success, false on the first failed creation. */
static bool create_descriptor_infra(rhi_device *dev)
{
    VkSamplerCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    if (vkCreateSampler(dev->device, &si, NULL, &dev->vram_sampler) != VK_SUCCESS) {
        RHI_LOG("vkCreateSampler failed\n"); return false;
    }
    /* LINEAR variant for high-res replacement art (rhi_texture_set_filter_linear)
     * - same addressing, filtered mag/min. */
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(dev->device, &si, NULL, &dev->linear_sampler) != VK_SUCCESS) {
        RHI_LOG("vkCreateSampler (linear) failed\n"); return false;
    }
    VkDescriptorSetLayoutBinding b = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &li, NULL, &dev->vram_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout failed\n"); return false;
    }
    VkDescriptorSetLayoutBinding pb[2] = { b, b };
    pb[1].binding = 1;
    VkDescriptorSetLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = pb,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &pli, NULL, &dev->pair_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (pair) failed\n"); return false;
    }
    VkDescriptorSetLayoutBinding tb[3] = { b, b, b };
    tb[1].binding = 1;
    tb[2].binding = 2;
    VkDescriptorSetLayoutCreateInfo tli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = tb,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &tli, NULL, &dev->trio_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (trio) failed\n"); return false;
    }
    VkDescriptorSetLayoutBinding qb[4] = { b, b, b, b };
    qb[1].binding = 1;
    qb[2].binding = 2;
    qb[3].binding = 3;
    VkDescriptorSetLayoutCreateInfo qli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = qb,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &qli, NULL, &dev->quad_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (quad) failed\n"); return false;
    }
    /* The contract set (set 3): binding 0 = engine block, binding 1 = mod
     * block, both dynamic UBOs visible to vertex and fragment; binding 2 =
     * the read-only table block a material fragment's moved constant tables
     * live in (rhi_ubo_data; mod_spv_cdata.h), fragment visible. */
    VkDescriptorSetLayoutBinding ub[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo uli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = ub,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &uli, NULL, &dev->ubo_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (ubo) failed\n"); return false;
    }
    /* Single dynamic-UBO binding, for engine blocks bound at a set index of
     * their own (rhi_ubo_bind_at) rather than inside the contract set. */
    VkDescriptorSetLayoutCreateInfo u1li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = ub,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &u1li, NULL,
                                    &dev->ubo1_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (ubo1) failed\n"); return false;
    }
    /* Single dynamic storage-buffer binding, vertex and fragment visible:
     * engine tables too large for a uniform block (rhi_create_ssbo),
     * retained vertex-stage data (rhi_storage) and compute output read by a
     * draw (rhi_gpu_buffer). */
    VkDescriptorSetLayoutBinding sb = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo s1li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &sb,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &s1li, NULL,
                                    &dev->ssbo1_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (ssbo1) failed\n"); return false;
    }
    /* A sampled image at binding 0 (the single-sampler binding a fragment
     * stage declares) beside a vertex-visible dynamic storage buffer at
     * binding 1 (rhi_bind_texture_storage). */
    VkDescriptorSetLayoutBinding tsb[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo tsli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = tsb,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &tsli, NULL,
                                    &dev->texssbo_set_layout) != VK_SUCCESS) {
        RHI_LOG("vkCreateDescriptorSetLayout (texssbo) failed\n"); return false;
    }
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev->phys, &props);
        dev->ubo_align = props.limits.minUniformBufferOffsetAlignment;
        if (dev->ubo_align == 0) dev->ubo_align = 256;
        dev->ssbo_align = props.limits.minStorageBufferOffsetAlignment;
        if (dev->ssbo_align == 0) dev->ssbo_align = 256;
        dev->max_desc_sets = props.limits.maxBoundDescriptorSets;
    }

    /* Every rhi_texture (the VRAM display texture, RTT targets, and every HD
     * texture-pack replacement) holds one COMBINED_IMAGE_SAMPLER set for its
     * lifetime. A whole-level HD pack loads one texture per unique replacement
     * image as surfaces come into view, hundreds for a large level. The first
     * pool is sized for a large per-level pack; desc_alloc adds pools past
     * it. */
    return desc_pool_add(dev);
}

/** @brief Allocate and begin a one-time-submit command buffer for a one-off transfer.
 *
 * Used for VRAM uploads, readbacks and initial image transitions. The paired
 * one_shot_end() submits it, waits for the queue to go idle and frees it; the
 * stall is accepted because these operations are occasional.
 * @param dev Device whose command pool supplies the buffer.
 * @return The recording command buffer, or VK_NULL_HANDLE when allocation
 *         fails. */
static VkCommandBuffer one_shot_begin(rhi_device *dev)
{
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev->device, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

/** @brief Submit one batch to the graphics queue; a failure is handled as device loss.
 *
 * Any failed result (an injected one included) is logged with @p where and
 * routed to rhi_signal_device_lost.
 * @param dev Device whose graphics queue receives the batch.
 * @param si The batch.
 * @param fence Fence the batch signals, or VK_NULL_HANDLE.
 * @param where Name of the submitting call, for the log.
 * @return true when the batch was submitted. */
static bool queue_submit(rhi_device *dev, const VkSubmitInfo *si, VkFence fence,
                         const char *where)
{
    const VkResult r = (fence != VK_NULL_HANDLE && fault_submit_due())
                           ? VK_ERROR_DEVICE_LOST
                           : vkQueueSubmit(dev->gfx_queue, 1, si, fence);
    if (r == VK_SUCCESS) return true;
    RHI_LOG("vkQueueSubmit (%s) failed: VkResult=%d\n", where, (int)r);
    rhi_signal_device_lost(where);
    return false;
}

/** @brief End, submit and drain a one-shot command buffer, then free it.
 *
 * Blocks in vkQueueWaitIdle; a failed submit or a lost device during the
 * drain reaches the device-lost handler.
 * @param dev Device whose graphics queue runs the buffer.
 * @param cb Command buffer from one_shot_begin. */
static void one_shot_end(rhi_device *dev, VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
    };
    if (queue_submit(dev, &si, VK_NULL_HANDLE, "one-shot"))
        fence_result(queue_wait_idle(dev->gfx_queue), "one-shot wait");
    vkFreeCommandBuffers(dev->device, dev->pool, 1, &cb);
}

/** @brief Destroy the stage images and their memory and invalidate stage-image users.
 *
 * Resets each stage layout to UNDEFINED and increments dev->stage_gen, which
 * rhi_stage_gen returns and a held staged-present acquire (pacquired_gen)
 * compares to learn the ring was rebuilt. Slots already null are skipped. The
 * caller guarantees no submitted work uses the images.
 * @param dev Device owning the stage ring. */
static void destroy_stages(rhi_device *dev)
{
    for (int i = 0; i < RHI_STAGE_RING; i++) {
        if (dev->stage_image[i]) { vkDestroyImage(dev->device, dev->stage_image[i], NULL); dev->stage_image[i] = VK_NULL_HANDLE; }
        if (dev->stage_mem[i])   { vkFreeMemory(dev->device, dev->stage_mem[i], NULL); dev->stage_mem[i] = VK_NULL_HANDLE; }
        dev->stage_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    dev->stage_gen++;
}

/** @brief Create the RHI_STAGE_RING stage images (RHI_RT_FORMAT, the swapchain extent) that staged presents blit through.
 *
 * @param dev Device whose extent is read.
 * @return false when any image or allocation fails. */
static bool create_stages(rhi_device *dev)
{
    for (int i = 0; i < RHI_STAGE_RING; i++) {
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = RHI_RT_FORMAT,
            .extent = { dev->off_extent.width, dev->off_extent.height, 1 },
            .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(dev->device, &ici, NULL, &dev->stage_image[i]) != VK_SUCCESS) {
            RHI_LOG("stage vkCreateImage failed\n"); return false;
        }
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev->device, dev->stage_image[i], &mr);
        uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) { RHI_LOG("no device-local memory type\n"); return false; }
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = mt,
        };
        if (vkAllocateMemory(dev->device, &mai, NULL, &dev->stage_mem[i]) != VK_SUCCESS) {
            RHI_LOG("stage vkAllocateMemory failed\n"); return false;
        }
        vkBindImageMemory(dev->device, dev->stage_image[i], dev->stage_mem[i], 0);
        dev->stage_layout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return true;
}

/** @brief Destroy the objects sized to the swapchain: stages, the offscreen color and depth targets and framebuffer, and render-done semaphores.
 *
 * The swapchain itself stays (swapchain_destroy), so recreate can hand it to
 * the new one as oldSwapchain. Each handle is nulled after destruction, so a
 * repeat call (after a partial failure in recreate) is harmless. The caller
 * guarantees the device is idle.
 * @param dev Device whose swapchain-sized objects are destroyed. */
static void destroy_swapchain_resources(rhi_device *dev)
{
    destroy_stages(dev);
    if (dev->off_fb)    { vkDestroyFramebuffer(dev->device, dev->off_fb, NULL); dev->off_fb = VK_NULL_HANDLE; }
    if (dev->off_view)  { vkDestroyImageView(dev->device, dev->off_view, NULL); dev->off_view = VK_NULL_HANDLE; }
    if (dev->off_image) { vkDestroyImage(dev->device, dev->off_image, NULL); dev->off_image = VK_NULL_HANDLE; }
    if (dev->off_mem)   { vkFreeMemory(dev->device, dev->off_mem, NULL); dev->off_mem = VK_NULL_HANDLE; }
    if (dev->off_depth_view)  { vkDestroyImageView(dev->device, dev->off_depth_view, NULL); dev->off_depth_view = VK_NULL_HANDLE; }
    if (dev->off_depth_image) { vkDestroyImage(dev->device, dev->off_depth_image, NULL); dev->off_depth_image = VK_NULL_HANDLE; }
    if (dev->off_depth_mem)   { vkFreeMemory(dev->device, dev->off_depth_mem, NULL); dev->off_depth_mem = VK_NULL_HANDLE; }
    for (uint32_t i = 0; i < RHI_MAX_SC_IMAGES; i++) {
        if (dev->render_done[i]) {
            vkDestroySemaphore(dev->device, dev->render_done[i], NULL);
            dev->render_done[i] = VK_NULL_HANDLE;
        }
    }
}

/** @brief Destroy the swapchain and null its handle.
 *
 * The sc_images array is not cleared; those images belong to the swapchain.
 * @param dev Device whose swapchain is destroyed; the device is idle. */
static void swapchain_destroy(rhi_device *dev)
{
    if (dev->swapchain) vkDestroySwapchainKHR(dev->device, dev->swapchain, NULL);
    dev->swapchain = VK_NULL_HANDLE;
}

/* Present-extent trace (OPENPETE_PRESENT_EXTENT_TRACE); see
 * rhi_present_trace_arm. The arm instant is 0 until the first arm. */
#define RHI_PXT_BUDGET 64
static int s_pxt_on = -1;
static _Atomic uint64_t s_pxt_t0;
static _Atomic int s_pxt_left;         /* present and skip lines left in this window */
static _Atomic uint64_t s_pxt_size_ns; /* last pixel-size event not yet followed by a matching present */
static _Atomic unsigned s_pxt_mismatch;
static _Atomic unsigned s_pxt_presents;
static uint64_t s_pxt_game0_ns;        /* first game present after the arm; presenting thread only */
static int s_pxt_content;              /* 0 none seen, 1 any non-black pixel seen, 2 >= 1% seen */
static long pxt_nonblack(rhi_device *dev, VkImage img, int thresh);

/** @brief Return whether OPENPETE_PRESENT_EXTENT_TRACE is set to a value not starting with 0 (read once). */
static bool pxt_enabled(void)
{
    if (s_pxt_on < 0) {
        const char *e = getenv("OPENPETE_PRESENT_EXTENT_TRACE");
        s_pxt_on = (e && *e && *e != '0');
    }
    return s_pxt_on > 0;
}

/** @brief Return milliseconds from the trace arm to @p now.
 *
 * @param now ns on the SDL tick clock.
 * @return The interval in ms. */
static double pxt_ms(uint64_t now)
{
    return (double)(now - atomic_load(&s_pxt_t0)) / 1e6;
}

/** @brief SDL event watch that logs window pixel-size and fullscreen events and stamps the pixel-size change.
 *
 * Runs on the thread that raises the event (on Win32 inside the call that
 * resized the window).
 * @param ud Unused.
 * @param ev The event.
 * @return true, so the event is queued as usual. */
static bool SDLCALL pxt_event_watch(void *ud, SDL_Event *ev)
{
    (void)ud;
    const char *what = ev->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ? "pixel-size"
                     : ev->type == SDL_EVENT_WINDOW_RESIZED ? "resized"
                     : ev->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ? "enter-fullscreen"
                     : ev->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN ? "leave-fullscreen" : NULL;
    if (!what) return true;
    const uint64_t now = SDL_GetTicksNS(); /* clock-guard: wall: trace timestamp */
    if (ev->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) atomic_store(&s_pxt_size_ns, now);
    RHI_LOG("pxt %9.2f ms event %s %dx%d\n", pxt_ms(now), what,
            (int)ev->window.data1, (int)ev->window.data2);
    return true;
}

void rhi_present_trace_arm(rhi_device *dev, const char *why)
{
    if (!dev || dev->headless || !pxt_enabled()) return;
    static bool watched;
    if (!watched) { SDL_AddEventWatch(pxt_event_watch, NULL); watched = true; }
    const uint64_t now = SDL_GetTicksNS(); /* clock-guard: wall: trace timestamp */
    atomic_store(&s_pxt_t0, now);
    atomic_store(&s_pxt_left, RHI_PXT_BUDGET);
    atomic_store(&s_pxt_mismatch, 0u);
    atomic_store(&s_pxt_presents, 0u);
    s_pxt_game0_ns = 0;
    s_pxt_content = 0;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(dev->window, &w, &h);
    RHI_LOG("pxt %9.2f ms arm (%s): window %dx%d extent %ux%u\n", 0.0, why ? why : "",
            w, h, dev->sc_extent.width, dev->sc_extent.height);
}

/* Launcher hand-off trace (OPENPETE_LAUNCHER_TRACE): every present from the
 * device's first through the game's first, each read back. Presents run on
 * the display owner, one thread at a time; the sim thread reads the counts
 * (rhi_launcher_trace_pending). */
static atomic_int s_lxt_on = -1;
static atomic_bool s_lxt_done;   /* the game's first present was logged */
static atomic_uint s_lxt_n;      /* presents logged */

/** @brief Return whether OPENPETE_LAUNCHER_TRACE is set to a value not starting with 0 (read once). */
static bool lxt_enabled(void)
{
    int on = atomic_load(&s_lxt_on);
    if (on < 0) {
        const char *e = getenv("OPENPETE_LAUNCHER_TRACE");
        on = (e && *e && *e != '0');
        atomic_store(&s_lxt_on, on);
    }
    return on > 0;
}

bool rhi_launcher_trace_pending(void)
{
    return lxt_enabled() && atomic_load(&s_lxt_n) > 0 && !atomic_load(&s_lxt_done);
}

/** @brief Log one present of the launcher hand-off trace with its window and swapchain size and its count of pixels that are not (0,0,0), until the game's first present.
 *
 * A launcher present (rhi_boot_launcher_frames set) and the game's first
 * present are read back from the image they presented; a stage-release
 * present, which draws nothing, is logged without a count (-1). The game's
 * first present ends the trace.
 * @param dev Device that presented.
 * @param src Source label; NULL names it from the launcher-frame flag.
 * @param img The image whose content was presented (TRANSFER_SRC), or VK_NULL_HANDLE. */
static void lxt_present(rhi_device *dev, const char *src, VkImage img)
{
    if (atomic_load(&s_lxt_done) || dev->headless || !lxt_enabled()) return;
    if (!src) src = atomic_load(&s_launcher_frames) ? "launcher" : "game";
    const long nz = img != VK_NULL_HANDLE ? pxt_nonblack(dev, img, 0) : -1;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(dev->window, &w, &h);
    const bool game = !strcmp(src, "game");
    RHI_LOG("lxt present #%u %s window %dx%d extent %ux%u nonzero %ld%s\n", atomic_load(&s_lxt_n) + 1u,
            src, w, h, dev->sc_extent.width, dev->sc_extent.height, nz,
            game ? " (the game's first present)" : "");
    if (game) atomic_store(&s_lxt_done, true);
    atomic_fetch_add(&s_lxt_n, 1u);
}

/** @brief Log one present against the window's pixel size inside an armed trace window, with its content until the game's first picture.
 *
 * A game present is read back (the image it presented, waited idle) while
 * no present since the arm held non-black content, within 30 s of the arm;
 * the first present with a non-black pixel and the first with at least 1%
 * non-black pixels each log their time after the arm and after the first
 * game present. Every present the driver queued (@p pr VK_SUCCESS or
 * VK_SUBOPTIMAL_KHR) counts in sc_presents, armed or not.
 * @param dev Device that presented.
 * @param src Source label; NULL names it from the launcher-frame flag.
 * @param pr The vkQueuePresentKHR result.
 * @param img The image whose content was presented (TRANSFER_SRC), or VK_NULL_HANDLE. */
static void pxt_present(rhi_device *dev, const char *src, VkResult pr, VkImage img)
{
    dev->sc_presents += pr >= 0;   /* VK_SUCCESS or VK_SUBOPTIMAL_KHR: the image was queued */
    lxt_present(dev, src, img);
    if (s_pxt_on <= 0 || atomic_load(&s_pxt_t0) == 0) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(dev->window, &w, &h);
    const bool match = w == (int)dev->sc_extent.width && h == (int)dev->sc_extent.height;
    const uint64_t now = SDL_GetTicksNS(); /* clock-guard: wall: trace timestamp */
    if (!src) src = atomic_load(&s_launcher_frames) ? "launcher" : "game";
    if (!match) atomic_fetch_add(&s_pxt_mismatch, 1u);
    const unsigned n = atomic_fetch_add(&s_pxt_presents, 1u) + 1u;
    uint64_t sz = atomic_load(&s_pxt_size_ns);
    if (match && sz && atomic_compare_exchange_strong(&s_pxt_size_ns, &sz, 0))
        RHI_LOG("pxt %9.2f ms first present at the window size %dx%d (%s): %.2f ms after the "
                "pixel-size change; presents since arm %u, mismatched %u\n", pxt_ms(now), w, h,
                src, (double)(now - sz) / 1e6, n, atomic_load(&s_pxt_mismatch));
    long nb = -1;
    const bool game = !strcmp(src, "game");
    if (game && !s_pxt_game0_ns) s_pxt_game0_ns = now;
    if (game && img != VK_NULL_HANDLE && s_pxt_content < 2 && pxt_ms(now) < 30000.0) {
        nb = pxt_nonblack(dev, img, 8);
        const long total = (long)dev->off_extent.width * (long)dev->off_extent.height;
        const int level = nb >= total / 100 && nb > 0 ? 2 : nb > 0 ? 1 : 0;
        if (level > s_pxt_content) {
            for (int l = s_pxt_content + 1; l <= level; l++)
                RHI_LOG("pxt %9.2f ms content: first game present with %s non-black pixels "
                        "(#%u, %ld of %ld px), %.2f ms after the first game present\n",
                        pxt_ms(now), l == 1 ? "any" : ">= 1%", n, nb, total,
                        (double)(now - s_pxt_game0_ns) / 1e6);
            s_pxt_content = level;
        }
    }
    if (atomic_fetch_sub(&s_pxt_left, 1) <= 0) return;
    RHI_LOG("pxt %9.2f ms present #%u %s window %dx%d extent %ux%u %s result %d nonblack %ld\n",
            pxt_ms(now), n, src, w, h, dev->sc_extent.width, dev->sc_extent.height,
            match ? "match" : "MISMATCH", (int)pr, nb);
}

/** @brief Log one present that ensure_swapchain refused inside an armed trace window.
 *
 * @param dev Device whose present was refused.
 * @param why Reason label. */
static void pxt_skip(rhi_device *dev, const char *why)
{
    if (s_pxt_on <= 0 || atomic_load(&s_pxt_t0) == 0) return;
    if (atomic_fetch_sub(&s_pxt_left, 1) <= 0) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(dev->window, &w, &h);
    RHI_LOG("pxt %9.2f ms skip (%s) %s window %dx%d extent %ux%u\n",
            pxt_ms(SDL_GetTicksNS()), why, /* clock-guard: wall: trace timestamp */
            atomic_load(&s_launcher_frames) ? "launcher" : "game", w, h,
            dev->sc_extent.width, dev->sc_extent.height);
}

/** @brief Create the new swapchain with the current one as oldSwapchain, destroy the old one, and on VK_ERROR_NATIVE_WINDOW_IN_USE_KHR retry once on a new surface.
 *
 * The old swapchain is retired by the creation whether or not it succeeds,
 * so it is destroyed either way. A window still in use once no swapchain of
 * it is alive is held by the driver for a destroyed swapchain;
 * surface_rebuild releases it.
 * @param dev Windowed device, idle, whose swapchain-sized objects are destroyed.
 * @return true when a swapchain was created. */
static bool swapchain_replace(rhi_device *dev)
{
    const VkSwapchainKHR old = dev->swapchain;
    dev->swapchain = VK_NULL_HANDLE;
    VkResult r = create_swapchain(dev, old);
    if (old) vkDestroySwapchainKHR(dev->device, old, NULL);
    if (r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR && surface_rebuild(dev))
        r = create_swapchain(dev, VK_NULL_HANDLE);
    return r == VK_SUCCESS;
}

/** @brief Rebuild the swapchain and every object sized to it, restarting the acquire and present synchronization.
 *
 * Waits for the device to go idle, replaces
 * every acquire semaphore and slot fence (a binary semaphore left signaled
 * cannot be reused, and a fence left unsignaled would stall its next wait),
 * clears the staged-present acquire flag, destroys the swapchain-sized
 * resources, replaces the swapchain (swapchain_replace), and creates the
 * offscreen targets, stage images and render-done semaphores. A surface
 * left null by a failed surface rebuild is created first. dev->needs_recreate is cleared only on success.
 * A zero surface extent (a minimized window) refuses before anything is
 * waited or destroyed: the swapchain and every object sized to it stay at
 * the last extent, so a capture-mode frame keeps rendering into off_image
 * and reading back at that extent without a present while the window is
 * minimized.
 * @param dev Windowed device.
 * @return false when the surface extent is zero, or when the swapchain,
 *         offscreen targets or stage images cannot be created; the caller
 *         retries because needs_recreate stays set. */
static bool recreate(rhi_device *dev)
{
    if (!dev->surface && !surface_create(dev)) return false;   /* a failed surface rebuild */
    {
        VkSurfaceCapabilitiesKHR caps;
        VkExtent2D extent;
        if (!surface_extent(dev, &caps, &extent)) return false;
    }
    const VkExtent2D pxt_old = dev->sc_extent;
    const uint64_t pxt_t0 = SDL_GetTicksNS(); /* clock-guard: wall: trace timestamp */
    vkDeviceWaitIdle(dev->device);
    /* The acquire semaphores and slot fences start over too. A binary
     * acquire semaphore can be left ARMED with no waiter (an acquire whose
     * frame was refused or superseded before its submit), and Vulkan has no
     * way to un-signal one: handing it to vkAcquireNextImageKHR on the new
     * swapchain is invalid reuse of a signaled semaphore. Likewise a slot
     * fence can be left UNSIGNALED with nothing pending (reset after an
     * acquire whose submit never happened), which would park the next wait
     * on it for its full timeout. After the device-idle above nothing
     * references any of them, so recreating the lot restores the freshly
     * created state every acquire/wait path assumes. Any acquire still held
     * died with its swapchain. */
    {
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(dev->device, dev->img_avail[i], NULL);
            VK_CHECK(vkCreateSemaphore(dev->device, &si, NULL, &dev->img_avail[i]));
            vkDestroyFence(dev->device, dev->in_flight[i], NULL);
            VK_CHECK(vkCreateFence(dev->device, &fi, NULL, &dev->in_flight[i]));
        }
        for (int i = 0; i < RHI_STAGE_RING; i++) {
            vkDestroySemaphore(dev->device, dev->pimg_avail[i], NULL);
            VK_CHECK(vkCreateSemaphore(dev->device, &si, NULL, &dev->pimg_avail[i]));
            vkDestroyFence(dev->device, dev->pfence[i], NULL);
            VK_CHECK(vkCreateFence(dev->device, &fi, NULL, &dev->pfence[i]));
        }
        dev->pacquired = false;
    }
    destroy_swapchain_resources(dev);
    const uint64_t pxt_t1 = SDL_GetTicksNS(); /* clock-guard: wall: trace timestamp */
    if (!swapchain_replace(dev)) return false;   /* also nulls images_in_flight */
    const uint64_t pxt_t2 = SDL_GetTicksNS(); /* clock-guard: wall: trace timestamp */
    if (!create_offscreen(dev)) return false;
    if (!create_stages(dev)) return false;
    VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < dev->sc_count; i++)
        VK_CHECK(vkCreateSemaphore(dev->device, &sem, NULL, &dev->render_done[i]));
    dev->needs_recreate = false;
    dev->sc_recreates++;
    dev->sc_presents_at_recreate = dev->sc_presents;
    if (s_pxt_on > 0 && atomic_load(&s_pxt_t0)) {
        const uint64_t now = SDL_GetTicksNS(); /* clock-guard: wall: trace timestamp */
        RHI_LOG("pxt %9.2f ms recreate %ux%u -> %ux%u in %.2f ms (idle+teardown %.2f, "
                "vkCreateSwapchainKHR+images %.2f, targets %.2f)\n", pxt_ms(now),
                pxt_old.width, pxt_old.height, dev->sc_extent.width, dev->sc_extent.height,
                (double)(now - pxt_t0) / 1e6, (double)(pxt_t1 - pxt_t0) / 1e6,
                (double)(pxt_t2 - pxt_t1) / 1e6, (double)(now - pxt_t2) / 1e6);
    }
    return true;
}

/* ------------------------------------------------------------------ */

/** @brief Log the names of the Vulkan layers active on the instance, implicit ones included, on one line.
 *
 * The loader answers vkEnumerateDeviceLayerProperties with the layers it
 * activated on the instance: the implicit layers the system installs (an
 * overlay, a capture tool, a vendor's presentation layer) and any the
 * instance enabled.
 * @param dev Device whose instance and physical device are read. */
static void log_active_layers(rhi_device *dev)
{
    uint32_t n = 0;
    vkEnumerateDeviceLayerProperties(dev->phys, &n, NULL);
    VkLayerProperties *lp = n ? calloc(n, sizeof(*lp)) : NULL;
    if (!lp) n = 0;
    if (n) vkEnumerateDeviceLayerProperties(dev->phys, &n, lp);
    char line[1024] = "";
    size_t at = 0;
    for (uint32_t i = 0; i < n && at < sizeof line; i++)
        at += (size_t)snprintf(line + at, sizeof line - at, "%s%s", i ? ", " : "", lp[i].layerName);
    free(lp);
    RHI_LOG("vulkan layers active: %s\n", n ? line : "none");
}

/** @brief Log the device's memory table: one line per heap (size, flags) and per type (property flags, heap).
 *
 * Runs at every device creation. A harness that must name a heap reads it
 * here instead of inferring it from an allocation's type index.
 * @param dev Device whose physical device is queried. */
static void rhi_log_memory_types(rhi_device *dev)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        RHI_LOG("memory heap %u: %llu MiB flags %x\n", i,
                (unsigned long long)(mp.memoryHeaps[i].size >> 20),
                (unsigned)mp.memoryHeaps[i].flags);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        RHI_LOG("memory type %u: flags %x heap %u\n", i,
                (unsigned)mp.memoryTypes[i].propertyFlags,
                mp.memoryTypes[i].heapIndex);
}

/** @brief Return the first memory type drawing from `heap`.
 *
 * @param mp The physical device's memory properties.
 * @param heap Heap index.
 * @return The lowest memory-type index whose heapIndex is @p heap, or -1 when
 *         the heap backs no type. */
static int balloon_heap_memtype(const VkPhysicalDeviceMemoryProperties *mp,
                                uint32_t heap)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (mp->memoryTypes[i].heapIndex == heap) return (int)i;
    return -1;
}

/** @brief Allocate RHI_BALLOON_BLOCK_MIB blocks of memory type `type`, then release a margin of them.
 *
 * Allocation stops when `want_blocks` new blocks are held, when the driver
 * refuses, or when this call's cap is reached (RHI_BALLOON_SPEC_CAP blocks,
 * bounded by RHI_BALLOON_MAX_BLOCKS in total); `want_blocks` 0 allocates
 * until refusal or the cap. Blocks from this call are then freed, newest
 * first, until at least `release_mib` MiB are released or none of this
 * call's blocks remain. Blocks are neither mapped nor bound to a buffer.
 * @param dev Device whose balloon_blocks and balloon_count are extended.
 * @param type Memory-type index to allocate from.
 * @param want_blocks Number of blocks to hold, or 0 to fill.
 * @param release_mib MiB to release after allocating.
 * @param out_blocks Receives the number of blocks this call still holds.
 * @return The driver's refusal code, or VK_SUCCESS when no allocation was
 *         refused. */
static VkResult balloon_hold(rhi_device *dev, uint32_t type, int want_blocks,
                             unsigned release_mib, int *out_blocks)
{
    const int first = dev->balloon_count;
    int cap = first + RHI_BALLOON_SPEC_CAP;
    if (cap > RHI_BALLOON_MAX_BLOCKS) cap = RHI_BALLOON_MAX_BLOCKS;
    VkResult r = VK_SUCCESS;
    while (dev->balloon_count < cap) {
        if (want_blocks > 0 && dev->balloon_count - first >= want_blocks) break;
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = (VkDeviceSize)RHI_BALLOON_BLOCK_MIB << 20,
            .memoryTypeIndex = type,
        };
        VkDeviceMemory m = VK_NULL_HANDLE;
        r = vkAllocateMemory(dev->device, &mai, NULL, &m);
        if (r != VK_SUCCESS) break;
        dev->balloon_blocks[dev->balloon_count++] = m;
    }
    if (dev->balloon_count >= cap)
        RHI_LOG("balloon: block cap %d reached\n", cap - first);
    for (unsigned freed = 0; freed < release_mib && dev->balloon_count > first;
         freed += RHI_BALLOON_BLOCK_MIB)
        vkFreeMemory(dev->device, dev->balloon_blocks[--dev->balloon_count], NULL);
    *out_blocks = dev->balloon_count - first;
    return r;
}

/** @brief Parse one OPENPETE_RHI_BALLOON spec and hold device memory in the named heap.
 *
 * `<heap>:<MiB>` holds that many MiB, rounded up to whole blocks;
 * `<heap>:fill` allocates until the driver refuses; `<heap>:fill-<MiB>`
 * fills and then releases at least <MiB>, leaving that margin free in the
 * heap. A spec whose heap part is malformed, a heap that does not exist, and
 * a heap that backs no memory type are logged and skipped. An amount is not
 * validated: a `<MiB>` that parses to 0 (or is not a number) holds no fixed
 * count and fills like `fill`. The held amount and the driver's refusal code
 * are logged.
 * @param dev  Device that holds the blocks.
 * @param mp   The physical device's memory properties.
 * @param spec One comma-free spec string. */
static void balloon_spec(rhi_device *dev,
                         const VkPhysicalDeviceMemoryProperties *mp,
                         const char *spec)
{
    char *end = NULL;
    unsigned long heap = strtoul(spec, &end, 10);
    if (end == spec || *end != ':') {
        RHI_LOG("balloon: spec '%s' is not <heap>:<MiB>|<heap>:fill[-<MiB>]\n", spec);
        return;
    }
    if (heap >= mp->memoryHeapCount) {
        RHI_LOG("balloon: heap %lu does not exist (%u heaps)\n", heap,
                mp->memoryHeapCount);
        return;
    }
    const int type = balloon_heap_memtype(mp, (uint32_t)heap);
    if (type < 0) { RHI_LOG("balloon: heap %lu backs no memory type\n", heap); return; }

    const char *amount = end + 1;
    int want_blocks = 0;
    unsigned release_mib = 0;
    if (strncmp(amount, "fill", 4) == 0)
        release_mib = amount[4] == '-' ? (unsigned)strtoul(amount + 5, NULL, 10) : 0u;
    else
        want_blocks = (int)((strtoul(amount, NULL, 10) + RHI_BALLOON_BLOCK_MIB - 1)
                            / RHI_BALLOON_BLOCK_MIB);

    int blocks = 0;
    VkResult r = balloon_hold(dev, (uint32_t)type, want_blocks, release_mib, &blocks);
    RHI_LOG("balloon heap %u type %u: %u MiB held in %d blocks, refusal %d\n",
            (unsigned)heap, (unsigned)type,
            (unsigned)blocks * RHI_BALLOON_BLOCK_MIB, blocks, (int)r);
}

/** @brief Apply every comma-separated spec in OPENPETE_RHI_BALLOON through balloon_spec().
 *
 * The balloon holds device memory in the named heaps after device creation,
 * so later allocations there are refused by the driver: the condition
 * rhi_create_vertex_buffer_device's memory-type retry handles. A `fill` spec
 * makes that a fixed condition for the life of the process; the
 * `fill-<MiB>` margin form reproduces a heap with some free space left, where
 * an allocation that fits early fails later. Specs longer than 63 characters
 * are truncated. Does nothing when the variable is unset or empty.
 * @param dev Device that holds the blocks. */
static void rhi_balloon_init(rhi_device *dev)
{
    const char *env = rhi_knob("OPENPETE_RHI_BALLOON");
    if (!env || !*env) return;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev->phys, &mp);
    for (const char *p = env; *p; ) {
        char spec[64];
        size_t n = strcspn(p, ",");
        size_t keep = n < sizeof spec ? n : sizeof spec - 1;
        memcpy(spec, p, keep);
        spec[keep] = '\0';
        balloon_spec(dev, &mp, spec);
        p += n;
        if (*p == ',') p++;
    }
}

/** @brief Free every block the balloon holds and reset balloon_count.
 *
 * Must run while dev->device is alive; device teardown calls it after
 * vkDeviceWaitIdle.
 * @param dev Device whose balloon blocks are freed. */
static void rhi_balloon_free(rhi_device *dev)
{
    for (int i = 0; i < dev->balloon_count; i++)
        vkFreeMemory(dev->device, dev->balloon_blocks[i], NULL);
    dev->balloon_count = 0;
}

static char s_pcache_dir[1024];   /* rhi_set_pipeline_cache_dir; "" = not persisted */
static char s_pcache_build[9];    /* rhi_pcf_build_tag of the engine build */
static int (*s_pcache_write)(const char *path, const void *data, size_t n);
static bool s_pcache_sync_class;  /* rhi_set_pipeline_cache_sync_class */

void rhi_set_pipeline_cache_sync_class(bool on) { s_pcache_sync_class = on; }

void rhi_set_pipeline_cache_dir(const char *dir, const char *build,
                                int (*write_file)(const char *path, const void *data, size_t n))
{
    snprintf(s_pcache_dir, sizeof s_pcache_dir, "%s", dir && write_file ? dir : "");
    rhi_pcf_build_tag(build, s_pcache_build);
    s_pcache_write = write_file;
}

/* The persisted file is this envelope, then vkGetPipelineCacheData's bytes,
 * then the key manifest: key_count 64-bit dedupe keys
 * (rhi_pc_variant_key with the render pass zeroed) of every pipeline
 * variant created into the cache. The driver treats pInitialData as trusted
 * (it must come from vkGetPipelineCacheData), so a file is offered only when
 * its length and FNV-1a 64 digest (over the cache bytes, then the keys)
 * match what was written and it names this driverVersion: a truncated,
 * corrupted or foreign file is never passed to the driver. */
#define PCACHE_MAGIC "OPVKPC\0\2"
struct pcache_envelope {
    char     magic[8];
    uint32_t driver_version;   /* VkPhysicalDeviceProperties::driverVersion */
    uint32_t payload_len;      /* cache bytes after the envelope */
    uint64_t digest;           /* FNV-1a 64 of the cache bytes, then the keys */
    uint32_t key_count;        /* manifest keys after the cache bytes */
    uint32_t reserved;         /* 0 */
};

/** @brief Fold bytes into an FNV-1a 64-bit digest.
 *
 * @param h Running digest; 0xcbf29ce484222325 starts one.
 * @param p The bytes.
 * @param n Their count.
 * @return The updated digest. */
static uint64_t fnv1a64_fold(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/** @brief Return the FNV-1a 64-bit digest of n bytes.
 *
 * @param p The bytes.
 * @param n Their count.
 * @return The digest. */
static uint64_t fnv1a64(const uint8_t *p, size_t n)
{
    return fnv1a64_fold(0xcbf29ce484222325ull, p, n);
}

/** @brief Read a whole file into a malloc'd buffer.
 *
 * @param path File path.
 * @param out_n Receives the byte count.
 * @return The bytes, or NULL when the file is absent, empty or unreadable. */
static void *read_whole_file(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    void *buf = NULL;
    long n = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    if (n > 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n)) != NULL &&
        fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *out_n = buf ? (size_t)n : 0;
    return buf;
}

/* A persisted file's accepted parts (pcache_payload). */
typedef struct {
    const uint8_t *payload;    /* the cache bytes, NULL when refused */
    size_t         payload_n;
    const uint8_t *keys;       /* key_count little-endian 64-bit keys */
    uint32_t       key_count;
} pcache_parts;

/** @brief Split a persisted file into its cache bytes and key manifest when the envelope and the Vulkan header both match this device.
 *
 * The envelope must carry PCACHE_MAGIC, this driverVersion, a payload
 * length and key count that add up to the file's length, and their
 * digest; the payload's VkPipelineCacheHeaderVersionOne must have
 * headerSize in [32, length], version ONE, and this vendorID, deviceID and
 * pipelineCacheUUID.
 * @param data The file's bytes.
 * @param n Their count.
 * @param props This device's properties.
 * @return The parts; payload NULL when refused. */
static pcache_parts pcache_payload(const uint8_t *data, size_t n,
                                   const VkPhysicalDeviceProperties *props)
{
    pcache_parts out = {0};
    struct pcache_envelope env;
    if (!data || n < sizeof env + 16 + VK_UUID_SIZE) return out;
    memcpy(&env, data, sizeof env);
    const uint8_t *pl = data + sizeof env;
    const size_t pn = env.payload_len;
    if (memcmp(env.magic, PCACHE_MAGIC, sizeof env.magic) != 0 ||
        env.driver_version != props->driverVersion ||
        (uint64_t)sizeof env + pn + 8u * (uint64_t)env.key_count != (uint64_t)n ||
        pn < 16 + VK_UUID_SIZE)
        return out;
    uint32_t hdr[4];
    memcpy(hdr, pl, sizeof hdr);
    if (hdr[0] < 16 + VK_UUID_SIZE || hdr[0] > pn ||
        hdr[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        hdr[2] != props->vendorID || hdr[3] != props->deviceID ||
        memcmp(pl + 16, props->pipelineCacheUUID, VK_UUID_SIZE) != 0 ||
        env.digest != fnv1a64(pl, n - sizeof env))
        return out;
    out = (pcache_parts){ pl, pn, pl + pn, env.key_count };
    return out;
}

/** @brief Return the directory the pipeline cache file lives in.
 *
 * OPENPETE_PIPELINE_CACHE_DIR, when set and non-empty and a writer is
 * installed, replaces the directory rhi_set_pipeline_cache_dir named, so a
 * measurement keeps its cache file apart from the player's.
 * @return The directory, "" when not persisted. */
static const char *pcache_dir(void)
{
    const char *e = rhi_knob("OPENPETE_PIPELINE_CACHE_DIR");
    return (e && *e && s_pcache_write) ? e : s_pcache_dir;
}

/** @brief Remove the cache file before it is read when OPENPETE_PIPELINE_CACHE_COLD is set, for an app-cold boot.
 *
 * Only openpete's own file is removed; no driver cache is touched.
 * @param path The cache file path, or "".
 * @return 1 when the cold mode is on, else 0. */
static int pcache_cold_remove(const char *path)
{
    const char *e = rhi_knob("OPENPETE_PIPELINE_CACHE_COLD");
    if (!(e && *e && *e != '0')) return 0;
    if (path[0] && remove(path) == 0) RHI_LOG("pipeline cache: removed %s (cold boot)\n", path);
    return 1;
}

/** @brief Return whether a driver's pipeline cache keeps what it was given, so the key manifest may stand in for a probe on it.
 *
 * Listed: RADV, whose creation feedback reports a cache hit for every
 * pipeline a warm boot recreates from its saved cache. A driver that drops
 * entries would read present in the manifest and absent in the cache.
 * @param driver_id VkDriverId of the device.
 * @return true for a calibrated driver. */
static bool pcache_manifest_calibrated(uint32_t driver_id)
{
    return driver_id == (uint32_t)VK_DRIVER_ID_MESA_RADV;
}

/** @brief Name a device's pipeline cache file under pcache_dir, or leave the path empty when the cache is not persisted.
 *
 * vk-pipelines-<build>-<vendor>-<device>-<pipelineCacheUUID>.bin, so a
 * driver update, another GPU or another engine build starts its own file;
 * a headless device or a run in the synchronous class
 * (rhi_set_pipeline_cache_sync_class) ends the name in -headless.bin, so a
 * harness never writes a windowed run's file.
 * @param dev Device whose pcache_path is written.
 * @param props Its properties. */
static void pcache_name(rhi_device *dev, const VkPhysicalDeviceProperties *props)
{
    dev->pcache_path[0] = '\0';
    const char *dir = pcache_dir();
    if (!dir[0]) return;
    int n = snprintf(dev->pcache_path, sizeof dev->pcache_path,
                     "%s/vk-pipelines-%s-%04x-%04x-", dir, s_pcache_build,
                     props->vendorID, props->deviceID);
    for (int i = 0; (unsigned)i < VK_UUID_SIZE && n > 0 && (size_t)n + 3 < sizeof dev->pcache_path; i++)
        n += snprintf(dev->pcache_path + n, sizeof dev->pcache_path - (size_t)n, "%02x",
                      props->pipelineCacheUUID[i]);
    const char *ext = (dev->headless || s_pcache_sync_class) ? "-headless.bin" : ".bin";
    const size_t en = strlen(ext) + 1;
    if (n > 0 && (size_t)n + en < sizeof dev->pcache_path) memcpy(dev->pcache_path + n, ext, en);
    else dev->pcache_path[0] = '\0';
}

/** @brief Load a file's key manifest into the device's key set.
 *
 * @param dev Device whose pc_keys and pcache_keys_saved are written.
 * @param parts The accepted file parts. */
static void pcache_load_keys(rhi_device *dev, const pcache_parts *parts)
{
    for (uint32_t i = 0; parts->keys && i < parts->key_count; i++) {
        uint64_t k;
        memcpy(&k, parts->keys + 8u * i, sizeof k);
        rhi_pc_keyset_insert(&dev->pc_keys, k);
    }
    dev->pcache_keys_saved = dev->pc_keys.n;
}

/** @brief Create the device's pipeline cache, seeded from its file when pcache_payload accepts the file.
 *
 * The file is named by pcache_name; its mtime is refreshed as its last-use
 * time, and the open ages out the other cache files of its class
 * (rhi_pcf_prune). Without a directory the cache lives for the process
 * only. A failure leaves dev->pcache VK_NULL_HANDLE, which every pipeline
 * create accepts. The file's key manifest seeds dev->pc_keys, trusted for
 * the warm-boot skip only on a calibrated driver. The file's size, the
 * bytes offered and the time spent reading the file and creating the cache
 * are recorded for rhi_boot_stats_get, and the cache's serialized size as
 * the last saved size. Creates dev->pc_mx, the lock of the key manifest.
 * @param dev Device whose physical device and logical device exist. */
static void pipeline_cache_open(rhi_device *dev)
{
    VkPhysicalDeviceDriverProperties drv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
    vkGetPhysicalDeviceProperties2(dev->phys, &props2);
    const VkPhysicalDeviceProperties *props = &props2.properties;
    dev->manifest_trusted = pcache_manifest_calibrated((uint32_t)drv.driverID);
    dev->pc_mx = SDL_CreateMutex();   /* NULL leaves SDL's lock calls no-ops */
    pcache_name(dev, props);
    s_boot.cache_cold = pcache_cold_remove(dev->pcache_path);
    rhi_pcf_touch(dev->pcache_path);
    rhi_pcf_prune(pcache_dir(), dev->pcache_path, dev->headless || s_pcache_sync_class);
    const int64_t t0 = boot_now();
    size_t n = 0;
    void *data = dev->pcache_path[0] ? read_whole_file(dev->pcache_path, &n) : NULL;
    const pcache_parts parts = pcache_payload((const uint8_t *)data, n, props);
    size_t pn = parts.payload ? parts.payload_n : 0;
    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = pn,
        .pInitialData = parts.payload,
    };
    if (vkCreatePipelineCache(dev->device, &ci, NULL, &dev->pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = NULL; pn = 0;
        if (vkCreatePipelineCache(dev->device, &ci, NULL, &dev->pcache) != VK_SUCCESS)
            dev->pcache = VK_NULL_HANDLE;
    }
    dev->pcache_digest = pn ? fnv1a64(parts.payload, pn) : 0;
    if (pn) pcache_load_keys(dev, &parts);
    if (!dev->pcache || vkGetPipelineCacheData(dev->device, dev->pcache, &dev->pcache_saved_n,
                                               NULL) != VK_SUCCESS)
        dev->pcache_saved_n = 0;
    s_boot.cache_load_ns = boot_now() - t0;
    s_boot.cache_file_bytes = n;
    s_boot.cache_offered_bytes = pn;
    if (dev->pcache_path[0])
        RHI_LOG("pipeline cache: %s (%zu bytes offered, %zu manifest keys%s%s)\n",
                dev->pcache_path, pn, dev->pc_keys.n,
                dev->manifest_trusted ? ", manifest trusted" : "",
                data && !parts.payload ? "; file refused" : "");
    free(data);
}

/** @brief Copy every key of the device's key set to an array.
 *
 * Caller holds dev->pc_mx.
 * @param dev Device whose pc_keys are read.
 * @param k Receives 8 * pc_keys.n bytes.
 * @return The key count. */
static uint32_t pcache_copy_keys(const rhi_device *dev, uint8_t *k)
{
    uint32_t kc = 0;
    for (size_t i = 0; i < dev->pc_keys.cap; i++) {
        if (!dev->pc_keys.slot[i]) continue;
        memcpy(k + 8u * kc, &dev->pc_keys.slot[i], 8);
        kc++;
    }
    return kc;
}

/** @brief Fill a cache file image's envelope over cache bytes and keys already in place.
 *
 * Any thread: reads and writes only the buffer.
 * @param buf Buffer of sizeof envelope + n + 8 * kc bytes, the cache bytes at
 *            buf + sizeof envelope and the keys after them.
 * @param n Count of the cache bytes.
 * @param kc Count of the keys.
 * @param driver_version The device's driverVersion.
 * @return The file's total length. */
static size_t pcache_seal(uint8_t *buf, size_t n, uint32_t kc, uint32_t driver_version)
{
    struct pcache_envelope env = {0};
    memcpy(env.magic, PCACHE_MAGIC, sizeof env.magic);
    env.driver_version = driver_version;
    env.payload_len = (uint32_t)n;
    env.key_count = kc;
    env.digest = fnv1a64(buf + sizeof env, n + 8u * kc);
    memcpy(buf, &env, sizeof env);
    return sizeof env + n + 8u * kc;
}

/** @brief Return the device's driverVersion.
 *
 * @param dev Device.
 * @return The version. */
static uint32_t pcache_driver_version(const rhi_device *dev)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev->phys, &props);
    return props.driverVersion;
}

/** @brief Write the pipeline cache to its file when its bytes or its key manifest changed since load or the last save.
 *
 * Display owner only. The cache bytes are compared by digest, so a
 * same-size change is saved. With the manifest unchanged and every create
 * since the last save reported a cache hit, nothing entered the cache and
 * the file is kept whatever the digest: a driver may serialize the same
 * entries in another order.
 * The image goes through the writer rhi_set_pipeline_cache_dir installed,
 * which replaces the file in one step.
 * @param dev Device.
 * @param why Log label of the save point.
 * @return 1 when written, 0 when unchanged, not persisted or failed. */
static int pipeline_cache_save(rhi_device *dev, const char *why)
{
    size_t n = 0;
    if (!dev->pcache || !dev->pcache_path[0] || !s_pcache_write ||
        vkGetPipelineCacheData(dev->device, dev->pcache, &n, NULL) != VK_SUCCESS ||
        n == 0 || n > UINT32_MAX)
        return 0;
    SDL_LockMutex(dev->pc_mx);
    const size_t kn = dev->pc_keys.n;
    const unsigned unhit = dev->pcache_unhit;
    uint8_t *buf = malloc(sizeof(struct pcache_envelope) + n + 8u * kn);
    const uint32_t kc = buf ? pcache_copy_keys(dev, buf + sizeof(struct pcache_envelope) + n) : 0u;
    SDL_UnlockMutex(dev->pc_mx);
    int wrote = 0;
    const size_t cap_n = n;   /* the keys sit after cap_n cache bytes */
    if (buf && vkGetPipelineCacheData(dev->device, dev->pcache, &n,
                                      buf + sizeof(struct pcache_envelope)) == VK_SUCCESS &&
        n == cap_n) {
        const uint64_t digest = fnv1a64(buf + sizeof(struct pcache_envelope), n);
        const bool keys_same = kn == dev->pcache_keys_saved;
        if (keys_same && (digest == dev->pcache_digest || !unhit)) {
            RHI_LOG("pipeline cache: unchanged at %s, not saved\n", why);
        } else if (s_pcache_write(dev->pcache_path, buf,
                                  pcache_seal(buf, n, kc, pcache_driver_version(dev))) == 0) {
            RHI_LOG("pipeline cache: saved %zu bytes, %zu manifest keys (%s)\n", n, kn, why);
            dev->pcache_digest = digest;
            dev->pcache_saved_n = n;
            SDL_LockMutex(dev->pc_mx);
            dev->pcache_keys_saved = kn;
            dev->pcache_unhit -= unhit;
            SDL_UnlockMutex(dev->pc_mx);
            wrote = 1;
        } else {
            RHI_LOG("pipeline cache: save to %s failed\n", dev->pcache_path);
        }
    }
    free(buf);
    return wrote;
}

/* One debounced first-use save, handed by the display owner to its worker:
 * the snapshot (cache bytes and keys behind envelope space) and the
 * worker's outcome. */
struct pcache_save_job {
    SDL_Thread *thread;
    atomic_bool done;
    char        path[1152];
    uint8_t    *buf;
    size_t      n;              /* cache bytes in the snapshot          */
    uint32_t    kc;             /* keys in the snapshot                 */
    size_t      kn;             /* pc_keys.n at the snapshot            */
    unsigned    unhit;          /* pcache_unhit at the snapshot         */
    uint32_t    driver_version;
    uint64_t    digest;         /* worker: digest of the cache bytes    */
    int         wrote;          /* worker: 1 when the file was replaced */
};

/** @brief Seal and write one save snapshot: the save worker's body.
 *
 * Reads and writes only its job; the file writer replaces the file in one
 * step, and its one environment read runs inside the guard installed with
 * op_atomic_file_set_env_guard (rhi_env_read_guard).
 * @param arg The pcache_save_job.
 * @return 0. */
static int SDLCALL pcache_save_worker(void *arg)
{
    OP_ROLE_BIND(pcache);
    struct pcache_save_job *j = arg;
    j->digest = fnv1a64(j->buf + sizeof(struct pcache_envelope), j->n);
    const size_t len = pcache_seal(j->buf, j->n, j->kc, j->driver_version);
    j->wrote = s_pcache_write(j->path, j->buf, len) == 0;
    atomic_store(&j->done, true);
    return 0;
}

/** @brief Join a finished (or, with wait, an in-flight) save worker and take its outcome into the device's save state.
 *
 * Display owner only.
 * @param dev Device.
 * @param wait true joins an in-flight worker; false returns while it runs. */
static void pcache_save_reap(rhi_device *dev, bool wait)
{
    struct pcache_save_job *j = dev->pcache_save;
    if (!j || (!wait && !atomic_load(&j->done))) return;
    if (j->thread) SDL_WaitThread(j->thread, NULL);
    if (j->wrote) {
        RHI_LOG("pipeline cache: saved %zu bytes, %zu manifest keys (first use)\n", j->n, j->kn);
        dev->pcache_digest = j->digest;
        dev->pcache_saved_n = j->n;
        SDL_LockMutex(dev->pc_mx);
        dev->pcache_keys_saved = j->kn;
        dev->pcache_unhit -= j->unhit;
        SDL_UnlockMutex(dev->pc_mx);
        dev->autosave.written++;
    } else {
        RHI_LOG("pipeline cache: first-use save to %s failed\n", j->path);
    }
    free(j->buf);
    free(j);
    dev->pcache_save = NULL;
}

/** @brief Snapshot the cache and its keys and hand the snapshot to a save worker.
 *
 * Display owner only. Skipped when the cache's byte count equals the last
 * saved one (a same-size change waits for the digest compare at shutdown).
 * The worker computes the envelope digest and replaces the file; a worker
 * that cannot be started runs inline.
 * @param dev Device, with no save in flight. */
static void pcache_save_start(rhi_device *dev)
{
    size_t n = 0;
    if (vkGetPipelineCacheData(dev->device, dev->pcache, &n, NULL) != VK_SUCCESS ||
        n == 0 || n > UINT32_MAX)
        return;
    if (n == dev->pcache_saved_n) { dev->autosave.same_size++; return; }
    struct pcache_save_job *j = calloc(1, sizeof(*j));
    if (!j) return;
    SDL_LockMutex(dev->pc_mx);
    j->kn = dev->pc_keys.n;
    j->unhit = dev->pcache_unhit;
    j->buf = malloc(sizeof(struct pcache_envelope) + n + 8u * j->kn);
    if (j->buf) j->kc = pcache_copy_keys(dev, j->buf + sizeof(struct pcache_envelope) + n);
    SDL_UnlockMutex(dev->pc_mx);
    const size_t cap_n = n;
    if (!j->buf || vkGetPipelineCacheData(dev->device, dev->pcache, &n,
                                          j->buf + sizeof(struct pcache_envelope)) != VK_SUCCESS ||
        n != cap_n) {
        free(j->buf);
        free(j);
        dev->pcache_fu_pending = true;   /* the cache grew under the copy: retry */
        return;
    }
    j->n = n;
    j->driver_version = pcache_driver_version(dev);
    snprintf(j->path, sizeof j->path, "%s", dev->pcache_path);
    dev->pcache_save = j;
    dev->autosave.started++;
    j->thread = SDL_CreateThread(pcache_save_worker, "openpete-pcache-save", j);
    if (!j->thread) pcache_save_worker(j);
}

/* Frames with no first-use cache miss before the debounced save runs. */
#define PCACHE_SAVE_QUIET_FRAMES 120u

/** @brief Run the debounced first-use cache save: reap a finished save, and start one once first-use misses have been quiet for PCACHE_SAVE_QUIET_FRAMES begun frames.
 *
 * Display owner only; called at every begun frame. A first-use create not
 * reported a cache hit, on any thread, counts in dev->pcache_fu_misses
 * (pipe_create_record); each new count restarts the quiet period.
 * @param dev Device. */
static void pcache_autosave(rhi_device *dev)
{
    pcache_save_reap(dev, false);
    if (!dev->pcache || !dev->pcache_path[0] || !s_pcache_write) return;
    const unsigned m = atomic_load_explicit(&dev->pcache_fu_misses, memory_order_relaxed);
    if (m != dev->pcache_fu_seen) {
        dev->pcache_fu_seen = m;
        dev->pcache_fu_serial = s_frame_serial;
        dev->pcache_fu_pending = true;
        return;
    }
    if (!dev->pcache_fu_pending || dev->pcache_save ||
        s_frame_serial - dev->pcache_fu_serial < PCACHE_SAVE_QUIET_FRAMES)
        return;
    dev->pcache_fu_pending = false;
    dev->autosave.triggers++;
    pcache_save_start(dev);
}

void rhi_pcache_autosave_stats_get(const rhi_device *dev, rhi_pcache_autosave_stats *out)
{
    *out = dev ? dev->autosave : (rhi_pcache_autosave_stats){0};
}

/** @brief Join an in-flight first-use save, save the pipeline cache when it changed (pipeline_cache_save), then destroy it.
 *
 * The join comes first, so an older snapshot never replaces the final save.
 * @param dev Device, idle. */
static void pipeline_cache_close(rhi_device *dev)
{
    if (dev->pcache_save) {
        dev->autosave.joined_at_shutdown++;
        pcache_save_reap(dev, true);
    }
    if (!dev->pcache) return;
    pipeline_cache_save(dev, "shutdown");
    const rhi_pcache_autosave_stats *a = &dev->autosave;
    RHI_LOG("pipeline cache autosave: triggers=%u same_size=%u started=%u written=%u "
            "joined_at_shutdown=%u\n", a->triggers, a->same_size, a->started, a->written,
            a->joined_at_shutdown);
    vkDestroyPipelineCache(dev->device, dev->pcache, NULL);
    dev->pcache = VK_NULL_HANDLE;
    rhi_pc_keyset_free(&dev->pc_keys);
}

/* The reason the last device creation failed (rhi_create_error). */
static char s_create_error[160];

const char *rhi_create_error(void) { return s_create_error; }

/** @brief Record the reason a device creation failed, logged and kept for rhi_create_error.
 *
 * @param why A sentence naming the failed step. */
static void create_error_set(const char *why)
{
    snprintf(s_create_error, sizeof s_create_error, "%s", why);
    RHI_LOG("device creation failed: %s\n", why);
}

/**
 * @brief Creates the Vulkan instance, device, queues and presentation target.
 *
 * With @p window the target is the window's swapchain; with @p headless it is
 * a surfaceless @p hl_w x @p hl_h image set. Logs the chosen device's name,
 * decoded driver version and driver info string on one `[rhi/vk] device=`
 * line, which the golden capture harnesses read as the capture environment's
 * device and Mesa strings.
 * Returns NULL, with the device torn down, on any failure.
 * @param gpu The physical device to prefer (rhi_create); NULL or "" for the
 *            automatic pick.
 */
static rhi_device *rhi_create_impl(struct SDL_Window *window, const char *app_name,
                                   bool headless, uint32_t hl_w, uint32_t hl_h,
                                   int vsync, const char *gpu)
{
    rhi_device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->window     = window;
    dev->headless   = headless;
    snprintf(dev->gpu_choice, sizeof dev->gpu_choice, "%s", gpu ? gpu : "");
    dev->vsync_pref = vsync;   /* consulted by choose_present_mode (windowed only) */
    {
        const char *pm = rhi_knob("OPENPETE_PRESENT_MODE");
        snprintf(dev->present_mode_name, sizeof dev->present_mode_name, "%s", pm ? pm : "");
    }

    /* Dynamically load the Vulkan loader (libvulkan.so.1 / vulkan-1.dll);
     * no loader import library is linked on any platform. */
    if (volkInitialize() != VK_SUCCESS) {
        RHI_LOG("volkInitialize failed - no Vulkan loader found\n");
        create_error_set("No Vulkan loader was found (libvulkan / vulkan-1.dll).");
        free(dev);
        return NULL;
    }

    /* The step under way, named in rhi_create_error when it fails. */
    const char *step = "The Vulkan instance could not be created.";
    if (!create_instance(dev, app_name)) goto fail;
    volkLoadInstance(dev->instance);   /* load instance- + device-level fns */

    /* SDL's reason names the missing capability, such as the offscreen video
     * driver's VK_EXT_headless_surface. */
    char surface_why[sizeof s_create_error];
    if (!headless && !SDL_Vulkan_CreateSurface(window, dev->instance, NULL, &dev->surface)) {
        RHI_LOG("SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        snprintf(surface_why, sizeof surface_why,
                 "The window's Vulkan surface could not be created: %s", SDL_GetError());
        step = surface_why;
        goto fail;
    }
    step = "No Vulkan graphics device that can draw to the window was found.";
    if (!pick_physical_and_device(dev)) goto fail;
    log_active_layers(dev);
    volkLoadDevice(dev->device);        /* device-specific dispatch (faster) */
    step = "The Vulkan swapchain could not be created.";
    if (!headless) {
        if (create_swapchain(dev, VK_NULL_HANDLE) != VK_SUCCESS) goto fail;
    } else {
        /* No swapchain: the offscreen image still needs an extent, the
         * requested size. sc_count stays 0 -> no render_done semaphores, no
         * present. */
        dev->sc_format = VK_FORMAT_UNDEFINED;
        dev->sc_extent = (VkExtent2D){ hl_w ? hl_w : 1u, hl_h ? hl_h : 1u };
        off_extent_sync(dev);
        dev->sc_count  = 0;
        for (uint32_t i = 0; i < RHI_MAX_SC_IMAGES; i++)
            dev->images_in_flight[i] = VK_NULL_HANDLE;
    }
    pipeline_cache_open(dev);
    step = "The Vulkan device's render targets could not be created.";
    if (!create_render_pass(dev))       goto fail;
    if (!create_rt_render_pass_fmt(dev, RHI_RT_FORMAT, &dev->rt_rp)) goto fail;
    if (!create_rt_render_pass_fmt(dev, VK_FORMAT_R32G32B32A32_UINT, &dev->rt_u32_rp)) goto fail;
    if (!create_rt_ds_render_pass(dev)) goto fail;
    if (!create_offscreen(dev))         goto fail;
    if (!headless && !create_stages(dev)) goto fail;
    if (!create_sync_and_commands(dev)) goto fail;
    if (!create_descriptor_infra(dev))  goto fail;
    if (!create_gpu_timers(dev))        goto fail;

    rhi_log_memory_types(dev);
    /* After the RHI's own images, buffers and staging rings are resident, so a
     * filled heap refuses only what the renderer allocates from here on. */
    rhi_balloon_init(dev);

    /* The driver's own name and version string (Vulkan 1.2 core): lavapipe
     * reports a placeholder driverVersion, and its driverInfo carries the
     * Mesa version. */
    VkPhysicalDeviceDriverProperties drv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
    vkGetPhysicalDeviceProperties2(dev->phys, &props2);
    RHI_LOG("device='%s'%s driver=%s driver-info='%s' format=%d internal=%d %ux%u images=%u\n",
            dev->device_name, headless ? " (headless)" : "",
            rhi_driver_version(dev), drv.driverInfo, (int)dev->sc_format, (int)RHI_RT_FORMAT,
            dev->sc_extent.width, dev->sc_extent.height, dev->sc_count);
    s_boot.device_created_ns = boot_now();
    s_boot.devices_created++;
    return dev;

fail:
    create_error_set(step);
    rhi_destroy(dev);
    return NULL;
}

rhi_device *rhi_create(struct SDL_Window *window, const char *app_name, int vsync,
                       const char *gpu)
{
    rhi_display_claim(NULL);   /* boot: the creating thread presents until a handoff */
    rhi_device *dev = rhi_create_impl(window, app_name, /*headless=*/false, 0, 0, vsync, gpu);
    if (dev) (void)SDL_AddEventWatch(display_change_watch, dev);
    return dev;
}

rhi_device *rhi_create_headless(const char *app_name, uint32_t width, uint32_t height,
                                const char *gpu)
{
    rhi_display_claim(NULL);
    /* Surfaceless: no swapchain, so present mode / vsync is irrelevant (-1). */
    return rhi_create_impl(NULL, app_name, /*headless=*/true, width, height, -1, gpu);
}

bool rhi_is_headless(const rhi_device *dev)
{
    return dev && dev->headless;
}

/** @brief Destroy every descriptor pool (their sets with them) and free the pair-set lists.
 *
 * @param dev Device, idle. */
static void desc_pools_free(rhi_device *dev)
{
    for (int i = 0; i < dev->desc_pool_n; i++)
        vkDestroyDescriptorPool(dev->device, dev->desc_pools[i], NULL);
    dev->desc_pool_n = 0;
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        for (int k = 0; k < 3; k++) {
            free(dev->vsets[k][i].sets);  dev->vsets[k][i].sets = NULL;
            free(dev->vsets[k][i].views); dev->vsets[k][i].views = NULL;
        }
    }
}

/** @brief Destroy every chunk of the device upload ring.
 *
 * @param dev Device, idle. */
static void upload_ring_free(rhi_device *dev)
{
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        for (struct rhi_stage_chunk *c = dev->up_ring[i], *n; c; c = n) {
            n = c->next; rhi_destroy_buffer(dev, c->buf); free(c);
        }
        dev->up_ring[i] = NULL;
    }
}

static void gpl_destroy_all(rhi_device *dev);
static void lto_destroy_ready(rhi_device *dev);

static void described_free(rhi_device *dev);
static void thumb_slot_free(rhi_device *dev, int i);

/** @brief Wait the logical device idle, destroy every object created from it, then the device itself.
 *
 * Every call here goes through device-level dispatch, which the loader
 * resolves by dereferencing the VkDevice handle, so a null device handle
 * faults even for a VK_NULL_HANDLE object; rhi_destroy calls this only when
 * vkCreateDevice succeeded.
 * @param dev Device whose dev->device is a live VkDevice; dev->device is
 *            VK_NULL_HANDLE on return. */
static void device_objects_destroy(rhi_device *dev)
{
    vkDeviceWaitIdle(dev->device);
    rhi_balloon_free(dev);
    retired_reclaim(dev, true);
    lto_destroy_ready(dev);
    gpl_destroy_all(dev);

    if (dev->readback_buf) { rhi_destroy_buffer(dev, dev->readback_buf); dev->readback_buf = NULL; }
    upload_ring_free(dev);
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        if (dev->readback_ring[i].staging) {
            rhi_destroy_buffer(dev, dev->readback_ring[i].staging);
            dev->readback_ring[i].staging = NULL;
        }
        if (dev->grid_ring[i].staging) {
            rhi_destroy_buffer(dev, dev->grid_ring[i].staging);
            dev->grid_ring[i].staging = NULL;
        }
        thumb_slot_free(dev, i);
    }
    destroy_swapchain_resources(dev);
    swapchain_destroy(dev);
    if (dev->off_rp) vkDestroyRenderPass(dev->device, dev->off_rp, NULL);
    if (dev->off_load_rp) vkDestroyRenderPass(dev->device, dev->off_load_rp, NULL);
    if (dev->rt_rp)  vkDestroyRenderPass(dev->device, dev->rt_rp, NULL);
    if (dev->rt_ds_rp) vkDestroyRenderPass(dev->device, dev->rt_ds_rp, NULL);
    if (dev->rt_u32_rp) vkDestroyRenderPass(dev->device, dev->rt_u32_rp, NULL);

    desc_pools_free(dev);
    if (dev->vram_set_layout) vkDestroyDescriptorSetLayout(dev->device, dev->vram_set_layout, NULL);
    if (dev->ubo_set_layout) vkDestroyDescriptorSetLayout(dev->device, dev->ubo_set_layout, NULL);
    if (dev->ubo_data_zero) { rhi_destroy_buffer(dev, dev->ubo_data_zero); dev->ubo_data_zero = NULL; }
    if (dev->ubo1_set_layout) vkDestroyDescriptorSetLayout(dev->device, dev->ubo1_set_layout, NULL);
    vkDestroyDescriptorSetLayout(dev->device, dev->ssbo1_set_layout, NULL);   /* VK_NULL_HANDLE is a no-op */
    vkDestroyDescriptorSetLayout(dev->device, dev->texssbo_set_layout, NULL);
    if (dev->pair_set_layout) vkDestroyDescriptorSetLayout(dev->device, dev->pair_set_layout, NULL);
    vkDestroyDescriptorSetLayout(dev->device, dev->trio_set_layout, NULL);   /* VK_NULL_HANDLE is a no-op */
    vkDestroyDescriptorSetLayout(dev->device, dev->quad_set_layout, NULL);
    if (dev->vram_sampler)   vkDestroySampler(dev->device, dev->vram_sampler, NULL);
    if (dev->linear_sampler) vkDestroySampler(dev->device, dev->linear_sampler, NULL);

    for (int i = 0; i < RHI_STAGE_RING; i++) {
        if (dev->pimg_avail[i]) vkDestroySemaphore(dev->device, dev->pimg_avail[i], NULL);
        if (dev->pfence[i])     vkDestroyFence(dev->device, dev->pfence[i], NULL);
    }
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        if (dev->img_avail[i]) vkDestroySemaphore(dev->device, dev->img_avail[i], NULL);
        if (dev->in_flight[i]) vkDestroyFence(dev->device, dev->in_flight[i], NULL);
        if (dev->gpu_qp[i])    vkDestroyQueryPool(dev->device, dev->gpu_qp[i], NULL);
    }
    pipeline_cache_close(dev);
    if (dev->pool)   vkDestroyCommandPool(dev->device, dev->pool, NULL);
    vkDestroyDevice(dev->device, NULL);
    dev->device = VK_NULL_HANDLE;
}

/** @brief Log the windowed device's swapchain census: rebuilds, creation failures, surface rebuilds and presents, in total and since the last rebuild.
 *
 * @param dev Device being destroyed; a headless one logs nothing. */
static void swapchain_census_log(const rhi_device *dev)
{
    if (dev->headless) return;
    RHI_LOG("swapchain census: %u recreates, %u create failures, %u surface rebuilds, "
            "%llu presents, %llu after the last recreate\n",
            dev->sc_recreates, dev->sc_create_failures, dev->sc_surface_rebuilds,
            (unsigned long long)dev->sc_presents,
            (unsigned long long)(dev->sc_presents - dev->sc_presents_at_recreate));
}

void rhi_destroy(rhi_device *dev)
{
    if (ring_gen_dbg())
        fprintf(stderr, "[rhi/vk] ring generations: bumps=%llu overflows=%llu vb_gen_writes=%llu vb_gen_binds=%llu\n",
                (unsigned long long)s_ring_gen_bumps, (unsigned long long)s_ring_gen_overflows,
                (unsigned long long)s_dbg_gw, (unsigned long long)s_dbg_gb);
    if (!dev) return;
    if (!dev->headless) SDL_RemoveEventWatch(display_change_watch, dev);
    swapchain_census_log(dev);
    rhi_compile_pool_stop(dev);   /* no thread may use the cache or the device past here */
    described_free(dev);
    rhi_pc_report();
    /* A creation that failed before vkCreateDevice (no surface, no
     * qualifying physical device) has no device-level object to destroy. */
    if (dev->device) device_objects_destroy(dev);
    if (dev->surface) vkDestroySurfaceKHR(dev->instance, dev->surface, NULL);
    if (dev->debug) {
        PFN_vkDestroyDebugUtilsMessengerEXT rm =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                dev->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (rm) rm(dev->instance, dev->debug, NULL);
    }
    if (dev->instance) vkDestroyInstance(dev->instance, NULL);
    SDL_DestroyMutex(dev->pc_mx);
    free(dev);
}

void rhi_wait_idle(rhi_device *dev)
{
    if (dev && dev->device) vkDeviceWaitIdle(dev->device);
}

bool rhi_frame_active(const rhi_device *dev)
{
    return dev && dev->frame_active;
}

uint64_t rhi_frame_serial(rhi_device *dev) { (void)dev; return s_frame_serial; }

/* Report any present-path phase that blocks long enough to threaten the sim.
 * Under the thread split the presenting thread is the sim thread, so a stall
 * here is a dropped tick - and "which call blocked" is otherwise invisible. */
#define RHI_STALL_REPORT_NS (12ull * 1000ull * 1000ull)

/* Block time per present-path phase, accumulated for the pacing log. The
 * >=12 ms report catches catastrophes; the pacing analysis needs the whole
 * distribution, including the sub-millisecond FIFO wait that IS the pacing on
 * a vsynced present. Statics rather than device fields: exactly one thread
 * presents, and the consumer (the host loop) has no rhi_device handle. */
static Uint64 s_block_ns[RHI_BLOCK_PHASE_COUNT];

/** @brief Add the time since `t0` to a present-path phase's block accumulator and log a stall of 12 ms or more.
 *
 * @param what Phase label for the log line.
 * @param phase RHI_BLOCK_* index into the accumulators; negative logs only.
 * @param t0 SDL_GetTicksNS value at the start of the blocking call. */
static void rhi_note_stall_phase(const char *what, int phase, Uint64 t0)
{
    Uint64 dt = SDL_GetTicksNS() - t0;
    if (phase >= 0) s_block_ns[phase] += dt;
    if (dt >= RHI_STALL_REPORT_NS)
        RHI_LOG("stall: %s blocked %.1f ms\n", what, (double)dt / 1e6);
}

uint64_t rhi_fence_sheds(const rhi_device *dev)
{
    return dev ? dev->fence_sheds : 0;
}

uint64_t rhi_acquire_sheds(const rhi_device *dev)
{
    return dev ? dev->acquire_sheds : 0;
}

void rhi_take_sheds(rhi_device *dev, uint64_t *fence, uint64_t *acquire)
{
    *fence = *acquire = 0;
    if (!dev) return;
    *fence = dev->fence_sheds - dev->fence_sheds_taken;
    *acquire = dev->acquire_sheds - dev->acquire_sheds_taken;
    dev->fence_sheds_taken = dev->fence_sheds;
    dev->acquire_sheds_taken = dev->acquire_sheds;
}

uint64_t rhi_acquire_timeouts(const rhi_device *dev)
{
    return dev ? dev->acquire_timeouts : 0;
}

uint32_t rhi_swapchain_image_count(const rhi_device *dev)
{
    return dev ? dev->sc_count : 0;
}

/* The last swapchain present's image index and extent, for the pacing log's
 * PRESENT_DETAIL record. Statics for the reason the block times are: only
 * the display owner presents and reads them. */
static uint32_t s_last_present_image = UINT32_MAX;
static uint32_t s_last_present_w, s_last_present_h;
static uint64_t s_presents;

/** @brief Note a swapchain present's image index and extent for rhi_last_present.
 * @param dev The presenting device. */
static void note_present(const rhi_device *dev)
{
    s_last_present_image = dev->image_index;
    s_last_present_w = dev->sc_extent.width;
    s_last_present_h = dev->sc_extent.height;
    s_presents++;
}

void rhi_last_present(uint32_t *image_index, uint32_t *width, uint32_t *height, uint64_t *presents)
{
    *image_index = s_last_present_image;
    *width = s_last_present_w;
    *height = s_last_present_h;
    *presents = s_presents;
}

void rhi_take_block_times(int64_t *out, int n)
{
    for (int i = 0; i < n && i < RHI_BLOCK_PHASE_COUNT; i++) {
        out[i] = (int64_t)s_block_ns[i];
        s_block_ns[i] = 0;
    }
}

const char *rhi_driver_version(const rhi_device *dev)
{
    return (dev && dev->driver_version[0]) ? dev->driver_version : "unknown";
}

int rhi_device_report(const rhi_device *dev, char *buf, size_t cap)
{
    if (!buf || !cap) return 0;
    buf[0] = '\0';
    if (!dev || !dev->phys) return snprintf(buf, cap, "vulkan: no device\n");
    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev->phys, &props);
    const int have_drv = props.apiVersion >= VK_API_VERSION_1_2;
    if (have_drv) {
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(dev->phys, &p2);
    }
    return snprintf(buf, cap,
        "vulkan: device=\"%s\" type=%s vendor=0x%04x device_id=0x%04x api=%u.%u.%u "
        "driver_version=%s (raw 0x%08x)\n"
        "vulkan: driver_id=%d driver_name=\"%s\" driver_info=\"%s\"\n"
        "vulkan: device extensions: %s\n"
        "vulkan: validation=%s headless=%d present_mode=%s swapchain=%ux%u format=%d "
        "colorspace=%d images=%u offscreen=%ux%u\n"
        "vulkan: compute=%s%s %s\n",
        props.deviceName, device_type_name(props.deviceType), props.vendorID, props.deviceID,
        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
        VK_API_VERSION_PATCH(props.apiVersion), rhi_driver_version(dev), props.driverVersion,
        have_drv ? (int)drv.driverID : -1, have_drv ? drv.driverName : "",
        have_drv ? drv.driverInfo : "", s_dev_exts[0] ? s_dev_exts : "(none)",
        s_validation_on ? "on" : "off", dev->headless ? 1 : 0, rhi_present_mode_name(dev),
        dev->sc_extent.width, dev->sc_extent.height, (int)dev->sc_format, s_sc_colorspace,
        dev->sc_count, dev->off_extent.width, dev->off_extent.height,
        dev->compute_ok ? "available" : "unavailable: ", dev->compute_ok ? "" : dev->compute_why,
        dev->compute_facts);
}

const char *rhi_present_mode_name(const rhi_device *dev)
{
    if (!dev) return "none";
    switch (dev->present_mode_granted) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR:         return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    default:                               return "other";
    }
}

float rhi_refresh_hz(const rhi_device *dev) { return dev ? dev->refresh_hz : 0.0f; }

void rhi_set_refresh_stub(rhi_device *dev, float hz)
{
    if (dev) dev->refresh_stub_hz = hz;
}
bool rhi_present_is_vsync(const rhi_device *dev)
{
    return dev && !dev->headless && (dev->present_mode_granted == VK_PRESENT_MODE_FIFO_KHR ||
                                     dev->present_mode_granted == VK_PRESENT_MODE_FIFO_RELAXED_KHR);
}

void rhi_set_present_blocking(rhi_device *dev, bool on)
{
    if (dev) dev->present_blocking = on;
}

void rhi_set_capture_mode(rhi_device *dev, bool on)
{
    if (dev) dev->capture_mode = on;
}

bool rhi_needs_recreate(const rhi_device *dev)
{
    return dev && dev->needs_recreate;
}

/** @brief Fault injection: refuse an acquire as a display that returns images late does, under OPENPETE_ACQUIRE_FAULT.
 *
 * OPENPETE_ACQUIRE_FAULT=OFF,PERIOD (harness, milliseconds): in the first
 * OFF ms of every PERIOD ms of SDL_GetTicksNS, an acquire blocks until the
 * window ends or the budget runs out, whichever is first, and reports
 * VK_TIMEOUT without calling vkAcquireNextImageKHR, so no semaphore is
 * signalled. Read once; unset, malformed or OFF 0 refuses nothing.
 * @param dev Device whose acquire is tested; holds the parsed window.
 * @param budget The acquire's budget in nanoseconds.
 * @return 1 when this acquire is refused, else 0. */
static int acquire_fault(rhi_device *dev, Uint64 budget)
{
    if (!dev->fault_read) {
        dev->fault_read = true;
        const char *e = getenv("OPENPETE_ACQUIRE_FAULT");
        unsigned off = 0, period = 0;
        if (e && sscanf(e, "%u,%u", &off, &period) == 2 && off > 0 && period > off) {
            dev->fault_off_ns = (Uint64)off * 1000000ull;
            dev->fault_period_ns = (Uint64)period * 1000000ull;
        }
    }
    if (!dev->fault_off_ns) return 0;
    const Uint64 now = SDL_GetTicksNS(); /* clock-guard: wall */ /* fault window */
    const Uint64 phase = now % dev->fault_period_ns;
    if (phase >= dev->fault_off_ns) return 0;
    Uint64 wait = dev->fault_off_ns - phase;
    if (wait > budget) wait = budget;
    SDL_DelayNS(wait);
    return 1;
}

/** @brief Acquire a swapchain image signalling `sem`, then wait for the submission that last used that image.
 *
 * The acquire waits up to dev->acquire_budget_ns (RHI_ACQUIRE_TIMEOUT_NS when
 * 0), and not at all in capture mode. A timeout counts in acquire_timeouts and acquire_sheds and, unless
 * present_blocking is set, opens the acquire back-off window. OUT_OF_DATE
 * sets needs_recreate; DEVICE_LOST signals device loss. After a successful
 * acquire, the fence of the submission still using the image is waited with
 * a bound (unbounded headless and in capture mode, where it is GPU work and
 * not the display); a timed-out wait sets needs_recreate, since
 * the acquire semaphore is already consumed and only a rebuild resets it. On
 * success the image's owner becomes `*owner_fence`.
 * @param dev Device whose swapchain is acquired from; image_index is written.
 * @param sem Semaphore the acquire signals.
 * @param owner_fence Fence of the submission that will use the image.
 * @return 1 when an image was acquired, 0 when the present must be skipped. */
static int acquire_image(rhi_device *dev, VkSemaphore sem, VkFence *owner_fence)
{
    {
        Uint64 acq_t0 = SDL_GetTicksNS();
        /* Capture mode never waits on the display: an image not free now
         * leaves the frame unpresented. */
        const Uint64 budget = dev->capture_mode ? 0
                            : dev->acquire_budget_ns ? dev->acquire_budget_ns
                            : RHI_ACQUIRE_TIMEOUT_NS;
        VkResult acq = acquire_fault(dev, budget) ? VK_TIMEOUT
                     : vkAcquireNextImageKHR(dev->device, dev->swapchain, budget,
                                             sem,
                                             VK_NULL_HANDLE, &dev->image_index);
        rhi_note_stall_phase("acquire", RHI_BLOCK_ACQUIRE, acq_t0);
        if (acq == VK_TIMEOUT || acq == VK_NOT_READY) {
            /* No image within the budget: drop this present, keep the sim
             * running. The semaphore was not signalled, so nothing waits on
             * it and the next frame re-acquires cleanly. */
            dev->acquire_timeouts++;
            dev->acquire_sheds++;
            if (!dev->present_blocking)
                dev->acquire_backoff_until_ns = SDL_GetTicksNS() + RHI_ACQUIRE_BACKOFF_NS;
            return 0;
        }
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            dev->needs_recreate = true;
            pxt_skip(dev, "acquire out of date");
            return 0;
        } else if (acq == VK_ERROR_DEVICE_LOST) {
            rhi_signal_device_lost("acquire");
            return 0;
        } else if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
            RHI_LOG("vkAcquireNextImageKHR failed: %d\n", (int)acq);
            return 0;
        }

        /* If a previous frame is still using this image, wait on its fence. */
        if (dev->images_in_flight[dev->image_index] != VK_NULL_HANDLE) {
            /* Bounded, and counted in RHI_BLOCK_IMAGE_FENCE. Not skippable:
             * acquire has already consumed img_avail by this point, and
             * returning here would leave that semaphore signalled with nothing
             * to wait on it. It is almost always already signalled once the
             * slot poll has passed, so the bound is a backstop. */
            Uint64 iif_t0 = SDL_GetTicksNS();
            VkResult iw = fence_result(
                vkWaitForFences(dev->device, 1, &dev->images_in_flight[dev->image_index],
                                VK_TRUE, (dev->headless || dev->capture_mode) ? UINT64_MAX
                                       : dev->present_blocking ? RHI_BLOCKING_FENCE_TIMEOUT_NS
                                       : RHI_IMAGE_FENCE_TIMEOUT_NS),
                "image fence wait");
            rhi_note_stall_phase("image-fence", RHI_BLOCK_IMAGE_FENCE, iif_t0);
            if (iw != VK_SUCCESS) {
                /* The image is still owned by a frame in flight and the
                 * acquire semaphore is already consumed: the only clean exit
                 * is a rebuild, which waits the device idle and starts the
                 * semaphores over. Recording against the image would be a
                 * device loss. */
                RHI_LOG("image fence timed out (%.1f ms): rebuilding the swapchain\n",
                        (double)(SDL_GetTicksNS() - iif_t0) / 1e6);
                dev->needs_recreate = true;
                return 0;
            }
        }
        dev->images_in_flight[dev->image_index] = *owner_fence;
    }
    return 1;
}

/** @brief Return whether the window's pixel size is no longer the one the swapchain was created for.
 *
 * A window reporting no size (minimized, or before it maps) counts as
 * unchanged.
 * @param dev Windowed device.
 * @return true when the window has a nonzero pixel size other than the
 *         creation-time size. */
static bool window_left_swapchain_size(const rhi_device *dev)
{
    int ww = 0, wh = 0;
    if (!SDL_GetWindowSizeInPixels(dev->window, &ww, &wh) || ww <= 0 || wh <= 0) return false;
    return ww != dev->sc_win_w || wh != dev->sc_win_h;
}

/** @brief Rebuild the swapchain when needs_recreate is set, unless the window is mid-resize or inside the rebuild back-off.
 *
 * Windowed only; headless always returns true. needs_recreate is also set
 * when the window's pixel size is no longer the one the swapchain was created
 * for, so no present goes out at an extent the window does not have. While
 * the window's pixel size changed since the last call, or has been stable for less than
 * RHI_RESIZE_SETTLE_NS, no rebuild runs (a discrete resize skips this wait).
 * A rebuild is also withheld until recreate_backoff_until_ns, which each
 * attempt sets RHI_ACQUIRE_BACKOFF_NS ahead, so an OUT_OF_DATE reported on
 * every present does not rebuild on every present.
 * @param dev Device whose swapchain is checked.
 * @return true when a usable swapchain exists, false when the caller must
 *         skip its present. */
static bool ensure_swapchain(rhi_device *dev)
{
    /* A window resized since the swapchain was built gets a rebuild before
     * its next present. Not every window system refuses a present of the old
     * extent (OUT_OF_DATE); one that accepts it shows the image at its old
     * size in the corner of the window, or scaled. */
    if (!dev->headless) consume_display_change(dev);
    if (!dev->needs_recreate && !dev->headless && window_left_swapchain_size(dev))
        dev->needs_recreate = true;
    if (dev->needs_recreate && !dev->headless) {
        /* Rate-limit. A live resize reports OUT_OF_DATE on every frame, and
         * each recreate is a vkDeviceWaitIdle plus a full swapchain teardown
         * and rebuild - doing that per frame stalls the sim for as long as the
         * user drags the edge. Retry on an interval and skip the present in
         * between; the last attempt before the size settles is the one that
         * sticks. */
        /* Do not rebuild while the window is still being dragged. Presenting
         * against a stale extent reports OUT_OF_DATE every frame, so without
         * this the rate-limit below just paces a 40-50 ms teardown-and-rebuild
         * forever. Skipping the present costs nothing the user can see during
         * a resize, and keeps the sim off this call entirely. */
        int cw = 0, ch = 0;
        if (!dev->resize_discrete &&
            SDL_GetWindowSizeInPixels(dev->window, &cw, &ch) && cw > 0 && ch > 0) {
            if (cw != dev->last_seen_w || ch != dev->last_seen_h) {
                dev->last_seen_w = cw;
                dev->last_seen_h = ch;
                dev->size_stable_since_ns = SDL_GetTicksNS();
                pxt_skip(dev, "size moving");
                return false;
            }
            if (SDL_GetTicksNS() - dev->size_stable_since_ns < RHI_RESIZE_SETTLE_NS) {
                pxt_skip(dev, "settle");
                return false;
            }
        }
        if (dev->recreate_backoff_until_ns &&
            SDL_GetTicksNS() < dev->recreate_backoff_until_ns) {
            pxt_skip(dev, "recreate back-off");
            return false;
        }
        Uint64 t0 = SDL_GetTicksNS();
        bool ok = recreate(dev);
        if (ok && dev->resize_discrete) {
            /* Consumed. A later OUT_OF_DATE (the compositor delivering the
             * size in two steps) rebuilds again through the normal path. */
            dev->resize_discrete = false;
            SDL_GetWindowSizeInPixels(dev->window, &dev->last_seen_w,
                                      &dev->last_seen_h);
            dev->size_stable_since_ns = SDL_GetTicksNS();
        }
        rhi_note_stall_phase("recreate", RHI_BLOCK_RECREATE, t0);
        dev->recreate_backoff_until_ns = SDL_GetTicksNS() + RHI_ACQUIRE_BACKOFF_NS;
        if (!ok) return false;
    }
    return true;
}

/** @brief Free retired pipelines, textures and buffers no submitted frame reads.
 *
 * A frame RHI_FRAMES_IN_FLIGHT serials back has had its fence waited by the
 * time the current serial began, so an entry retired at serial R is freed
 * once s_frame_serial reaches R + RHI_FRAMES_IN_FLIGHT. A retired pipeline
 * loses its pipeline and layout, a retired texture goes through
 * rhi_destroy_texture, and a retired buffer is unmapped, its memory freed
 * and its buffer destroyed.
 * @param dev Device whose retired lists are walked.
 * @param all true frees every entry; the caller has waited the device idle. */
static void retired_reclaim(rhi_device *dev, bool all)
{
    for (struct rhi_retired_pipe **pp = &dev->retired_pipes; *pp; ) {
        struct rhi_retired_pipe *r = *pp;
        if (!all && r->serial + RHI_FRAMES_IN_FLIGHT > s_frame_serial) { pp = &r->next; continue; }
        *pp = r->next;
        if (r->pipeline) vkDestroyPipeline(dev->device, r->pipeline, NULL);
        if (r->layout)   vkDestroyPipelineLayout(dev->device, r->layout, NULL);
        free(r);
    }
    for (rhi_texture **tp = &dev->retired_tex; *tp; ) {
        rhi_texture *t = *tp;
        if (!all && t->retire_serial + RHI_FRAMES_IN_FLIGHT > s_frame_serial) { tp = &t->retire_next; continue; }
        *tp = t->retire_next;
        rhi_destroy_texture(dev, t);
    }
    struct rhi_retired_buf **pp = &dev->retired;
    while (*pp) {
        struct rhi_retired_buf *r = *pp;
        if (!all && r->serial + RHI_FRAMES_IN_FLIGHT > s_frame_serial) { pp = &r->next; continue; }
        *pp = r->next;
        vkUnmapMemory(dev->device, r->memory);
        vkFreeMemory(dev->device, r->memory, NULL);
        vkDestroyBuffer(dev->device, r->buffer, NULL);
        free(r);
    }
}

/** @brief Retire the frame slot's previous submission: wait on its fence, wait with a bound, or poll it, by the device's mode.
 *
 * Do not block on the slot's previous submission on the inline path; poll
 * it. Under FIFO this fence retires at the display's consumption rate, so
 * waiting on it is waiting for a vblank. Whichever thread presents is then
 * paced by the monitor, and when that thread also runs the sim the sim
 * clock follows the display. Skipping is safe: this runs before
 * vkAcquireNextImageKHR, so nothing has been consumed and no semaphore is
 * left pending. The queue self-paces to the display rate either way; the
 * difference is whether the excess is absorbed by shedding a present or by
 * stalling the caller.
 *
 * Headless and capture mode wait without bound: there is no display to
 * pace them, the readback depends on the slot having retired, and a shed
 * would lose a frame the capture owes. A dedicated present thread
 * (present_blocking) and a caller passing @p bounded wait up to
 * RHI_BLOCKING_FENCE_TIMEOUT_NS. A busy poll or an expired bounded wait
 * counts in fence_sheds. VK_ERROR_DEVICE_LOST refuses in every mode.
 * @param dev Device whose current slot fence is checked.
 * @param bounded true to wait up to the bound where the device would poll.
 * @return true when the slot's fence has signalled. */
static bool slot_fence_retired(rhi_device *dev, bool bounded)
{
    Uint64 t0 = SDL_GetTicksNS();
    if (dev->headless || dev->capture_mode) {
        if (fence_result(vkWaitForFences(dev->device, 1, &dev->in_flight[dev->frame],
                                         VK_TRUE, UINT64_MAX), "fence wait") != VK_SUCCESS)
            return false;
    } else if (dev->present_blocking || bounded) {
        /* A dedicated present thread is paced by the display by design:
         * waiting here costs the sim nothing. Bounded so a dead compositor
         * still returns control, and the bound is honoured: proceeding past
         * a timed-out wait resets a fence and re-records a command buffer the
         * GPU is still executing, which is a device loss a few submits
         * later. */
        VkResult fw = fence_result(vkWaitForFences(dev->device, 1, &dev->in_flight[dev->frame],
                                                   VK_TRUE, RHI_BLOCKING_FENCE_TIMEOUT_NS),
                                   "fence wait");
        if (fw != VK_SUCCESS) {
            dev->fence_sheds++;
            rhi_note_stall_phase("fence-wait", RHI_BLOCK_FENCE, t0);
            return false;
        }
    } else if (fence_result(vkGetFenceStatus(dev->device, dev->in_flight[dev->frame]),
                            "fence poll") != VK_SUCCESS) {
        dev->fence_sheds++;
        rhi_note_stall_phase("fence-poll", RHI_BLOCK_FENCE, t0);
        return false;
    }
    rhi_note_stall_phase("fence-wait", RHI_BLOCK_FENCE, t0);
    return true;
}

/** @brief Acquire the frame's swapchain image unless the acquire back-off is open, and decide whether the frame may begin.
 *
 * Headless acquires nothing. Inside the back-off window opened by a failed
 * acquire (inline presenter only) the present is skipped without blocking
 * and counted in acquire_sheds, so a compositor that is not handing out
 * images cannot pace the sim. Outside capture mode a refused acquire or an
 * open back-off refuses the frame; in capture mode the frame still begins,
 * unacquired, and renders into off_image for the readback.
 * @param dev Device beginning a frame; frame_acquired is written.
 * @param acquire Whether this frame takes a swapchain image.
 * @return true when the frame may begin. */
static bool frame_image_step(rhi_device *dev, bool acquire)
{
    dev->frame_acquired = false;
    if (dev->headless) return true;
    if (!dev->present_blocking && dev->acquire_backoff_until_ns) {
        if (SDL_GetTicksNS() < dev->acquire_backoff_until_ns) {
            dev->acquire_sheds++;
            return dev->capture_mode;
        }
        dev->acquire_backoff_until_ns = 0;
    }
    if (!acquire) return true;
    /* Bounded, not UINT64_MAX. Under FIFO this normally returns within a
     * vblank, but Windows stops handing out images for the whole of a live
     * resize (the modal loop inside DefWindowProc). An unbounded wait here
     * parks whichever thread presents, and that thread may also run the sim.
     * Timing out lets the caller skip the present and keep ticking. */
    dev->frame_acquired = acquire_image(dev, dev->img_avail[dev->frame],
                                        &dev->in_flight[dev->frame]) != 0;
    return dev->frame_acquired || (dev->capture_mode && !s_device_lost);
}

/** @brief Open the current slot's command buffer for a frame whose slot fence has retired.
 *
 * Resets the slot fence and command buffer, begins recording, advances the
 * frame serial and resets the per-frame recording state.
 * @param dev Device whose current slot opens. */
static void frame_open(rhi_device *dev)
{
    vkResetFences(dev->device, 1, &dev->in_flight[dev->frame]);

    VkCommandBuffer cb = dev->cmd[dev->frame];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    /* The frame serial counts frames that actually began: slot fence
     * retired, image acquired, command buffer recording. Scene retirement
     * relies on "a frame RHI_FRAMES_IN_FLIGHT serials back has had its fence
     * waited"; counting refused begins could advance the serial without
     * observing the oldest slot's fence and free arena ranges a
     * still-executing submission reads. Every counted begin is followed by
     * its submit, so slot alternation is exact and the promise holds. */
    s_frame_serial++;
    census_frame();
    retired_reclaim(dev, false);
    pcache_autosave(dev);
    dev->vsets[0][dev->frame].used = 0;   /* the slot's pair and trio sets have retired */
    dev->vsets[1][dev->frame].used = 0;
    dev->vsets[2][dev->frame].used = 0;
    dev->cur_layout = VK_NULL_HANDLE; /* a layout bound last frame may be retired */
    dev->frame_active = true;
    dev->draw_calls = 0;
    dev->bias_bound = false;          /* a fresh command buffer binds nothing */
    dev->pass_open = false;
    dev->compute_open = false;
    dev->compute_dispatches = 0;
    if (dev->gpu_timers) {
        /* Reset must be recorded outside a render pass - here, before any. */
        vkCmdResetQueryPool(cb, dev->gpu_qp[dev->frame], 0, RHI_GPU_ZONES_MAX * 2);
        dev->gpu_zone_sp = 0;   /* fresh nesting stack for this frame */
    }
}

/** @brief Begin recording a frame: retire the slot's previous submission, optionally acquire a swapchain image, reset and begin the command buffer.
 *
 * The slot fence is retired by slot_fence_retired (waited headless and in
 * capture mode, bounded under a blocking presenter, polled inline). After a
 * device loss every call refuses. In capture mode a swapchain that cannot
 * be used yet (a pending rebuild, a resize settling, the rebuild back-off)
 * or a refused acquire does not refuse the frame: it begins without an
 * image, and rhi_end_frame submits it without a present, unless a failed
 * rebuild left the offscreen framebuffer destroyed, which refuses the frame.
 * @param dev Device to begin the frame on.
 * @param acquire Whether to acquire a swapchain image (windowed only).
 * @return true when a command buffer is recording. */
static bool begin_frame_impl(rhi_device *dev, bool acquire)
{
    seam_check("begin_frame");
    if (s_device_lost) return false;
    const bool sc_ok = ensure_swapchain(dev);
    if (!sc_ok && !(dev->capture_mode && dev->off_fb)) return false;
    if (!slot_fence_retired(dev, false)) return false;

    /* This slot's previous submission has fully retired - its timestamp
     * queries are ready for a block-free harvest. */
    if (dev->gpu_timers) gpu_timers_harvest(dev);

    if (!frame_image_step(dev, acquire && sc_ok)) return false;
    frame_open(dev);
    return true;
}

bool rhi_begin_frame_offline(rhi_device *dev)
{
    seam_check("begin_frame_offline");
    if (s_device_lost) return false;
    if (!slot_fence_retired(dev, true)) return false;
    if (dev->gpu_timers) gpu_timers_harvest(dev);
    dev->frame_acquired = false;
    frame_open(dev);
    return true;
}

static void image_barrier(VkCommandBuffer cb, VkImage img,
                          VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage);

/** @brief Set the depth-bias factors, recording them now when a depth_bias pipeline is bound and at the next bind of one otherwise.
 *
 * A bound pipeline without depth_bias holds the state static; recording
 * the dynamic value under it is legal but makes a later rebind of the same
 * pipeline depend on drivers restoring its static state. Deferring keeps
 * every recorded value under a pipeline that takes it, with the value each
 * depth_bias draw sees unchanged.
 * @param dev Device, inside a frame.
 * @param constant Constant factor.
 * @param clamp Clamp.
 * @param slope Slope factor. */
static void depth_bias_set(rhi_device *dev, float constant, float clamp, float slope)
{
    dev->bias_pending[0] = constant;
    dev->bias_pending[1] = clamp;
    dev->bias_pending[2] = slope;
    dev->bias_owed = !dev->bias_bound;
    if (dev->bias_bound) vkCmdSetDepthBias(dev->cmd[dev->frame], constant, clamp, slope);
}

void rhi_begin_offscreen(rhi_device *dev, float r, float g, float b, float a)
{
    if (!dev->frame_active) return;
    dev->clear[0] = r; dev->clear[1] = g; dev->clear[2] = b; dev->clear[3] = a;
    VkClearValue cv[2];
    cv[0].color.float32[0] = r; cv[0].color.float32[1] = g;
    cv[0].color.float32[2] = b; cv[0].color.float32[3] = a;
    cv[1].depthStencil.depth = dev->depth_clear[RHI_DEPTH_SCENE]; cv[1].depthStencil.stencil = 0; /* far */
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = dev->off_rp,
        .framebuffer = dev->off_fb,
        .renderArea = { .offset = {0, 0}, .extent = dev->off_extent },
        .clearValueCount = 2,
        .pClearValues = cv,
    };
    record_pending_inits(dev);
    vkCmdBeginRenderPass(dev->cmd[dev->frame], &rp, VK_SUBPASS_CONTENTS_INLINE);
    dev->pass_open = true;
    dev->cur_extent = dev->off_extent;   /* render area of this (offscreen) pass */
    dev->cur_depth_clear = cv[1].depthStencil.depth;

    /* Y-flipped viewport (Vulkan clip-space Y is down) so NDC/UV match the
     * GL/texture convention the ported PSX shaders assume. Dynamic so a
     * resize needs no pipeline rebuild. */
    VkViewport vp = {
        .x = 0.0f,
        .y = (float)dev->off_extent.height,
        .width = (float)dev->off_extent.width,
        .height = -(float)dev->off_extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D scissor = { .offset = {0, 0}, .extent = dev->off_extent };
    vkCmdSetViewport(dev->cmd[dev->frame], 0, 1, &vp);
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &scissor);
    /* Belt-only: a defined value if a depth_bias pipeline is somehow bound
     * first in the pass. NOT a guarantee - binding any static-state pipeline
     * invalidates dynamic depth bias, so callers must set it after every
     * depth_bias pipeline bind (see rhi.h). */
    depth_bias_set(dev, 0.0f, 0.0f, 0.0f);
}

/* Loads off_image from TRANSFER_SRC and resumes the offscreen pass (see rhi.h). */
void rhi_begin_offscreen_load(rhi_device *dev)
{
    if (!dev->frame_active) return;
    image_barrier(dev->cmd[dev->frame], dev->off_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkClearValue cv[2];
    memset(&cv[0], 0, sizeof cv[0]);   /* color LOADs; value ignored */
    cv[1].depthStencil.depth = dev->depth_clear[RHI_DEPTH_SCENE]; cv[1].depthStencil.stencil = 0;
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = dev->off_load_rp,
        .framebuffer = dev->off_fb,
        .renderArea = { .offset = {0, 0}, .extent = dev->off_extent },
        .clearValueCount = 2,
        .pClearValues = cv,
    };
    record_pending_inits(dev);
    vkCmdBeginRenderPass(dev->cmd[dev->frame], &rp, VK_SUBPASS_CONTENTS_INLINE);
    dev->pass_open = true;
    dev->cur_extent = dev->off_extent;
    dev->cur_depth_clear = cv[1].depthStencil.depth;
    VkViewport vp = {
        .x = 0.0f, .y = (float)dev->off_extent.height,
        .width = (float)dev->off_extent.width, .height = -(float)dev->off_extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D scissor = { .offset = {0, 0}, .extent = dev->off_extent };
    vkCmdSetViewport(dev->cmd[dev->frame], 0, 1, &vp);
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &scissor);
}

void rhi_end_offscreen(rhi_device *dev)
{
    if (!dev->frame_active) return;
    vkCmdEndRenderPass(dev->cmd[dev->frame]);
    dev->pass_open = false;
}

/** @brief Record a single-image layout transition with an execution and memory barrier into `cb`.
 *
 * The barrier covers the first mip level and array layer of the color aspect
 * and transfers no queue-family ownership.
 * @param cb Command buffer in the recording state.
 * @param img Image to transition.
 * @param from Layout the image is in.
 * @param to Layout the image moves to.
 * @param src_access Accesses that must complete before the barrier.
 * @param dst_access Accesses that wait for the barrier.
 * @param src_stage Pipeline stages of the earlier accesses.
 * @param dst_stage Pipeline stages of the later accesses. */
static void image_barrier(VkCommandBuffer cb, VkImage img,
                          VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/** @brief Record the blit of an off_extent image onto a swapchain image in TRANSFER_DST layout, fitted to the swapchain extent.
 *
 * At equal extents the blit is 1:1 with @p same_filter. Otherwise the
 * swapchain image is cleared to black and the source is scaled with a
 * linear filter into the largest centered rect of its own aspect ratio, so
 * a pinned offscreen extent shows letterboxed or pillarboxed in a window of
 * another size.
 * @param dev Device whose off_extent and sc_extent are read.
 * @param cb Command buffer in the recording state.
 * @param src Image of off_extent in TRANSFER_SRC_OPTIMAL.
 * @param sc Swapchain image in TRANSFER_DST_OPTIMAL.
 * @param same_filter Filter of the 1:1 blit. */
static void present_blit(rhi_device *dev, VkCommandBuffer cb, VkImage src, VkImage sc,
                         VkFilter same_filter)
{
    const VkExtent2D o = dev->off_extent, d = dev->sc_extent;
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0,0,0}, {(int32_t)o.width, (int32_t)o.height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0,0,0}, {(int32_t)d.width, (int32_t)d.height, 1} },
    };
    if (o.width == d.width && o.height == d.height) {
        vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, same_filter);
        return;
    }
    /* Fit: the axis with the smaller scale fills the swapchain, the other
     * is centered. Products in 64 bits: 4K extents overflow 32. */
    int32_t fw = (int32_t)d.width, fh = (int32_t)d.height;
    if ((uint64_t)d.width * o.height > (uint64_t)d.height * o.width)
        fw = (int32_t)(((uint64_t)d.height * o.width + o.height / 2) / o.height);
    else
        fh = (int32_t)(((uint64_t)d.width * o.height + o.width / 2) / o.width);
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    const int32_t x0 = ((int32_t)d.width - fw) / 2, y0 = ((int32_t)d.height - fh) / 2;
    region.dstOffsets[0] = (VkOffset3D){ x0, y0, 0 };
    region.dstOffsets[1] = (VkOffset3D){ x0 + fw, y0 + fh, 1 };
    const VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    const VkImageSubresourceRange all = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                          .levelCount = 1, .layerCount = 1 };
    vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
}

void rhi_blit_to_swapchain(rhi_device *dev)
{
    /* No image to blit onto: headless, staged, or a capture-mode frame the
     * swapchain could not take. */
    if (!dev->frame_active || !dev->frame_acquired) return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    VkImage sc = dev->sc_images[dev->image_index];

    /* Swapchain image: undefined -> transfer dst. (Offscreen is already
     * TRANSFER_SRC_OPTIMAL via the render pass final layout.) The submit
     * waits for the acquire at TRANSFER, so the transition's first scope is
     * TRANSFER: it then follows the acquire's semaphore wait. */
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    present_blit(dev, cb, dev->off_image, sc, VK_FILTER_LINEAR);

    /* Swapchain image: transfer dst -> present. */
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void rhi_snapshot_offscreen_to_rt(rhi_device *dev, rhi_texture *dst,
                                  int sx, int sy, int sw, int sh)
{
    if (!dev->frame_active || !dst || !dst->is_rt) return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    /* off_image is TRANSFER_SRC (offscreen render-pass final layout). Flip dst
     * from shader-read to transfer-dst, hardware-blit the source region (scaled
     * to fill dst), then flip dst back to shader-read so it stays sampleable. */
    image_barrier(cb, dst->image, dst->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { { sx, sy, 0 }, { sx + sw, sy + sh, 1 } },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)dst->width, (int32_t)dst->height, 1 } },
    };
    vkCmdBlitImage(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    image_barrier(cb, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    dst->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/** @brief Submit the current frame's ended command buffer with no swapchain semaphores and no present, then advance the frame slot.
 *
 * The in_flight fence still gates begin_frame's reuse of the slot and the
 * readback ring's retrieval, and rhi_read_render_target's vkDeviceWaitIdle
 * guarantees completion before a blocking readback.
 * @param dev Device whose current frame is submitted.
 * @param cb The frame's command buffer, already ended.
 * @param what Label for a failed submit. */
static void submit_frame_unpresented(rhi_device *dev, VkCommandBuffer cb, const char *what)
{
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    queue_submit(dev, &si, dev->in_flight[dev->frame], what);
    dev->frame = (dev->frame + 1) % RHI_FRAMES_IN_FLIGHT;
    dev->frame_active = false;
    boot_frame_submitted();
}

void rhi_end_frame(rhi_device *dev)
{
    seam_check("end_frame");
    if (!dev->frame_active) return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    record_pending_inits(dev);
    vkEndCommandBuffer(cb);

    /* No acquired image (headless, or a capture-mode frame the swapchain
     * could not take): no acquire semaphore to wait on and nothing to
     * present. */
    if (!dev->frame_acquired) {
        submit_frame_unpresented(dev, cb, dev->headless ? "headless frame" : "unpresented frame");
        return;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->img_avail[dev->frame],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev->render_done[dev->image_index],
    };
    if (!queue_submit(dev, &si, dev->in_flight[dev->frame], "frame")) {
        dev->frame = (dev->frame + 1) % RHI_FRAMES_IN_FLIGHT;
        dev->frame_active = false;
        return;
    }

    /* Ids start at 1 and must strictly increase per swapchain; a rebuild
     * restarts the sequence, which rhi_present_wait handles by refusing to
     * wait on an id from a swapchain that no longer exists. */
    const uint64_t this_id = dev->present_id_next;
    VkPresentIdKHR pid = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &this_id,
    };
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = dev->has_present_timing ? (const void *)&pid : NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->render_done[dev->image_index],
        .swapchainCount = 1,
        .pSwapchains = &dev->swapchain,
        .pImageIndices = &dev->image_index,
    };
    Uint64 pres_t0 = SDL_GetTicksNS();
    VkResult pr = vkQueuePresentKHR(dev->gfx_queue, &pi);
    note_present(dev);
    pxt_present(dev, NULL, pr, dev->off_image);
    if (dev->has_present_timing && pr >= 0) {
        dev->present_id_submitted = this_id;
        dev->present_id_next++;
    }
    rhi_note_stall_phase("present", RHI_BLOCK_PRESENT, pres_t0);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        dev->needs_recreate = true;
    } else if (pr == VK_ERROR_DEVICE_LOST) {
        rhi_signal_device_lost("present");
    } else if (pr != VK_SUCCESS) {
        RHI_LOG("vkQueuePresentKHR failed: %d\n", (int)pr);
    }

    dev->frame = (dev->frame + 1) % RHI_FRAMES_IN_FLIGHT;
    dev->frame_active = false;
    boot_frame_submitted();
}

bool rhi_begin_frame(rhi_device *dev) { return begin_frame_impl(dev, true); }

/* ---- staged presents ------------------------------------------------ */

/* No acquire: the frame ends in a stage image (see rhi.h). */
bool rhi_begin_frame_staged(rhi_device *dev)
{
    if (dev->headless) return false;
    return begin_frame_impl(dev, false);
}

int rhi_stage_count(void) { return RHI_STAGE_RING; }
uint32_t rhi_stage_gen(const rhi_device *dev) { return dev ? dev->stage_gen : 0; }

void rhi_end_frame_to_stage(rhi_device *dev, int stage)
{
    seam_check("end_frame_to_stage");
    if (!dev->frame_active) return;
    if (dev->headless || stage < 0 || stage >= RHI_STAGE_RING || !dev->stage_image[stage]) {
        rhi_end_frame(dev);
        return;
    }
    VkCommandBuffer cb = dev->cmd[dev->frame];
    VkImage st = dev->stage_image[stage];
    /* Stage image -> transfer dst. Its previous reader was a present step's
     * blit, in an earlier submission on this queue: the barrier's TRANSFER
     * source stage covers that read (barriers order against every prior
     * submission, not only this command buffer). */
    image_barrier(cb, st, dev->stage_layout[stage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  dev->stage_layout[stage] == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  dev->stage_layout[stage] == VK_IMAGE_LAYOUT_UNDEFINED
                      ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0,0,0}, {(int32_t)dev->off_extent.width, (int32_t)dev->off_extent.height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0,0,0}, {(int32_t)dev->off_extent.width, (int32_t)dev->off_extent.height, 1} },
    };
    vkCmdBlitImage(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   st, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    /* And back to transfer src, ready for the present step's blit. */
    image_barrier(cb, st, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    dev->stage_layout[stage] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    record_pending_inits(dev);
    vkEndCommandBuffer(cb);
    /* No swapchain image was acquired for this frame, so nothing to wait on
     * or signal: the fence alone gates the slot's reuse. */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    queue_submit(dev, &si, dev->in_flight[dev->frame], "stage frame");
    dev->frame = (dev->frame + 1) % RHI_FRAMES_IN_FLIGHT;
    dev->frame_active = false;
    boot_frame_submitted();
}

bool rhi_present_stage_acquire(rhi_device *dev)
{
    seam_check("present_stage_acquire");
    if (dev->headless || dev->frame_active || s_device_lost) return false;
    if (dev->pacquired) return true;
    if (!ensure_swapchain(dev)) return false;
    const uint32_t p = dev->pslot;
    /* This ring slot's previous present step must have retired before its
     * command buffer is re-recorded. A dedicated presenter blocks here by
     * design; the bound is honoured (a stale re-record is a device loss). */
    Uint64 t0 = SDL_GetTicksNS();
    VkResult fw = fence_result(vkWaitForFences(dev->device, 1, &dev->pfence[p], VK_TRUE,
                                               RHI_BLOCKING_FENCE_TIMEOUT_NS), "stage fence wait");
    if (fw != VK_SUCCESS) { dev->fence_sheds++; rhi_note_stall_phase("stage-fence", RHI_BLOCK_FENCE, t0); return false; }
    rhi_note_stall_phase("stage-fence", RHI_BLOCK_FENCE, t0);
    if (!acquire_image(dev, dev->pimg_avail[p], &dev->pfence[p])) return false;
    vkResetFences(dev->device, 1, &dev->pfence[p]);
    dev->pacquired = true;
    dev->pacquired_gen = dev->stage_gen;
    dev->pacquired_index = dev->image_index;
    return true;
}

bool rhi_present_stage_submit(rhi_device *dev, int stage)
{
    seam_check("present_stage_submit");
    if (dev->headless || dev->frame_active || s_device_lost) return false;
    if (stage < 0 || stage >= RHI_STAGE_RING) return false;
    if (!dev->pacquired) { if (!rhi_present_stage_acquire(dev)) return false; }
    /* The acquire belongs to the swapchain it was made on: a rebuild in
     * between (stage_gen moved) waited the device idle and started the
     * semaphores over, so the held image is gone with it. */
    if (dev->pacquired_gen != dev->stage_gen) { dev->pacquired = false; return false; }
    if (!dev->stage_image[stage] || dev->stage_layout[stage] != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        return false;                    /* never filled, or rebuilt since; the acquire stays held */
    }
    dev->pacquired = false;
    const uint32_t p = dev->pslot;
    dev->image_index = dev->pacquired_index;
    VkCommandBuffer cb = dev->pcmd[p];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    VkImage sc = dev->sc_images[dev->image_index];
    /* First scope TRANSFER, the stage the submit waits for the acquire at. */
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    present_blit(dev, cb, dev->stage_image[stage], sc, VK_FILTER_NEAREST);
    image_barrier(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->pimg_avail[p],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev->render_done[dev->image_index],
    };
    dev->pslot = (p + 1) % RHI_STAGE_RING;
    if (!queue_submit(dev, &si, dev->pfence[p], "present step")) return false;

    const uint64_t this_id = dev->present_id_next;
    VkPresentIdKHR pid = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &this_id,
    };
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = dev->has_present_timing ? (const void *)&pid : NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->render_done[dev->image_index],
        .swapchainCount = 1,
        .pSwapchains = &dev->swapchain,
        .pImageIndices = &dev->image_index,
    };
    Uint64 pres_t0 = SDL_GetTicksNS();
    VkResult pr = vkQueuePresentKHR(dev->gfx_queue, &pi);
    note_present(dev);
    pxt_present(dev, NULL, pr, dev->stage_image[stage]);
    if (dev->has_present_timing && pr >= 0) {
        dev->present_id_submitted = this_id;
        dev->present_id_next++;
    }
    rhi_note_stall_phase("present", RHI_BLOCK_PRESENT, pres_t0);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        dev->needs_recreate = true;
    } else if (pr == VK_ERROR_DEVICE_LOST) {
        rhi_signal_device_lost("present");
        return false;
    } else if (pr != VK_SUCCESS) {
        RHI_LOG("vkQueuePresentKHR failed: %d\n", (int)pr);
        return false;
    }
    return true;
}

bool rhi_present_stage(rhi_device *dev, int stage)
{
    return rhi_present_stage_acquire(dev) && rhi_present_stage_submit(dev, stage);
}

/* Give back an image held by rhi_present_stage_acquire whose frame will
 * never be submitted. Vulkan has no un-acquire: the only clean exits for
 * an acquired image are a present or a swapchain rebuild. Present it with
 * whatever it already holds -- a frame the display showed a few refreshes
 * ago, on screen for one refresh -- which consumes the armed semaphore and
 * leaves the swapchain whole for the next owner of the display. Without
 * this, a handoff to the inline path strands one armed acquire per
 * handoff, later submits pair that stale semaphore with images it never
 * covered, and the driver faults the process after a few cycles. */
bool rhi_present_stage_release(rhi_device *dev)
{
    seam_check("present_stage_release");
    if (!dev || dev->headless || !dev->pacquired) return true;
    dev->pacquired = false;
    if (s_device_lost) return false;
    if (dev->pacquired_gen != dev->stage_gen) return true;  /* rebuilt: acquire died with the swapchain */
    const uint32_t p = dev->pslot;
    dev->image_index = dev->pacquired_index;
    VkCommandBuffer cb = dev->pcmd[p];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &bi);
    /* PRESENT_SRC -> PRESENT_SRC via UNDEFINED keeps whatever pixels the
     * image last held; no draw, no blit. */
    image_barrier(cb, dev->sc_images[dev->image_index], VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->pimg_avail[p],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev->render_done[dev->image_index],
    };
    dev->pslot = (p + 1) % RHI_STAGE_RING;
    if (!queue_submit(dev, &si, dev->pfence[p], "stage release")) return false;
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &dev->render_done[dev->image_index],
        .swapchainCount = 1,
        .pSwapchains = &dev->swapchain,
        .pImageIndices = &dev->image_index,
    };
    /* No VkPresentIdKHR on this present: it is not a frame anyone waits on,
     * and the id sequence stays with the real submits. */
    VkResult pr = vkQueuePresentKHR(dev->gfx_queue, &pi);
    pxt_present(dev, "release", pr, VK_NULL_HANDLE);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) dev->needs_recreate = true;
    else if (pr == VK_ERROR_DEVICE_LOST) { rhi_signal_device_lost("stage-release"); return false; }
    return true;
}

int rhi_present_timing_supported(const rhi_device *dev)
{
    return dev && dev->has_present_timing ? 1 : 0;
}

int rhi_present_outstanding(const rhi_device *dev)
{
    if (!dev || !dev->has_present_timing || !dev->swapchain) return 0;
    return (int)(dev->present_id_submitted - dev->present_id_seen);
}

int rhi_present_wait(rhi_device *dev, uint64_t timeout_ns, uint64_t *out_id,
                     uint64_t *out_waited_ns)
{
    seam_check("present_wait");
    if (!dev || !dev->has_present_timing || !dev->swapchain) return -1;
    if (dev->needs_recreate) return -1;

    /* A timeout under about a millisecond is answered VK_SUCCESS at once for a
     * present that has NOT been displayed (measured: every first ask after a
     * present, at 0, 1 and 50 us). This function is therefore only called
     * with millisecond-scale timeouts, and never as a poll. */
    /* Confirm ids IN ORDER, oldest first, even on a dedicated presenter that
     * only ever wants the newest: a wait issued on the newest id while an
     * older one was still unconfirmed came back with 0.5 ms-quantised return
     * times (residual to the vblank lattice ~160 us against ~20 us), every
     * time, on the same driver. Confirming the older ids first -- each an
     * instant return -- keeps the wait that matters on its event path. */
    /* Confirm ids in order, and keep going while they come back INSTANTLY.
     *
     * Waiting on the newest present is useless - under FIFO it is one to three
     * refreshes from the screen, past any single sleep budget, so it times out
     * nearly every time. Waiting on the oldest unconfirmed one is right, but
     * one confirmation per call is slower than presents are queued (measured
     * 337/s confirmed against 423/s presented), so the backlog grows and every
     * answer is a frame that flipped long ago.
     *
     * Draining fixes both: an id that has already flipped returns with no
     * blocking, costing a driver call, and the loop stops at the first id that
     * has NOT - which is the next real flip, and the only one whose return
     * time means anything. The drain is bounded so a long backlog cannot turn
     * one measurement into unbounded work on the sim thread. */
    /* Small: every call costs a driver round-trip even when the id is already
     * signalled, and inline (non-eager) this runs on the sim thread. */
    enum { RHI_FLIP_DRAIN_MAX = 8 };
    const Uint64 t_start = SDL_GetTicksNS();
    for (int i = 0; i < RHI_FLIP_DRAIN_MAX; i++) {
        const uint64_t id = dev->present_id_seen + 1;
        if (id > dev->present_id_submitted) return -1;
        const Uint64 elapsed = SDL_GetTicksNS() - t_start;
        if (elapsed >= timeout_ns) return 0;
        const Uint64 t0 = SDL_GetTicksNS();
        VkResult r = vkWaitForPresentKHR(dev->device, dev->swapchain, id,
                                         timeout_ns - elapsed);
        const Uint64 waited = SDL_GetTicksNS() - t0;
        if (r == VK_SUCCESS) {
            dev->present_id_seen = id;
            if (out_id) *out_id = id;
            if (out_waited_ns) *out_waited_ns = (uint64_t)waited;
            /* Blocked, so the caller's clock at return IS the flip. A stale
             * confirmation keeps draining instead. */
            if (waited >= RHI_FLIP_OBSERVED_NS) return 1;
            continue;
        }
        if (r == VK_TIMEOUT) return 0;
        /* SUBOPTIMAL/OUT_OF_DATE arrive here when the surface changed under
         * us; the present path already flags the rebuild, so this only
         * declines. */
        return -1;
    }
    return 0;
}

void rhi_notify_resize(rhi_device *dev)
{
    if (dev) dev->needs_recreate = true;
}

void rhi_notify_resize_immediate(rhi_device *dev)
{
    if (!dev) return;
    dev->needs_recreate = true;
    dev->resize_discrete = true;
    dev->recreate_backoff_until_ns = 0;
    dev->acquire_backoff_until_ns = 0;
}

void rhi_set_vsync(rhi_device *dev, int vsync)
{
    if (!dev) return;
    if (dev->vsync_pref == vsync) return;            /* no change */
    dev->vsync_pref = vsync;                          /* choose_present_mode reads it on recreate */
    if (!dev->headless) dev->needs_recreate = true;  /* rebuild swapchain next begin_frame */
}

void rhi_set_present_mode_name(rhi_device *dev, const char *mode)
{
    if (!dev) return;
    if (!strcmp(dev->present_mode_name, mode ? mode : "")) return;   /* no change */
    snprintf(dev->present_mode_name, sizeof dev->present_mode_name, "%s", mode ? mode : "");
    if (!dev->headless) dev->needs_recreate = true;  /* rebuild swapchain next begin_frame */
}

int rhi_get_vsync(const rhi_device *dev)
{
    return dev ? dev->vsync_pref : -1;
}

bool rhi_get_imgui_info(rhi_device *dev, rhi_imgui_info *out)
{
    if (!dev || !out || dev->headless) return false;
    out->instance        = (void *)dev->instance;
    out->physical_device = (void *)dev->phys;
    out->device          = (void *)dev->device;
    out->queue_family    = dev->gfx_family;
    out->queue           = (void *)dev->gfx_queue;
    out->render_pass     = (uint64_t)dev->off_rp;
    out->pipeline_cache  = (uint64_t)dev->pcache;
    out->image_count     = dev->sc_count >= 2 ? dev->sc_count : 2;
    out->api_version     = VK_API_VERSION_1_2;   /* matches create_instance */
    return true;
}

void *rhi_current_command_buffer(rhi_device *dev)
{
    if (!dev || !dev->frame_active) return NULL;
    return (void *)dev->cmd[dev->frame];
}

uint32_t rhi_max_image_dimension(const rhi_device *dev)
{
    if (!dev) return 0;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev->phys, &props);
    return props.limits.maxImageDimension2D;
}

void rhi_texture_extent(const rhi_texture *tex, int *w, int *h)
{
    if (w) *w = tex ? (int)tex->width  : 0;
    if (h) *h = tex ? (int)tex->height : 0;
}

const char *rhi_device_name(const rhi_device *dev)
{
    return dev ? dev->device_name : "";
}

bool rhi_device_is_software(const rhi_device *dev)
{
    return dev ? dev->device_software : false;
}

uint32_t rhi_device_max_descriptor_sets(const rhi_device *dev)
{
    return dev ? dev->max_desc_sets : 0u;
}

bool rhi_device_has_fill_non_solid(const rhi_device *dev)
{
    return dev ? dev->has_fill_non_solid : false;
}

/* ------------------------------------------------------------------ */
/* Buffers                                                            */
/* ------------------------------------------------------------------ */

/** @brief Create a host-visible, coherent buffer with the given usage, mapped for its lifetime.
 *
 * With prefer_cached the memory type must also be HOST_CACHED, falling back
 * to plain host-visible coherent when no such type exists; the choice is
 * logged. It is set for GPU-to-CPU readback buffers: on discrete GPUs the
 * plain host-visible coherent type is write-combined, where CPU reads of a
 * full image take tens of milliseconds. Upload buffers (vertex, staging),
 * which the CPU only writes, leave it false. Coherent memory in both cases
 * needs no flush or invalidate around the mapping.
 * @param dev Device the buffer is created on.
 * @param bytes Buffer size in bytes.
 * @param usage Vulkan buffer usage flags.
 * @param prefer_cached true to request a HOST_CACHED memory type.
 * @return The mapped buffer, or NULL when buffer creation, memory-type
 *         selection or allocation fails. */
static rhi_buffer *create_host_buffer(rhi_device *dev, size_t bytes,
                                      VkBufferUsageFlags usage, bool prefer_cached)
{
    rhi_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->size = (VkDeviceSize)bytes;

    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev->device, &bi, NULL, &b->buffer) != VK_SUCCESS) {
        RHI_LOG("vkCreateBuffer failed\n"); free(b); return NULL;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev->device, b->buffer, &mr);
    const VkMemoryPropertyFlags base = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t mt = UINT32_MAX;
    if (prefer_cached) {
        mt = find_memtype(dev->phys, mr.memoryTypeBits,
                          base | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        RHI_LOG("readback buffer (%zu bytes): %s\n", bytes,
                mt != UINT32_MAX ? "HOST_CACHED (fast CPU read)"
                                 : "no HOST_CACHED type - falling back to "
                                   "write-combined (slow CPU read)");
    }
    if (mt == UINT32_MAX)
        mt = find_memtype(dev->phys, mr.memoryTypeBits, base);
    if (mt == UINT32_MAX) {
        RHI_LOG("no host-visible memory type\n");
        vkDestroyBuffer(dev->device, b->buffer, NULL); free(b); return NULL;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(dev->device, &mai, NULL, &b->memory) != VK_SUCCESS) {
        RHI_LOG("vkAllocateMemory (buffer) failed\n");
        vkDestroyBuffer(dev->device, b->buffer, NULL); free(b); return NULL;
    }
    vkBindBufferMemory(dev->device, b->buffer, b->memory, 0);
    vkMapMemory(dev->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped);
    b->dev = dev;
    return b;
}

rhi_buffer *rhi_create_vertex_buffer(rhi_device *dev, size_t bytes)
{
    return create_host_buffer(dev, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              /*prefer_cached=*/false);
}

/** @brief Return the memory-type indices OPENPETE_RHI_FAIL_MEMTYPE names, as a bit per index.
 *
 * The variable is <index>[,<index>...], parsed on first call. A device
 * offers at most VK_MAX_MEMORY_TYPES (32) types, so one word holds the
 * whole table. An index outside the table is ignored and parsing stops at
 * a malformed element; an unset variable refuses nothing.
 * @return The mask. */
static uint32_t tc_fail_mask(void)
{
    static uint32_t mask;
    static bool parsed;
    if (!parsed) {
        parsed = true;
        const char *s = getenv("OPENPETE_RHI_FAIL_MEMTYPE");
        while (s && *s) {
            char *end = NULL;
            long v = strtol(s, &end, 10);
            if (end == s) break;
            if (v >= 0 && v < (long)VK_MAX_MEMORY_TYPES) mask |= 1u << (unsigned)v;
            if (*end != ',') break;
            s = end + 1;
        }
    }
    return mask;
}

/** @brief Make one allocation attempt for a device vertex buffer, the only vkAllocateMemory call site of that path.
 *
 * Every attempt on a memory-type index OPENPETE_RHI_FAIL_MEMTYPE names
 * reports VK_ERROR_OUT_OF_DEVICE_MEMORY without reaching the driver, so the
 * fallback order can be exercised on a device whose heaps do not run out, and
 * a device with more host-visible candidates than a heap balloon can fill can
 * have the remaining candidates refused here.
 * @param dev Device to allocate on.
 * @param mai Allocation info; its memoryTypeIndex is checked against
 *            tc_fail_mask.
 * @param out Receives the memory on success.
 * @return The vkAllocateMemory result, or VK_ERROR_OUT_OF_DEVICE_MEMORY for
 *         a refused index. */
static VkResult tc_alloc_try(rhi_device *dev, const VkMemoryAllocateInfo *mai,
                             VkDeviceMemory *out)
{
    if (tc_fail_mask() & (1u << mai->memoryTypeIndex))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return vkAllocateMemory(dev->device, mai, NULL, out);
}

/** @brief Create a host-mapped buffer of the given usage, preferring device-local memory.
 *
 * Every host-visible, host-coherent memory type the buffer accepts is tried,
 * DEVICE_LOCAL types first (rhi_memtype_candidates); the first that
 * allocates wins, and every attempt is logged under @p what.
 * @param dev Device the buffer is created on.
 * @param bytes Buffer size in bytes.
 * @param usage Vulkan buffer usage flags.
 * @param what Name of the buffer kind in log lines.
 * @return The mapped buffer, or NULL when every candidate type fails. */
static rhi_buffer *create_device_mapped_buffer(rhi_device *dev, size_t bytes,
                                               VkBufferUsageFlags usage,
                                               const char *what)
{
    static uint64_t s_uid;
    rhi_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->size = (VkDeviceSize)bytes;
    b->uid = ++s_uid;
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev->device, &bi, NULL, &b->buffer) != VK_SUCCESS) {
        RHI_LOG("vkCreateBuffer failed\n"); free(b); return NULL;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev->device, b->buffer, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(dev->phys, &mp);
    uint32_t cand[VK_MAX_MEMORY_TYPES];
    int ncand = rhi_memtype_candidates(&mp, mr.memoryTypeBits, cand,
                                       (int)(sizeof cand / sizeof cand[0]));
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (int i = 0; i < ncand; i++) {
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = cand[i],
        };
        r = tc_alloc_try(dev, &mai, &b->memory);
        if (r == VK_SUCCESS) {
            RHI_LOG("%s %zu B: type %u flags %x\n", what, bytes,
                    cand[i], (unsigned)mp.memoryTypes[cand[i]].propertyFlags);
            b->device_local = (mp.memoryTypes[cand[i]].propertyFlags &
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            break;
        }
        RHI_LOG("%s %zu B: type %u failed (%d)\n", what, bytes,
                cand[i], (int)r);
    }
    if (r != VK_SUCCESS) {
        RHI_LOG("%s %zu B: no memory type of %d allocated\n",
                what, bytes, ncand);
        vkDestroyBuffer(dev->device, b->buffer, NULL); free(b); return NULL;
    }
    vkBindBufferMemory(dev->device, b->buffer, b->memory, 0);
    vkMapMemory(dev->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped);
    b->dev = dev;
    return b;
}

int rhi_buffer_device_local(const rhi_buffer *b)
{
    return b && b->device_local;
}

rhi_buffer *rhi_create_vertex_buffer_device(rhi_device *dev, size_t bytes)
{
    return create_device_mapped_buffer(dev, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       "device vertex buffer");
}

rhi_buffer *rhi_create_storage_buffer_device(rhi_device *dev, size_t bytes)
{
    return create_device_mapped_buffer(dev, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       "device storage buffer");
}

rhi_buffer *rhi_create_vertex_buffer_dynamic_gens(rhi_device *dev, size_t bytes,
                                                  uint32_t gens)
{
    if (gens < 1) gens = 1;
    rhi_buffer *b = create_host_buffer(dev, bytes * RHI_FRAMES_IN_FLIGHT * gens,
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       /*prefer_cached=*/false);
    if (b) {
        b->ring_stride = (VkDeviceSize)bytes; b->gens = gens;
        b->slot_hwm = calloc((size_t)RHI_FRAMES_IN_FLIGHT * gens, sizeof(*b->slot_hwm));
        if (!b->slot_hwm) { rhi_destroy_buffer(dev, b); return NULL; }
    }
    return b;
}

rhi_buffer *rhi_create_vertex_buffer_dynamic(rhi_device *dev, size_t bytes)
{
    return rhi_create_vertex_buffer_dynamic_gens(dev, bytes, 1);
}

size_t rhi_vertex_buffer_slot_bytes(const rhi_buffer *buf)
{
    if (!buf) return 0;
    return (size_t)(buf->ring_stride ? buf->ring_stride : buf->size);
}

/** @brief Return whether OPENPETE_RING_GROW_FULL_COPY asks a ring growth to copy every slot whole; read once.
 * @return 1 when set, non-empty and not starting with '0', else 0. */
static int ring_grow_full_copy(void)
{
    static OP_ONCE_INT v = -1;
    int r = op_once_int_get(&v);
    if (r < 0) {
        const char *e = getenv("OPENPETE_RING_GROW_FULL_COPY");
        r = op_once_int_fill(&v, (e && *e && *e != '0') ? 1 : 0);
    }
    return r;
}

/** @brief Raise a ring slot's written high-water mark to the end of a write.
 * @param buf Ring buffer.
 * @param slot Slot index.
 * @param end End offset of the write within the slot. */
static void ring_note_write(rhi_buffer *buf, uint32_t slot, VkDeviceSize end)
{
    if (buf->slot_hwm && buf->slot_hwm[slot] < end) buf->slot_hwm[slot] = end;
}

bool rhi_vertex_buffer_reserve(rhi_device *dev, rhi_buffer *buf, size_t bytes)
{
    if (!buf) return false;
    if (!buf->ring_stride) return (VkDeviceSize)bytes <= buf->size;
    if ((VkDeviceSize)bytes <= buf->ring_stride) return true;
    seam_check("vertex_buffer_reserve");   /* the retire list is the display owner's */
    VkDeviceSize stride = buf->ring_stride;
    while (stride < (VkDeviceSize)bytes) stride *= 2;
    const uint32_t slots = RHI_FRAMES_IN_FLIGHT * (buf->gens ? buf->gens : 1);
    struct rhi_retired_buf *r = calloc(1, sizeof(*r));
    rhi_buffer *nb = r ? create_host_buffer(dev, (size_t)(stride * slots),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                            /*prefer_cached=*/false)
                       : NULL;
    if (!nb) {
        free(r);
        RHI_LOG("vertex ring buffer: growth to %llu B per slot FAILED; holding %llu B\n",
                (unsigned long long)stride, (unsigned long long)buf->ring_stride);
        return false;
    }
    /* Every slot and generation keeps its bytes: a range written earlier in
     * this frame and bound after the growth reads the same data. The copy
     * stops at each slot's written high-water mark: the bytes above it were
     * never written, and reading them out of write-combined memory is the
     * cost of a growth (about 140 MB/s). OPENPETE_RING_GROW_FULL_COPY=1
     * copies every slot whole. */
    VkDeviceSize copied = 0;
    for (uint32_t i = 0; i < slots; i++) {
        const VkDeviceSize n = ring_grow_full_copy() ? buf->ring_stride : buf->slot_hwm[i];
        if (n) memcpy((uint8_t *)nb->mapped + stride * i,
                      (const uint8_t *)buf->mapped + buf->ring_stride * i, (size_t)n);
        copied += n;
    }
    r->buffer = buf->buffer; r->memory = buf->memory; r->serial = s_frame_serial;
    r->next = dev->retired; dev->retired = r;
    RHI_LOG("vertex ring buffer grew %llu -> %llu B per slot; copied %llu of %llu B\n",
            (unsigned long long)buf->ring_stride, (unsigned long long)stride,
            (unsigned long long)copied, (unsigned long long)(buf->ring_stride * slots));
    buf->buffer = nb->buffer; buf->memory = nb->memory; buf->mapped = nb->mapped;
    buf->size = nb->size; buf->ring_stride = stride;
    free(nb);
    return true;
}

void rhi_update_buffer_range(rhi_buffer *buf, const void *data,
                             size_t offset, size_t bytes)
{
    if (!buf || !buf->mapped) return;
    if (g_openpete_guest_running) op_gpu_audit_map_write(bytes);
    VkDeviceSize cap = buf->ring_stride ? buf->ring_stride : buf->size;
    if ((VkDeviceSize)offset >= cap) return;
    if ((VkDeviceSize)(offset + bytes) > cap) bytes = (size_t)(cap - offset);
    if (!buf->ring_stride) {
        memcpy((uint8_t *)buf->mapped + offset, data, bytes);
        return;
    }
    if (buf->dev && buf->dev->frame_active) {
        const uint32_t slot = ring_write_slot(buf);
        memcpy((uint8_t *)buf->mapped + buf->ring_stride * slot + offset, data, bytes);
        ring_note_write(buf, slot, (VkDeviceSize)(offset + bytes));
    } else {
        for (uint32_t i = 0; i < RHI_FRAMES_IN_FLIGHT * (buf->gens ? buf->gens : 1); i++) {
            memcpy((uint8_t *)buf->mapped + buf->ring_stride * i + offset,
                   data, bytes);
            ring_note_write(buf, i, (VkDeviceSize)(offset + bytes));
        }
    }
}

void rhi_update_buffer(rhi_buffer *buf, const void *data, size_t bytes)
{
    if (!buf || !buf->mapped) return;
    if (g_openpete_guest_running) op_gpu_audit_map_write(bytes);
    VkDeviceSize cap = buf->ring_stride ? buf->ring_stride : buf->size;
    if ((VkDeviceSize)bytes > cap) bytes = (size_t)cap;
    if (!buf->ring_stride) {
        memcpy(buf->mapped, data, bytes);
        return;
    }
    if (buf->dev && buf->dev->frame_active) {
        /* In-frame: write only the current frame's slot (its fence was waited
         * in rhi_begin_frame, so no in-flight GPU read can see this write). */
        const uint32_t slot = ring_write_slot(buf);
        memcpy((uint8_t *)buf->mapped + buf->ring_stride * slot, data, bytes);
        ring_note_write(buf, slot, (VkDeviceSize)bytes);
    } else {
        /* Outside a frame (init / rare re-layout under rhi_wait_idle): fill
         * every slot so any frame parity binds the same content. */
        for (uint32_t i = 0; i < RHI_FRAMES_IN_FLIGHT * (buf->gens ? buf->gens : 1); i++) {
            memcpy((uint8_t *)buf->mapped + buf->ring_stride * i, data, bytes);
            ring_note_write(buf, i, (VkDeviceSize)bytes);
        }
    }
}

void rhi_destroy_buffer(rhi_device *dev, rhi_buffer *buf)
{
    if (!buf) return;
    if (buf->memory) {
        vkUnmapMemory(dev->device, buf->memory);
        vkFreeMemory(dev->device, buf->memory, NULL);
    }
    if (buf->buffer) vkDestroyBuffer(dev->device, buf->buffer, NULL);
    free(buf->slot_hwm);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* Sampled textures: the R16_UINT VRAM model + RGBA8 (frame readback)  */
/* ------------------------------------------------------------------ */

/** @brief Create a device-local sampled 2D image of `fmt` with a descriptor set.
 *
 * No host staging is kept: in-frame uploads take the device upload ring and
 * out-of-frame uploads a temporary buffer. The descriptor set binds the image view
 * with the shared NEAREST sampler; its layout (one combined image sampler at
 * binding 0) is format-agnostic, so it serves the VRAM (usampler2D) and the
 * RGBA8 (sampler2D) shaders alike. Outside a frame the image is
 * transitioned to SHADER_READ_ONLY_OPTIMAL by a one-shot submit before
 * return; inside a frame the transition is recorded into the frame's
 * command buffer before its next render pass, so it is sampleable before
 * its first upload either way without draining the queue.
 * @param dev Device the image is created on.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param fmt Image format.
 * @param bpp Bytes per pixel of @p fmt.
 * @return The texture, or NULL when any creation step fails (partial
 *         resources are destroyed). */
static rhi_texture *create_sampled_image(rhi_device *dev, uint32_t width,
                                         uint32_t height, VkFormat fmt,
                                         uint32_t bpp)
{
    rhi_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->width = width; t->height = height; t->bpp = bpp;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { width, height, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, NULL, &t->image) != VK_SUCCESS) {
        RHI_LOG("vram vkCreateImage failed\n"); goto fail;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev->device, t->image, &mr);
    uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { RHI_LOG("vram: no device-local memory\n"); goto fail; }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(dev->device, &mai, NULL, &t->mem) != VK_SUCCESS) {
        RHI_LOG("vram vkAllocateMemory failed\n"); goto fail;
    }
    vkBindImageMemory(dev->device, t->image, t->mem, 0);
    t->census_dev_bytes = (int64_t)mr.size;
    t->census = true;
    s_census.tex_live++;
    s_census.tex_dev_bytes += (int64_t)mr.size;

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    if (vkCreateImageView(dev->device, &vci, NULL, &t->view) != VK_SUCCESS) {
        RHI_LOG("vram vkCreateImageView failed\n"); goto fail;
    }

    t->uploadable = true;

    /* Allocate + point a descriptor set at (this view, the shared sampler). */
    if (!desc_alloc(dev, dev->vram_set_layout, &t->set, &t->set_pool)) {
        RHI_LOG("vram descriptor set allocation failed\n"); goto fail;
    }
    VkDescriptorImageInfo dii = {
        .sampler = dev->vram_sampler,
        .imageView = t->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = t->set, .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w, 0, NULL);

    /* The image must be in SHADER_READ_ONLY before any pass can sample it,
     * upload or not. Inside a frame the transition joins the frame's
     * command buffer (record_pending_inits); outside one it is a one-shot. */
    if (dev->frame_active) {
        t->init_pending = true;
        t->init_next = dev->init_head;
        dev->init_head = t;
        return t;
    }
    VkCommandBuffer cb = one_shot_begin(dev);
    if (!cb) goto fail;
    image_barrier(cb, t->image, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  0, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(dev, cb);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;

fail:
    rhi_destroy_texture(dev, t);
    return NULL;
}

rhi_texture *rhi_create_vram(rhi_device *dev, uint32_t width, uint32_t height)
{
    /* R16_UINT: one uint per PSX VRAM pixel, sampled via texelFetch (the PSX
     * shader does its own integer indexing - no GPU filtering/normalization). */
    return create_sampled_image(dev, width, height, VK_FORMAT_R16_UINT, 2);
}

rhi_texture *rhi_create_u32x4(rhi_device *dev, uint32_t width, uint32_t height)
{
    /* R32G32B32A32_UINT: four 32-bit words per texel, read with texelFetch
     * through a usampler2D (no filtering, no normalization). */
    return create_sampled_image(dev, width, height, VK_FORMAT_R32G32B32A32_UINT, 16);
}

rhi_texture *rhi_create_rgba8(rhi_device *dev, uint32_t width, uint32_t height)
{
    /* R8G8B8A8_UNORM: bytes laid out R,G,B,A - matches a GL_RGBA readback;
     * sampled as a normal sampler2D (e.g. another renderer's frame for A/B). */
    return create_sampled_image(dev, width, height, VK_FORMAT_R8G8B8A8_UNORM, 4);
}

/** @brief Return a render target's depth image usage flags.
 *
 * Always the depth-stencil attachment; sampled for a depth-sampled target,
 * a transfer source for a readable one.
 * @param depth_sampleable Whether the depth image is sampled.
 * @param depth_readable Whether the depth image is read back.
 * @return The usage flags. */
static VkImageUsageFlags rt_depth_usage(bool depth_sampleable, bool depth_readable)
{
    VkImageUsageFlags u = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (depth_sampleable) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (depth_readable)   u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return u;
}

/** @brief Return the colour format, texel bytes and render pass of a render target.
 *
 * A u32 target is R32G32B32A32_UINT (16 bytes) through rt_u32_rp; any other
 * is RHI_RT_FORMAT (4 bytes) through rt_ds_rp when its depth is sampled,
 * else rt_rp.
 * @param dev Device.
 * @param depth_sampleable Whether the depth is sampled.
 * @param u32 Whether the colour format is R32G32B32A32_UINT.
 * @param fmt Receives the colour format.
 * @param bpp Receives the bytes per texel.
 * @param pass Receives the render pass. */
static void rt_kind_of(const rhi_device *dev, bool depth_sampleable, bool u32,
                       VkFormat *fmt, uint32_t *bpp, VkRenderPass *pass)
{
    *fmt = RHI_RT_FORMAT;
    *bpp = 4u;
    *pass = depth_sampleable ? dev->rt_ds_rp : dev->rt_rp;
    if (!u32) return;
    *fmt = VK_FORMAT_R32G32B32A32_UINT;
    *bpp = 16u;
    *pass = dev->rt_u32_rp;
}

/** @brief Give a render target its colour sampling set (its view with the shared sampler), so it binds like any texture; a u32 target is read back, never sampled, and gets none.
 *
 * @param dev Device.
 * @param t The target, its view made.
 * @param u32 Whether the colour format is R32G32B32A32_UINT.
 * @return false when the set could not be allocated. */
static bool rt_color_set(rhi_device *dev, rhi_texture *t, bool u32)
{
    if (u32) return true;
    if (!desc_alloc(dev, dev->vram_set_layout, &t->set, &t->set_pool)) {
        RHI_LOG("rt descriptor set allocation failed\n");
        return false;
    }
    VkDescriptorImageInfo dii = {
        .sampler = dev->vram_sampler, .imageView = t->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->set, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w, 0, NULL);
    return true;
}

/** @brief Create a render target: a color image of the offscreen format, a depth-stencil attachment, a framebuffer and a sampling descriptor set.
 *
 * A depth-sampleable target also gets a depth-aspect view and descriptor
 * set and renders through rt_ds_rp; a depth-readable one's depth image is
 * a transfer source. A u32 target's colour image is R32G32B32A32_UINT
 * (16 bytes per texel), renders through rt_u32_rp and has no sampling set.
 * @param dev Device.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param depth_sampleable Whether the depth is sampled.
 * @param depth_readable Whether the depth is read back.
 * @param u32 Whether the colour format is R32G32B32A32_UINT.
 * @return The target, or NULL when any step fails (partial resources are destroyed). */
static rhi_texture *create_render_target_impl(rhi_device *dev, uint32_t width,
                                              uint32_t height, bool depth_sampleable,
                                              bool depth_readable, bool u32)
{
    if (depth_sampleable) {
        /* SAMPLED_IMAGE is not in the spec's REQUIRED feature set for the
         * combined depth+stencil formats - verify before building a pipeline
         * around sampling it (universally supported in practice; the caller
         * falls back to a plain depth-tested pipe if this ever trips). */
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(dev->phys, dev->depth_format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            RHI_LOG("depth format %d not sampleable on this device\n", (int)dev->depth_format);
            return NULL;
        }
    }
    rhi_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->is_rt = true;
    VkFormat cfmt;
    VkRenderPass pass;
    t->width = width; t->height = height;
    rt_kind_of(dev, depth_sampleable, u32, &cfmt, &t->bpp, &pass);
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    /* Color image: same format as the offscreen/swapchain (so rt_rp & off_rp
     * pipelines are compatible), usable as a color attachment AND a sampler. */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = cfmt,
        .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        /* TRANSFER_DST: rhi_snapshot_offscreen_to_rt blits into a render
         * target (the freeze backdrop capture), and a blit destination needs
         * the usage bit. */
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, NULL, &t->image) != VK_SUCCESS) {
        RHI_LOG("rt vkCreateImage failed\n"); goto fail;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev->device, t->image, &mr);
    uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { RHI_LOG("rt: no device-local memory\n"); goto fail; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = mt };
    if (vkAllocateMemory(dev->device, &mai, NULL, &t->mem) != VK_SUCCESS) {
        RHI_LOG("rt vkAllocateMemory failed\n"); goto fail;
    }
    vkBindImageMemory(dev->device, t->image, t->mem, 0);
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = cfmt,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    if (vkCreateImageView(dev->device, &vci, NULL, &t->view) != VK_SUCCESS) {
        RHI_LOG("rt vkCreateImageView failed\n"); goto fail;
    }

    /* Depth attachment for the RT framebuffer (combined depth+stencil). */
    VkImageCreateInfo dici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = dev->depth_format,
        .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = rt_depth_usage(depth_sampleable, depth_readable),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &dici, NULL, &t->rt_depth_image) != VK_SUCCESS) {
        RHI_LOG("rt depth vkCreateImage failed\n"); goto fail;
    }
    VkMemoryRequirements dmr;
    vkGetImageMemoryRequirements(dev->device, t->rt_depth_image, &dmr);
    uint32_t dmt = find_memtype(dev->phys, dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dmt == UINT32_MAX) { RHI_LOG("rt: no device-local memory (depth)\n"); goto fail; }
    VkMemoryAllocateInfo dmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = dmr.size, .memoryTypeIndex = dmt };
    if (vkAllocateMemory(dev->device, &dmai, NULL, &t->rt_depth_mem) != VK_SUCCESS) {
        RHI_LOG("rt depth vkAllocateMemory failed\n"); goto fail;
    }
    vkBindImageMemory(dev->device, t->rt_depth_image, t->rt_depth_mem, 0);
    VkImageViewCreateInfo dvci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->rt_depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dev->depth_format,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                              .levelCount = 1, .layerCount = 1 },
    };
    if (vkCreateImageView(dev->device, &dvci, NULL, &t->rt_depth_view) != VK_SUCCESS) {
        RHI_LOG("rt depth vkCreateImageView failed\n"); goto fail;
    }
    t->depth_readable = depth_readable;
    t->rt_pass = pass;
    VkImageView fb_atts[2] = { t->view, t->rt_depth_view };
    VkFramebufferCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = t->rt_pass,
        .attachmentCount = 2, .pAttachments = fb_atts,
        .width = width, .height = height, .layers = 1,
    };
    if (vkCreateFramebuffer(dev->device, &fci, NULL, &t->rt_fb) != VK_SUCCESS) {
        RHI_LOG("rt vkCreateFramebuffer failed\n"); goto fail;
    }
    /* Depth-sampled RT: a DEPTH-aspect-only view (a sampler view must select
     * exactly one aspect of a combined depth+stencil image) + a descriptor set
     * so the pre-pass depth binds like any texture (texelFetch .r = depth). */
    if (depth_sampleable) {
        VkImageViewCreateInfo svci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->rt_depth_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = dev->depth_format,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                  .levelCount = 1, .layerCount = 1 },
        };
        if (vkCreateImageView(dev->device, &svci, NULL, &t->rt_depth_sample_view) != VK_SUCCESS) {
            RHI_LOG("rt depth sample vkCreateImageView failed\n"); goto fail;
        }
        if (!desc_alloc(dev, dev->vram_set_layout, &t->depth_set, &t->depth_set_pool)) {
            RHI_LOG("rt depth descriptor set allocation failed\n"); goto fail;
        }
        VkDescriptorImageInfo ddii = {
            .sampler = dev->vram_sampler, .imageView = t->rt_depth_sample_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet dw = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->depth_set,
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ddii,
        };
        vkUpdateDescriptorSets(dev->device, 1, &dw, 0, NULL);
    }

    if (!rt_color_set(dev, t, u32)) goto fail;

    /* Valid sampling layout before the first render (defensive). */
    VkCommandBuffer cb = one_shot_begin(dev);
    image_barrier(cb, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (depth_sampleable) {
        /* Same for the sampleable DEPTH image (both aspects - a combined
         * depth+stencil image transitions as a whole without the
         * separateDepthStencilLayouts feature). */
        VkImageMemoryBarrier db = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = t->rt_depth_image,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                                VK_IMAGE_ASPECT_STENCIL_BIT,
                                  .levelCount = 1, .layerCount = 1 },
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &db);
    }
    one_shot_end(dev, cb);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return t;

fail:
    rhi_destroy_texture(dev, t);
    return NULL;
}

rhi_texture *rhi_create_render_target(rhi_device *dev, uint32_t width, uint32_t height)
{
    return create_render_target_impl(dev, width, height, false, false, false);
}

rhi_texture *rhi_create_render_target_format(rhi_device *dev, uint32_t width, uint32_t height,
                                             rhi_color_format fmt)
{
    return create_render_target_impl(dev, width, height, false, false,
                                     fmt == RHI_COLOR_FORMAT_RGBA32_UINT);
}

rhi_texture *rhi_create_render_target_depth_sampled(rhi_device *dev,
                                                    uint32_t width, uint32_t height)
{
    return create_render_target_impl(dev, width, height, true, false, false);
}

rhi_texture *rhi_create_render_target_depth_readable(rhi_device *dev,
                                                     uint32_t width, uint32_t height)
{
    return create_render_target_impl(dev, width, height, true, true, false);
}

void rhi_bind_depth_sampler(rhi_device *dev, rhi_texture *rt, uint32_t set)
{
    if (!dev->frame_active || !rt || !rt->depth_set) return;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &rt->depth_set, 0, NULL);
}

void rhi_set_viewport(rhi_device *dev, int w, int h)
{
    if (!dev->frame_active) return;
    VkViewport vp = { .x = 0.0f, .y = (float)h, .width = (float)w, .height = -(float)h,
                      .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D sc = { .offset = {0, 0}, .extent = { (uint32_t)w, (uint32_t)h } };
    vkCmdSetViewport(dev->cmd[dev->frame], 0, 1, &vp);
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &sc);
}

void rhi_swapchain_extent(const rhi_device *dev, int *w, int *h)
{
    if (w) *w = dev ? (int)dev->sc_extent.width  : 0;
    if (h) *h = dev ? (int)dev->sc_extent.height : 0;
}

void rhi_offscreen_extent(const rhi_device *dev, int *w, int *h)
{
    if (w) *w = dev ? (int)dev->off_extent.width  : 0;
    if (h) *h = dev ? (int)dev->off_extent.height : 0;
}

void rhi_pin_offscreen_extent(rhi_device *dev, int w, int h)
{
    if (!dev || dev->headless) return;
    dev->off_pin = (w > 0 && h > 0) ? (VkExtent2D){ (uint32_t)w, (uint32_t)h }
                                    : (VkExtent2D){ 0, 0 };
    const VkExtent2D was = dev->off_extent;
    off_extent_sync(dev);
    RHI_LOG("offscreen extent %s at %ux%u (swapchain %ux%u)\n",
            dev->off_pin.width ? "pinned" : "follows the swapchain",
            dev->off_extent.width, dev->off_extent.height,
            dev->sc_extent.width, dev->sc_extent.height);
    if (was.width == dev->off_extent.width && was.height == dev->off_extent.height) return;
    /* The targets still have the old extent; the next frame begin rebuilds
     * them through recreate, without the drag settle (the window did not move). */
    dev->off_extent = was;
    dev->needs_recreate = true;
    dev->resize_discrete = true;
}

void rhi_pass_extent(const rhi_device *dev, int *w, int *h)
{
    if (w) *w = dev ? (int)dev->cur_extent.width  : 0;
    if (h) *h = dev ? (int)dev->cur_extent.height : 0;
}

void rhi_begin_render_target(rhi_device *dev, rhi_texture *rt,
                             float r, float g, float b, float a)
{
    if (!dev->frame_active || !rt || !rt->is_rt) return;
    VkClearValue cv[2];
    cv[0].color.float32[0] = r; cv[0].color.float32[1] = g;
    cv[0].color.float32[2] = b; cv[0].color.float32[3] = a;
    cv[1].depthStencil.depth = dev->depth_clear[rt->depth_class]; cv[1].depthStencil.stencil = 0;
    VkExtent2D ext = { rt->width, rt->height };
    VkRenderPassBeginInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rt->rt_pass ? rt->rt_pass : dev->rt_rp, .framebuffer = rt->rt_fb,
        .renderArea = { .offset = {0, 0}, .extent = ext },
        .clearValueCount = 2, .pClearValues = cv,
    };
    record_pending_inits(dev);
    vkCmdBeginRenderPass(dev->cmd[dev->frame], &rp, VK_SUBPASS_CONTENTS_INLINE);
    dev->pass_open = true;
    dev->cur_extent = ext;   /* render area of this (render-target) pass */
    dev->cur_depth_clear = cv[1].depthStencil.depth;
    /* Y-flipped viewport, matching rhi_begin_offscreen. */
    VkViewport vp = { .x = 0.0f, .y = (float)rt->height,
                      .width = (float)rt->width, .height = -(float)rt->height,
                      .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D scissor = { .offset = {0, 0}, .extent = ext };
    vkCmdSetViewport(dev->cmd[dev->frame], 0, 1, &vp);
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &scissor);
    /* Belt-only: a defined value if a depth_bias pipeline is somehow bound
     * first in the pass. NOT a guarantee - binding any static-state pipeline
     * invalidates dynamic depth bias, so callers must set it after every
     * depth_bias pipeline bind (see rhi.h). */
    depth_bias_set(dev, 0.0f, 0.0f, 0.0f);
}

void rhi_end_render_target(rhi_device *dev, rhi_texture *rt)
{
    if (!dev->frame_active || !rt || !rt->is_rt) return;
    vkCmdEndRenderPass(dev->cmd[dev->frame]);
    dev->pass_open = false;
    rt->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; /* rt_rp finalLayout */
}

void rhi_set_render_target_depth_class(rhi_texture *rt, rhi_depth_class cls)
{
    if (rt && rt->is_rt) rt->depth_class = cls;
}

void rhi_clear_depth(rhi_device *dev)
{
    if (!dev->frame_active) return;
    VkClearAttachment att = {
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .clearValue = { .depthStencil = { .depth = dev->cur_depth_clear, .stencil = 0 } },
    };
    VkClearRect rect = {
        .rect = { .offset = {0, 0}, .extent = dev->cur_extent },
        .baseArrayLayer = 0, .layerCount = 1,
    };
    vkCmdClearAttachments(dev->cmd[dev->frame], 1, &att, 1, &rect);
}

void rhi_clear_stencil(rhi_device *dev)
{
    if (!dev->frame_active) return;
    VkClearAttachment att = {
        .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
        .clearValue = { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
    };
    VkClearRect rect = {
        .rect = { .offset = {0, 0}, .extent = dev->cur_extent },
        .baseArrayLayer = 0, .layerCount = 1,
    };
    vkCmdClearAttachments(dev->cmd[dev->frame], 1, &att, 1, &rect);
}

/* Smallest chunk the device upload ring allocates. A whole-VRAM R16
 * upload (1024x512x2) fits one chunk; larger requests get a chunk of their
 * own size. */
#define RHI_UPLOAD_CHUNK ((size_t)4 << 20)

/** @brief Hand out a `need`-byte range of the device upload ring for an in-frame upload.
 *
 * The range is disjoint from every range handed out in the current frame.
 * The ring keeps a chunk list per frame slot, reset on the slot's first use
 * in a new frame (the slot's fence has retired it); a request no chunk fits
 * appends a chunk of max(RHI_UPLOAD_CHUNK, need) bytes. Offsets are 16-byte
 * aligned (VkBufferImageCopy.bufferOffset is a multiple of the texel size:
 * 2, 4 or 16 bytes).
 * @param dev Device with a frame recording.
 * @param need Bytes required.
 * @param out_buf Receives the staging VkBuffer.
 * @param out_map Receives the mapped pointer to the range.
 * @param out_off Receives the range's offset within @p out_buf.
 * @return false only when a chunk could not be allocated. */
static bool stage_acquire(rhi_device *dev, size_t need,
                          VkBuffer *out_buf, uint8_t **out_map, size_t *out_off)
{
    const uint32_t slot = dev->frame;
    if (dev->up_serial[slot] != s_frame_serial) {
        for (struct rhi_stage_chunk *c = dev->up_ring[slot]; c; c = c->next) c->used = 0;
        dev->up_serial[slot] = s_frame_serial;
    }
    struct rhi_stage_chunk *c = dev->up_ring[slot], *last = NULL;
    for (; c; last = c, c = c->next) {
        size_t off = (c->used + 15) & ~(size_t)15;
        if (off + need <= c->cap) {
            *out_buf = c->buf->buffer; *out_map = (uint8_t *)c->buf->mapped + off; *out_off = off;
            c->used = off + need;
            return true;
        }
    }
    size_t cap = need > RHI_UPLOAD_CHUNK ? need : RHI_UPLOAD_CHUNK;
    struct rhi_stage_chunk *n = calloc(1, sizeof(*n));
    if (!n) return false;
    n->buf = create_host_buffer(dev, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*prefer_cached=*/false);
    if (!n->buf || !n->buf->mapped) { if (n->buf) rhi_destroy_buffer(dev, n->buf); free(n); return false; }
    n->cap = cap; n->used = need;
    s_census.tex_host_bytes += (int64_t)cap;
    if (last) last->next = n; else dev->up_ring[slot] = n;
    *out_buf = n->buf->buffer; *out_map = (uint8_t *)n->buf->mapped; *out_off = 0;
    return true;
}

/* Where one texture upload's bytes go and which command buffer records it. */
typedef struct {
    VkCommandBuffer cb;     /* the frame's, or a one-shot */
    VkBuffer        src;    /* staging buffer */
    uint8_t        *map;    /* mapped range start */
    size_t          off;    /* range offset within src */
    rhi_buffer     *temp;   /* one-shot staging, destroyed by upload_end */
} upload_ctx;

/** @brief Reserve `need` staging bytes and a command buffer for one texture upload.
 *
 * Inside a frame the bytes come from the device upload ring and the copy is
 * recorded into the frame's command buffer, which must not be inside a
 * render pass. Outside a frame a temporary host buffer and a one-shot
 * command buffer are used; upload_end submits, drains and destroys them.
 * @param dev Device.
 * @param need Bytes the upload stages.
 * @param u Receives the context.
 * @return false when staging or the command buffer could not be allocated. */
static bool upload_begin(rhi_device *dev, size_t need, upload_ctx *u)
{
    memset(u, 0, sizeof(*u));
    if (dev->frame_active) {
        if (!stage_acquire(dev, need, &u->src, &u->map, &u->off)) return false;
        u->cb = dev->cmd[dev->frame];
        return true;
    }
    u->temp = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, /*prefer_cached=*/false);
    if (!u->temp || !u->temp->mapped) {
        if (u->temp) rhi_destroy_buffer(dev, u->temp);
        RHI_LOG("upload: staging alloc failed (%zu bytes)\n", need);
        return false;
    }
    u->src = u->temp->buffer;
    u->map = (uint8_t *)u->temp->mapped;
    u->cb = one_shot_begin(dev);
    if (!u->cb) { rhi_destroy_buffer(dev, u->temp); return false; }
    return true;
}

/** @brief Finish an upload started by upload_begin: a one-shot is submitted, drained and its staging destroyed.
 *
 * In-frame uploads need nothing: the frame's submit carries the copy.
 * @param dev Device.
 * @param u Context from upload_begin. */
static void upload_end(rhi_device *dev, upload_ctx *u)
{
    if (!u->temp) return;
    one_shot_end(dev, u->cb);
    rhi_destroy_buffer(dev, u->temp);
    u->temp = NULL;
}

/** @brief Record the initial UNDEFINED -> SHADER_READ_ONLY transition of every texture created during this frame that no upload has transitioned yet.
 *
 * Called before each render pass begins and before the frame's command
 * buffer ends, so no pass samples an image in UNDEFINED layout.
 * @param dev Device with a frame recording. */
static void record_pending_inits(rhi_device *dev)
{
    if (!dev->init_head || !dev->frame_active) return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    for (rhi_texture *t = dev->init_head, *n; t; t = n) {
        n = t->init_next;
        if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            image_barrier(cb, t->image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          0, VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        t->init_pending = false;
        t->init_next = NULL;
    }
    dev->init_head = NULL;
}

/** @brief Upload a whole image to a sampled texture and leave it in SHADER_READ_ONLY.
 *
 * Inside a frame the copy and its barriers are recorded into the frame's
 * command buffer, which must be before the frame's first render pass
 * (callers upload right after rhi_begin_frame or between passes); the
 * payload is staged in the device upload ring. Outside a frame the upload
 * is a blocking one-shot through a temporary buffer.
 * @param dev Device.
 * @param tex Sampled texture; NULL or a render target is ignored.
 * @param pixels width * height * bpp bytes. */
static void update_image(rhi_device *dev, rhi_texture *tex, const void *pixels)
{
    if (!tex || !tex->uploadable) return;
    if (!pixels) { RHI_LOG("update_image: NULL pixels (tex %ux%u)\n", tex->width, tex->height); return; }
    const size_t bytes = (size_t)tex->width * tex->height * tex->bpp;
    upload_ctx u;
    if (!upload_begin(dev, bytes, &u)) return;
    memcpy(u.map, pixels, bytes);
    image_barrier(u.cb, tex->image, tex->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {
        .bufferOffset = u.off,
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { tex->width, tex->height, 1 },
    };
    vkCmdCopyBufferToImage(u.cb, u.src, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(u.cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    upload_end(dev, &u);
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void rhi_update_vram(rhi_device *dev, rhi_texture *tex, const uint16_t *pixels)
{
    update_image(dev, tex, pixels);
}

/* Most rects one dirty-rect upload records as regions of one
 * vkCmdCopyBufferToImage. */
#define RHI_VRAM_MAX_RECTS 96
/** @brief Upload pre-clamped rects to a texture in one barrier pair around one N-region copy.
 *
 * The rects' texels are packed into staging in upload order (each rect's
 * rows contiguous, rects at 4-byte-aligned offsets), copied row by row from
 * the full-stride `mirror` or in one block from `packed`, which already has
 * that layout; the regions describe it. Rects with a non-positive size are
 * skipped. Past RHI_VRAM_MAX_RECTS rects, or when the packed payload exceeds
 * the image-sized staging, a mirror source falls back to a whole-image
 * upload and a packed source uploads nothing. In-frame and one-shot paths
 * as in update_image; the texture ends in SHADER_READ_ONLY.
 * @param dev Device.
 * @param tex Texture; NULL or a render target is ignored.
 * @param mirror Full-stride source, or NULL when @p packed is given.
 * @param packed Packed source, or NULL when @p mirror is given.
 * @param rects Rects, pre-clamped to the texture extent.
 * @param nrects Rect count; 0 or less does nothing. */
static void update_vram_rects_impl(rhi_device *dev, rhi_texture *tex,
                                   const uint16_t *mirror, const uint8_t *packed,
                                   const rhi_rect *rects, int nrects)
{
    if (!tex || !tex->uploadable || nrects <= 0) return;
    /* Whole-image fallbacks need a mirror; a packed source has none and is
     * batched to fit by its caller, so an oversize packed batch is dropped. */
    if (nrects > RHI_VRAM_MAX_RECTS) { if (mirror) update_image(dev, tex, mirror); return; }
    const size_t cap = (size_t)tex->width * tex->height * tex->bpp;
    size_t need = 0;
    for (int i = 0; i < nrects; i++) {
        if (rects[i].w <= 0 || rects[i].h <= 0) continue;
        need = (need + 3) & ~(size_t)3;
        need += (size_t)rects[i].w * rects[i].h * tex->bpp;
    }
    if (need == 0) return;
    if (need > cap) { if (mirror) update_image(dev, tex, mirror); return; }

    upload_ctx u;
    if (!upload_begin(dev, need, &u)) return;
    VkCommandBuffer cb = u.cb;
    uint8_t *map = u.map;
    const size_t src_off = u.off;

    image_barrier(cb, tex->image, tex->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy regions[RHI_VRAM_MAX_RECTS];
    uint32_t nregions = 0;
    size_t off = 0;
    for (int i = 0; i < nrects; i++) {
        const int rx = rects[i].x, ry = rects[i].y, rw = rects[i].w, rh = rects[i].h;
        if (rw <= 0 || rh <= 0) continue;
        off = (off + 3) & ~(size_t)3;               /* bufferOffset: 4-byte aligned */
        uint8_t *dst = map + off;
        const size_t row = (size_t)rw * tex->bpp;
        if (packed) memcpy(dst, packed + off, row * (size_t)rh);
        else
            for (int r = 0; r < rh; r++)
                memcpy(dst + (size_t)r * row,
                       (const uint8_t *)mirror + (((size_t)(ry + r) * tex->width + rx) * tex->bpp),
                       row);
        regions[nregions] = (VkBufferImageCopy){
            .bufferOffset = src_off + off,
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .imageOffset = { rx, ry, 0 },
            .imageExtent = { (uint32_t)rw, (uint32_t)rh, 1 },
        };
        nregions++;
        off += row * rh;
    }
    vkCmdCopyBufferToImage(cb, u.src, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nregions, regions);
    image_barrier(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    upload_end(dev, &u);
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void rhi_update_vram_rects(rhi_device *dev, rhi_texture *tex,
                           const uint16_t *mirror,
                           const rhi_rect *rects, int nrects)
{
    update_vram_rects_impl(dev, tex, mirror, NULL, rects, nrects);
}

void rhi_update_vram_rects_packed(rhi_device *dev, rhi_texture *tex,
                                  const uint8_t *packed, size_t packed_bytes,
                                  const rhi_rect *rects, int nrects)
{
    (void)packed_bytes;
    if (!tex) return;
    /* The shared body falls back to a whole-image upload past its region cap
     * or when the payload outgrows the (image-sized) staging slot, and that
     * fallback needs a mirror a packed source does not have. Split the batch
     * on both bounds instead: overlapping dirty rects (a level load rewrites
     * the same texture pages many times before one present drains them) pack
     * to more bytes than the image holds. */
    const size_t cap = (size_t)tex->width * tex->height * tex->bpp;
    while (nrects > 0) {
        int n = 0; size_t off = 0;
        while (n < nrects && n < RHI_VRAM_MAX_RECTS) {
            const size_t next = ((off + 3) & ~(size_t)3) +
                                (size_t)rects[n].w * rects[n].h * tex->bpp;
            if (n > 0 && next > cap) break;
            off = next; n++;
        }
        update_vram_rects_impl(dev, tex, NULL, packed, rects, n);
        off = (off + 3) & ~(size_t)3;
        packed += off; rects += n; nrects -= n;
    }
}


void rhi_update_rgba8(rhi_device *dev, rhi_texture *tex, const void *pixels)
{
    update_image(dev, tex, pixels);
}

/* Sub-rect uploads from TIGHT per-rect source buffers (each rects[i].pixels
 * is w*h*4 bytes) - the atlas-append path. Same staging/in-frame semantics
 * as update_image; payloads are packed into one staging slot and copied in
 * one barrier pair. Rects that don't fit the remaining slot are dropped
 * (the caller re-queues them next frame). */
void rhi_update_rgba8_rects(rhi_device *dev, rhi_texture *tex,
                            const rhi_tex_rect_src *rects, int nrects)
{
    if (!tex || !tex->uploadable || nrects <= 0) return;
    const size_t cap = (size_t)tex->width * tex->height * tex->bpp;
    /* The batch this call uploads: rects in order until the next one would
     * overflow the image-sized bound (the caller re-queues the rest). */
    size_t need = 0; int nfit = 0, seen = 0;
    for (int i = 0; i < nrects && seen < 96; i++) {
        const rhi_tex_rect_src *r = &rects[i];
        if (!r->pixels || r->w <= 0 || r->h <= 0) continue;
        size_t bytes = (size_t)r->w * r->h * 4u;
        size_t at = (need + 3) & ~(size_t)3;
        if (at + bytes > cap) break;
        need = at + bytes; nfit = i + 1; seen++;
    }
    if (need == 0) return;
    upload_ctx u;
    if (!upload_begin(dev, need, &u)) return;
    VkCommandBuffer cb = u.cb;
    uint8_t *dst = u.map;
    const size_t src_off = u.off;

    VkBufferImageCopy regions[96];
    int nreg = 0;
    size_t off = 0;
    for (int i = 0; i < nfit && nreg < 96; i++) {
        const rhi_tex_rect_src *r = &rects[i];
        if (!r->pixels || r->w <= 0 || r->h <= 0) continue;
        size_t bytes = (size_t)r->w * r->h * 4u;
        off = (off + 3) & ~(size_t)3;
        memcpy(dst + off, r->pixels, bytes);
        regions[nreg++] = (VkBufferImageCopy){
            .bufferOffset = src_off + off,
            .bufferRowLength = (uint32_t)r->w,
            .bufferImageHeight = (uint32_t)r->h,
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
            .imageOffset = { r->x, r->y, 0 },
            .imageExtent = { (uint32_t)r->w, (uint32_t)r->h, 1 },
        };
        off += bytes;
    }
    if (nreg == 0) { upload_end(dev, &u); return; }

    image_barrier(cb, tex->image, tex->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(cb, u.src, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)nreg, regions);
    image_barrier(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    upload_end(dev, &u);
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/** @brief Copy RHI_RT_FORMAT pixels from a readback staging buffer into an RGBA8 buffer with opaque alpha.
 *
 * Red and blue are swapped for a BGRA internal format and copied as is
 * otherwise; alpha is forced to 0xFF. Used by the blocking screenshot path,
 * the async --record ring, the render-target readback ring and the blocking
 * render-target read.
 * @param dev Device (unused: the internal format is fixed).
 * @param src_ Source pixels, 4 bytes each.
 * @param dst_ Destination RGBA8 pixels.
 * @param w Width in pixels.
 * @param h Height in pixels. */
static void readback_swizzle(const rhi_device *dev, const void *src_, void *dst_,
                             int w, int h)
{
    const uint8_t *src = (const uint8_t *)src_;
    uint8_t *dst = (uint8_t *)dst_;
    (void)dev;
    const int swap_rb = (RHI_RT_FORMAT == VK_FORMAT_B8G8R8A8_UNORM);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        uint8_t c0 = src[i*4+0], c1 = src[i*4+1], c2 = src[i*4+2];
        dst[i*4+0] = swap_rb ? c2 : c0;
        dst[i*4+1] = c1;
        dst[i*4+2] = swap_rb ? c0 : c2;
        dst[i*4+3] = 0xFF;
    }
}

/** @brief Return the staging bytes a color readback of a w x h RHI_RT_FORMAT image takes.
 *
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return w * h * RHI_RT_TEXEL_BYTES. */
static size_t rt_readback_bytes(int w, int h)
{
    return (size_t)w * (size_t)h * RHI_RT_TEXEL_BYTES;
}

/** @brief Count the non-black pixels of a swapchain-sized image in TRANSFER_SRC layout, for the present traces.
 *
 * Waits the device idle and copies the image through the readback staging
 * buffer. A pixel is non-black when any color channel exceeds @p thresh.
 * @param dev Device owning the image.
 * @param img Image of the swapchain extent, in TRANSFER_SRC_OPTIMAL.
 * @param thresh The largest channel value that still counts as black.
 * @return The count, or -1 when staging cannot be allocated. */
static long pxt_nonblack(rhi_device *dev, VkImage img, int thresh)
{
    const int w = (int)dev->off_extent.width, h = (int)dev->off_extent.height;
    const size_t need = rt_readback_bytes(w, h);
    vkDeviceWaitIdle(dev->device);
    if (!dev->readback_buf || dev->readback_buf->size < need) {
        if (dev->readback_buf) rhi_destroy_buffer(dev, dev->readback_buf);
        dev->readback_buf = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }
    rhi_buffer *rb = dev->readback_buf;
    if (!rb) return -1;
    VkCommandBuffer cb = one_shot_begin(dev);
    if (cb == VK_NULL_HANDLE) return -1;
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb->buffer, 1, &region);
    one_shot_end(dev, cb);
    const uint8_t *px = (const uint8_t *)rb->mapped;
    long n = 0;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        n += (px[i * 4] > thresh || px[i * 4 + 1] > thresh || px[i * 4 + 2] > thresh);
    return n;
}

bool rhi_read_offscreen(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h)
{
    /* off_image is swapchain-sized and (for SSAA) already holds the resolved 1×
     * result, so the screenshot matches the present. */
    int w = (int)dev->off_extent.width, h = (int)dev->off_extent.height;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    size_t need = rt_readback_bytes(w, h);
    if (!out || out_bytes < need || w <= 0 || h <= 0) return false;

    /* The offscreen target is in TRANSFER_SRC_OPTIMAL after a rendered frame
     * (rhi_end_offscreen transitions it, the blit reads it as src). Wait for the
     * render to finish before copying - without a submission dependency the copy
     * could race the render's writes to off_image (queue submission order does not
     * guarantee completion order). Keep the device-wait for that correctness; the
     * big per-frame --record cost was the staging buffer allocated+freed EVERY
     * call, so reuse a persistent, grown-on-demand one instead. (A future async
     * readback ring removes the per-frame wait entirely for true real-speed.) */
    vkDeviceWaitIdle(dev->device);
    if (!dev->readback_buf || dev->readback_buf->size < need) {
        if (dev->readback_buf) rhi_destroy_buffer(dev, dev->readback_buf);
        dev->readback_buf = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               /*prefer_cached=*/true);
    }
    rhi_buffer *rb = dev->readback_buf;
    if (!rb) return false;

    VkCommandBuffer cb = one_shot_begin(dev);
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyImageToBuffer(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb->buffer, 1, &region);
    one_shot_end(dev, cb);

    /* The swapchain/offscreen format is B8G8R8A8 (see the format pick); the
     * caller wants R,G,B,A. rb is dev->readback_buf - persistent; freed in
     * rhi_destroy. */
    readback_swizzle(dev, rb->mapped, out, w, h);
    return true;
}

/* ---- async frame readback ring (--record real-speed capture) ------------- */

bool rhi_cycle_readback(rhi_device *dev, uint64_t tag, void *out, size_t out_bytes,
                        int *out_w, int *out_h, uint64_t *out_tag)
{
    if (!dev || !dev->frame_active) return false;
    uint32_t fi = dev->frame;

    /* (1) Retrieve the capture queued the LAST time this slot was used. The copy
     * rode cmd[fi]/in_flight[fi]; rhi_begin_frame already waited on in_flight[fi]
     * this frame, so it has completed and its host-coherent staging is safe to
     * read with no extra GPU wait. */
    bool got = false;
    if (dev->readback_ring[fi].pending && dev->readback_ring[fi].staging &&
        dev->readback_ring[fi].w > 0 && dev->readback_ring[fi].h > 0) {
        int pw = dev->readback_ring[fi].w, ph = dev->readback_ring[fi].h;
        size_t need = rt_readback_bytes(pw, ph);
        if (out && out_bytes >= need) {
            readback_swizzle(dev, dev->readback_ring[fi].staging->mapped, out, pw, ph);
            if (out_w)   *out_w   = pw;
            if (out_h)   *out_h   = ph;
            if (out_tag) *out_tag = dev->readback_ring[fi].tag;
            got = true;
        }
        dev->readback_ring[fi].pending = false;   /* consumed (or dropped if out too small) */
    }

    /* (2) Queue THIS frame's copy into the slot. off_image is TRANSFER_SRC here
     * (render-pass final layout, already read by the blit); another transfer
     * read needs no barrier. The off_rp WAR dependency keeps the next frame from
     * overwriting off_image before this copy runs. */
    int w = (int)dev->off_extent.width, h = (int)dev->off_extent.height;
    if (w > 0 && h > 0) {
        size_t need = rt_readback_bytes(w, h);
        rhi_buffer **slot_staging = &dev->readback_ring[fi].staging;
        if (!*slot_staging || (*slot_staging)->size < need) {
            if (*slot_staging) rhi_destroy_buffer(dev, *slot_staging);
            *slot_staging = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               /*prefer_cached=*/true);
        }
        if (*slot_staging) {
            VkBufferImageCopy region = {
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
            };
            vkCmdCopyImageToBuffer(dev->cmd[fi], dev->off_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   (*slot_staging)->buffer, 1, &region);
            dev->readback_ring[fi].pending = true;
            dev->readback_ring[fi].w = w;
            dev->readback_ring[fi].h = h;
            dev->readback_ring[fi].tag = tag;
        }
    }
    return got;
}

/** @brief Destroy a thumbnail ring slot's image, its memory and its staging buffer.
 *
 * The caller guarantees the slot's last submission completed (its fence
 * was waited, or the device is idle).
 * @param dev Device.
 * @param i The slot. */
static void thumb_slot_free(rhi_device *dev, int i)
{
    if (dev->thumb_ring[i].image) vkDestroyImage(dev->device, dev->thumb_ring[i].image, NULL);
    if (dev->thumb_ring[i].mem) vkFreeMemory(dev->device, dev->thumb_ring[i].mem, NULL);
    if (dev->thumb_ring[i].staging) rhi_destroy_buffer(dev, dev->thumb_ring[i].staging);
    memset(&dev->thumb_ring[i], 0, sizeof dev->thumb_ring[i]);
}

/** @brief Give a thumbnail ring slot an image and a staging buffer of an extent, recreating them when the extent differs.
 *
 * @param dev Device.
 * @param i The slot, whose last submission completed.
 * @param w Width.
 * @param h Height.
 * @return false when an image, allocation or buffer fails (the slot is left empty). */
static bool thumb_slot_ensure(rhi_device *dev, int i, int w, int h)
{
    if (dev->thumb_ring[i].image && dev->thumb_ring[i].iw == w && dev->thumb_ring[i].ih == h)
        return true;
    thumb_slot_free(dev, i);
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { (uint32_t)w, (uint32_t)h, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, NULL, &dev->thumb_ring[i].image) != VK_SUCCESS) {
        dev->thumb_ring[i].image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev->device, dev->thumb_ring[i].image, &mr);
    const uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    if (mt == UINT32_MAX || vkAllocateMemory(dev->device, &mai, NULL, &dev->thumb_ring[i].mem) != VK_SUCCESS) {
        dev->thumb_ring[i].mem = VK_NULL_HANDLE;
        thumb_slot_free(dev, i);
        return false;
    }
    vkBindImageMemory(dev->device, dev->thumb_ring[i].image, dev->thumb_ring[i].mem, 0);
    dev->thumb_ring[i].staging = create_host_buffer(dev, (size_t)w * (size_t)h * 4u,
                                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                    /*prefer_cached=*/true);
    if (!dev->thumb_ring[i].staging) { thumb_slot_free(dev, i); return false; }
    dev->thumb_ring[i].iw = w;
    dev->thumb_ring[i].ih = h;
    return true;
}

/** @brief Copy a thumbnail ring slot's completed capture out as R,G,B,A with opaque alpha and release the slot.
 *
 * The slot's copy completed: rhi_begin_frame waited on its fence. A
 * capture that does not fit @p out is dropped.
 * @param dev Device.
 * @param fi The slot (the current frame).
 * @param out Destination.
 * @param out_bytes Its capacity.
 * @param out_w Receives the width; may be NULL.
 * @param out_h Receives the height; may be NULL.
 * @param out_tag Receives the tag; may be NULL.
 * @return true when a capture was written. */
static bool thumb_retrieve(rhi_device *dev, uint32_t fi, void *out, size_t out_bytes,
                           int *out_w, int *out_h, uint64_t *out_tag)
{
    if (!dev->thumb_ring[fi].pending) return false;
    dev->thumb_ring[fi].pending = false;
    const int pw = dev->thumb_ring[fi].w, ph = dev->thumb_ring[fi].h;
    const size_t need = (size_t)pw * (size_t)ph * 4u;
    if (!out || out_bytes < need) return false;
    uint8_t *dst = (uint8_t *)out;
    memcpy(dst, dev->thumb_ring[fi].staging->mapped, need);
    for (size_t p = 3; p < need; p += 4) dst[p] = 0xFF;
    if (out_w) *out_w = pw;
    if (out_h) *out_h = ph;
    if (out_tag) *out_tag = dev->thumb_ring[fi].tag;
    return true;
}

/** @brief Fit a source extent inside a box at its aspect, rounding, never upscaling and never below 1 x 1.
 * @param sw Source width (> 0).
 * @param sh Source height (> 0).
 * @param max_w Box width (> 0).
 * @param max_h Box height (> 0).
 * @param tw Receives the width.
 * @param th Receives the height. */
static void thumb_fit(int sw, int sh, int max_w, int max_h, int *tw, int *th)
{
    int w = max_w, h = (int)(((int64_t)sh * max_w + sw / 2) / sw);
    if (h > max_h) { h = max_h; w = (int)(((int64_t)sw * max_h + sh / 2) / sh); }
    *tw = w < 1 ? 1 : (w > sw ? sw : w);
    *th = h < 1 ? 1 : (h > sh ? sh : h);
}

bool rhi_cycle_thumb(rhi_device *dev, int queue, int max_w, int max_h, uint64_t tag,
                     void *out, size_t out_bytes, int *out_w, int *out_h, uint64_t *out_tag)
{
    if (!dev || !dev->frame_active) return false;
    const uint32_t fi = dev->frame;
    /* (1) The copy queued the last time this slot cycled. */
    const bool got = thumb_retrieve(dev, fi, out, out_bytes, out_w, out_h, out_tag);
    /* (2) Queue this present's downscale. The offscreen is in TRANSFER_SRC
     * (its render pass's final layout, already read by the blit). */
    const int sw = (int)dev->off_extent.width, sh = (int)dev->off_extent.height;
    if (!queue || sw <= 0 || sh <= 0 || max_w <= 0 || max_h <= 0) return got;
    int tw = 0, th = 0;
    thumb_fit(sw, sh, max_w, max_h, &tw, &th);
    if (!thumb_slot_ensure(dev, (int)fi, tw, th)) return got;
    VkCommandBuffer cb = dev->cmd[fi];
    VkImage img = dev->thumb_ring[fi].image;
    /* The image's previous reader is this slot's last copy, retired by the
     * fence wait, so its contents are discarded from UNDEFINED. */
    image_barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0, 0, 0}, {sw, sh, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0, 0, 0}, {tw, th, 1} },
    };
    vkCmdBlitImage(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    image_barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { (uint32_t)tw, (uint32_t)th, 1 },
    };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dev->thumb_ring[fi].staging->buffer, 1, &copy);
    VkBufferMemoryBarrier to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dev->thumb_ring[fi].staging->buffer,
        .offset = 0, .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &to_host, 0, NULL);
    dev->thumb_ring[fi].pending = true;
    dev->thumb_ring[fi].w = tw;
    dev->thumb_ring[fi].h = th;
    dev->thumb_ring[fi].tag = tag;
    return got;
}

bool rhi_cycle_rt_readback(rhi_device *dev, rhi_texture *rt,
                           void *out, size_t out_bytes, int *out_w, int *out_h)
{
    if (!dev || !dev->frame_active || !rt || !rt->is_rt) return false;
    uint32_t fi = dev->frame;

    /* (1) Retrieve the capture queued the last time this slot cycled (its copy
     * rode cmd[fi] under in_flight[fi], which begin_frame already waited on). */
    bool got = false;
    if (dev->grid_ring[fi].pending && dev->grid_ring[fi].staging &&
        dev->grid_ring[fi].w > 0 && dev->grid_ring[fi].h > 0) {
        int pw = dev->grid_ring[fi].w, ph = dev->grid_ring[fi].h;
        size_t need = rt_readback_bytes(pw, ph);
        if (out && out_bytes >= need) {
            /* Normalize to R,G,B,A byte order (the RT color format may be BGRA). */
            readback_swizzle(dev, dev->grid_ring[fi].staging->mapped, out, pw, ph);
            if (out_w) *out_w = pw;
            if (out_h) *out_h = ph;
            got = true;
        }
        dev->grid_ring[fi].pending = false;
    }

    /* (2) Queue THIS frame's copy. The RT sits in SHADER_READ_ONLY after its
     * render pass (rt_rp finalLayout); round-trip it through TRANSFER_SRC in
     * this frame's command buffer. Call AFTER the pass that renders the RT. */
    int w = (int)rt->width, h = (int)rt->height;
    if (w > 0 && h > 0) {
        size_t need = rt_readback_bytes(w, h);
        rhi_buffer **staging = &dev->grid_ring[fi].staging;
        if (!*staging || (*staging)->size < need) {
            if (*staging) rhi_destroy_buffer(dev, *staging);
            *staging = create_host_buffer(dev, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          /*prefer_cached=*/true);
        }
        if (*staging) {
            VkImageMemoryBarrier to_src = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = rt->image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            vkCmdPipelineBarrier(dev->cmd[fi],
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &to_src);
            VkBufferImageCopy region = {
                .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
            };
            vkCmdCopyImageToBuffer(dev->cmd[fi], rt->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   (*staging)->buffer, 1, &region);
            VkImageMemoryBarrier back = to_src;
            back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(dev->cmd[fi],
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &back);
            dev->grid_ring[fi].pending = true;
            dev->grid_ring[fi].w = w;
            dev->grid_ring[fi].h = h;
        }
    }
    return got;
}

bool rhi_drain_readback(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h, uint64_t *out_tag)
{
    if (!dev) return false;
    /* Shutdown only: block until every queued copy is complete, then hand back
     * the still-pending slots oldest-first (by tag) so the final frames keep
     * monotonic PTS. Called ≤ RHI_FRAMES_IN_FLIGHT times; the wait is cheap once
     * the device is already idle. */
    vkDeviceWaitIdle(dev->device);
    int best = -1;
    for (int i = 0; i < RHI_FRAMES_IN_FLIGHT; i++) {
        if (dev->readback_ring[i].pending && dev->readback_ring[i].staging &&
            dev->readback_ring[i].w > 0 && dev->readback_ring[i].h > 0) {
            if (best < 0 || dev->readback_ring[i].tag < dev->readback_ring[best].tag)
                best = i;
        }
    }
    if (best < 0) return false;
    int w = dev->readback_ring[best].w, h = dev->readback_ring[best].h;
    size_t need = rt_readback_bytes(w, h);
    dev->readback_ring[best].pending = false;
    if (!out || out_bytes < need) return false;
    readback_swizzle(dev, dev->readback_ring[best].staging->mapped, out, w, h);
    if (out_w)   *out_w   = w;
    if (out_h)   *out_h   = h;
    if (out_tag) *out_tag = dev->readback_ring[best].tag;
    return true;
}

/** @brief Read a render target's colour image into host RGBA8 (alpha forced opaque or kept as stored), or its texels as stored.
 *
 * The body of rhi_read_render_target, rhi_read_render_target_raw and
 * rhi_read_render_target_texels.
 * @param dev Device.
 * @param rt Render target, or NULL.
 * @param out Destination; must hold width x height x 4 bytes (mode 2:
 *            width x height texels of the target's size).
 * @param out_bytes Size of @p out in bytes.
 * @param out_w Receives the width; may be NULL.
 * @param out_h Receives the height; may be NULL.
 * @param mode 0: RGBA8 with alpha 0xFF; 1: RGBA8 with the stored alpha;
 *             2: the texels as stored, no swizzle.
 * @return false on the preconditions rhi_read_render_target names. */
static bool read_render_target_color(rhi_device *dev, rhi_texture *rt, void *out,
                                     size_t out_bytes, int *out_w, int *out_h,
                                     int mode)
{
    if (!rt || !rt->is_rt) return false;
    int w = (int)rt->width, h = (int)rt->height;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    const size_t texels = (size_t)w * (size_t)h * rt->bpp;
    size_t need = mode == 2 ? texels : rt_readback_bytes(w, h);
    if (!out || out_bytes < need || w <= 0 || h <= 0) return false;

    /* The RT is left in SHADER_READ_ONLY by rhi_end_render_target. Wait for the
     * frame that rendered it, transition to TRANSFER_SRC, copy to a host buffer,
     * then restore SHADER_READ_ONLY so it stays sampleable / safe to destroy. */
    vkDeviceWaitIdle(dev->device);
    rhi_buffer *rb = create_host_buffer(dev, texels, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        /*prefer_cached=*/true);
    if (!rb) return false;

    VkCommandBuffer cb = one_shot_begin(dev);
    image_barrier(cb, rt->image,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyImageToBuffer(cb, rt->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb->buffer, 1, &region);
    image_barrier(cb, rt->image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    one_shot_end(dev, cb);
    rt->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (mode == 2) memcpy(out, rb->mapped, texels);
    else readback_swizzle(dev, rb->mapped, out, w, h);
    if (mode == 1) {
        const uint8_t *src = (const uint8_t *)rb->mapped;
        uint8_t *dst = (uint8_t *)out;
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) dst[i * 4 + 3] = src[i * 4 + 3];
    }
    rhi_destroy_buffer(dev, rb);
    return true;
}

bool rhi_read_render_target(rhi_device *dev, rhi_texture *rt, void *out,
                            size_t out_bytes, int *out_w, int *out_h)
{
    return read_render_target_color(dev, rt, out, out_bytes, out_w, out_h, 0);
}

bool rhi_read_render_target_raw(rhi_device *dev, rhi_texture *rt, void *out,
                                size_t out_bytes, int *out_w, int *out_h)
{
    return read_render_target_color(dev, rt, out, out_bytes, out_w, out_h, 1);
}

bool rhi_read_render_target_texels(rhi_device *dev, rhi_texture *rt, void *out,
                                   size_t out_bytes, int *out_w, int *out_h)
{
    return read_render_target_color(dev, rt, out, out_bytes, out_w, out_h, 2);
}

bool rhi_read_render_target_depth(rhi_device *dev, rhi_texture *rt, float *out,
                                  size_t out_count, int *out_w, int *out_h)
{
    if (!rt || !rt->is_rt || !rt->depth_readable || !rt->rt_depth_image) return false;
    int w = (int)rt->width, h = (int)rt->height;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    size_t n = (size_t)w * (size_t)h;
    if (!out || out_count < n || w <= 0 || h <= 0) return false;

    /* The depth image is left in SHADER_READ_ONLY by the depth-sampled pass
     * (both aspects: a combined image transitions as a whole). The depth
     * aspect copies out as 4 bytes per texel on both formats: a float on
     * D32_SFLOAT_S8_UINT, 24 unorm bits in the low bytes on
     * D24_UNORM_S8_UINT. */
    vkDeviceWaitIdle(dev->device);
    rhi_buffer *rb = create_host_buffer(dev, n * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        /*prefer_cached=*/true);
    if (!rb) return false;
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = rt->rt_depth_image,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                            VK_IMAGE_ASPECT_STENCIL_BIT,
                              .levelCount = 1, .layerCount = 1 },
    };
    VkCommandBuffer cb = one_shot_begin(dev);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .layerCount = 1 },
        .imageExtent = { (uint32_t)w, (uint32_t)h, 1 },
    };
    vkCmdCopyImageToBuffer(cb, rt->rt_depth_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb->buffer, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    one_shot_end(dev, cb);
    const uint8_t *src = (const uint8_t *)rb->mapped;
    const bool f32 = dev->depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (size_t i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, src + i * 4u, 4);
        if (f32) memcpy(&out[i], &u, 4);
        else     out[i] = (float)(u & 0xFFFFFFu) / 16777215.0f;
    }
    rhi_destroy_buffer(dev, rb);
    return true;
}

void rhi_destroy_texture(rhi_device *dev, rhi_texture *tex)
{
    if (!tex) return;
    if (tex->census) {
        s_census.tex_live--;
        s_census.tex_dev_bytes -= tex->census_dev_bytes;
    }
    if (tex->set)       vkFreeDescriptorSets(dev->device, tex->set_pool, 1, &tex->set);
    if (tex->depth_set) vkFreeDescriptorSets(dev->device, tex->depth_set_pool, 1, &tex->depth_set);
    for (int i = 0; i < RHI_TEX_STORE_SETS; i++)
        if (tex->store_set[i]) vkFreeDescriptorSets(dev->device, tex->store_pool[i], 1, &tex->store_set[i]);
    if (tex->rt_depth_sample_view) vkDestroyImageView(dev->device, tex->rt_depth_sample_view, NULL);
    if (tex->init_pending)
        for (rhi_texture **pp = &dev->init_head; *pp; pp = &(*pp)->init_next)
            if (*pp == tex) { *pp = tex->init_next; break; }
    if (tex->view)    vkDestroyImageView(dev->device, tex->view, NULL);
    if (tex->image)   vkDestroyImage(dev->device, tex->image, NULL);
    if (tex->mem)     vkFreeMemory(dev->device, tex->mem, NULL);
    /* Render-target extras. */
    if (tex->rt_fb)         vkDestroyFramebuffer(dev->device, tex->rt_fb, NULL);
    if (tex->rt_depth_view) vkDestroyImageView(dev->device, tex->rt_depth_view, NULL);
    if (tex->rt_depth_image) vkDestroyImage(dev->device, tex->rt_depth_image, NULL);
    if (tex->rt_depth_mem)  vkFreeMemory(dev->device, tex->rt_depth_mem, NULL);
    free(tex);
}

void rhi_destroy_texture_deferred(rhi_device *dev, rhi_texture *tex)
{
    if (!tex) return;
    tex->retire_serial = s_frame_serial;
    tex->retire_next = dev->retired_tex;
    dev->retired_tex = tex;
}

/* ------------------------------------------------------------------ */
/* Pipelines                                                          */
/* ------------------------------------------------------------------ */

/* rhi_set_create_hooks: read on every creating thread. */
typedef void (*create_begin_fn)(int what, const char *line);
typedef void (*create_end_fn)(void);
static _Atomic(create_begin_fn) s_create_begin;
static _Atomic(create_end_fn)   s_create_end;
static atomic_uint s_create_seq;   /* pipeline creates numbered from 1 */
/* The calling thread's pipeline create in progress (0 none) and its line. */
static _Thread_local unsigned t_create_seq;
static _Thread_local char t_create_line[RHI_CREATE_LINE_MAX];

void rhi_set_create_hooks(const rhi_create_hooks *hooks)
{
    const bool on = hooks && hooks->begin;
    atomic_store_explicit(&s_create_end, on ? hooks->end : NULL, memory_order_release);
    atomic_store_explicit(&s_create_begin, on ? hooks->begin : NULL, memory_order_release);
}

/** @brief Append printf-formatted text to a create line, truncating at RHI_CREATE_LINE_MAX.
 *
 * @param b The line buffer, RHI_CREATE_LINE_MAX bytes.
 * @param n Its length so far; advanced.
 * @param fmt Format. */
static void crumb_put(char *b, size_t *n, const char *fmt, ...)
{
    if (*n >= RHI_CREATE_LINE_MAX - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(b + *n, RHI_CREATE_LINE_MAX - *n, fmt, ap);
    va_end(ap);
    if (w > 0) *n = (*n + (size_t)w < RHI_CREATE_LINE_MAX) ? *n + (size_t)w : RHI_CREATE_LINE_MAX - 1;
}

/** @brief Run the begin hook for a shader module create inside the calling thread's pipeline create: the module's name, size and hash, then the pipeline's line.
 *
 * No-op without hooks.
 * @param name The stage's name.
 * @param n The module's SPIR-V size in bytes.
 * @param hash Its content hash (rhi_pc_bytes_hash). */
static void module_crumb_begin(const char *name, size_t n, uint64_t hash)
{
    const create_begin_fn begin = atomic_load_explicit(&s_create_begin, memory_order_acquire);
    if (!begin) return;
    char b[RHI_CREATE_LINE_MAX];
    size_t k = 0;
    crumb_put(b, &k, "shader-module %s/%zuB/%016" PRIx64 " for %s", name, n, hash,
              t_create_seq ? t_create_line : "(no pipeline create)");
    begin(RHI_CREATE_STEP, b);
}

/** @brief Return the calling thread's breadcrumb to its pipeline create's line after a module create; no-op without hooks or a create in progress. */
static void module_crumb_end(void)
{
    const create_begin_fn begin = atomic_load_explicit(&s_create_begin, memory_order_acquire);
    if (begin && t_create_seq) begin(RHI_CREATE_STEP, t_create_line);
}

/** @brief End the calling thread's pipeline create: run the end hook and forget the line. */
static void pipe_crumb_end(void)
{
    const create_end_fn end = atomic_load_explicit(&s_create_end, memory_order_acquire);
    if (end && t_create_seq) end();
    t_create_seq = 0;
}

/** @brief Build a shader module from an in-memory SPIR-V blob.
 *
 * vkCreateShaderModule requires pCode to be 4-byte aligned, which an embedded
 * `unsigned char[]` is not guaranteed to be, so the blob is always copied into
 * a fresh malloc block (suitably aligned) that is freed after creation. Called
 * for embedded shaders, loose .spv files under OPENPETE_SHADER_DIR, and mod
 * shader words at pipeline compile.
 * @param dev Device the module is created on.
 * @param spv SPIR-V bytes.
 * @param n Size of @p spv in bytes; must be a non-zero multiple of 4.
 * The driver call runs inside a module breadcrumb (module_crumb_begin).
 * @param name Name used in failure logs and the breadcrumb.
 * @param hash The blob's content hash (rhi_pc_bytes_hash), for the breadcrumb.
 * @return The module, or VK_NULL_HANDLE on a bad size, allocation failure or
 *         creation failure. */
static VkShaderModule create_shader_module(rhi_device *dev, const void *spv,
                                           size_t n, const char *name, uint64_t hash)
{
    if (n == 0 || (n % 4) != 0) {
        RHI_LOG("bad SPIR-V size %zu: %s\n", n, name); return VK_NULL_HANDLE;
    }
    uint32_t *code = malloc(n);
    if (!code) { RHI_LOG("oom for shader: %s\n", name); return VK_NULL_HANDLE; }
    memcpy(code, spv, n);
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = n,
        .pCode = code,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    module_crumb_begin(name, n, hash);
    const VkResult r = vkCreateShaderModule(dev->device, &ci, NULL, &m);
    module_crumb_end();
    if (r != VK_SUCCESS) {
        RHI_LOG("vkCreateShaderModule failed: %s\n", name); m = VK_NULL_HANDLE;
    }
    free(code);
    return m;
}

/** @brief Look up an embedded SPIR-V blob by its .spv filename.
 *
 * The filename is the same string the pipeline descs use; the search is
 * linear over g_rhi_embedded_shaders.
 * @param filename .spv filename to match exactly.
 * @param out_size Receives the blob's size in bytes when found.
 * @return The blob's data, or NULL when no embedded shader has that name. */
static const void *find_embedded_shader(const char *filename, size_t *out_size)
{
    for (unsigned i = 0; i < g_rhi_embedded_shader_count; ++i) {
        if (strcmp(g_rhi_embedded_shaders[i].name, filename) == 0) {
            *out_size = g_rhi_embedded_shaders[i].size;
            return g_rhi_embedded_shaders[i].data;
        }
    }
    return NULL;
}

/** @brief Return a stage's SPIR-V bytes: the stored mod words, else OPENPETE_SHADER_DIR/<name>, else the embedded blob of its .spv name.
 *
 * When OPENPETE_SHADER_DIR is set and holds a readable, non-empty
 * <dir>/<name>, that file is read into a buffer the caller frees. A missing
 * or unreadable file logs and falls through to the SPIR-V embedded in the
 * binary (find_embedded_shader).
 * @param words Stored mod words, or NULL.
 * @param nw Their count.
 * @param name .spv name from the pipeline desc, or NULL.
 * @param out_n Receives the byte count, 0 when no source exists (logged).
 * @param owned Receives a buffer for the caller to free, or NULL.
 * @return The bytes, or NULL. */
static const void *stage_bytes(const uint32_t *words, size_t nw, const char *name,
                               size_t *out_n, void **owned)
{
    *out_n = 0;
    *owned = NULL;
    if (words) { *out_n = nw * 4; return words; }
    if (!name) return NULL;
    /* Dev/modding override: OPENPETE_SHADER_DIR=<dir> loads loose .spv from
     * disk (e.g. the build's core/rhi/shaders) so a shader edit applies on the
     * next launch without relinking. Unset in shipping builds, where the
     * embedded SPIR-V baked in at build time is used and the binary needs no
     * shaders/ dir. */
    const char *dir = getenv("OPENPETE_SHADER_DIR");
    if (dir && dir[0]) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        size_t n = 0;
        void *buf = read_whole_file(path, &n);
        if (buf) { *owned = buf; *out_n = n; return buf; }
        RHI_LOG("OPENPETE_SHADER_DIR: %s not readable; using embedded\n", path);
    }
    const void *spv = find_embedded_shader(name, out_n);
    if (!spv) RHI_LOG("no embedded shader: %s\n", name);
    return spv;
}

/* Shared pipeline builder: both public entry points store their shader
 * sources on the rhi_pipeline and land in pipeline_compile, which (re)creates
 * the Vulkan objects from those stored sources - the same path
 * a live rebuild (rhi_rebuild_request) uses to re-bake live pipelines after a spec-constant
 * change. desc->postfx selects the mod-postfx layout - no vertex input
 * (fullscreen triangle from gl_VertexIndex), sets 0..2 = one sampler
 * each, set 3 = the dynamic-UBO pair. */
/* Specialization constant 0 (`DEPTH_REVERSED`): the pipeline's depth class's
 * convention on this device (depth_convention_init): true = reversed
 * z_ndc = 1/vz, false = forward z_ndc = 1 - 1/vz (psx_viewz.glsl
 * op_scene_clip). Declared by the stages that emit scene depth
 * (psx_scene.vert, quad.vert, shadow_decal_tex.vert; terrain.vert reads it
 * from its push constants, rhi_bound_depth_reversed) and by
 * every stage that decodes gl_FragCoord.z back to view-Z (hd_tex_cutout.frag,
 * quad_key.frag, psx_scene_key.frag, terrain_shade.glsl, shadow_decal.frag,
 * shadow_decal_tex.frag, openpete_psx_material.glsl, openpete_psx_decal.glsl)
 * - the decode must follow the encode, which is why the entry is attached to
 * BOTH stages below. A stage that declares no spec constant ignores it. */
bool rhi_depth_reversed(const rhi_device *dev)
{
    return dev && dev->depth_rev[RHI_DEPTH_SCENE];
}

bool rhi_bound_depth_reversed(const rhi_device *dev)
{
    return dev->cur_depth_rev;
}

/* Vertex-stage specialization constant 1 (`AFFINE_UV`): PSX-affine UV/colour
 * interpolation (--texture-mapping affine). Like DEPTH_AFFINE it is captured
 * at pipeline creation - set it before the pipelines are built, or follow a
 * live change with rhi_rebuild_request to re-bake the existing ones.
 * Declared by psx_scene.vert, quad.vert, terrain.vert, terrain_key.vert and
 * moby_rank.vert: the key and rank stages have to select the same texel
 * lookups the scene draw samples, or a raster stamps where the face's own
 * paint discards. */
static VkBool32 s_vs_affine_uv = VK_FALSE;

/* Variant epoch (rhi_pipe_variant_epoch): bumped by each variant setter that
 * changes its value. */
static _Atomic uint32_t s_variant_epoch = 1;

/** @brief Store a variant static and bump the variant epoch when the value changes.
 *
 * @param slot The static.
 * @param v The new value. */
static void variant_store(VkBool32 *slot, VkBool32 v)
{
    if (*slot != v) atomic_fetch_add(&s_variant_epoch, 1u);
    *slot = v;
}

int rhi_depth_format(const rhi_device *dev) { return dev ? (int)dev->depth_format : 0; }

unsigned rhi_depth_pipelines_by_compare(const rhi_device *dev, int compare_op)
{
    return (dev && compare_op >= 0 && compare_op < 8) ? atomic_load(&dev->pipes_by_cmp[compare_op]) : 0u;
}

void rhi_depth_report(const rhi_device *dev)
{
    if (!dev) return;
    RHI_LOG("depth: format %s, scene %s compare %d clear %g, forward compare %d clear %g; "
            "pipelines LESS_OR_EQUAL=%u GREATER_OR_EQUAL=%u ALWAYS=%u\n",
            depth_format_name(dev->depth_format),
            dev->depth_rev[RHI_DEPTH_SCENE] ? "reversed 1/vz" : "forward 1-1/vz",
            (int)dev->depth_cmp[RHI_DEPTH_SCENE], (double)dev->depth_clear[RHI_DEPTH_SCENE],
            (int)dev->depth_cmp[RHI_DEPTH_FORWARD], (double)dev->depth_clear[RHI_DEPTH_FORWARD],
            atomic_load(&dev->pipes_by_cmp[VK_COMPARE_OP_LESS_OR_EQUAL]),
            atomic_load(&dev->pipes_by_cmp[VK_COMPARE_OP_GREATER_OR_EQUAL]),
            atomic_load(&dev->pipes_by_cmp[VK_COMPARE_OP_ALWAYS]));
}
void rhi_set_affine_uv(int on) { variant_store(&s_vs_affine_uv, on ? VK_TRUE : VK_FALSE); }

/* Fragment-stage specialization constant 2 (`NOTEX`): untextured lighting
 * view - the textured scene fragments replace the sampled texel colour with
 * the modulation-neutral grey (128/255), so the output is the raw gouraud
 * vertex colour while transparency cutouts keep their real texel shapes.
 * Debug view; live toggle via rhi_rebuild_request like the others. */
static VkBool32 s_fs_notex = VK_FALSE;
void rhi_set_notex(int on) { variant_store(&s_fs_notex, on ? VK_TRUE : VK_FALSE); }

/* Wireframe view (rhi.h rhi_set_wireframe): the scene pipelines rasterize
 * as lines and bake NOTEX on. Captured at pipeline creation like the
 * specialization constants, so a live change rebuilds every pipeline in
 * place (rhi_rebuild_request); there is no second pipeline set. */
static bool s_wireframe = false;
void rhi_set_wireframe(int on)
{
    if (s_wireframe != (on != 0)) atomic_fetch_add(&s_variant_epoch, 1u);
    s_wireframe = on != 0;
}

/** @brief Return whether a pipeline rasterizes as lines when the wireframe view is in the given state.
 *
 * True only with the view on, for a non-postfx pipeline that is marked
 * wire_scene or tests depth. A depth-tested colour-write-off pipeline
 * without depth_overwrite (the opaque-depth pre-pass, the portal aperture
 * mask) stays filled so its coverage stays whole-face.
 * @param desc Pipeline description.
 * @param wireframe The wireframe view's state.
 * @return true when the pipeline is drawn as lines. */
static bool pipeline_wire_view(const rhi_pipeline_desc *desc, bool wireframe)
{
    if (!wireframe || desc->postfx) return false;
    if (desc->wire_scene) return true;
    return desc->depth_test && (!desc->color_write_off || desc->depth_overwrite);
}

/* Fragment-stage specialization constant 3 (`MOBY_TRUESCOPE`): scopes the
 * key-contest's moby og-key yields by the covering fragment's own TRUE
 * view-Z. Default 1 - og's pull+push OT compounding reads as
 * moby-through-terrain punch-through from ordinary gameplay angles too;
 * 0 restores og's painter for A/B. Live toggle via rhi_rebuild_request
 * like the others. */
static VkBool32 s_fs_moby_truescope = VK_TRUE;
void rhi_set_moby_truescope(int on) { variant_store(&s_fs_moby_truescope, on ? VK_TRUE : VK_FALSE); }

/* Fragment-stage specialization constant 4 (`SLOT_CONTEST`): the key
 * contest compares 128-world ordering-table slot indices instead of world
 * distances for terrain against terrain, an equal slot keeping the fragment;
 * an actor face is decided against terrain by true depth. A per-game constant
 * (game_layout.terrain.slot_ot_contest), set before the pipelines are built
 * and never toggled after. */
static VkBool32 s_fs_slot_contest = VK_FALSE;
void rhi_set_slot_contest(int on) { s_fs_slot_contest = on ? VK_TRUE : VK_FALSE; }

/* Terrain sector-mesh layout (rhi_terrain_vertex, 64 B): integer-packed
 * retained world-space vertices; see rhi.h for the field/bit contract.
 * SINT/UINT formats - terrain.vert consumes ivec/uvec, no normalization. */
#define TERRAIN_VERTEX_ATTRS \
    { .location = 0, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_vertex, px) }, \
    { .location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UINT,     .offset = offsetof(rhi_terrain_vertex, uvf_u) }, \
    { .location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,    .offset = offsetof(rhi_terrain_vertex, rgba_far) }, \
    { .location = 3, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,    .offset = offsetof(rhi_terrain_vertex, rgba_near) }, \
    { .location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = offsetof(rhi_terrain_vertex, mat_far0) }, \
    { .location = 5, .binding = 0, .format = VK_FORMAT_R32_UINT,          .offset = offsetof(rhi_terrain_vertex, face_flags) }, \
    { .location = 6, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_vertex, c0x) }, \
    { .location = 7, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_vertex, c1y) }, \
    { .location = 8, .binding = 0, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_vertex, c2z) }
static const VkVertexInputAttributeDescription k_terrain_attrs[9] = { TERRAIN_VERTEX_ATTRS };
/* terrain_anim: cur verts at binding 0 + the prev-tick twin at binding 1
 * (a parallel array, same vertex indexing - see rhi_terrain_prev). */
static const VkVertexInputBindingDescription k_terrain_anim_binds[2] = {
    { .binding = 0, .stride = sizeof(rhi_terrain_vertex),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
    { .binding = 1, .stride = sizeof(rhi_terrain_prev),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
};
static const VkVertexInputAttributeDescription k_terrain_anim_attrs[14] = {
    TERRAIN_VERTEX_ATTRS,
    { .location = 9,  .binding = 1, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_prev, px) },
    { .location = 10, .binding = 1, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_prev, c0x) },
    { .location = 11, .binding = 1, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_prev, c1y) },
    { .location = 12, .binding = 1, .format = VK_FORMAT_R16G16B16A16_SINT, .offset = offsetof(rhi_terrain_prev, c2z) },
    { .location = 13, .binding = 1, .format = VK_FORMAT_R32G32_UINT,       .offset = offsetof(rhi_terrain_prev, rgb_far) },
};
/* terrain_near: cur verts at binding 0 + the emitted-tri member context
 * at binding 1 (a parallel array - see rhi_terrain_nearctx). */
static const VkVertexInputBindingDescription k_terrain_near_binds[2] = {
    { .binding = 0, .stride = sizeof(rhi_terrain_vertex),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
    { .binding = 1, .stride = sizeof(rhi_terrain_nearctx),
      .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
};
static const VkVertexInputAttributeDescription k_terrain_near_attrs[13] = {
    TERRAIN_VERTEX_ATTRS,
    { .location = 9,  .binding = 1, .format = VK_FORMAT_R8G8B8A8_UINT, .offset = offsetof(rhi_terrain_nearctx, a_uf) },
    { .location = 10, .binding = 1, .format = VK_FORMAT_R32G32_UINT,   .offset = offsetof(rhi_terrain_nearctx, a_far) },
    { .location = 11, .binding = 1, .format = VK_FORMAT_R8G8B8A8_UINT, .offset = offsetof(rhi_terrain_nearctx, b_uf) },
    { .location = 12, .binding = 1, .format = VK_FORMAT_R32G32_UINT,   .offset = offsetof(rhi_terrain_nearctx, b_far) },
};
#undef TERRAIN_VERTEX_ATTRS
/* terrain_cells: one INSTANCE-rate binding of cell records (rhi_terrain_cell;
 * see rhi.h), no per-vertex binding - the vertex stage derives every cell
 * vertex from its record and gl_VertexIndex. Locations 0-1 carry the twelve
 * int16 corners (c0x..c3z, two to a word) then the face flags and record
 * info as raw words, unpacked by cells_fetch; 2 the far colours, 3 the
 * previous-corner record word and a zero word, 4 the near colours, 5-12 the eight cell tiles; the anim
 * records append the previous corners, packed the same way, at 13-14. The
 * corners are read as words so the anim layout fits 15 locations. */
#define CELL_TILE_ATTR(i) \
    { .location = 5 + (i), .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, \
      .offset = offsetof(rhi_terrain_cell, tile) + (i) * sizeof(rhi_terrain_cell_tile) }
static const VkVertexInputAttributeDescription k_terrain_cell_attrs[15] = {
    { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = offsetof(rhi_terrain_cell, c0x) },
    { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = offsetof(rhi_terrain_cell, c2z) },
    { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = offsetof(rhi_terrain_cell, far_rgb) },
    { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_UINT,       .offset = offsetof(rhi_terrain_cell, fog_a) },
    { .location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = offsetof(rhi_terrain_cell, near_rgb) },
    CELL_TILE_ATTR(0), CELL_TILE_ATTR(1), CELL_TILE_ATTR(2), CELL_TILE_ATTR(3),
    CELL_TILE_ATTR(4), CELL_TILE_ATTR(5), CELL_TILE_ATTR(6), CELL_TILE_ATTR(7),
    { .location = 13, .binding = 0, .format = VK_FORMAT_R32G32B32A32_UINT, .offset = offsetof(rhi_terrain_cell_anim, p0x) },
    { .location = 14, .binding = 0, .format = VK_FORMAT_R32G32_UINT,       .offset = offsetof(rhi_terrain_cell_anim, p2z) },
};
_Static_assert(offsetof(rhi_terrain_cell, c2z) == 16 &&
               offsetof(rhi_terrain_cell, face_flags) == 24 &&
               offsetof(rhi_terrain_cell, info) == 28 &&
               offsetof(rhi_terrain_cell_anim, p2z) == offsetof(rhi_terrain_cell_anim, p0x) + 16,
               "cell record corner words: terrain.vert cells_fetch unpacks this layout");
#undef CELL_TILE_ATTR
static const VkVertexInputBindingDescription k_terrain_cell_bind = {
    .binding = 0, .stride = sizeof(rhi_terrain_cell), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
};
static const VkVertexInputBindingDescription k_terrain_cell_anim_bind = {
    .binding = 0, .stride = sizeof(rhi_terrain_cell_anim), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
};

/** @brief Fill the vertex input state of a terrain pipeline.
 *
 * The layout is the plain sector mesh (one per-vertex binding, 9
 * attributes), a cell-record flavour (one per-instance binding, 13
 * attributes, 15 with anim), or the anim or near twin pair (two bindings,
 * 14 or 13 attributes), selected by terrain_cells, then terrain_anim, then
 * terrain_near. `vi` arrives holding the one-binding default, whose binding
 * the plain mesh replaces.
 * @param desc Terrain pipeline description.
 * @param vi Vertex input state to overwrite. */
static void terrain_vertex_input(const rhi_pipeline_desc *desc,
                                 VkPipelineVertexInputStateCreateInfo *vi)
{
    static const VkVertexInputBindingDescription plain = {
        .binding = 0, .stride = sizeof(rhi_terrain_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    vi->vertexBindingDescriptionCount = 1;
    vi->pVertexBindingDescriptions = &plain;
    vi->vertexAttributeDescriptionCount = 9;
    vi->pVertexAttributeDescriptions = k_terrain_attrs;
    if (desc->terrain_cells) {
        vi->pVertexBindingDescriptions = desc->terrain_anim ? &k_terrain_cell_anim_bind
                                                            : &k_terrain_cell_bind;
        vi->vertexAttributeDescriptionCount = desc->terrain_anim ? 15 : 13;
        vi->pVertexAttributeDescriptions = k_terrain_cell_attrs;
    } else if (desc->terrain_anim) {
        vi->vertexBindingDescriptionCount = 2;
        vi->pVertexBindingDescriptions = k_terrain_anim_binds;
        vi->vertexAttributeDescriptionCount = 14;
        vi->pVertexAttributeDescriptions = k_terrain_anim_attrs;
    } else if (desc->terrain_near) {
        vi->vertexBindingDescriptionCount = 2;
        vi->pVertexBindingDescriptions = k_terrain_near_binds;
        vi->vertexAttributeDescriptionCount = 13;
        vi->pVertexAttributeDescriptions = k_terrain_near_attrs;
    }
}

/* Descriptor set layouts per pipeline kind, as pipeline_set_layouts writes
 * them to out[0..6].
 *
 * set 0 (uses_vram) + optional set 1 (uses_vram2, e.g. the diff pass's
 * reference sampler) both reuse the single-binding vram set layout.
 * postfx: sets 0..2 samplers + set 3 the dynamic-UBO pair. terrain: set 0 =
 * VRAM sampler, set 3 = the UBO pair (globals + sector params); sets 1/2 are
 * declared for layout continuity but never bound/used, and terrain keeps its
 * own two blocks at 5/6 - set 3 binding 1 is the mod's, and a set views one
 * buffer per binding. set1_vram_depth replaces set 1 with the two-sampler
 * layout; uniform_set2 replaces set 2 with the single dynamic-UBO layout;
 * uniform_set5 puts the single dynamic-UBO layout at set 5 above the sets the
 * other flags give (the keyed scene pipes' sets 0..4, then the portal cover
 * planes); pair_set56 puts the two-sampler pair layout at sets 5 and 6 (the
 * moby front-shell and intruder images of the _shell variants); uniform_set7
 * puts the dynamic-UBO layout at set 7 above a material layout's seven sets
 * (the portal profile of a mod material); pair_set7 puts the two-sampler
 * pair layout at set 7 above the terrain's seven sets (the actor key image
 * and its owner colour of the stock terrain stage); ssbo_set7 and ssbo_set2
 * put the single dynamic storage binding at set 7 or set 2 (the terrain
 * layout's unused set 2).
 *
 * material: the mod layout PLUS the engine's occlusion inputs, which a
 * material's fragment stage runs before the mod's colour maths. They sit
 * ABOVE the UBO pair (sets 4..6) rather than replacing sets 1/2 so a
 * material's own uniform block stays at set 3 binding 1 - the one part of
 * the layout mod source actually writes. */
/** @brief Return the larger of two unsigned 32-bit values.
 *
 * @param a First value.
 * @param b Second value.
 * @return max(@p a, @p b). */
static uint32_t rhi_max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

/** @brief Replace the single-sampler sets of a kind's layout that hold several images with the pair or trio layout.
 *
 * The key-resolve scene layouts (uses_vram3 outside the terrain, postfx and
 * mod kinds) hold the terrain key set at set 2 (trio: key depth, drain
 * records, key owner attributes) and, once uses_vram4 declares the moby key
 * sets, the actor key set at set 1 (quad: the flat and the per-pixel actor
 * key images, each depth with its owner colour). A material layout holds
 * the terrain key set at set 2 and the actor key set at set 5 (quad). A
 * terrain layout pairs the terrain key depth
 * with its owner attributes at set 1.
 * @param dev Device holding the set layouts.
 * @param desc Pipeline description.
 * @param out The pipeline's eight set layouts, rewritten in place. */
static void pair_set_layouts(const rhi_device *dev, const rhi_pipeline_desc *desc,
                             VkDescriptorSetLayout out[8])
{
    if (desc->material_pipeline) {
        out[2] = dev->trio_set_layout;
        out[5] = dev->quad_set_layout;
        return;
    }
    if (desc->terrain_pipeline) {
        out[1] = dev->pair_set_layout;
        return;
    }
    if (!desc->uses_vram3 || desc->postfx || desc->mod_pipeline)
        return;
    out[2] = dev->trio_set_layout;
    if (desc->uses_vram4) out[1] = dev->quad_set_layout;
}

/** @brief Return how many descriptor sets, from set 0, a pipeline kind's own layout uses.
 *
 * Material: sets 0..3 plus the og keys and rank at 4..6. Terrain: sets 0..3
 * plus the HD atlas at 4 and the two terrain blocks at 5/6. Postfx and mod:
 * sets 0..3. Otherwise the highest declared VRAM-kind set. The per-set
 * flags of k_set_flags raise the count further.
 * @param desc Pipeline description.
 * @return The kind's set count. */
static uint32_t kind_set_count(const rhi_pipeline_desc *desc)
{
    if (desc->material_pipeline || desc->terrain_pipeline) return 7u;
    if (desc->postfx || desc->mod_pipeline) return 4u;
    if (desc->uses_vram5) return 5u;
    if (desc->uses_vram4) return 4u;
    if (desc->uses_vram3) return 3u;
    if (desc->uses_vram) return desc->uses_vram2 ? 2u : 1u;
    return 0u;
}

/* The descriptor set layouts a per-set flag selects. */
typedef enum { SET_FLAG_PAIR, SET_FLAG_UBO1, SET_FLAG_SSBO1, SET_FLAG_TEXSSBO } set_flag_layout;

/* One per-set flag of rhi_pipeline_desc: when the bool at `flag` is set, set
 * `set` takes `layout` and the pipeline layout spans sets 0..set. A flag
 * that names two sets has two rows. Rows apply in order, a later row
 * overriding an earlier one on the same set. */
typedef struct {
    size_t          flag;     /* offsetof the bool in rhi_pipeline_desc */
    uint8_t         set;
    set_flag_layout layout;
} set_flag_row;

static const set_flag_row k_set_flags[] = {
    { offsetof(rhi_pipeline_desc, set1_vram_depth), 1u, SET_FLAG_PAIR },
    { offsetof(rhi_pipeline_desc, uniform_set2),    2u, SET_FLAG_UBO1 },
    { offsetof(rhi_pipeline_desc, uniform_set5),    5u, SET_FLAG_UBO1 },
    { offsetof(rhi_pipeline_desc, pair_set56),      5u, SET_FLAG_PAIR },
    { offsetof(rhi_pipeline_desc, pair_set56),      6u, SET_FLAG_PAIR },
    { offsetof(rhi_pipeline_desc, uniform_set7),    7u, SET_FLAG_UBO1 },
    { offsetof(rhi_pipeline_desc, pair_set7),       7u, SET_FLAG_PAIR },
    { offsetof(rhi_pipeline_desc, ssbo_set7),       7u, SET_FLAG_SSBO1 },
    { offsetof(rhi_pipeline_desc, ssbo_set2),       2u, SET_FLAG_SSBO1 },
    { offsetof(rhi_pipeline_desc, tex_ssbo_set2),   2u, SET_FLAG_TEXSSBO },
};

/** @brief Write the descriptor set layouts of a pipeline described by `desc` to out[0..7].
 *
 * The kind gives the base layout (the table in the comment above
 * rhi_max_u32) and its set count (kind_set_count); each set flag row of
 * k_set_flags then places its layout and extends the count to its set;
 * the kind's image pairs (pair_set_layouts) apply last.
 * @param dev Device holding the set layouts.
 * @param desc Pipeline description.
 * @param out Receives eight set layouts.
 * @return How many of them, from set 0, the pipeline layout uses. */
static uint32_t pipeline_set_layouts(const rhi_device *dev, const rhi_pipeline_desc *desc,
                                     VkDescriptorSetLayout out[8])
{
    const bool ubo_pair = desc->postfx || desc->mod_pipeline ||
                          desc->material_pipeline || desc->terrain_pipeline;
    const VkDescriptorSetLayout high = desc->terrain_pipeline ? dev->ubo1_set_layout
                                                              : dev->vram_set_layout;
    for (int i = 0; i < 8; i++) out[i] = dev->vram_set_layout;
    out[3] = ubo_pair ? dev->ubo_set_layout : dev->vram_set_layout;
    out[5] = high;
    out[6] = high;
    uint32_t n = kind_set_count(desc);
    for (size_t r = 0; r < sizeof k_set_flags / sizeof k_set_flags[0]; r++) {
        const set_flag_row *row = &k_set_flags[r];
        if (!*(const bool *)((const char *)desc + row->flag)) continue;
        out[row->set] = row->layout == SET_FLAG_PAIR  ? dev->pair_set_layout
                      : row->layout == SET_FLAG_SSBO1 ? dev->ssbo1_set_layout
                      : row->layout == SET_FLAG_TEXSSBO ? dev->texssbo_set_layout
                                                      : dev->ubo1_set_layout;
        n = rhi_max_u32(n, row->set + 1u);
    }
    pair_set_layouts(dev, desc, out);
    return n;
}

/* Creation feedback for one create: the pipeline and up to two stages. */
typedef struct {
    VkPipelineCreationFeedbackEXT           pipe;
    VkPipelineCreationFeedbackEXT           stages[2];
    VkPipelineCreationFeedbackCreateInfoEXT info;
} pipe_feedback;

/** @brief Prepare a creation-feedback chain for one create.
 *
 * @param dev Device whose has_creation_feedback is read.
 * @param fb Storage, zeroed here.
 * @param stages The create's stage count (0 to 2), which the stage feedback
 *               count must equal.
 * @return The struct to chain onto VkGraphicsPipelineCreateInfo, or NULL
 *         when the extension is not enabled. */
static const void *pipe_feedback_chain(const rhi_device *dev, pipe_feedback *fb, uint32_t stages)
{
    memset(fb, 0, sizeof *fb);
    if (!dev->has_creation_feedback) return NULL;
    fb->info.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT;
    fb->info.pPipelineCreationFeedback = &fb->pipe;
    fb->info.pipelineStageCreationFeedbackCount = stages;
    fb->info.pPipelineStageCreationFeedbacks = stages ? fb->stages : NULL;
    return &fb->info;
}

/** @brief Return the instrument phase of a create on the calling thread.
 *
 * PROBE and the rebuild marker take precedence, then PRECOMPILE and LAUNCHER; otherwise
 * bring-up until the first frame is submitted or the precompile ends, and
 * first use after.
 * @return The phase. */
static rhi_pc_phase pipe_phase_now(void)
{
    if (t_pipe_phase == RHI_PIPE_PHASE_PROBE) return RHI_PC_PHASE_PROBE;
    if (t_in_rebuild) return RHI_PC_PHASE_REBUILD;
    if (t_pipe_phase == RHI_PIPE_PHASE_PRECOMPILE) return RHI_PC_PHASE_PRECOMPILE;
    if (t_pipe_phase == RHI_PIPE_PHASE_LAUNCHER) return RHI_PC_PHASE_LAUNCHER;
    return (atomic_load(&s_first_frame_done) || atomic_load(&s_precompile_done)) ? RHI_PC_PHASE_FIRST_USE
                                                     : RHI_PC_PHASE_BRINGUP;
}

/** @brief Count one vkCreateGraphicsPipelines call in the resource census and the pipeline-create instruments.
 *
 * Any thread. The cache reading is the feedback's
 * APPLICATION_PIPELINE_CACHE_HIT bit when the feedback is marked valid, else
 * unknown. The present a display-owner create delays is the frame being
 * recorded, or the next one outside a frame; a create on another thread
 * delays none and records 0. A first-use create not reported a hit arms the
 * debounced cache save (pcache_autosave).
 * @param dev Device the pipeline was created on.
 * @param site The site class of the pipeline the create builds or serves.
 * @param kind What the create builds: a whole pipeline, a library part or a link.
 * @param fb Its creation feedback.
 * @param wall_ns Time inside the call. */
static void pipe_create_record(rhi_device *dev, rhi_pipe_site site, rhi_pc_kind kind,
                               const pipe_feedback *fb, uint64_t wall_ns)
{
    atomic_fetch_add_explicit(&s_census.pipe_builds, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_census.pipe_ns, wall_ns, memory_order_relaxed);
    const bool valid = (fb->pipe.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0;
    const bool hit = (fb->pipe.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT) != 0;
    const SDL_ThreadID owner_id = atomic_load(&s_display_owner);
    const bool owner = !owner_id || SDL_GetCurrentThreadID() == owner_id;
    const rhi_pc_create c = {
        .phase = pipe_phase_now(),
        .site = site,
        .kind = kind,
        .fb = !valid ? RHI_PC_FB_UNKNOWN : hit ? RHI_PC_FB_HIT : RHI_PC_FB_MISS,
        .owner = owner,
        .present = owner ? s_frame_serial + (dev->frame_active ? 0u : 1u) : 0u,
        .wall_ns = wall_ns,
        .driver_ns = valid ? fb->pipe.duration : 0u,
    };
    rhi_pc_record_create(&c);
    if (c.phase == RHI_PC_PHASE_FIRST_USE && c.fb != RHI_PC_FB_HIT)
        atomic_fetch_add_explicit(&dev->pcache_fu_misses, 1u, memory_order_relaxed);
}

/** @brief Return the render pass a pipeline is built against: rt_u32_rp for an RGBA32_UINT colour format, else off_rp.
 *
 * Every SCENE-format pass (off_rp, rt_rp, rt_ds_rp) is compatible with
 * off_rp, so one build serves all of them.
 * @param dev Device.
 * @param d The pipeline's description.
 * @return The render pass. */
static VkRenderPass pipe_render_pass(const rhi_device *dev, const rhi_pipeline_desc *d)
{
    return d->color_format == RHI_COLOR_FORMAT_RGBA32_UINT ? dev->rt_u32_rp : dev->off_rp;
}

/** @brief Describe the create info a pipeline gets for a variant: the dedupe fields, the constants each stage receives and the polygon mode.
 *
 * A pipeline the wireframe view selects (pipeline_wire_view) rasterizes as
 * lines when the device has fillModeNonSolid and bakes NOTEX on, so its
 * edges carry the gouraud vertex colour; NOTEX is otherwise the variant's.
 * A constant no stage of the pipeline declares is not handed over and reads
 * 0 here. Reads only the arguments and device fields fixed at device
 * creation, so any thread may call it.
 * @param dev Device the pipeline lives on.
 * @param d Its description.
 * @param info Its stage facts.
 * @param var The variant.
 * @param slot_contest SLOT_CONTEST's value.
 * @param v Receives the variant's create-info description. */
static void pipe_variant_view(const rhi_device *dev, const rhi_pipeline_desc *d,
                              const pipe_stage_info *info, rhi_pipe_variant var,
                              VkBool32 slot_contest, rhi_pc_variant *v)
{
    const bool w = pipeline_wire_view(d, var.wireframe);
    const uint32_t c[6] = { dev->depth_rev[d->depth_class], var.affine,
                            (var.notex || w) ? 1u : 0u, var.truescope, slot_contest,
                            (uint32_t)d->texel_stp_select };
    *v = (rhi_pc_variant){
        .desc_hash = info->desc_hash, .vs_hash = info->vs_hash, .fs_hash = info->fs_hash,
        .declared = info->vs_ids | info->fs_ids,
        .poly_mode = (uint32_t)((w && dev->has_fill_non_solid) ? VK_POLYGON_MODE_LINE
                                                               : VK_POLYGON_MODE_FILL),
        .render_pass = (uint64_t)(uintptr_t)pipe_render_pass(dev, d),
    };
    for (int i = 0; i < 6; i++) v->consts[i] = ((v->declared >> i) & 1u) ? c[i] : 0u;
}

rhi_pipe_variant rhi_pipe_variant_current(void)
{
    return (rhi_pipe_variant){ .affine = s_vs_affine_uv != VK_FALSE,
                               .notex = s_fs_notex != VK_FALSE,
                               .wireframe = s_wireframe,
                               .truescope = s_fs_moby_truescope != VK_FALSE };
}

uint32_t rhi_pipe_variant_epoch(void) { return atomic_load(&s_variant_epoch); }

/** @brief Describe the create info a registered pipeline gets for a variant.
 *
 * Display owner only: SLOT_CONTEST is read from its static.
 * @param dev Device the pipeline lives on.
 * @param p The pipeline.
 * @param var The variant.
 * @param v Receives the variant's create-info description. */
static void pipe_variant_of(const rhi_device *dev, const rhi_pipeline *p, rhi_pipe_variant var,
                            rhi_pc_variant *v)
{
    pipe_variant_view(dev, &p->desc, &p->info, var, s_fs_slot_contest, v);
}

/** @brief Return the key manifest's key of a variant: its dedupe key with the render pass left out, which is per device and not stable across runs.
 *
 * @param v The variant.
 * @return The key. */
static uint64_t pipe_manifest_key(const rhi_pc_variant *v)
{
    rhi_pc_variant m = *v;
    m.render_pass = 0;
    return rhi_pc_variant_key(&m);
}

/* The reachable views of a pipeline, texture mapping by (notex, wireframe):
 * the census counts all six; a variant walk visits the first PIPE_WALK_VIEWS,
 * the texture-mapping dimension at the textured view. */
static const bool k_pipe_views[6][3] = {
    { false, false, false }, { true, false, false }, { false, true, false },
    { true, true, false },   { false, false, true }, { true, false, true },
};

/* Views of k_pipe_views a variant walk visits: perspective and affine. */
#define PIPE_WALK_VIEWS 2
_Static_assert(PIPE_WALK_VIEWS * RHI_AHEAD_GROUPS == AHEAD_RANKS, "a rank per walked view and group");

/** @brief Return a reachable view as a variant, MOBY_TRUESCOPE at its current value.
 *
 * @param view Index into k_pipe_views.
 * @return The variant. */
static rhi_pipe_variant pipe_view_variant(int view)
{
    return (rhi_pipe_variant){ .affine = k_pipe_views[view][0], .notex = k_pipe_views[view][1],
                               .wireframe = k_pipe_views[view][2],
                               .truescope = s_fs_moby_truescope != VK_FALSE };
}

/** @brief Add a new pipeline base and its six reachable variant create infos to the census row.
 *
 * The variants are texture mapping (perspective, affine) by view (textured,
 * notex, wireframe), as pipe_variant_view describes them. No-op unless
 * OPENPETE_PIPE_CENSUS is set.
 * @param dev Device the pipeline was created on.
 * @param p The pipeline. */
static void census_add_base(const rhi_device *dev, const rhi_pipeline *p)
{
    if (!rhi_pc_level()) return;
    rhi_pc_variant v[6];
    for (int i = 0; i < 6; i++) pipe_variant_of(dev, p, pipe_view_variant(i), &v[i]);
    rhi_pc_add_base(rhi_pc_site_of(&p->desc, p->vert_words || p->frag_words), v, 6);
}

/* One stage's specialization info: an entry and a packed value for each
 * constant the stage declares, and nothing for the rest. */
typedef struct {
    VkSpecializationMapEntry e[9];
    VkBool32                 d[9];
    VkSpecializationInfo     info;
} stage_spec;

/** @brief Return OPENPETE_KEY_TIER_ORDER, the key contest's in-bucket tier order constant 7.
 *
 * psx_key_contest.glsl OP_KEY_TIER_ORDER: 1 the order (the default), 0 off
 * (the pre-order contest, an in-binary A/B leg), 2 the order with the
 * fragments its rule discards painted instead (a census). Unset, empty or
 * any other value reads 1. Read once per process: the value is the same for
 * every variant, so the variant keys do not carry it.
 * @return 0, 1 or 2. */
static int32_t key_tier_order(void)
{
    static _Atomic int32_t s_order = -1;   /* any compile thread reads it */
    int32_t v = s_order;
    if (v < 0) {
        const char *e = getenv("OPENPETE_KEY_TIER_ORDER");
        v = (e && (*e == '0' || *e == '2') && e[1] == '\0') ? *e - '0' : 1;
        int32_t unset = -1;
        if (atomic_compare_exchange_strong(&s_order, &unset, v) && v != 1)
            RHI_LOG("OPENPETE_KEY_TIER_ORDER=%d: in-bucket tier order %s\n", (int)v,
                    v == 0 ? "off" : "census (rule pixels painted)");
    }
    return v;
}

/** @brief Return OPENPETE_KEY_BLEND_CENSUS, the blended-terrain contest census constant 8.
 *
 * psx_key_contest.glsl OP_KEY_BLEND_CENSUS: 0 off (the default); 1 a blended
 * terrain fragment the terrain-against-terrain key contest would discard is
 * kept and painted by its true-depth verdict against the key owner (red in
 * front, green behind, blue owner unknown); 2 every blended terrain fragment
 * that survives its contests is painted yellow. Unset, empty or any other
 * value reads 0. Read once per process, as key_tier_order.
 * @return 0, 1 or 2. */
static int32_t key_blend_census(void)
{
    static _Atomic int32_t s_census = -1;  /* any compile thread reads it */
    int32_t v = s_census;
    if (v < 0) {
        const char *e = getenv("OPENPETE_KEY_BLEND_CENSUS");
        v = (e && (e[0] == '1' || e[0] == '2') && e[1] == '\0') ? e[0] - '0' : 0;
        int32_t unset = -1;
        if (atomic_compare_exchange_strong(&s_census, &unset, v) && v)
            RHI_LOG("OPENPETE_KEY_BLEND_CENSUS=%d: %s\n", (int)v,
                    v == 1 ? "blended terrain key-contest discards painted (red in front, "
                             "green behind, blue unknown)"
                           : "blended terrain fragments painted yellow");
    }
    return v;
}

int32_t rhi_terrain_source_tint(void)
{
    static _Atomic int32_t s_tint = -1;    /* any compile thread reads it */
    int32_t v = s_tint;
    if (v < 0) {
        const char *e = getenv("OPENPETE_TERRAIN_SOURCE_TINT");
        v = 0;
        if (e && *e && *e != '0') {
            const char *a = getenv("OPENPETE_TERRAIN_SOURCE_TINT_ALPHA");
            double al = (a && *a) ? atof(a) : 0.4;
            al = al < 0.0 ? 0.0 : (al > 1.0 ? 1.0 : al);
            v = (int32_t)(al * 1000.0 + 0.5);
        }
        int32_t unset = -1;
        if (atomic_compare_exchange_strong(&s_tint, &unset, v) && v != 0)
            RHI_LOG("OPENPETE_TERRAIN_SOURCE_TINT: retained terrain green, walked terrain "
                    "red, blend %.3f\n", v / 1000.0);
    }
    return v;
}

/** @brief Fill a stage's specialization info with only the constants it declares.
 *
 * Ids 0-5 take the variant's values, id 6 (OP_SRC_TINT)
 * rhi_terrain_source_tint(), id 7 (OP_KEY_TIER_ORDER) key_tier_order() and
 * id 8 (OP_KEY_BLEND_CENSUS) key_blend_census().
 * @param s Storage.
 * @param ids The stage's declared constant ids (bit i = constant_id i).
 * @param c The six variant constant values.
 * @return The info, or NULL when the stage declares none. */
static const VkSpecializationInfo *stage_spec_fill(stage_spec *s, uint32_t ids, const uint32_t c[6])
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 9; i++) {
        if (!((ids >> i) & 1u)) continue;
        s->e[n] = (VkSpecializationMapEntry){ .constantID = i, .offset = n * (uint32_t)sizeof(VkBool32),
                                              .size = sizeof(VkBool32) };
        s->d[n] = i < 6 ? c[i] : i == 6 ? (uint32_t)rhi_terrain_source_tint()
                               : i == 7 ? (uint32_t)key_tier_order()
                                        : (uint32_t)key_blend_census();
        n++;
    }
    s->info = (VkSpecializationInfo){ .mapEntryCount = n, .pMapEntries = s->e,
                                      .dataSize = n * sizeof(VkBool32), .pData = s->d };
    return n ? &s->info : NULL;
}

/** @brief Create a job's two shader modules from its resolved stage bytes.
 *
 * Any thread: reads only the job.
 * @param dev Device.
 * @param job The job.
 * @param vs Receives the vertex module.
 * @param fs Receives the fragment module.
 * @return true when both modules exist; on false neither is left alive. */
static bool pipe_stage_modules(rhi_device *dev, const rhi_compile_job *job, VkShaderModule *vs,
                               VkShaderModule *fs)
{
    const rhi_pipeline_desc *d = &job->desc;
    *vs = job->vb ? create_shader_module(dev, job->vb, job->vbn, d->vert_spv ? d->vert_spv : "(mod vert)",
                                         job->info.vs_hash)
                  : VK_NULL_HANDLE;
    *fs = job->fb ? create_shader_module(dev, job->fb, job->fbn, d->frag_spv ? d->frag_spv : "(mod frag)",
                                         job->info.fs_hash)
                  : VK_NULL_HANDLE;
    if (*vs && *fs) return true;
    if (*vs) vkDestroyShaderModule(dev->device, *vs, NULL);
    if (*fs) vkDestroyShaderModule(dev->device, *fs, NULL);
    *vs = *fs = VK_NULL_HANDLE;
    return false;
}

/** @brief Release what a failed or refused pipeline create left and classify the failure.
 *
 * VK_PIPELINE_COMPILE_REQUIRED_EXT is a probe's answer that the create info
 * is not cached and is not logged; any other result is logged.
 * @param dev Device.
 * @param r The create's result.
 * @param pipeline A pipeline the driver returned anyway, or VK_NULL_HANDLE.
 * @param layout The layout created for the attempt.
 * @return 1 for a probe miss, -1 for a failure. */
static int pipeline_create_failed(rhi_device *dev, VkResult r, VkPipeline pipeline,
                                  VkPipelineLayout layout)
{
    const bool uncached = r == VK_PIPELINE_COMPILE_REQUIRED_EXT;
    if (!uncached) RHI_LOG("vkCreateGraphicsPipelines failed: %d\n", (int)r);
    if (pipeline) vkDestroyPipeline(dev->device, pipeline, NULL);
    vkDestroyPipelineLayout(dev->device, layout, NULL);
    return uncached ? 1 : -1;
}

/* Every create-info part of one pipeline: what a whole create passes, and the
 * subsets each graphics pipeline library part takes. The parts point at one
 * another, so a pipe_state is filled in place and never copied. */
typedef struct {
    stage_spec                             vspec, fspec;
    VkPipelineShaderStageCreateInfo        stages[2];
    VkVertexInputBindingDescription        bind;
    VkVertexInputAttributeDescription      attrs[8];
    VkPipelineVertexInputStateCreateInfo   vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo      vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo   ms;
    VkPipelineColorBlendAttachmentState    cba;
    VkPipelineColorBlendStateCreateInfo    cb;
    VkPipelineDepthStencilStateCreateInfo  dss;
    VkDynamicState                         dyn[3];
    VkPipelineDynamicStateCreateInfo       ds;
    VkPushConstantRange                    pcr;
    VkDescriptorSetLayout                  set_layouts[8];
    VkPipelineLayoutCreateInfo             plci;
} pipe_state;

/* What a pipeline create's breadcrumb line describes (pipe_crumb_begin). */
typedef struct {
    rhi_pc_kind              kind;
    rhi_pipe_site            site;
    const rhi_pipeline_desc *desc;    /* stage names, blend mode, push-constant size */
    const pipe_stage_info   *info;    /* stage content hashes, desc hash */
    size_t                   vbn, fbn; /* SPIR-V bytes of each stage the create compiles; 0 when absent */
    const pipe_state        *st;
    VkPipelineCreateFlags    flags;
} pipe_crumb;

/** @brief Append one stage's name, SPIR-V size, content hash and the specialization constants it is handed to a create line.
 *
 * @param b The line.
 * @param n Its length; advanced.
 * @param tag "vs" or "fs".
 * @param name The .spv name, or NULL for SPIR-V words.
 * @param bytes The stage's size.
 * @param hash Its content hash.
 * @param spec Its specialization info (stage_spec_fill). */
static void crumb_stage(char *b, size_t *n, const char *tag, const char *name, size_t bytes,
                        uint64_t hash, const stage_spec *spec)
{
    crumb_put(b, n, " %s=%s/%zuB/%016" PRIx64, tag, name ? name : "(words)", bytes, hash);
    for (uint32_t i = 0; i < spec->info.mapEntryCount; i++)
        crumb_put(b, n, "%s%u:%u", i ? "," : (tag[0] == 'v' ? " vs.spec=" : " fs.spec="),
                  spec->e[i].constantID, (unsigned)spec->d[i]);
}

/** @brief Number a pipeline create, format its line into the calling thread's buffer and run the begin hook; no-op without hooks.
 *
 * The line names the kind, site class, phase and create flags, each stage
 * the create compiles, and the fixed-function state as Vulkan enum values:
 * topology, polygon mode, blend mode and enable, colour write mask, depth
 * test/write/compare, stencil test, depth bias, sample count, vertex
 * attribute count, colour and depth attachment formats, descriptor set
 * count, push-constant size and the desc hash.
 * @param dev Device.
 * @param c The create. */
static void pipe_crumb_begin(const rhi_device *dev, const pipe_crumb *c)
{
    const create_begin_fn begin = atomic_load_explicit(&s_create_begin, memory_order_acquire);
    if (!begin) return;
    char *b = t_create_line;
    size_t n = 0;
    t_create_seq = atomic_fetch_add_explicit(&s_create_seq, 1u, memory_order_relaxed) + 1u;
    crumb_put(b, &n, "pipe-create #%u kind=%s site=%s phase=%s flags=0x%x", t_create_seq,
              rhi_pc_kind_name(c->kind), rhi_pc_site_name(c->site),
              rhi_pc_phase_name(pipe_phase_now()), (unsigned)c->flags);
    if (c->vbn) crumb_stage(b, &n, "vs", c->desc->vert_spv, c->vbn, c->info->vs_hash, &c->st->vspec);
    if (c->fbn) crumb_stage(b, &n, "fs", c->desc->frag_spv, c->fbn, c->info->fs_hash, &c->st->fspec);
    const pipe_state *s = c->st;
    crumb_put(b, &n, " topo=%d poly=%d blend=%d/%u cwmask=0x%x depth=%u/%u/op%d stencil=%u bias=%u"
              " samples=%d attrs=%u rp=%d/%d sets=%u push=%u desc=%016" PRIx64,
              (int)s->ia.topology, (int)s->rs.polygonMode, (int)c->desc->blend,
              (unsigned)s->cba.blendEnable, (unsigned)s->cba.colorWriteMask,
              (unsigned)s->dss.depthTestEnable, (unsigned)s->dss.depthWriteEnable,
              (int)s->dss.depthCompareOp, (unsigned)s->dss.stencilTestEnable,
              (unsigned)s->rs.depthBiasEnable, (int)s->ms.rasterizationSamples,
              s->vi.vertexAttributeDescriptionCount, (int)RHI_RT_FORMAT, (int)dev->depth_format,
              s->plci.setLayoutCount, (unsigned)c->desc->push_const_size, c->info->desc_hash);
    begin(RHI_CREATE_PIPELINE, b);
}

/** @brief Fill the blend attachment of a pipeline from its desc's blend mode and colour-write switch.
 *
 * @param desc Pipeline description.
 * @param cba Receives the attachment state. */
static void pipe_blend_fill(const rhi_pipeline_desc *desc, VkPipelineColorBlendAttachmentState *cba)
{
    *cba = (VkPipelineColorBlendAttachmentState){
        .blendEnable = (desc->blend != RHI_BLEND_NONE) ? VK_TRUE : VK_FALSE,
        /* color_write_off = an invisible pass (stencil-mask rasterization). */
        .colorWriteMask = desc->color_write_off ? 0u :
                          (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT),
        .colorBlendOp = VK_BLEND_OP_ADD, .alphaBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
    };
    if (desc->blend == RHI_BLEND_ALPHA) {
        cba->srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    } else if (desc->blend == RHI_BLEND_ADD) {
        cba->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (desc->blend == RHI_BLEND_SUBTRACT) {
        /* PSX ABR mode 2: dst = B - F  (reverse-subtract, src ONE, dst ONE). */
        cba->colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        cba->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba->alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
    } else if (desc->blend == RHI_BLEND_ADD_QUARTER) {
        /* PSX ABR mode 3: dst = B + F/4. The quarter rides the pipeline's
         * static blend constant, so no shader or vertex scaling is involved
         * and the source keeps its own clamp. */
        cba->srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        cba->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba->srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        cba->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else {
        cba->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
}

/** @brief Fill the depth and stencil state of a pipeline from its desc.
 *
 * Depth: the offscreen target always carries a depth attachment; a pipeline
 * opts into testing and writing it via desc->depth_test. The compare is the
 * pipeline's depth class's (depth_convention_init), an OR_EQUAL op so
 * equal-depth later draws still paint; depth_overwrite swaps it for ALWAYS
 * with the test left enabled, because Vulkan forces depth writes off
 * whenever depthTestEnable is VK_FALSE. Stencil roles: MARK writes 1 where
 * the fragment passes the depth test; TEST draws only where stencil == 1;
 * TEST_ZERO draws where it is not 1 (the hole fill); ONCE draws where it is
 * not 1 and writes 1 (one fragment per pixel). The reference is a fixed
 * 1; per-object isolation is rhi_clear_stencil between groups.
 * @param dev Device whose depth compare per class is read.
 * @param desc Pipeline description.
 * @param dss Receives the state. */
static void pipe_depth_stencil_fill(const rhi_device *dev, const rhi_pipeline_desc *desc,
                                    VkPipelineDepthStencilStateCreateInfo *dss)
{
    const VkCompareOp cmp = desc->depth_overwrite ? VK_COMPARE_OP_ALWAYS
                                                  : dev->depth_cmp[desc->depth_class];
    *dss = (VkPipelineDepthStencilStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable  = desc->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (desc->depth_test && !desc->depth_write_off) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = cmp,
    };
    VkStencilOpState sop = {
        .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP,
        .depthFailOp = VK_STENCIL_OP_KEEP, .compareMask = 0xFFu, .reference = 1u,
    };
    if (desc->stencil == RHI_STENCIL_MARK) {
        sop.passOp = VK_STENCIL_OP_REPLACE;
        sop.compareOp = VK_COMPARE_OP_ALWAYS;
        sop.writeMask = 0xFFu;
    } else if (desc->stencil == RHI_STENCIL_TEST) {
        sop.compareOp = VK_COMPARE_OP_EQUAL;
    } else if (desc->stencil == RHI_STENCIL_TEST_ZERO) {
        sop.compareOp = VK_COMPARE_OP_NOT_EQUAL;
    } else if (desc->stencil == RHI_STENCIL_ONCE) {
        sop.compareOp = VK_COMPARE_OP_NOT_EQUAL;
        sop.passOp = VK_STENCIL_OP_REPLACE;
        sop.writeMask = 0xFFu;
    } else {
        return;
    }
    dss->stencilTestEnable = VK_TRUE;
    dss->front = sop;
    dss->back = sop;
}

/** @brief Fill every create-info part of a pipeline from its desc, stage facts and variant view, with no shader module set.
 *
 * Any thread: reads the arguments and device fields fixed at device
 * creation. Each stage receives only the specialization constants it
 * declares, with the values the variant view gives: 0 DEPTH_REVERSED (per
 * depth class), 1 AFFINE_UV, 2 NOTEX, 3 MOBY_TRUESCOPE, 4 SLOT_CONTEST,
 * 5 STP_SELECT, 6 OP_SRC_TINT, 7 OP_KEY_TIER_ORDER and 8
 * OP_KEY_BLEND_CENSUS from the process (rhi_terrain_source_tint,
 * key_tier_order, key_blend_census). The fragment stage gets the same values as the vertex
 * stage: the terrain key-resolve fragments invert gl_FragCoord.z back to
 * view-Z, and the inversion follows the encoding the vertex stage selected.
 * The caller sets stages[0].module and stages[1].module.
 * @param dev Device the pipeline is created on.
 * @param desc Pipeline description.
 * @param info Its stage facts (the declared constant ids).
 * @param var Its variant view (pipe_variant_view).
 * @param st Receives the parts, in place.
 * @return false when the layout needs more descriptor sets than the device
 *         allows (logged). */
static bool pipe_state_fill(const rhi_device *dev, const rhi_pipeline_desc *desc,
                            const pipe_stage_info *info, const rhi_pc_variant *var,
                            pipe_state *st)
{
    memset(st, 0, sizeof *st);
    const uint32_t *cvals = var->consts;
    st->stages[0] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .pName = "main",
        .pSpecializationInfo = stage_spec_fill(&st->vspec, info->vs_ids, cvals) };
    st->stages[1] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main",
        .pSpecializationInfo = stage_spec_fill(&st->fspec, info->fs_ids, cvals) };
    st->bind = (VkVertexInputBindingDescription){
        .binding = 0, .stride = sizeof(rhi_vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    /* The vertex STRUCT keeps all fields (stable buffer layout), but a pipeline
     * only needs to declare the attributes its shaders consume - an attribute
     * declared here yet not read by a shader merely warns "not consumed", so one
     * superset attribute list serves every pipeline. Location 3 (the moby paint
     * rank/ordinal pair) is read by the scene vertex stages' same-moby painter
     * select; every other shader ignores it. Location 4 is the perspective W (true
     * view-Z), read by the scene vertex stages. Location 5 is the terrain
     * og-key slot offset (rhi_vertex.koff), read by the scene vertex stages
     * and the terrain key pass. Location 6 is the moby-against-moby key
     * offset of an actor face (rhi_vertex.akc), read by the same stages;
     * location 7 the colour residual of any face (rhi_vertex.rgbq), read by
     * the scene vertex stages. */
    static const VkVertexInputAttributeDescription k_attrs[8] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(rhi_vertex, x) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = offsetof(rhi_vertex, u) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,   .offset = offsetof(rhi_vertex, rgba) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = offsetof(rhi_vertex, sx) },
        { .location = 4, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,       .offset = offsetof(rhi_vertex, w) },
        { .location = 5, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,       .offset = offsetof(rhi_vertex, koff) },
        { .location = 6, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,       .offset = offsetof(rhi_vertex, akc) },
        { .location = 7, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,       .offset = offsetof(rhi_vertex, rgbq) },
    };
    memcpy(st->attrs, k_attrs, sizeof k_attrs);
    st->vi = (VkPipelineVertexInputStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &st->bind,
        .vertexAttributeDescriptionCount = 8, .pVertexAttributeDescriptions = st->attrs,
    };
    if (desc->terrain_pipeline) terrain_vertex_input(desc, &st->vi);
    if (desc->postfx) {   /* fullscreen triangle: no vertex buffer at all */
        st->vi.vertexBindingDescriptionCount = 0;
        st->vi.pVertexBindingDescriptions = NULL;
        st->vi.vertexAttributeDescriptionCount = 0;
        st->vi.pVertexAttributeDescriptions = NULL;
    }
    st->ia = (VkPipelineInputAssemblyStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = desc->topology == RHI_TOPOLOGY_POINT_LIST ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                                              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    st->vp = (VkPipelineViewportStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    st->rs = (VkPipelineRasterizationStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = (VkPolygonMode)var->poly_mode,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        /* Decal pipelines (desc->depth_bias) enable the hardware polygon
         * offset with DYNAMIC factors (VK_DYNAMIC_STATE_DEPTH_BIAS below);
         * both pass begins set the factors to 0 so state is always defined. */
        .depthBiasEnable = desc->depth_bias ? VK_TRUE : VK_FALSE,
        .lineWidth = 1.0f,
    };
    st->ms = (VkPipelineMultisampleStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    pipe_blend_fill(desc, &st->cba);
    st->cb = (VkPipelineColorBlendStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &st->cba,
        .blendConstants = { 0.25f, 0.25f, 0.25f, 0.25f },
    };
    pipe_depth_stencil_fill(dev, desc, &st->dss);
    st->dyn[0] = VK_DYNAMIC_STATE_VIEWPORT;
    st->dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
    st->dyn[2] = VK_DYNAMIC_STATE_DEPTH_BIAS;
    st->ds = (VkPipelineDynamicStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = desc->depth_bias ? 3u : 2u, .pDynamicStates = st->dyn,
    };
    /* Optional VRAM sampler set (0) + a push-constant block (the per-draw PSX
     * material: tpage/clut/bpp). Terrain pipelines read the per-draw block
     * (clip affine + class select) in the VERTEX stage too, as does a
     * push_vertex pipeline; everything else is fragment-only. A pipeline
     * that uses neither keeps an empty layout. */
    st->pcr = (VkPushConstantRange){
        .stageFlags = (desc->terrain_pipeline || desc->push_vertex)
                          ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                          : VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0, .size = desc->push_const_size,
    };
    const uint32_t set_count = pipeline_set_layouts(dev, desc, st->set_layouts);
    if (set_count > dev->max_desc_sets) {
        /* Degrade, never crash: the caller treats a NULL pipeline as "draw
         * this run stock", so a device too small for the material layout
         * loses reshading rather than the frame. */
        RHI_LOG("pipeline needs %u descriptor sets, device allows %u\n",
                set_count, dev->max_desc_sets);
        return false;
    }
    st->plci = (VkPipelineLayoutCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = set_count,
        .pSetLayouts = set_count ? st->set_layouts : NULL,
        .pushConstantRangeCount = desc->push_const_size ? 1u : 0u,
        .pPushConstantRanges = desc->push_const_size ? &st->pcr : NULL,
    };
    return true;
}

/* ---- graphics pipeline libraries ----------------------------------------- */

/* The three library parts a linked pipeline is made of: vertex input with
 * pre-rasterization (the vertex stage), the fragment shader state, and the
 * fragment output interface. */
enum { GPL_PRERASTER = 0, GPL_FRAGMENT, GPL_OUTPUT, GPL_PARTS };

/* A library entry's state: QUEUED nobody builds it yet (the first thread to
 * need it claims it), BUILDING one thread creates it, READY created, FAILED
 * its create failed (every pipeline needing it is created whole). */
enum { GPL_ST_QUEUED = 0, GPL_ST_BUILDING, GPL_ST_READY, GPL_ST_FAILED };

/* One library part, created once per run and shared by every link whose part
 * create info hashes to its key. Lives until the device is destroyed. */
struct gpl_lib {
    struct gpl_lib  *next;
    uint64_t         key;
    int              state;    /* GPL_ST_*, under dev->gpl_mx */
    VkPipeline       lib;
    VkPipelineLayout layout;   /* its create's layout; VK_NULL_HANDLE for the output part */
};

/* What building one library part reads: the desc, stage facts and variant
 * view of a pipeline that uses the part, and the bytes of the part's shader
 * stage. The desc's stage names are read only for the create's breadcrumb;
 * an owned source points them at its own copies (vname, fname), so a
 * gpl_src that owns its names is never copied by value. */
typedef struct {
    rhi_pipeline_desc desc;
    pipe_stage_info   info;
    rhi_pc_variant    var;
    rhi_pipe_site     site;
    const void       *spv;   /* the part's stage bytes; NULL for the output part */
    size_t            n;
    void             *own;   /* an owned copy behind spv, or NULL */
    char              vname[96], fname[96];   /* an owned source's stage names */
} gpl_src;

/* XOR'd into the key-manifest key of a pipeline linked from libraries: the
 * pipeline cache holds its libraries and its link, not the whole create of
 * the same key, so the two are listed apart. A link with link-time
 * optimization is a third cache entry and takes its own salt. */
#define GPL_MANIFEST_SALT 0x9e3779b97f4a7c15ull
#define LTO_MANIFEST_SALT 0xc2b2ae3d27d4eb4full

/** @brief Return whether a pipeline is created by linking graphics pipeline libraries rather than whole.
 *
 * On a device with the library extension and fast linking enabled
 * (pipeline_exts_select), three site classes are: the terrain-cache
 * pipelines, whose large vertex stages then compile once per
 * pre-rasterization create info instead of once per (vertex, fragment,
 * state) pairing; the terrain materials, which share those vertex parts
 * (same stages, state and layout) and add one fragment part per mod
 * fragment stage and depth-stencil state; and the mod materials, whose
 * fragment stage then compiles once per depth-stencil state instead of once
 * per blend group, the blend equation living in the output part.
 * @param dev Device.
 * @param desc Pipeline description.
 * @param words True when a stage was given as SPIR-V words.
 * @return true for the library path. */
static bool pipe_uses_library(const rhi_device *dev, const rhi_pipeline_desc *desc, bool words)
{
    const rhi_pipe_site site = rhi_pc_site_of(desc, words);
    return dev->has_gpl && (site == RHI_PIPE_SITE_TERRAIN_CACHE ||
                            site == RHI_PIPE_SITE_TERRAIN_MATERIAL ||
                            site == RHI_PIPE_SITE_MOD_MATERIAL);
}

/** @brief Return the key-manifest key of a variant created by the given path.
 *
 * @param v The variant.
 * @param link How the pipeline is made.
 * @return The key. */
static uint64_t pipe_manifest_key_as(const rhi_pc_variant *v, pipe_link link)
{
    const uint64_t k = pipe_manifest_key(v);
    return link == PIPE_LINK_FAST ? k ^ GPL_MANIFEST_SALT
         : link == PIPE_LINK_LTO  ? k ^ LTO_MANIFEST_SALT : k;
}

/** @brief Fold n bytes into an FNV-1a 64 hash.
 *
 * @param h Running hash.
 * @param p The bytes.
 * @param n Their count.
 * @return The updated hash. */
static uint64_t gpl_mix(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

/** @brief Fold a stage's specialization data into a hash: its entry count, constant ids and values.
 *
 * @param h Running hash.
 * @param s The stage's specialization, as stage_spec_fill left it.
 * @return The updated hash. */
static uint64_t gpl_mix_spec(uint64_t h, const stage_spec *s)
{
    const uint32_t n = s->info.mapEntryCount;
    h = gpl_mix(h, &n, sizeof n);
    for (uint32_t i = 0; i < n; i++) {
        h = gpl_mix(h, &s->e[i].constantID, sizeof s->e[i].constantID);
        h = gpl_mix(h, &s->d[i], sizeof s->d[i]);
    }
    return h;
}

/** @brief Fold a pipeline layout's create info into a hash: its set layouts and push-constant range.
 *
 * Two layouts with equal hashes are identically defined, the compatibility
 * a link without independent sets requires of its libraries' layouts.
 * @param h Running hash.
 * @param st The pipeline's parts.
 * @return The updated hash. */
static uint64_t gpl_mix_layout(uint64_t h, const pipe_state *st)
{
    h = gpl_mix(h, &st->plci.setLayoutCount, sizeof st->plci.setLayoutCount);
    h = gpl_mix(h, st->set_layouts, st->plci.setLayoutCount * sizeof st->set_layouts[0]);
    h = gpl_mix(h, &st->plci.pushConstantRangeCount, sizeof st->plci.pushConstantRangeCount);
    if (st->plci.pushConstantRangeCount) h = gpl_mix(h, &st->pcr, sizeof st->pcr);
    return h;
}

/** @brief Compute the key of each library part of a pipeline from the create-info parts that part takes.
 *
 * Pre-rasterization: the vertex stage's bytes hash and constants, the
 * vertex input, input assembly, rasterization and dynamic state, the
 * layout and the render pass. Fragment shader: the fragment stage's hash
 * and constants, depth-stencil and multisample state, the layout and the
 * render pass. Output: the blend attachment and constants, multisample
 * state and the render pass. Any thread.
 * @param dev Device whose render passes are read.
 * @param desc The pipeline's description (its render pass, pipe_render_pass).
 * @param st The pipeline's parts (pipe_state_fill).
 * @param info Its stage facts.
 * @param key Receives the three keys. */
static void gpl_part_keys(const rhi_device *dev, const rhi_pipeline_desc *desc,
                          const pipe_state *st, const pipe_stage_info *info,
                          uint64_t key[GPL_PARTS])
{
    const uint64_t basis = 0xcbf29ce484222325ull;
    const uint64_t rp = (uint64_t)(uintptr_t)pipe_render_pass(dev, desc);
    for (int p = 0; p < GPL_PARTS; p++) {
        key[p] = gpl_mix(basis, &p, sizeof p);
        key[p] = gpl_mix(key[p], &rp, sizeof rp);
    }
    uint64_t h = key[GPL_PRERASTER];
    h = gpl_mix(h, &info->vs_hash, sizeof info->vs_hash);
    h = gpl_mix_spec(h, &st->vspec);
    const VkPipelineVertexInputStateCreateInfo *vi = &st->vi;
    h = gpl_mix(h, &vi->vertexBindingDescriptionCount, sizeof vi->vertexBindingDescriptionCount);
    h = gpl_mix(h, vi->pVertexBindingDescriptions,
                vi->vertexBindingDescriptionCount * sizeof vi->pVertexBindingDescriptions[0]);
    h = gpl_mix(h, &vi->vertexAttributeDescriptionCount, sizeof vi->vertexAttributeDescriptionCount);
    h = gpl_mix(h, vi->pVertexAttributeDescriptions,
                vi->vertexAttributeDescriptionCount * sizeof vi->pVertexAttributeDescriptions[0]);
    const uint32_t rs[6] = { (uint32_t)st->ia.topology, (uint32_t)st->rs.polygonMode,
                             (uint32_t)st->rs.cullMode, (uint32_t)st->rs.frontFace,
                             st->rs.depthBiasEnable, st->ds.dynamicStateCount };
    h = gpl_mix(h, rs, sizeof rs);
    h = gpl_mix(h, &st->rs.lineWidth, sizeof st->rs.lineWidth);
    h = gpl_mix(h, st->dyn, st->ds.dynamicStateCount * sizeof st->dyn[0]);
    key[GPL_PRERASTER] = gpl_mix_layout(h, st);

    h = key[GPL_FRAGMENT];
    h = gpl_mix(h, &info->fs_hash, sizeof info->fs_hash);
    h = gpl_mix_spec(h, &st->fspec);
    const uint32_t ds[5] = { st->dss.depthTestEnable, st->dss.depthWriteEnable,
                             (uint32_t)st->dss.depthCompareOp, st->dss.stencilTestEnable,
                             (uint32_t)st->ms.rasterizationSamples };
    h = gpl_mix(h, ds, sizeof ds);
    h = gpl_mix(h, &st->dss.front, sizeof st->dss.front);
    h = gpl_mix(h, &st->dss.back, sizeof st->dss.back);
    key[GPL_FRAGMENT] = gpl_mix_layout(h, st);

    h = key[GPL_OUTPUT];
    h = gpl_mix(h, &st->cba, sizeof st->cba);
    h = gpl_mix(h, st->cb.blendConstants, sizeof st->cb.blendConstants);
    key[GPL_OUTPUT] = gpl_mix(h, &st->ms.rasterizationSamples, sizeof st->ms.rasterizationSamples);
}

/** @brief Make a shader library part's stage: its shader module and the part's pipeline layout; the output part has neither.
 *
 * Runs inside the part's breadcrumb, so the module create names the part.
 * @param dev Device.
 * @param part GPL_PRERASTER, GPL_FRAGMENT or GPL_OUTPUT.
 * @param src The source.
 * @param st The part's create-info parts (pipe_state_fill).
 * @param out_m Receives the module, VK_NULL_HANDLE for the output part.
 * @param out_layout Receives the layout, VK_NULL_HANDLE for the output part.
 * @return false when the module or the layout failed (nothing is left alive). */
static bool gpl_part_stage(rhi_device *dev, int part, const gpl_src *src, const pipe_state *st,
                           VkShaderModule *out_m, VkPipelineLayout *out_layout)
{
    *out_m = VK_NULL_HANDLE;
    if (part == GPL_OUTPUT) return true;
    const bool vert = part == GPL_PRERASTER;
    const char *name = vert ? src->desc.vert_spv : src->desc.frag_spv;
    *out_m = create_shader_module(dev, src->spv, src->n,
                                  name ? name : vert ? "(library vertex stage)" : "(library fragment stage)",
                                  vert ? src->info.vs_hash : src->info.fs_hash);
    if (!*out_m) return false;
    if (vkCreatePipelineLayout(dev->device, &st->plci, NULL, out_layout) == VK_SUCCESS) return true;
    RHI_LOG("vkCreatePipelineLayout failed (pipeline library)\n");
    vkDestroyShaderModule(dev->device, *out_m, NULL);
    *out_m = VK_NULL_HANDLE;
    return false;
}

/** @brief Point a library part's create info at the subset of the pipeline's create-info parts its library flags name.
 *
 * Pre-rasterization: the vertex stage, vertex input, input assembly,
 * viewport, rasterization and dynamic state. Fragment: the fragment stage,
 * depth-stencil and multisample state. Output: colour blend and multisample
 * state.
 * @param part GPL_PRERASTER, GPL_FRAGMENT or GPL_OUTPUT.
 * @param st The parts; the part's stage module is set here.
 * @param m The part's shader module, or VK_NULL_HANDLE for the output part.
 * @param gp Receives the pointers and stage count. */
static void gpl_part_states(int part, pipe_state *st, VkShaderModule m,
                            VkGraphicsPipelineCreateInfo *gp)
{
    if (part == GPL_PRERASTER) {
        st->stages[0].module = m;
        gp->stageCount = 1;
        gp->pStages = &st->stages[0];
        gp->pVertexInputState = &st->vi;
        gp->pInputAssemblyState = &st->ia;
        gp->pViewportState = &st->vp;
        gp->pRasterizationState = &st->rs;
        gp->pDynamicState = &st->ds;
    } else if (part == GPL_FRAGMENT) {
        st->stages[1].module = m;
        gp->stageCount = 1;
        gp->pStages = &st->stages[1];
        gp->pDepthStencilState = &st->dss;
        gp->pMultisampleState = &st->ms;
    } else {
        gp->pColorBlendState = &st->cb;
        gp->pMultisampleState = &st->ms;
    }
}

/** @brief Create one library part from a source, through the pipeline cache.
 *
 * Any thread. The part takes the subset of the pipeline's create-info parts
 * its library flags name; the shader parts carry the layout, which is
 * created here and kept with the library. With link-time optimization on
 * (has_lto) the part retains what an optimized link of it reads. The create
 * is counted by pipe_create_record under its part's kind.
 * @param dev Device.
 * @param part GPL_PRERASTER, GPL_FRAGMENT or GPL_OUTPUT.
 * @param src The source.
 * @param out_lib Receives the library.
 * @param out_layout Receives its layout, VK_NULL_HANDLE for the output part.
 * @return true when the library was created. */
static bool gpl_build(rhi_device *dev, int part, const gpl_src *src, VkPipeline *out_lib,
                      VkPipelineLayout *out_layout)
{
    static const VkFlags k_flags[GPL_PARTS] = {
        RHI_GPL_VERTEX_INPUT_INTERFACE | RHI_GPL_PRE_RASTERIZATION,
        RHI_GPL_FRAGMENT_SHADER, RHI_GPL_FRAGMENT_OUTPUT };
    static const rhi_pc_kind k_kind[GPL_PARTS] = {
        RHI_PC_KIND_LIB_PRERASTER, RHI_PC_KIND_LIB_FRAGMENT, RHI_PC_KIND_LIB_OUTPUT };
    *out_lib = VK_NULL_HANDLE;
    *out_layout = VK_NULL_HANDLE;
    pipe_state st;
    if (!pipe_state_fill(dev, &src->desc, &src->info, &src->var, &st)) return false;
    const VkPipelineCreateFlags lf =
        VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (dev->has_lto ? RHI_PIPE_CREATE_RETAIN_LTO : 0u);
    const bool vert = part == GPL_PRERASTER;
    const pipe_crumb crumb = { .kind = k_kind[part], .site = src->site, .desc = &src->desc,
                               .info = &src->info, .vbn = vert ? src->n : 0,
                               .fbn = part == GPL_FRAGMENT ? src->n : 0, .st = &st, .flags = lf };
    pipe_crumb_begin(dev, &crumb);
    VkShaderModule m;
    if (!gpl_part_stage(dev, part, src, &st, &m, out_layout)) {
        pipe_crumb_end();
        return false;
    }
    rhi_gpl_create_info li = { .sType = RHI_ST_GPL_CREATE_INFO, .flags = k_flags[part] };
    VkGraphicsPipelineCreateInfo gp = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &li,
        .flags = lf,
        .layout = *out_layout,
        .renderPass = pipe_render_pass(dev, &src->desc),
        .subpass = 0,
    };
    gpl_part_states(part, &st, m, &gp);
    pipe_feedback fb;
    li.pNext = pipe_feedback_chain(dev, &fb, gp.stageCount);
    const Uint64 t0 = SDL_GetTicksNS();  /* clock-guard: wall: pipeline create timing for the census */
    const VkResult r = vkCreateGraphicsPipelines(dev->device, dev->pcache, 1, &gp, NULL, out_lib);
    pipe_crumb_end();
    pipe_create_record(dev, src->site, k_kind[part], &fb, SDL_GetTicksNS() - t0);  /* clock-guard: wall: pipeline create timing for the census */
    if (m) vkDestroyShaderModule(dev->device, m, NULL);
    if (r != VK_SUCCESS) {
        RHI_LOG("vkCreateGraphicsPipelines failed: %d (pipeline library part %d)\n", (int)r, part);
        if (*out_lib) vkDestroyPipeline(dev->device, *out_lib, NULL);
        if (*out_layout) vkDestroyPipelineLayout(dev->device, *out_layout, NULL);
        *out_lib = VK_NULL_HANDLE;
        *out_layout = VK_NULL_HANDLE;
        return false;
    }
    SDL_LockMutex(dev->pc_mx);
    dev->pcache_unhit += !(fb.pipe.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT);
    SDL_UnlockMutex(dev->pc_mx);
    return true;
}

/** @brief Return the library entry with a key, inserting a QUEUED one when none exists.
 *
 * Caller holds dev->gpl_mx.
 * @param dev Device.
 * @param key The part's key.
 * @param inserted Receives whether the entry is new.
 * @return The entry, or NULL on allocation failure. */
static struct gpl_lib *gpl_entry(rhi_device *dev, uint64_t key, bool *inserted)
{
    *inserted = false;
    for (struct gpl_lib *e = dev->gpl_head; e; e = e->next)
        if (e->key == key) return e;
    struct gpl_lib *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->key = key;
    e->state = GPL_ST_QUEUED;
    e->next = dev->gpl_head;
    dev->gpl_head = e;
    *inserted = true;
    return e;
}

/** @brief Return a library part, building it on the calling thread when no thread has claimed it, or waiting for the thread that has.
 *
 * Any thread. An entry is built once per run; a failed build stays failed.
 * @param dev Device.
 * @param part The part.
 * @param key Its key (gpl_part_keys).
 * @param src What building it reads.
 * @return The library, or VK_NULL_HANDLE when its build failed. */
static VkPipeline gpl_acquire(rhi_device *dev, int part, uint64_t key, const gpl_src *src)
{
    bool inserted;
    SDL_LockMutex(dev->gpl_mx);
    struct gpl_lib *e = gpl_entry(dev, key, &inserted);
    while (e && e->state == GPL_ST_BUILDING) SDL_WaitCondition(dev->gpl_cv, dev->gpl_mx);
    if (e && e->state == GPL_ST_QUEUED) {
        e->state = GPL_ST_BUILDING;
        SDL_UnlockMutex(dev->gpl_mx);
        VkPipeline lib;
        VkPipelineLayout layout;
        const bool ok = gpl_build(dev, part, src, &lib, &layout);
        SDL_LockMutex(dev->gpl_mx);
        e->lib = lib;
        e->layout = layout;
        e->state = ok ? GPL_ST_READY : GPL_ST_FAILED;
        SDL_BroadcastCondition(dev->gpl_cv);
    }
    const VkPipeline out = (e && e->state == GPL_ST_READY) ? e->lib : VK_NULL_HANDLE;
    SDL_UnlockMutex(dev->gpl_mx);
    return out;
}

/** @brief Destroy every library entry and the library lock (device destruction, after the pool stopped and the device idled).
 *
 * @param dev Device. */
static void gpl_destroy_all(rhi_device *dev)
{
    unsigned n = 0, failed = 0;
    for (struct gpl_lib *e = dev->gpl_head, *next; e; e = next) {
        next = e->next;
        if (e->lib) vkDestroyPipeline(dev->device, e->lib, NULL);
        if (e->layout) vkDestroyPipelineLayout(dev->device, e->layout, NULL);
        n++;
        failed += e->state == GPL_ST_FAILED;
        free(e);
    }
    dev->gpl_head = NULL;
    if (dev->has_gpl) RHI_LOG("pipeline libraries: %u parts (%u failed) destroyed\n", n, failed);
    if (dev->gpl_cv) SDL_DestroyCondition(dev->gpl_cv);
    if (dev->gpl_mx) SDL_DestroyMutex(dev->gpl_mx);
    dev->gpl_cv = NULL;
    dev->gpl_mx = NULL;
}

/** @brief Point a library source's desc stage names at copies the source holds, truncated to its buffers; a NULL name stays NULL.
 *
 * @param src The source, at its final address. */
static void gpl_src_own_names(gpl_src *src)
{
    src->desc.vert_spv_whole = NULL;   /* a library part never reads it */
    if (src->desc.vert_spv) {
        snprintf(src->vname, sizeof src->vname, "%s", src->desc.vert_spv);
        src->desc.vert_spv = src->vname;
    }
    if (src->desc.frag_spv) {
        snprintf(src->fname, sizeof src->fname, "%s", src->desc.frag_spv);
        src->desc.frag_spv = src->fname;
    }
}

/** @brief Describe what building one library part of a job's pipeline reads.
 *
 * The part's stage is the job's vertex stage for pre-rasterization, its
 * fragment stage for the fragment part, and none for the output part.
 * @param job The job, its stage bytes resolved.
 * @param var Its variant view.
 * @param part The part.
 * @param own true: the stage bytes are copied into src->own and the desc's
 *            stage names into src (gpl_src_own_names), so the source
 *            outlives the job; false: the source points into the job.
 * @param src Receives the source.
 * @return false when the copy could not be allocated (nothing is held). */
static bool gpl_src_of(const rhi_compile_job *job, const rhi_pc_variant *var, int part, bool own,
                       gpl_src *src)
{
    *src = (gpl_src){ .desc = job->desc, .info = job->info, .var = *var,
                      .site = rhi_pc_site_of(&job->desc, job->vert_words || job->frag_words) };
    if (part == GPL_PRERASTER) { src->spv = job->vb; src->n = job->vbn; }
    if (part == GPL_FRAGMENT)  { src->spv = job->fb; src->n = job->fbn; }
    if (!own) return true;
    gpl_src_own_names(src);
    if (!src->n) return true;
    src->own = malloc(src->n);
    if (!src->own) return false;
    memcpy(src->own, src->spv, src->n);
    src->spv = src->own;
    return true;
}

/** @brief Link a job's pipeline from its three library parts, acquiring each part first (gpl_acquire).
 *
 * Any thread. Without @p lto the link is the fast link the device
 * reported; with it the link carries LINK_TIME_OPTIMIZATION and compiles
 * the parts together from the information they retained. The create is
 * counted by pipe_create_record as a link or an optimized link.
 * @param dev Device.
 * @param job The job.
 * @param st The pipeline's parts (pipe_state_fill).
 * @param var Its variant view.
 * @param layout The linked pipeline's layout, identically defined to its parts'.
 * @param flags Pipeline create flags (the probe's FAIL_ON_PIPELINE_COMPILE_REQUIRED).
 * @param lto true for the link-time-optimized link.
 * @param out Receives the pipeline.
 * @param fb Receives the link's creation feedback.
 * @return 0 when linked, 1 when a probe found the link uncached, -1 when a
 *         part or the link failed (the caller creates the pipeline whole). */
static int pipeline_link(rhi_device *dev, const rhi_compile_job *job, const pipe_state *st,
                         const rhi_pc_variant *var, VkPipelineLayout layout,
                         VkPipelineCreateFlags flags, bool lto, VkPipeline *out, pipe_feedback *fb)
{
    uint64_t key[GPL_PARTS];
    gpl_part_keys(dev, &job->desc, st, &job->info, key);
    VkPipeline libs[GPL_PARTS];
    for (int p = 0; p < GPL_PARTS; p++) {
        gpl_src src;
        (void)gpl_src_of(job, var, p, false, &src);
        libs[p] = gpl_acquire(dev, p, key[p], &src);
        if (!libs[p]) return -1;
    }
    VkPipelineLibraryCreateInfoKHR lci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
        .pNext = pipe_feedback_chain(dev, fb, 0),
        .libraryCount = GPL_PARTS, .pLibraries = libs,
    };
    const VkGraphicsPipelineCreateInfo gp = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &lci,
        .flags = flags | (lto ? RHI_PIPE_CREATE_LTO : 0u),
        .layout = layout,
        .renderPass = pipe_render_pass(dev, &job->desc),
        .subpass = 0,
    };
    *out = VK_NULL_HANDLE;
    const rhi_pipe_site site = rhi_pc_site_of(&job->desc, job->vert_words || job->frag_words);
    const rhi_pc_kind kind = lto ? RHI_PC_KIND_LINK_LTO : RHI_PC_KIND_LINK;
    const pipe_crumb crumb = { .kind = kind, .site = site, .desc = &job->desc, .info = &job->info,
                               .vbn = job->vbn, .fbn = job->fbn, .st = st, .flags = gp.flags };
    pipe_crumb_begin(dev, &crumb);
    const Uint64 t0 = SDL_GetTicksNS();  /* clock-guard: wall: pipeline create timing for the census */
    const VkResult r = vkCreateGraphicsPipelines(dev->device, dev->pcache, 1, &gp, NULL, out);
    pipe_crumb_end();
    pipe_create_record(dev, site, kind, fb, SDL_GetTicksNS() - t0);  /* clock-guard: wall: pipeline create timing for the census */
    if (r == VK_SUCCESS) return 0;
    if (*out) vkDestroyPipeline(dev->device, *out, NULL);
    *out = VK_NULL_HANDLE;
    if (r == VK_PIPELINE_COMPILE_REQUIRED_EXT) return 1;
    RHI_LOG("vkCreateGraphicsPipelines failed: %d (library link); creating the pipeline whole\n",
            (int)r);
    return -1;
}

/** @brief Create a job's pipeline whole from its two shader stages and every create-info part.
 *
 * Any thread. The create is counted by pipe_create_record as a pipeline.
 * @param dev Device.
 * @param job The job, its stage bytes resolved.
 * @param st The pipeline's parts (pipe_state_fill); the stage modules are set here.
 * @param layout The pipeline's layout.
 * @param flags Pipeline create flags.
 * @param out Receives the pipeline, or a pipeline a failing driver returned.
 * @param fb Receives the creation feedback.
 * @return The create's result; VK_ERROR_INITIALIZATION_FAILED when a module failed. */
static VkResult pipeline_create_whole(rhi_device *dev, const rhi_compile_job *job, pipe_state *st,
                                      VkPipelineLayout layout, VkPipelineCreateFlags flags,
                                      VkPipeline *out, pipe_feedback *fb)
{
    *out = VK_NULL_HANDLE;
    const rhi_pipe_site site = rhi_pc_site_of(&job->desc, job->vert_words || job->frag_words);
    const pipe_crumb crumb = { .kind = RHI_PC_KIND_PIPELINE, .site = site, .desc = &job->desc,
                               .info = &job->info, .vbn = job->vbn, .fbn = job->fbn, .st = st,
                               .flags = flags };
    pipe_crumb_begin(dev, &crumb);
    VkShaderModule vs, fs;
    if (!pipe_stage_modules(dev, job, &vs, &fs)) {
        pipe_crumb_end();
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    st->stages[0].module = vs;
    st->stages[1].module = fs;
    VkGraphicsPipelineCreateInfo gp = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st->stages,
        .pVertexInputState = &st->vi,
        .pInputAssemblyState = &st->ia,
        .pViewportState = &st->vp,
        .pRasterizationState = &st->rs,
        .pMultisampleState = &st->ms,
        .pDepthStencilState = &st->dss,
        .pColorBlendState = &st->cb,
        .pDynamicState = &st->ds,
        .layout = layout,
        .renderPass = pipe_render_pass(dev, &job->desc),
        .subpass = 0,
        .flags = flags,
    };
    gp.pNext = pipe_feedback_chain(dev, fb, 2);
    const Uint64 t0 = SDL_GetTicksNS();  /* clock-guard: wall: pipeline create timing for the census */
    const VkResult r = vkCreateGraphicsPipelines(dev->device, dev->pcache, 1, &gp, NULL, out);
    pipe_crumb_end();
    pipe_create_record(dev, site, RHI_PC_KIND_PIPELINE, fb, SDL_GetTicksNS() - t0);  /* clock-guard: wall: pipeline create timing for the census */
    vkDestroyShaderModule(dev->device, vs, NULL);
    vkDestroyShaderModule(dev->device, fs, NULL);
    return r;
}

/** @brief Return whether a create should try a variant's optimized form first: the key manifest lists it and the create can tell a cached one.
 *
 * The optimized form is the link-time-optimized link (PIPE_LINK_LTO) or,
 * for a desc naming a whole-create vertex stage, the whole pipeline from
 * that stage (PIPE_LINK_WHOLE, var being that stage's view). With pipeline
 * cache control the try is a probe, so an uncached optimized form costs a
 * lookup; without it only a calibrated driver's manifest is trusted. Any
 * thread.
 * @param dev Device.
 * @param var The variant.
 * @param link The optimized form's link kind.
 * @return true to try it. */
static bool pipeline_optimized_listed(rhi_device *dev, const rhi_pc_variant *var, pipe_link link)
{
    if (!dev->has_lto || (!dev->has_cache_control && !dev->manifest_trusted)) return false;
    SDL_LockMutex(dev->pc_mx);
    const int listed = rhi_pc_keyset_has(&dev->pc_keys, pipe_manifest_key_as(var, link));
    SDL_UnlockMutex(dev->pc_mx);
    return listed != 0;
}

/** @brief Link a job's pipeline from library parts: link-time optimized when the job asks for it or the cache holds it, else fast.
 *
 * A background optimization job (job->lto) links optimized with the
 * caller's flags. Any other create tries the optimized link first when
 * pipeline_optimized_listed says it is cached, as a probe where the device
 * has cache control, and links fast on a probe miss; a job with a
 * whole-create vertex stage (job->wb) skips that try, its optimized form
 * being the whole pipeline (pipeline_compile_optimized). Any thread.
 * @param dev Device.
 * @param job The job.
 * @param st The pipeline's parts.
 * @param var Its variant view.
 * @param layout The linked pipeline's layout.
 * @param flags Pipeline create flags (the caller's probe).
 * @param out Receives the pipeline.
 * @param fb Receives the link's creation feedback.
 * @param link Receives how the pipeline was linked.
 * @return As pipeline_link. */
static int pipeline_link_choose(rhi_device *dev, const rhi_compile_job *job, const pipe_state *st,
                                const rhi_pc_variant *var, VkPipelineLayout layout,
                                VkPipelineCreateFlags flags, VkPipeline *out, pipe_feedback *fb,
                                pipe_link *link)
{
    *link = PIPE_LINK_LTO;
    if (job->lto) return pipeline_link(dev, job, st, var, layout, flags, true, out, fb);
    if (!job->wb && pipeline_optimized_listed(dev, var, PIPE_LINK_LTO)) {
        const VkPipelineCreateFlags probe = dev->has_cache_control
            ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT : 0u;
        const int r = pipeline_link(dev, job, st, var, layout, flags | probe, true, out, fb);
        if (r == 0) atomic_fetch_add_explicit(&dev->lto.direct, 1u, memory_order_relaxed);
        if (r != 1 || !probe) return r;
        atomic_fetch_add_explicit(&dev->lto.probe_miss, 1u, memory_order_relaxed);
    }
    *link = PIPE_LINK_FAST;
    return pipeline_link(dev, job, st, var, layout, flags, false, out, fb);
}

/** @brief Make the stage view a whole create of a job compiles: the job with its whole-create vertex stage (job->wb) in place of its vertex stage.
 *
 * The view is a shallow copy that shares the job's buffers; it is read,
 * never freed or installed.
 * @param job The job.
 * @param w Receives the view.
 * @return false when the job has no whole-create stage (w is untouched). */
static bool job_whole_view(const rhi_compile_job *job, rhi_compile_job *w)
{
    if (!job->wb) return false;
    *w = *job;
    w->vb = job->wb;
    w->vbn = job->wbn;
    w->desc.vert_spv = job->desc.vert_spv_whole;
    w->info.vs_hash = job->info.wvs_hash;
    w->info.vs_ids = job->info.wvs_ids;
    return true;
}

/** @brief Create the layout a pipeline's parts describe.
 *
 * @param dev Device.
 * @param st The parts (pipe_state_fill).
 * @param layout Receives the layout.
 * @return false when the create failed (logged). */
static bool pipe_layout_new(rhi_device *dev, const pipe_state *st, VkPipelineLayout *layout)
{
    *layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev->device, &st->plci, NULL, layout) == VK_SUCCESS) return true;
    RHI_LOG("vkCreatePipelineLayout failed\n");
    return false;
}

/** @brief Store a successful create as a job's result and add its manifest key, salted per link kind, to dev->pc_keys under dev->pc_mx.
 *
 * @param dev Device.
 * @param job The job; job->out receives c.
 * @param var The view the create compiled (its manifest key).
 * @param fb The create's feedback.
 * @param c The result. */
static void pipeline_compiled_keep(rhi_device *dev, rhi_compile_job *job, const rhi_pc_variant *var,
                                   const pipe_feedback *fb, const pipe_compiled *c)
{
    SDL_LockMutex(dev->pc_mx);
    const int fresh = rhi_pc_keyset_insert(&dev->pc_keys, pipe_manifest_key_as(var, c->link));
    dev->pcache_unhit += !(fb->pipe.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT);
    SDL_UnlockMutex(dev->pc_mx);
    if (!fresh) rhi_pc_known_add(pipe_phase_now());
    job->out = *c;
}

/** @brief Create a job's pipeline whole from a stage view of it and keep the result.
 *
 * @param dev Device.
 * @param job The job; job->out receives the result.
 * @param src The stage view: the job, or its whole-create view (job_whole_view).
 * @param flags Pipeline create flags.
 * @param key The result's dedupe key: the job's own create-info key.
 * @return 0 on success, 1 when a probe found it uncached, -1 on failure. */
static int pipeline_compile_whole(rhi_device *dev, rhi_compile_job *job, const rhi_compile_job *src,
                                  VkPipelineCreateFlags flags, uint64_t key)
{
    rhi_pc_variant var;
    pipe_variant_view(dev, &src->desc, &src->info, src->variant, src->slot_contest, &var);
    pipe_state st;
    VkPipelineLayout layout;
    if (!pipe_state_fill(dev, &src->desc, &src->info, &var, &st) ||
        !pipe_layout_new(dev, &st, &layout))
        return -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    pipe_feedback fb;
    const VkResult r = pipeline_create_whole(dev, src, &st, layout, flags, &pipeline, &fb);
    if (r != VK_SUCCESS) return pipeline_create_failed(dev, r, pipeline, layout);
    const pipe_compiled c = { .pipeline = pipeline, .layout = layout,
                              .pc_stages = src->desc.push_const_size ? st.pcr.stageFlags : 0,
                              .key = key, .link = PIPE_LINK_WHOLE, .whole_stage = src != job };
    pipeline_compiled_keep(dev, job, &var, &fb, &c);
    return 0;
}

/** @brief Create a library-path job's optimized form whole from its whole-create stage view, when it is owed or cached.
 *
 * A background optimization job (job->lto) creates it unconditionally. Any
 * other create makes it only when pipeline_optimized_listed says it is
 * cached, as a probe where the device has cache control, so a warm start
 * installs the optimized form at once and owes no background job.
 * @param dev Device.
 * @param job The job.
 * @param whole Its whole-create view (job_whole_view).
 * @param flags Pipeline create flags (the caller's probe).
 * @param key The job's own create-info key.
 * @return 0 when made, -1 on failure, 1 when the caller's probe found it
 *         uncached on a device without cache control, 2 when not made (not
 *         listed, or the probe found it uncached): the caller links from
 *         libraries. */
static int pipeline_compile_optimized(rhi_device *dev, rhi_compile_job *job,
                                      const rhi_compile_job *whole, VkPipelineCreateFlags flags,
                                      uint64_t key)
{
    if (job->lto) return pipeline_compile_whole(dev, job, whole, flags, key);
    rhi_pc_variant wvar;
    pipe_variant_view(dev, &whole->desc, &whole->info, whole->variant, whole->slot_contest, &wvar);
    if (!pipeline_optimized_listed(dev, &wvar, PIPE_LINK_WHOLE)) return 2;
    const VkPipelineCreateFlags probe = dev->has_cache_control
        ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT : 0u;
    const int r = pipeline_compile_whole(dev, job, whole, flags | probe, key);
    if (r == 0) atomic_fetch_add_explicit(&dev->lto.direct, 1u, memory_order_relaxed);
    if (r != 1 || !probe) return r;
    atomic_fetch_add_explicit(&dev->lto.probe_miss, 1u, memory_order_relaxed);
    return 2;
}

/** @brief Link a job's pipeline from its library parts (pipeline_link_choose) and keep the result.
 *
 * @param dev Device.
 * @param job The job; job->out receives the result.
 * @param var Its variant view.
 * @param st Its parts.
 * @param flags Pipeline create flags (the caller's probe).
 * @param key The job's create-info key.
 * @return 0 when linked, 1 when a probe found the link uncached, -1 when a
 *         part, the link or the layout failed (the caller creates it whole). */
static int pipeline_compile_linked(rhi_device *dev, rhi_compile_job *job, const rhi_pc_variant *var,
                                   const pipe_state *st, VkPipelineCreateFlags flags, uint64_t key)
{
    VkPipelineLayout layout;
    if (!pipe_layout_new(dev, st, &layout)) return -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    pipe_feedback fb;
    pipe_link link;
    const int r = pipeline_link_choose(dev, job, st, var, layout, flags, &pipeline, &fb, &link);
    if (r == 1) return pipeline_create_failed(dev, VK_PIPELINE_COMPILE_REQUIRED_EXT, VK_NULL_HANDLE,
                                              layout);
    if (r != 0) {
        vkDestroyPipelineLayout(dev->device, layout, NULL);
        return -1;
    }
    const pipe_compiled c = { .pipeline = pipeline, .layout = layout,
                              .pc_stages = job->desc.push_const_size ? st->pcr.stageFlags : 0,
                              .key = key, .link = link };
    pipeline_compiled_keep(dev, job, var, &fb, &c);
    return 0;
}

/** @brief Compile a job's Vulkan pipeline and layout from its desc, resolved stage bytes and variant.
 *
 * Any thread: reads the job, the device fields fixed at device creation and
 * the pipeline cache (internally synchronized); writes only the job's
 * result, the library entries (under their lock) and the atomic or locked
 * instruments. A pipeline pipe_uses_library selects is linked from its
 * library parts (pipeline_compile_linked), each built at most once per
 * run; a part or link failure creates it whole instead. A whole create
 * compiles the desc's whole-create vertex stage (vert_spv_whole) when it
 * names one, and such a library-path pipeline's optimized form is that
 * whole pipeline (pipeline_compile_optimized). The result's key is the
 * job's own create-info key whichever stage compiled, so a whole result
 * replaces the link it optimizes (lto_target_ok). Every create carries a
 * creation-feedback chain when the extension is enabled and is counted by
 * pipe_create_record; a successful create adds its manifest key (salted
 * per link kind) to dev->pc_keys under dev->pc_mx.
 * @param dev Device the objects are created on.
 * @param job The job; job->out receives the pipeline, layout,
 *            push-constant stage flags, dedupe key and link kind on success.
 * @param flags Pipeline create flags (the probe's FAIL_ON_PIPELINE_COMPILE_REQUIRED).
 * @param variant True for a precompile variant, which the depth report's
 *                per-compare count leaves out.
 * @return 0 on success, 1 when a probe found the create info uncached,
 *         -1 on failure. */
static int pipeline_compile(rhi_device *dev, rhi_compile_job *job, VkPipelineCreateFlags flags,
                            bool variant)
{
    rhi_pc_variant var;
    pipe_variant_view(dev, &job->desc, &job->info, job->variant, job->slot_contest, &var);
    pipe_state st;
    if (!pipe_state_fill(dev, &job->desc, &job->info, &var, &st)) return -1;
    if (!variant)
        atomic_fetch_add_explicit(&dev->pipes_by_cmp[st.dss.depthCompareOp],
                                  (unsigned)job->desc.depth_test, memory_order_relaxed);
    const uint64_t key = rhi_pc_variant_key(&var);
    rhi_compile_job wv;
    const rhi_compile_job *whole = job_whole_view(job, &wv) ? &wv : job;
    if (!pipe_uses_library(dev, &job->desc, job->vert_words || job->frag_words))
        return pipeline_compile_whole(dev, job, whole, flags, key);
    if (whole != job) {
        const int r = pipeline_compile_optimized(dev, job, whole, flags, key);
        if (r != 2) return r;
    }
    const int r = pipeline_compile_linked(dev, job, &var, &st, flags, key);
    return r == -1 ? pipeline_compile_whole(dev, job, whole, flags, key) : r;
}

/** @brief Free a compile job's owned copies and the job; its result is left to the caller.
 *
 * @param job The job, or NULL. */
static void job_free(rhi_compile_job *job)
{
    if (!job) return;
    free(job->vert_name); free(job->frag_name); free(job->whole_name);
    free(job->vert_words); free(job->frag_words);
    free(job->vown); free(job->fown); free(job->wown);
    free(job);
}

/** @brief Copy one stage's source into a job: SPIR-V words, else the .spv name.
 *
 * @param words Stage words, or NULL.
 * @param n Their count.
 * @param name The desc's .spv name, or NULL.
 * @param out_words Receives the word copy.
 * @param out_n Receives the word count.
 * @param out_name Receives the name copy.
 * @return false on allocation failure. */
static bool job_copy_stage(const uint32_t *words, size_t n, const char *name,
                           uint32_t **out_words, size_t *out_n, char **out_name)
{
    if (words && n) {
        *out_words = malloc(n * 4);
        if (!*out_words) return false;
        memcpy(*out_words, words, n * 4);
        *out_n = n;
        return true;
    }
    if (!name) return true;
    *out_name = strdup(name);
    return *out_name != NULL;
}

/** @brief Resolve a job's whole-create vertex stage (rhi_pipeline_desc.vert_spv_whole) into an owned name copy and wb.
 *
 * Display owner only (stage_bytes). A vertex stage given words takes none;
 * a name that resolves to no bytes leaves the job on its vert_spv stage.
 * @param job The job, its vertex stage resolved.
 * @param name The desc's vert_spv_whole, or NULL.
 * @return false on allocation failure. */
static bool job_whole_stage(rhi_compile_job *job, const char *name)
{
    if (!name || job->vert_words) return true;
    job->whole_name = strdup(name);
    if (!job->whole_name) return false;
    job->wb = stage_bytes(NULL, 0, job->whole_name, &job->wbn, &job->wown);
    if (job->wb) job->desc.vert_spv_whole = job->whole_name;
    return true;
}

/** @brief Make a job holding copies of a desc and its stage sources, with its stage bytes resolved.
 *
 * Display owner only: stage_bytes reads OPENPETE_SHADER_DIR and its file
 * here, and the census level, the terrain source tint and the key tier
 * order are latched here, so the job's run reads no environment and no
 * file. A stage given words has its .spv
 * name cleared from the desc copy.
 * @param desc Pipeline description.
 * @param vert_words Vertex words, or NULL.
 * @param vert_n Their count.
 * @param frag_words Fragment words, or NULL.
 * @param frag_n Their count.
 * @param variant Variant the job compiles for.
 * @param generation The enqueuer's slot generation.
 * @param epoch The variant epoch.
 * @return The job, or NULL on allocation failure or a stage with no source. */
static rhi_compile_job *job_make(const rhi_pipeline_desc *desc,
                                 const uint32_t *vert_words, size_t vert_n,
                                 const uint32_t *frag_words, size_t frag_n,
                                 rhi_pipe_variant variant, uint32_t generation, uint32_t epoch)
{
    rhi_compile_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->desc = *desc;
    if (!job_copy_stage(vert_words, vert_n, desc->vert_spv, &job->vert_words, &job->vert_n,
                        &job->vert_name) ||
        !job_copy_stage(frag_words, frag_n, desc->frag_spv, &job->frag_words, &job->frag_n,
                        &job->frag_name)) {
        job_free(job);
        return NULL;
    }
    job->desc.vert_spv = job->vert_name;
    job->desc.frag_spv = job->frag_name;
    job->desc.vert_spv_whole = NULL;
    job->vb = stage_bytes(job->vert_words, job->vert_n, job->vert_name, &job->vbn, &job->vown);
    job->fb = stage_bytes(job->frag_words, job->frag_n, job->frag_name, &job->fbn, &job->fown);
    if (!job->vb || !job->fb || !job_whole_stage(job, desc->vert_spv_whole)) {
        job_free(job);
        return NULL;
    }
    /* Latch every knob a run reads here, never in a run: the census level,
     * and the specialization constants stage_spec_fill passes (ids 6-8). */
    (void)rhi_pc_level();
    (void)rhi_terrain_source_tint();
    (void)key_tier_order();
    (void)key_blend_census();
    job->variant = variant;
    job->slot_contest = s_fs_slot_contest;
    job->generation = generation;
    job->epoch = epoch;
    return job;
}

void rhi_compile_job_set_slot_contest(rhi_compile_job *job, int on)
{
    if (job) job->slot_contest = on ? VK_TRUE : VK_FALSE;
}

rhi_compile_job *rhi_compile_job_new(rhi_device *dev, const rhi_pipeline_desc *desc,
                                     const uint32_t *vert_words, size_t vert_n,
                                     const uint32_t *frag_words, size_t frag_n,
                                     rhi_pipe_variant variant, uint32_t generation,
                                     uint32_t epoch)
{
    if (!dev || !desc) return NULL;
    seam_check("compile_job_new");   /* stage resolution reads the environment and files */
    rhi_compile_job *job = job_make(desc, vert_words, vert_n, frag_words, frag_n, variant,
                                    generation, epoch);
    if (!job) return NULL;
    job->info = (pipe_stage_info){
        .desc_hash = rhi_pc_desc_hash(&job->desc),
        .vs_hash = rhi_pc_bytes_hash(job->vb, job->vbn),
        .fs_hash = rhi_pc_bytes_hash(job->fb, job->fbn),
        .vs_ids = rhi_pc_spec_ids(job->vb, job->vbn),
        .fs_ids = rhi_pc_spec_ids(job->fb, job->fbn),
        .wvs_hash = job->wb ? rhi_pc_bytes_hash(job->wb, job->wbn) : 0u,
        .wvs_ids = rhi_pc_spec_ids(job->wb, job->wbn),
    };
    return job;
}

rhi_compile_job *rhi_compile_job_of(rhi_device *dev, const rhi_pipeline *pipe,
                                    rhi_pipe_variant variant, uint32_t generation,
                                    uint32_t epoch)
{
    if (!dev || !pipe) return NULL;
    seam_check("compile_job_of");   /* the registry and stage resolution are the display owner's */
    rhi_compile_job *job = job_make(&pipe->desc, pipe->vert_words, pipe->vert_n,
                                    pipe->frag_words, pipe->frag_n, variant, generation, epoch);
    if (job) job->info = pipe->info;
    return job;
}

static void env_read_begin(void);
static void env_read_end(void);

int rhi_compile_job_run(rhi_device *dev, rhi_compile_job *job, unsigned flags)
{
    if (!dev || !job) return -1;
    if (job->out.pipeline) return 0;
    const VkPipelineCreateFlags vf = (flags & RHI_COMPILE_PROBE)
        ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT : 0;
    env_read_begin();   /* the driver reads the environment inside the create */
    const int rc = pipeline_compile(dev, job, vf, (flags & RHI_COMPILE_VARIANT) != 0);
    env_read_end();
    return rc;
}

uint32_t rhi_compile_job_generation(const rhi_compile_job *job) { return job ? job->generation : 0u; }
uint32_t rhi_compile_job_epoch(const rhi_compile_job *job) { return job ? job->epoch : 0u; }

void rhi_compile_job_discard(rhi_device *dev, rhi_compile_job *job)
{
    if (!job) return;
    if (dev && job->out.pipeline) vkDestroyPipeline(dev->device, job->out.pipeline, NULL);
    if (dev && job->out.layout)   vkDestroyPipelineLayout(dev->device, job->out.layout, NULL);
    job_free(job);
}

static bool variant_eq(rhi_pipe_variant a, rhi_pipe_variant b);

bool rhi_compile_job_variant_stale(const rhi_device *dev, const rhi_compile_job *job)
{
    if (!dev || !job) return false;
    const rhi_pipe_variant cur = rhi_pipe_variant_current();
    if (variant_eq(cur, job->variant)) return false;
    rhi_pc_variant now, was;
    pipe_variant_view(dev, &job->desc, &job->info, cur, job->slot_contest, &now);
    pipe_variant_view(dev, &job->desc, &job->info, job->variant, job->slot_contest, &was);
    return rhi_pc_variant_key(&now) != rhi_pc_variant_key(&was);
}

void rhi_compile_job_retarget(rhi_device *dev, rhi_compile_job *job)
{
    if (!dev || !job) return;
    if (job->out.pipeline) vkDestroyPipeline(dev->device, job->out.pipeline, NULL);
    if (job->out.layout)   vkDestroyPipelineLayout(dev->device, job->out.layout, NULL);
    job->out = (pipe_compiled){0};
    job->variant = rhi_pipe_variant_current();
    job->epoch = rhi_pipe_variant_epoch();
}

/** @brief Link a pipeline at the head of the device's rebuild registry and give it the next registry serial.
 *
 * Display owner only (checked).
 * @param dev Device.
 * @param p The pipeline, not linked. */
static void registry_link(rhi_device *dev, rhi_pipeline *p)
{
    seam_check("pipeline_link");
    p->reg_serial = ++dev->pipe_serial;
    p->prev = NULL;
    p->next = dev->pipes_head;
    if (p->next) p->next->prev = p;
    dev->pipes_head = p;
}

/** @brief Unlink a pipeline from the device's rebuild registry.
 *
 * Display owner only (checked). A pipeline with no prev is the head.
 * @param dev Device.
 * @param p The pipeline, linked. */
static void registry_unlink(rhi_device *dev, rhi_pipeline *p)
{
    seam_check("pipeline_unlink");
    if (p->prev) p->prev->next = p->next;
    else         dev->pipes_head = p->next;
    if (p->next) p->next->prev = p->prev;
    p->prev = p->next = NULL;
}

static void rebuild_forget(rhi_device *dev, rhi_pipeline *p);
static void rebuild_join(rhi_device *dev, rhi_pipeline *p);

/** @brief Note that a pipeline's installed objects were made a given way: a fast link owes a link-time-optimized link (rhi_pipeline_lto_step).
 *
 * @param dev Device.
 * @param p The pipeline; its link is written.
 * @param link How its objects were made. */
static void lto_note_install(rhi_device *dev, rhi_pipeline *p, pipe_link link)
{
    p->link = link;
    if (!dev->has_lto || link != PIPE_LINK_FAST) return;
    dev->lto.owed = true;
    dev->lto.drained = false;
}

/** @brief Free a pipeline's owned stage copies and the struct; its Vulkan objects are left to the caller.
 *
 * @param p The pipeline, unlinked. */
static void pipeline_free(rhi_pipeline *p)
{
    free(p->vert_name); free(p->frag_name); free(p->whole_name);
    free(p->vert_words); free(p->frag_words);
    free(p);
}

rhi_pipeline *rhi_compile_job_install(rhi_device *dev, rhi_compile_job *job)
{
    if (!dev || !job) return NULL;
    if (!job->out.pipeline) {
        rhi_compile_job_discard(dev, job);
        return NULL;
    }
    rhi_pipeline *p = calloc(1, sizeof(*p));
    if (!p) {
        rhi_compile_job_discard(dev, job);
        return NULL;
    }
    p->desc = job->desc;
    p->vert_name = job->vert_name;   p->frag_name = job->frag_name;
    p->whole_name = job->whole_name;
    p->vert_words = job->vert_words; p->frag_words = job->frag_words;
    p->vert_n = job->vert_n;         p->frag_n = job->frag_n;
    job->vert_name = job->frag_name = job->whole_name = NULL;
    job->vert_words = job->frag_words = NULL;
    p->info = job->info;
    p->pipeline = job->out.pipeline;
    p->layout = job->out.layout;
    p->pc_stages = job->out.pc_stages;
    p->vkey = job->out.key;
    p->whole_stage = job->out.whole_stage;
    lto_note_install(dev, p, job->out.link);
    job_free(job);
    census_add_base(dev, p);
    registry_link(dev, p);
    rebuild_join(dev, p);
    return p;
}

/** @brief Retire a pipeline and layout by frame serial: destroyed once every frame that may have bound them has retired.
 *
 * An allocation failure waits the device idle and destroys them at once.
 * @param dev Device.
 * @param pipeline The pipeline, or VK_NULL_HANDLE.
 * @param layout The layout, or VK_NULL_HANDLE. */
static void retire_objects(rhi_device *dev, VkPipeline pipeline, VkPipelineLayout layout)
{
    struct rhi_retired_pipe *r = calloc(1, sizeof(*r));
    if (r) {
        *r = (struct rhi_retired_pipe){ dev->retired_pipes, pipeline, layout, s_frame_serial };
        dev->retired_pipes = r;
        return;
    }
    vkDeviceWaitIdle(dev->device);
    if (pipeline) vkDestroyPipeline(dev->device, pipeline, NULL);
    if (layout)   vkDestroyPipelineLayout(dev->device, layout, NULL);
}

void rhi_retire_pipeline(rhi_device *dev, rhi_pipeline *pipe)
{
    if (!dev || !pipe) return;
    rebuild_forget(dev, pipe);
    registry_unlink(dev, pipe);
    retire_objects(dev, pipe->pipeline, pipe->layout);
    pipeline_free(pipe);
}

/** @brief Create a pipeline on the display owner: make its job, compile it for the current variant, and install it.
 *
 * Word buffers (mod SPIR-V) take precedence over the desc's filenames; a
 * stage given words has its filename cleared. The new pipeline is pushed at
 * the head of dev->pipes_head.
 * @param dev Device the pipeline is created on.
 * @param desc Pipeline description, copied.
 * @param vert_words Vertex SPIR-V words, or NULL to use desc->vert_spv.
 * @param vert_n Word count of @p vert_words.
 * @param frag_words Fragment SPIR-V words, or NULL to use desc->frag_spv.
 * @param frag_n Word count of @p frag_words.
 * @return The pipeline, or NULL on allocation or compile failure. */
static rhi_pipeline *create_pipeline_common(rhi_device *dev,
                                            const rhi_pipeline_desc *desc,
                                            const uint32_t *vert_words, size_t vert_n,
                                            const uint32_t *frag_words, size_t frag_n)
{
    rhi_compile_job *job = rhi_compile_job_new(dev, desc, vert_words, vert_n, frag_words, frag_n,
                                               rhi_pipe_variant_current(), 0u,
                                               rhi_pipe_variant_epoch());
    if (!job) return NULL;
    rhi_compile_job_run(dev, job, 0);
    return rhi_compile_job_install(dev, job);
}

rhi_pipeline *rhi_create_pipeline(rhi_device *dev, const rhi_pipeline_desc *desc)
{
    return create_pipeline_common(dev, desc, NULL, 0, NULL, 0);
}

rhi_pipeline *rhi_create_pipeline_spv(rhi_device *dev,
                                      const rhi_pipeline_desc *desc,
                                      const uint32_t *vert_words, size_t vert_n,
                                      const uint32_t *frag_words, size_t frag_n)
{
    return create_pipeline_common(dev, desc, vert_words, vert_n,
                                  frag_words, frag_n);
}

/** @brief Install a rebuilt pipeline's new objects, retiring the replaced ones by frame serial.
 *
 * Frames in flight may still execute the old objects: they are destroyed
 * once those frames retire, so no device idle is needed. Objects a rebuild
 * installed outside a frame, with no frame begun since, were never recorded:
 * they are destroyed now instead of retired, so rebuilds while no frame
 * begins (a minimized window, a lost device) do not grow the retired list.
 * @param dev Device.
 * @param p The pipeline.
 * @param c Its new objects. */
static void rebuild_swap(rhi_device *dev, rhi_pipeline *p, const pipe_compiled *c)
{
    const bool unrecorded = p->swap_idle && !dev->frame_active &&
                            p->swap_serial == s_frame_serial;
    if (unrecorded) {
        if (p->pipeline) vkDestroyPipeline(dev->device, p->pipeline, NULL);
        if (p->layout)   vkDestroyPipelineLayout(dev->device, p->layout, NULL);
    } else {
        retire_objects(dev, p->pipeline, p->layout);
    }
    p->pipeline = c->pipeline;
    p->layout = c->layout;
    p->pc_stages = c->pc_stages;
    p->vkey = c->key;
    p->whole_stage = c->whole_stage;
    lto_note_install(dev, p, c->link);
    p->swap_serial = s_frame_serial;
    p->swap_idle = !dev->frame_active;
}

/* ---- environment write guard ---------------------------------------------
 * A compile run (rhi_compile_job_run, on any thread) holds the read side
 * for its create; an environment write holds the write side. A waiting
 * writer stops new compile runs, so a write waits for at most the runs
 * already in progress. A short read (rhi_env_read_guard) waits only while a
 * write is in progress, never behind a waiting writer, so it never waits
 * for a compile. */
static _Atomic int    s_env_state;   /* 0 none, 1 being created, 2 ready (env_guard_ready) */
static SDL_Mutex     *s_env_mx;
static SDL_Condition *s_env_cv;
static unsigned       s_env_readers, s_env_writers_waiting;
static bool           s_env_writing;

/** @brief Create the environment guard's lock on first use.
 *
 * The first caller creates it; a concurrent caller waits until it is
 * published. The state is a C11 atomic with acquire and release ordering,
 * so the lock's creation happens before any use on another thread.
 * @return true when the lock exists. */
static bool env_guard_ready(void)
{
    int st = atomic_load_explicit(&s_env_state, memory_order_acquire);
    if (st != 2) {
        int expect = 0;
        if (atomic_compare_exchange_strong(&s_env_state, &expect, 1)) {
            s_env_mx = SDL_CreateMutex();
            s_env_cv = SDL_CreateCondition();
            atomic_store_explicit(&s_env_state, 2, memory_order_release);
        } else {
            while (atomic_load_explicit(&s_env_state, memory_order_acquire) != 2) SDL_Delay(0);
        }
    }
    return s_env_mx && s_env_cv;
}

/** @brief Take the environment guard's read side for a compile run: wait while a write is in progress or waiting. */
static void env_read_begin(void)
{
    if (!env_guard_ready()) return;
    SDL_LockMutex(s_env_mx);
    while (s_env_writing || s_env_writers_waiting) SDL_WaitCondition(s_env_cv, s_env_mx);
    s_env_readers++;
    SDL_UnlockMutex(s_env_mx);
}

/** @brief Take the environment guard's read side for a short read: wait only while a write is in progress. */
static void env_short_read_begin(void)
{
    if (!env_guard_ready()) return;
    SDL_LockMutex(s_env_mx);
    while (s_env_writing) SDL_WaitCondition(s_env_cv, s_env_mx);
    s_env_readers++;
    SDL_UnlockMutex(s_env_mx);
}

/** @brief Release the environment guard's read side. */
static void env_read_end(void)
{
    if (!env_guard_ready()) return;
    SDL_LockMutex(s_env_mx);
    if (--s_env_readers == 0) SDL_BroadcastCondition(s_env_cv);
    SDL_UnlockMutex(s_env_mx);
}

void rhi_env_read_guard(int begin)
{
    if (begin) env_short_read_begin();
    else env_read_end();
}

void rhi_env_write_guard(int begin)
{
    if (!env_guard_ready()) return;
    SDL_LockMutex(s_env_mx);
    if (begin) {
        s_env_writers_waiting++;
        while (s_env_writing || s_env_readers) SDL_WaitCondition(s_env_cv, s_env_mx);
        s_env_writers_waiting--;
        s_env_writing = true;
    } else {
        s_env_writing = false;
        SDL_BroadcastCondition(s_env_cv);
    }
    SDL_UnlockMutex(s_env_mx);
}

/* ---- compile pool (rhi.h) ------------------------------------------------ */

#define POOL_MAX_WORKERS 32

/* A submit flag above the RHI_COMPILE_* bits: the job belongs to the ahead
 * walk, so rhi_precompile_ahead_stop may drop it while it is queued. */
#define POOL_AHEAD (1u << 8)

/* A submit flag above the RHI_COMPILE_* bits: the job waits on the low
 * queue, taken only when no library part and no other job is queued, and a
 * wait for the pool (pool_wait_below) does not wait for it. */
#define POOL_LOW (1u << 9)

/* A submit flag above the RHI_COMPILE_* bits: no wait of the boot is on the
 * job (the background variant walk). It and its library parts are background
 * items, as are POOL_AHEAD and POOL_LOW jobs: at most the pool's steady count
 * of its own workers run background items at once (pool_worker_pick_locked). */
#define POOL_BG (1u << 10)

/* Submit flag bits above POOL_LOW: an ahead job's rank (walk_item.rank). */
#define POOL_RANK_SHIFT 12u
#define POOL_RANK_MASK  (0xFu << POOL_RANK_SHIFT)

static uint64_t job_variant_key(const rhi_device *dev, const rhi_compile_job *job);

/* One submitted job and what its run returned, one graphics pipeline
 * library part to build (gpl non-NULL, job NULL), or one host task (task
 * non-NULL, job NULL; rhi_compile_pool_submit_task). */
typedef struct pool_item {
    struct pool_item   *next;
    rhi_compile_job    *job;
    rhi_pool_task_fn    task;      /* a host task, run with tag */
    gpl_src            *gpl;       /* a library part's source, owned */
    int                 gpl_part;
    uint64_t            gpl_key;
    unsigned            flags;     /* RHI_COMPILE_* */
    rhi_pool_install_fn fn;        /* NULL: a precompile variant */
    void               *tag;
    rhi_pipe_phase      phase;     /* the submitter's create phase */
    uint64_t            key;       /* the job's create-info key (job_variant_key) */
    bool                rebuild;   /* the submitter was in a live rebuild */
    bool                ahead;     /* a job of the ahead walk (rhi_precompile_ahead_stop drops it) */
    unsigned            rank;      /* with ahead: its rank (POOL_RANK_MASK) */
    bool                low;       /* a job of the low queue (POOL_LOW) */
    bool                bg;        /* a background item (POOL_BG): no wait of the boot is on it */
    bool                key_held;  /* key is in the pool's run_keys while the item runs */
    bool                probe_hit; /* a probe found the create info cached */
    int                 rc;        /* the run's result */
    uint64_t            run_ns;
} pool_item;

struct compile_pool {
    SDL_Mutex     *mx;
    SDL_Condition *work;           /* the pool's own workers: signalled on submit outside the low mode, and on stop */
    SDL_Condition *lo_work;        /* the low-priority workers: signalled on submit in the low mode, and on stop */
    SDL_Condition *done;           /* broadcast when a run returns */
    SDL_Thread    *thr[POOL_MAX_WORKERS];   /* worker-line: written at pool start only */
    int            n;              /* worker threads; 0 runs jobs at submit */
    int            n_steady;       /* of them, the most that run an item at once from the first tick on (pool_cap_locked) */
    unsigned       own_running;    /* items running on the pool's own workers, under mx */
    unsigned       own_bg_running; /* of them, background items (pool_item.bg), under mx */
    bool           stepped;        /* a worker has applied the first tick's cap, under mx */
    SDL_Thread    *lo_thr[POOL_MAX_WORKERS]; /* low-priority workers (rhi_compile_pool_low); worker-line: written at start only */
    int            lo_n;           /* low-priority worker threads started */
    pool_item     *q_head, *q_tail;   /* queued */
    pool_item     *l_head, *l_tail;   /* library parts queued, taken before q_head */
    pool_item     *s_head, *s_tail;   /* staged for the display owner */
    pool_item     *lo_head, *lo_tail; /* low queue, taken after q_head */
    unsigned       queued, running;   /* both count library parts and the low queue too */
    unsigned       lo_queued, lo_running; /* of them, low-queue jobs */
    bool           stop;
    bool           paused;         /* workers take no item (rhi_compile_pool_pause) */
    bool           low;            /* only the low-priority workers take items (rhi_compile_pool_low) */
    uint64_t       run_keys[POOL_MAX_WORKERS]; /* create-info keys of the jobs running; worker-line: under mx, per pipeline compile */
    unsigned       run_keys_n;
    rhi_compile_pool_stats st;     /* under mx */
    uint64_t       run_ns;         /* under mx */
    unsigned       libs_run;       /* library part items run, under mx */
    uint64_t       libs_ns;        /* their run time, under mx */
    unsigned       ahead_back[AHEAD_RANKS]; /* ahead jobs returned per rank, under mx */
    void         (*ahead_wake)(void);       /* rhi_precompile_ahead_start's wake, under mx */
};

/* Set once, at the process's first tick (rhi_compile_pool_first_tick), from
 * any thread; a pool's workers read it each time they take an item. */
static _Atomic int s_pool_first_tick;
/* The steady worker count of the last pool started, for the first tick's
 * log line; 0 before any pool starts. */
static _Atomic int s_pool_steady_n;

/** @brief Free a library part item and its source.
 *
 * @param it The item. */
static void gpl_item_free(pool_item *it)
{
    if (it->gpl) free(it->gpl->own);
    free(it->gpl);
    free(it);
}

/** @brief Mark a library part queued when no thread has claimed or queued it yet.
 *
 * Any thread.
 * @param dev Device.
 * @param key The part's key.
 * @return true when the call inserted the entry, so its caller owes it a pool item. */
static bool gpl_claim_new(rhi_device *dev, uint64_t key)
{
    bool inserted = false;
    SDL_LockMutex(dev->gpl_mx);
    const struct gpl_lib *e = gpl_entry(dev, key, &inserted);
    SDL_UnlockMutex(dev->gpl_mx);
    return e && inserted;
}

/** @brief Make the pool item that builds one library part of a job's pipeline from an owned copy of its source.
 *
 * @param job The job.
 * @param var Its variant view.
 * @param part The part.
 * @param key The part's key.
 * @param phase The submitter's create phase.
 * @param rebuild Whether the submitter is in a live rebuild.
 * @return The item, or NULL on allocation failure (the entry stays QUEUED,
 *         and the link builds it). */
static pool_item *gpl_item_new(const rhi_compile_job *job, const rhi_pc_variant *var, int part,
                               uint64_t key, rhi_pipe_phase phase, bool rebuild)
{
    pool_item *it = calloc(1, sizeof *it);
    gpl_src *src = it ? calloc(1, sizeof *src) : NULL;
    if (!src || !gpl_src_of(job, var, part, true, src)) {
        free(src);
        free(it);
        return NULL;
    }
    *it = (pool_item){ .gpl = src, .gpl_part = part, .gpl_key = key, .phase = phase,
                       .rebuild = rebuild };
    return it;
}

/** @brief Make one pool item per library part of a job's pipeline that no thread has claimed or queued yet, each entry marked queued.
 *
 * Display owner (a pool submit). The items run before any job on the pool
 * (pool_worker), so a link that waits on a part waits only on a part some
 * worker is building. A job off the library path, or an optimization job
 * created whole from its whole-create stage, gets none.
 * @param dev Device.
 * @param job The job about to be queued.
 * @param phase The submitter's create phase.
 * @param rebuild Whether the submitter is in a live rebuild.
 * @return A list of items linked by next, or NULL. */
static pool_item *gpl_items_for(rhi_device *dev, const rhi_compile_job *job, rhi_pipe_phase phase,
                                bool rebuild)
{
    if (!pipe_uses_library(dev, &job->desc, job->vert_words || job->frag_words)) return NULL;
    if (job->lto && job->wb) return NULL;   /* created whole (pipeline_compile_optimized) */
    rhi_pc_variant var;
    pipe_variant_view(dev, &job->desc, &job->info, job->variant, job->slot_contest, &var);
    pipe_state st;
    if (!pipe_state_fill(dev, &job->desc, &job->info, &var, &st)) return NULL;
    uint64_t key[GPL_PARTS];
    gpl_part_keys(dev, &job->desc, &st, &job->info, key);
    pool_item *head = NULL;
    for (int p = 0; p < GPL_PARTS; p++) {
        pool_item *it = gpl_claim_new(dev, key[p])
                            ? gpl_item_new(job, &var, p, key[p], phase, rebuild) : NULL;
        if (!it) continue;
        it->next = head;
        head = it;
    }
    return head;
}

/** @brief Run one pool item: probe first when asked, compile on a miss, and destroy a variant's result.
 *
 * Any thread. The create phase is the submitter's; each create holds the
 * environment guard's read side (rhi_compile_job_run).
 * @param dev Device.
 * @param it The item; it->rc, probe_hit and run_ns receive the outcome. */
static void pool_run_item(rhi_device *dev, pool_item *it)
{
    t_pipe_phase = it->phase;
    t_in_rebuild = it->rebuild;
    const Uint64 t0 = SDL_GetTicksNS();  /* clock-guard: wall: worker run time for the pool report */
    if (it->task) {
        it->task(it->tag);
        it->run_ns = SDL_GetTicksNS() - t0;  /* clock-guard: wall: worker run time for the pool report */
        t_pipe_phase = RHI_PIPE_PHASE_AUTO;
        t_in_rebuild = false;
        return;
    }
    if (it->gpl) {
        it->rc = gpl_acquire(dev, it->gpl_part, it->gpl_key, it->gpl) ? 0 : -1;
        it->run_ns = SDL_GetTicksNS() - t0;  /* clock-guard: wall: worker run time for the pool report */
        t_pipe_phase = RHI_PIPE_PHASE_AUTO;
        t_in_rebuild = false;
        return;
    }
    int rc = 1;
    if (it->flags & RHI_COMPILE_PROBE) {
        t_pipe_phase = RHI_PIPE_PHASE_PROBE;
        rc = rhi_compile_job_run(dev, it->job, it->flags);
        t_pipe_phase = it->phase;
        it->probe_hit = rc == 0;
    }
    if (rc == 1) rc = rhi_compile_job_run(dev, it->job, it->flags & ~(unsigned)RHI_COMPILE_PROBE);
    it->run_ns = SDL_GetTicksNS() - t0;  /* clock-guard: wall: worker run time for the pool report */
    it->rc = rc;
    if (!it->fn) {
        rhi_compile_job_discard(dev, it->job);
        it->job = NULL;
    }
    t_pipe_phase = RHI_PIPE_PHASE_AUTO;
    t_in_rebuild = false;
}

/** @brief Account a run item under the pool lock: count it, then stage it for the display owner or free a variant.
 *
 * @param pl The pool, locked.
 * @param it The item, run. */
static void pool_finish_locked(struct compile_pool *pl, pool_item *it)
{
    if (it->task) {
        pl->st.tasks_run++;
        pl->st.tasks_ms += (double)it->run_ns / 1e6;
        free(it);
        SDL_BroadcastCondition(pl->done);
        return;
    }
    if (it->gpl) {
        pl->libs_run++;
        pl->libs_ns += it->run_ns;
        gpl_item_free(it);
        SDL_BroadcastCondition(pl->done);
        return;
    }
    pl->st.run++;
    pl->run_ns += it->run_ns;
    if (it->ahead && it->rank < AHEAD_RANKS) pl->ahead_back[it->rank]++;
    if (!it->fn) {
        pl->st.variant_probe_hits += it->probe_hit;
        pl->st.variant_created += it->rc == 0 && !it->probe_hit;
        pl->st.variant_failed += it->rc < 0;
        free(it);
    } else {
        it->next = NULL;
        if (pl->s_tail) pl->s_tail->next = it; else pl->s_head = it;
        pl->s_tail = it;
        pl->st.staged++;
    }
    SDL_BroadcastCondition(pl->done);
}

/** @brief Return whether a worker is running a job of a create-info key.
 *
 * @param pl The pool, locked.
 * @param key The key.
 * @return The index of the key in run_keys, or -1. */
static int pool_key_running(const struct compile_pool *pl, uint64_t key)
{
    for (unsigned i = 0; i < pl->run_keys_n; i++)
        if (pl->run_keys[i] == key) return (int)i;
    return -1;
}

/** @brief Hold a job's create-info key for its run: wait while another worker runs the same key, then mark it running.
 *
 * Two concurrent creates of one create info would both compile it; the
 * second waits and then reads the first's result from the pipeline cache.
 * A library part item holds no key.
 * @param pl The pool, locked; unlocked while waiting.
 * @param it The item taken; key_held is set when the key was marked. */
static void pool_key_take(struct compile_pool *pl, pool_item *it)
{
    if (!it->job || !it->key) return;
    if (pool_key_running(pl, it->key) >= 0) {
        pl->st.same_key_waits++;
        while (pool_key_running(pl, it->key) >= 0) SDL_WaitCondition(pl->done, pl->mx);
    }
    if (pl->run_keys_n < POOL_MAX_WORKERS) {
        pl->run_keys[pl->run_keys_n++] = it->key;
        it->key_held = true;
    }
}

/** @brief Release the create-info key a job held for its run.
 *
 * The run of a callback-less job has already destroyed it, so the release
 * reads the item's flag, never its job.
 * @param pl The pool, locked.
 * @param it The item run. */
static void pool_key_drop(struct compile_pool *pl, const pool_item *it)
{
    const int i = it->key_held ? pool_key_running(pl, it->key) : -1;
    if (i >= 0) pl->run_keys[i] = pl->run_keys[--pl->run_keys_n];
}

/* A queued item a worker may take, with its place in its list. */
typedef struct {
    pool_item **head, **tail;   /* its list */
    pool_item  *prev;           /* the item before it, or NULL at the head */
    pool_item  *it;
} pool_pick;

/** @brief Find the first item of one queue a worker may take: any item, or with @p bg_ok false only an item that is not background.
 *
 * @param head The queue's head.
 * @param tail The queue's tail.
 * @param bg_ok Whether a background item may be taken.
 * @param out Receives the item and its place.
 * @return true when one was found. */
static bool pool_pick_in(pool_item **head, pool_item **tail, bool bg_ok, pool_pick *out)
{
    pool_item *prev = NULL;
    for (pool_item *it = *head; it; prev = it, it = it->next) {
        if (it->bg && !bg_ok) continue;
        *out = (pool_pick){ head, tail, prev, it };
        return true;
    }
    return false;
}

/** @brief Find the next item a worker may take: a library part first (a link waits only on parts already taken, or on a background part a background worker takes next), then a job, then a low-queue job.
 *
 * @param pl The pool, locked.
 * @param bg_ok Whether a background item may be taken.
 * @param out Receives the item and its place.
 * @return true when one was found. */
static bool pool_pick_locked(struct compile_pool *pl, bool bg_ok, pool_pick *out)
{
    return pool_pick_in(&pl->l_head, &pl->l_tail, bg_ok, out) ||
           pool_pick_in(&pl->q_head, &pl->q_tail, bg_ok, out) ||
           pool_pick_in(&pl->lo_head, &pl->lo_tail, bg_ok, out);
}

/** @brief Unlink a picked item from its queue and count it out of the queued counts.
 *
 * @param pl The pool, locked.
 * @param pk The pick (pool_pick_locked).
 * @return The item. */
static pool_item *pool_take_locked(struct compile_pool *pl, const pool_pick *pk)
{
    pool_item *it = pk->it;
    if (pk->prev) pk->prev->next = it->next; else *pk->head = it->next;
    if (*pk->tail == it) *pk->tail = pk->prev;
    pl->queued--;
    pl->lo_queued -= it->low;
    return it;
}

/** @brief Wake a worker of the set that takes items now (the low-priority workers in the low mode, the pool's own otherwise) after a submit.
 *
 * Each set waits on its own condition: a signal on a shared one could wake a
 * worker of the idle set, which sleeps again and consumes the wakeup.
 * @param pl The pool, locked.
 * @param all true to wake every worker of the set (a library part, whose link waits on its parts). */
static void pool_wake_locked(struct compile_pool *pl, bool all)
{
    SDL_Condition *c = pl->low ? pl->lo_work : pl->work;
    if (all) SDL_BroadcastCondition(c); else SDL_SignalCondition(c);
}

/** @brief Return how many of the pool's own workers may run an item at once: every worker before the first tick, the steady count from it on.
 *
 * At the first observation of the first tick the pool logs the step-down
 * with its running and queued counts. Items already running when the cap
 * falls finish; the cap only withholds new takes.
 * @param pl The pool, locked.
 * @return The cap. */
static unsigned pool_cap_locked(struct compile_pool *pl)
{
    if (!atomic_load_explicit(&s_pool_first_tick, memory_order_relaxed)) return (unsigned)pl->n;
    if (!pl->stepped) {
        pl->stepped = true;
        if (pl->n_steady < pl->n)
            RHI_LOG("compile pool: stepped down from %d to %d worker(s) taking items; %u running "
                    "(%u background), %u queued\n",
                    pl->n, pl->n_steady, pl->own_running, pl->own_bg_running, pl->queued);
    }
    return (unsigned)pl->n_steady;
}

/** @brief Find the item a worker takes next, if it may take one: none while the pool is paused or in the other mode; on a pool's own worker none at its cap (pool_cap_locked), and a background item only while fewer than the steady count of background items run.
 *
 * The workers beyond the steady count therefore run only items a wait of
 * the boot is on, and at the first tick at most the steady count of
 * background items are in flight.
 * @param pl The pool, locked.
 * @param low true on a low-priority worker.
 * @param out Receives the item and its place.
 * @return true to take it, false to wait. */
static bool pool_worker_pick_locked(struct compile_pool *pl, bool low, pool_pick *out)
{
    if (pl->paused || pl->low != low) return false;
    if (low) return pool_pick_locked(pl, true, out);
    if (pl->own_running >= pool_cap_locked(pl)) return false;
    return pool_pick_locked(pl, pl->own_bg_running < (unsigned)pl->n_steady, out);
}

/** @brief Run queued items until the pool stops, each the one pool_worker_pick_locked lets this worker take.
 *
 * @param dev The device.
 * @param low true on a low-priority worker (pool_worker_low). */
static void pool_worker_loop(rhi_device *dev, bool low)
{
    struct compile_pool *pl = dev->cpool;
    SDL_LockMutex(pl->mx);
    for (;;) {
        pool_pick pk;
        while (!pl->stop && !pool_worker_pick_locked(pl, low, &pk))
            SDL_WaitCondition(low ? pl->lo_work : pl->work, pl->mx);
        if (pl->stop) break;
        pool_item *it = pool_take_locked(pl, &pk);
        const bool own_bg = !low && it->bg;
        pl->running++;
        pl->own_running += !low;
        pl->own_bg_running += own_bg;
        pl->lo_running += it->low;
        pool_key_take(pl, it);
        SDL_UnlockMutex(pl->mx);
        pool_run_item(dev, it);
        SDL_LockMutex(pl->mx);
        pool_key_drop(pl, it);
        pl->running--;
        pl->own_running -= !low;
        pl->own_bg_running -= own_bg;
        pl->lo_running -= it->low;
        void (*wake)(void) = it->ahead ? pl->ahead_wake : NULL;
        pool_finish_locked(pl, it);
        if (wake) {
            SDL_UnlockMutex(pl->mx);
            wake();
            SDL_LockMutex(pl->mx);
        }
    }
    SDL_UnlockMutex(pl->mx);
}

/** @brief Worker thread at the creator's priority: run queued items while the pool is not in its low mode.
 *
 * @param arg The device.
 * @return 0. */
static int SDLCALL pool_worker(void *arg)
{
    OP_ROLE_BIND(compile);
    pool_worker_loop(arg, false);
    return 0;
}

/** @brief Low-priority worker thread: lower its own scheduling priority once, then run queued items while the pool is in its low mode.
 *
 * The priority is set once at the thread's start and never raised: an
 * unprivileged Linux thread cannot lower its niceness again (RLIMIT_NICE),
 * so the pool keeps separate threads for each priority instead of moving
 * one set between them.
 * @param arg The device.
 * @return 0. */
static int SDLCALL pool_worker_low(void *arg)
{
    OP_ROLE_BIND(compile);
    if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW))
        RHI_LOG("compile pool: a low-priority worker kept its priority (%s)\n", SDL_GetError());
    pool_worker_loop(arg, true);
    return 0;
}

/** @brief Return the pool's steady worker count, the most that run an item at once from the first tick on: OPENPETE_COMPILE_POOL when set, else half the logical cores, at least 1.
 *
 * Display owner only (reads the environment).
 * @return The count, 0 to POOL_MAX_WORKERS. */
static int pool_size_wanted(void)
{
    const char *e = getenv("OPENPETE_COMPILE_POOL");
    int n;
    if (e && *e) {
        n = atoi(e);
    } else {
        n = SDL_GetNumLogicalCPUCores() / 2;
        if (n < 1) n = 1;
    }
    return n < 0 ? 0 : n > POOL_MAX_WORKERS ? POOL_MAX_WORKERS : n;
}

/** @brief Return the worker threads the pool starts: OPENPETE_COMPILE_POOL when set, else every logical core before the first tick, else the steady count.
 *
 * Display owner only (reads the environment).
 * @param steady The steady count (pool_size_wanted).
 * @return The count, @p steady to POOL_MAX_WORKERS. */
static int pool_size_boot(int steady)
{
    const char *e = getenv("OPENPETE_COMPILE_POOL");
    if ((e && *e) || atomic_load(&s_pool_first_tick)) return steady;
    const int n = SDL_GetNumLogicalCPUCores();
    return n < steady ? steady : n > POOL_MAX_WORKERS ? POOL_MAX_WORKERS : n;
}

/** @brief Free a pool's lock objects and the pool.
 *
 * @param pl The pool, with no thread running. */
static void pool_free(struct compile_pool *pl)
{
    if (pl->mx) SDL_DestroyMutex(pl->mx);
    if (pl->work) SDL_DestroyCondition(pl->work);
    if (pl->lo_work) SDL_DestroyCondition(pl->lo_work);
    if (pl->done) SDL_DestroyCondition(pl->done);
    free(pl);
}

/** @brief Enter or leave the pool's low mode, starting its low-priority workers on the first entry.
 *
 * Half the pool's steady worker count (at least 1) run at low priority: where the
 * OS priority ties with the work that runs beside it (Windows' lowest thread
 * priority of a normal-class process equals a below-normal-class process's
 * normal thread), the count bounds the pool's share. A pool with no worker
 * runs jobs at submit and starts none. A low worker that cannot be started
 * leaves fewer; with none the mode is not entered, so the queue still runs.
 * @param dev Device.
 * @param pl Its pool, unlocked.
 * @param on true to enter, false to leave. */
static void pool_low_set(rhi_device *dev, struct compile_pool *pl, bool on)
{
    if (on && pl->n > 0 && pl->lo_n == 0) {
        const int want = pl->n_steady / 2 > 0 ? pl->n_steady / 2 : 1;
        for (int i = 0; i < want; i++) {
            pl->lo_thr[pl->lo_n] = SDL_CreateThread(pool_worker_low, "openpete-compile-low", dev);
            if (!pl->lo_thr[pl->lo_n]) break;
            pl->lo_n++;
        }
        RHI_LOG("compile pool: %d low-priority worker(s)\n", pl->lo_n);
    }
    SDL_LockMutex(pl->mx);
    pl->low = on && pl->lo_n > 0;
    const unsigned running = pl->running, queued = pl->queued;
    SDL_BroadcastCondition(pl->work);
    SDL_BroadcastCondition(pl->lo_work);
    SDL_UnlockMutex(pl->mx);
    RHI_LOG("compile pool: %s; %u running, %u queued\n",
            on ? "low mode (only the low-priority workers take items)" : "normal mode", running, queued);
}

/** @brief Return the device's compile pool, starting it on first use.
 *
 * Display owner only. A worker that cannot be started leaves the pool
 * smaller; with none the pool runs jobs at submit.
 * @param dev Device.
 * @return The pool, or NULL after rhi_compile_pool_stop or on allocation failure. */
static struct compile_pool *pool_get(rhi_device *dev)
{
    if (dev->cpool || dev->cpool_stopped) return dev->cpool;
    struct compile_pool *pl = calloc(1, sizeof *pl);
    if (!pl) return NULL;
    pl->mx = SDL_CreateMutex();
    pl->work = SDL_CreateCondition();
    pl->lo_work = SDL_CreateCondition();
    pl->done = SDL_CreateCondition();
    if (!pl->mx || !pl->work || !pl->lo_work || !pl->done) { pool_free(pl); return NULL; }
    (void)env_guard_ready();   /* created here, before any worker runs */
    pl->paused = dev->cpool_paused;
    dev->cpool = pl;
    const int steady = pool_size_wanted();
    const int want = pool_size_boot(steady);
    for (int i = 0; i < want; i++) {
        pl->thr[pl->n] = SDL_CreateThread(pool_worker, "openpete-compile", dev);
        if (!pl->thr[pl->n]) break;
        pl->n++;
    }
    pl->n_steady = steady < pl->n ? steady : pl->n;
    pl->st.workers = (unsigned)pl->n;
    pl->st.workers_steady = (unsigned)pl->n_steady;
    atomic_store(&s_pool_steady_n, pl->n_steady);
    RHI_LOG("compile pool: %d worker(s), %d of them taking items from the first tick%s\n", pl->n,
            pl->n_steady, pl->n ? "" : " (jobs run at submit)");
    if (dev->cpool_low) pool_low_set(dev, pl, true);
    return pl;
}

/** @brief Append library part items to the pool's part queue, each counted as queued.
 *
 * @param pl The pool, locked.
 * @param libs Items linked by next (gpl_items_for), or NULL.
 * @param bg Whether they are background items (parts of a background job). */
static void pool_queue_parts_locked(struct compile_pool *pl, pool_item *libs, bool bg)
{
    for (pool_item *l = libs, *next; l; l = next) {
        next = l->next;
        l->next = NULL;
        l->bg = bg;
        if (pl->l_tail) pl->l_tail->next = l; else pl->l_head = l;
        pl->l_tail = l;
        pl->queued++;
    }
}

int rhi_compile_pool_submit(rhi_device *dev, rhi_compile_job *job, unsigned flags,
                            rhi_pool_install_fn fn, void *tag)
{
    if (!dev || !job) return -1;
    seam_check("compile_pool_submit");
    struct compile_pool *pl = pool_get(dev);
    pool_item *it = calloc(1, sizeof *it);
    pool_item local;
    if (!it) { memset(&local, 0, sizeof local); it = &local; }
    *it = (pool_item){ .job = job,
                       .flags = flags & ~(unsigned)(POOL_AHEAD | POOL_LOW | POOL_BG | POOL_RANK_MASK),
                       .fn = fn,
                       .tag = tag, .phase = t_pipe_phase,
                       .key = job_variant_key(dev, job) ^ (job->lto ? LTO_MANIFEST_SALT : 0u),
                       .rebuild = t_in_rebuild, .ahead = (flags & POOL_AHEAD) != 0,
                       .rank = (flags & POOL_RANK_MASK) >> POOL_RANK_SHIFT,
                       .low = (flags & POOL_LOW) != 0,
                       .bg = (flags & (POOL_BG | POOL_AHEAD | POOL_LOW)) != 0 };
    if (!pl || it == &local) {   /* no pool: run now, install now */
        pool_run_item(dev, it);
        if (it->fn) it->fn(dev, it->job, it->rc, it->tag);
        if (it != &local) free(it);
        return 0;
    }
    if (pl->n == 0) {            /* a pool of none: run now, install at the drain */
        pool_run_item(dev, it);
        SDL_LockMutex(pl->mx);
        pl->st.submitted++;
        pool_finish_locked(pl, it);
        SDL_UnlockMutex(pl->mx);
        return 0;
    }
    pool_item *libs = gpl_items_for(dev, job, it->phase, it->rebuild);
    SDL_LockMutex(pl->mx);
    pool_queue_parts_locked(pl, libs, it->bg);
    pool_item **head = it->low ? &pl->lo_head : &pl->q_head;
    pool_item **tail = it->low ? &pl->lo_tail : &pl->q_tail;
    if (*tail) (*tail)->next = it; else *head = it;
    *tail = it;
    pl->queued++;
    pl->lo_queued += it->low;
    pl->st.submitted++;
    if (pl->queued > pl->st.max_queued) pl->st.max_queued = pl->queued;
    pool_wake_locked(pl, libs);
    SDL_UnlockMutex(pl->mx);
    return 0;
}

/** @brief Queue a host task behind the jobs already queued, as a background item or not; run it at once without a worker.
 *
 * Display owner only.
 * @param dev Device.
 * @param run The task.
 * @param tag Passed to @p run.
 * @param bg Whether the task is a background item.
 * @return 0, or -1 for a NULL device or task. */
static int pool_submit_task(rhi_device *dev, rhi_pool_task_fn run, void *tag, bool bg)
{
    if (!dev || !run) return -1;
    seam_check("compile_pool_submit");
    struct compile_pool *pl = pool_get(dev);
    pool_item *it = pl ? calloc(1, sizeof *it) : NULL;
    if (!it || pl->n == 0) {     /* no pool, or a pool of none: run now */
        free(it);
        run(tag);
        return 0;
    }
    *it = (pool_item){ .task = run, .tag = tag, .phase = t_pipe_phase, .bg = bg };
    SDL_LockMutex(pl->mx);
    if (pl->q_tail) pl->q_tail->next = it; else pl->q_head = it;
    pl->q_tail = it;
    pl->queued++;
    pl->st.tasks_submitted++;
    if (pl->queued > pl->st.max_queued) pl->st.max_queued = pl->queued;
    pool_wake_locked(pl, false);
    SDL_UnlockMutex(pl->mx);
    return 0;
}

int rhi_compile_pool_submit_task(rhi_device *dev, rhi_pool_task_fn run, void *tag)
{
    return pool_submit_task(dev, run, tag, false);
}

int rhi_compile_pool_submit_background_task(rhi_device *dev, rhi_pool_task_fn run, void *tag)
{
    return pool_submit_task(dev, run, tag, true);
}

unsigned rhi_compile_pool_drain(rhi_device *dev)
{
    struct compile_pool *pl = dev ? dev->cpool : NULL;
    if (!pl) return 0;
    seam_check("compile_pool_drain");
    SDL_LockMutex(pl->mx);
    pool_item *it = pl->s_head;
    pl->s_head = pl->s_tail = NULL;
    SDL_UnlockMutex(pl->mx);
    unsigned n = 0;
    for (; it; n++) {
        pool_item *next = it->next;
        it->fn(dev, it->job, it->rc, it->tag);
        free(it);
        it = next;
    }
    return n;
}

/** @brief Wait for the pool, draining staged results and pumping, until fewer than @p cap jobs are queued (cap 0: until nothing is queued, running or staged).
 *
 * Low-queue jobs (POOL_LOW) are not waited for; their staged results are
 * drained with the rest.
 * Display owner only.
 * @param dev Device.
 * @param cap Queue depth to wait below, or 0.
 * @param pump Called between waits, or NULL.
 * @param ctx Passed to @p pump. */
static void pool_wait_below(rhi_device *dev, unsigned cap, void (*pump)(void *), void *ctx)
{
    struct compile_pool *pl = dev->cpool;
    if (!pl) return;
    for (;;) {
        rhi_compile_pool_drain(dev);
        SDL_LockMutex(pl->mx);
        const unsigned queued = pl->queued - pl->lo_queued, running = pl->running - pl->lo_running;
        const bool met = cap ? queued < cap : (!queued && !running && !pl->s_head);
        if (!met && !pl->s_head) SDL_WaitConditionTimeout(pl->done, pl->mx, 10);
        SDL_UnlockMutex(pl->mx);
        if (met) return;
        if (pump) pump(ctx);
    }
}

void rhi_compile_pool_wait(rhi_device *dev, void (*pump)(void *ctx), void *ctx)
{
    if (dev) pool_wait_below(dev, 0, pump, ctx);
}

void rhi_compile_pool_stats_get(const rhi_device *dev, rhi_compile_pool_stats *out)
{
    struct compile_pool *pl = dev ? dev->cpool : NULL;
    *out = dev ? dev->cpool_final : (rhi_compile_pool_stats){0};
    if (!pl) return;
    SDL_LockMutex(pl->mx);
    *out = pl->st;
    out->run_ms = (double)pl->run_ns / 1e6;
    SDL_UnlockMutex(pl->mx);
}

/** @brief Drop a list of pool items: an install callback receives RHI_POOL_CANCELLED, a variant job is destroyed.
 *
 * @param dev Device.
 * @param it The list head.
 * @return The items dropped. */
static unsigned pool_cancel_list(rhi_device *dev, pool_item *it)
{
    unsigned n = 0;
    for (; it; n++) {
        pool_item *next = it->next;
        if (it->task) { /* a host task never run: its owner sees it unfinished */ }
        else if (it->fn) it->fn(dev, it->job, RHI_POOL_CANCELLED, it->tag);
        else rhi_compile_job_discard(dev, it->job);
        free(it);
        it = next;
    }
    return n;
}

static void rebuild_abandon(rhi_device *dev);
static void walk_end(rhi_device *dev, struct precompile_walk *w, const char *cut);

void rhi_compile_pool_stop(rhi_device *dev)
{
    if (!dev) return;
    dev->cpool_stopped = true;
    struct compile_pool *pl = dev->cpool;
    if (pl) {
        seam_check("compile_pool_stop");
        SDL_LockMutex(pl->mx);
        pl->stop = true;
        SDL_BroadcastCondition(pl->work);
        SDL_BroadcastCondition(pl->lo_work);
        SDL_UnlockMutex(pl->mx);
        for (int i = 0; i < pl->n; i++) SDL_WaitThread(pl->thr[i], NULL);
        for (int i = 0; i < pl->lo_n; i++) SDL_WaitThread(pl->lo_thr[i], NULL);
        pl->st.cancelled += pool_cancel_list(dev, pl->q_head);
        pl->st.cancelled += pool_cancel_list(dev, pl->lo_head);
        pl->st.cancelled += pool_cancel_list(dev, pl->s_head);
        for (pool_item *l = pl->l_head, *next; l; l = next) {   /* their entries stay QUEUED */
            next = l->next;
            gpl_item_free(l);
        }
        pl->q_head = pl->q_tail = pl->s_head = pl->s_tail = pl->l_head = pl->l_tail = NULL;
        pl->lo_head = pl->lo_tail = NULL;
        RHI_LOG("compile pool: stopped; %u workers (%u from the first tick), %u submitted, %u run "
                "(%.1f ms), %u staged, variants %u created %u probe hits %u failed, %u cancelled, "
                "queue max %u, %u library parts run (%.1f ms), %u same-key waits\n",
                pl->st.workers, pl->st.workers_steady, pl->st.submitted, pl->st.run, (double)pl->run_ns / 1e6,
                pl->st.staged, pl->st.variant_created, pl->st.variant_probe_hits,
                pl->st.variant_failed, pl->st.cancelled, pl->st.max_queued, pl->libs_run,
                (double)pl->libs_ns / 1e6, pl->st.same_key_waits);
        dev->cpool_final = pl->st;
        dev->cpool_final.run_ms = (double)pl->run_ns / 1e6;
        dev->cpool = NULL;
        pool_free(pl);
    }
    rebuild_abandon(dev);
    if (dev->walk.state == RHI_WALK_RUNNING) walk_end(dev, &dev->walk, "abandoned at the pool's stop");
}

void rhi_compile_pool_pause(rhi_device *dev, bool on)
{
    if (!dev) return;
    seam_check("compile_pool_pause");
    dev->cpool_paused = on;
    struct compile_pool *pl = dev->cpool;
    if (!pl) return;
    SDL_LockMutex(pl->mx);
    pl->paused = on;
    const unsigned running = pl->running, queued = pl->queued;
    if (!on) {
        SDL_BroadcastCondition(pl->work);
        SDL_BroadcastCondition(pl->lo_work);
    }
    SDL_UnlockMutex(pl->mx);
    RHI_LOG("compile pool: %s; %u running, %u queued\n",
            on ? "paused (the running items finish, no other starts)" : "resumed", running, queued);
}

void rhi_compile_pool_first_tick(void)
{
    if (atomic_load_explicit(&s_pool_first_tick, memory_order_relaxed)) return;
    if (atomic_exchange(&s_pool_first_tick, 1)) return;
    const int steady = atomic_load(&s_pool_steady_n);
    if (steady > 0)
        RHI_LOG("compile pool: first tick; from here at most %d worker(s) take items at once\n", steady);
    else
        RHI_LOG("compile pool: first tick before the pool started; it starts at its steady count\n");
}

void rhi_compile_pool_low(rhi_device *dev, bool on)
{
    if (!dev) return;
    seam_check("compile_pool_low");
    dev->cpool_low = on;
    if (dev->cpool) pool_low_set(dev, dev->cpool, on);
}

/* ---- pipeline batch (rhi.h) ---------------------------------------------- */

/* Where a batch create installs: the slot and the value a failure leaves. */
typedef struct {
    void **slot;
    void  *failed;
} batch_slot;

/** @brief Install a batch create's result into its slot, or the failed value, or discard it when the slot was filled first.
 *
 * Display owner (an rhi_pool_install_fn).
 * @param dev Device.
 * @param job The job, consumed.
 * @param rc Its run's result, or RHI_POOL_CANCELLED.
 * @param tag Its batch_slot, freed. */
static void batch_install(rhi_device *dev, rhi_compile_job *job, int rc, void *tag)
{
    batch_slot *b = tag;
    if (rc == RHI_POOL_CANCELLED || *b->slot) {
        dev->batch.duplicate += rc != RHI_POOL_CANCELLED;
        rhi_compile_job_discard(dev, job);
        free(b);
        return;
    }
    char what[160];
    snprintf(what, sizeof what, "%s + %s", job->desc.vert_spv ? job->desc.vert_spv : "(words)",
             job->desc.frag_spv ? job->desc.frag_spv : "(words)");
    rhi_pipeline *p = NULL;
    if (rc == 0) p = rhi_compile_job_install(dev, job);
    else rhi_compile_job_discard(dev, job);
    if (p) {
        dev->batch.installed++;
        *b->slot = p;
    } else {
        dev->batch.failed++;
        *b->slot = b->failed;
        RHI_LOG("pipeline batch: build FAILED for %s\n", what);
    }
    free(b);
}

void rhi_pipeline_batch_begin(rhi_device *dev)
{
    if (!dev) return;
    seam_check("pipeline_batch_begin");
    dev->batch_open = true;
    dev->batch = (rhi_pipeline_batch_stats){0};
    dev->batch_t0 = SDL_GetTicksNS();  /* clock-guard: wall: batch duration for the log */
}

void rhi_pipeline_batch_end(rhi_device *dev, void (*pump)(void *ctx), void *ctx,
                            rhi_pipeline_batch_stats *out)
{
    if (!dev) return;
    rhi_compile_pool_wait(dev, pump, ctx);
    dev->batch_open = false;
    dev->batch.ms = (double)(SDL_GetTicksNS() - dev->batch_t0) / 1e6;  /* clock-guard: wall: batch duration for the log */
    if (out) *out = dev->batch;
}

/* The value description mode leaves in a slot: non-NULL, never a pipeline. */
static char s_described_marker;

/** @brief Record a description in description mode: copy it with owned .spv names, or count a stage given SPIR-V words as skipped.
 *
 * @param dev Device, describing.
 * @param desc The description.
 * @param words Nonzero when a stage was given SPIR-V words. */
static void describe_add(rhi_device *dev, const rhi_pipeline_desc *desc, bool words)
{
    if (words) { dev->described_words++; return; }
    if (dev->described_n == dev->described_cap) {
        const unsigned cap = dev->described_cap ? dev->described_cap * 2u : 64u;
        described_desc *v = realloc(dev->described, cap * sizeof *v);
        if (!v) return;
        dev->described = v;
        dev->described_cap = cap;
    }
    described_desc *d = &dev->described[dev->described_n];
    *d = (described_desc){ .desc = *desc,
                           .vert = desc->vert_spv ? strdup(desc->vert_spv) : NULL,
                           .frag = desc->frag_spv ? strdup(desc->frag_spv) : NULL,
                           .whole = desc->vert_spv_whole ? strdup(desc->vert_spv_whole) : NULL,
                           .group = dev->describe_group };
    if ((desc->vert_spv && !d->vert) || (desc->frag_spv && !d->frag) ||
        (desc->vert_spv_whole && !d->whole)) {
        free(d->vert);
        free(d->frag);
        free(d->whole);
        return;
    }
    d->desc.vert_spv = d->vert;
    d->desc.frag_spv = d->frag;
    d->desc.vert_spv_whole = d->whole;
    dev->described_n++;
}

void rhi_pipeline_describe_begin(rhi_device *dev)
{
    if (!dev) return;
    seam_check("pipeline_describe_begin");
    dev->describing = true;
    dev->describe_group = 0;
    dev->describe_mark_n = dev->described_n;
    dev->describe_mark_words = dev->described_words;
}

void rhi_pipeline_describe_group(rhi_device *dev, unsigned group)
{
    if (dev) dev->describe_group = group < RHI_AHEAD_GROUPS ? group : RHI_AHEAD_GROUPS - 1u;
}

unsigned rhi_pipeline_describe_end(rhi_device *dev)
{
    if (!dev) return 0;
    dev->describing = false;
    return dev->described_n;
}

unsigned rhi_pipeline_describe_discard(rhi_device *dev)
{
    if (!dev) return 0;
    dev->describing = false;
    const unsigned seen = (dev->described_n - dev->describe_mark_n) +
                          (dev->described_words - dev->describe_mark_words);
    for (unsigned i = dev->describe_mark_n; i < dev->described_n; i++) {
        free(dev->described[i].vert);
        free(dev->described[i].frag);
        free(dev->described[i].whole);
    }
    dev->described_n = dev->describe_mark_n;
    dev->described_words = dev->describe_mark_words;
    return seen;
}

/** @brief Free the recorded descriptions.
 *
 * @param dev Device. */
static void described_free(rhi_device *dev)
{
    for (unsigned i = 0; i < dev->described_n; i++) {
        free(dev->described[i].vert);
        free(dev->described[i].frag);
        free(dev->described[i].whole);
    }
    free(dev->described);
    dev->described = NULL;
    dev->described_n = dev->described_cap = 0;
}

int rhi_create_pipeline_slot(rhi_device *dev, const rhi_pipeline_desc *desc,
                             const uint32_t *vert_words, size_t vert_n,
                             const uint32_t *frag_words, size_t frag_n,
                             void **slot, void *failed)
{
    if (!dev || !desc || !slot) return -1;
    if (dev->describing) {
        describe_add(dev, desc, (vert_words && vert_n) || (frag_words && frag_n));
        *slot = &s_described_marker;
        return 1;
    }
    if (!dev->batch_open) {
        rhi_pipeline *p = create_pipeline_common(dev, desc, vert_words, vert_n, frag_words, frag_n);
        *slot = p ? (void *)p : failed;
        return p ? 0 : -1;
    }
    rhi_compile_job *job = rhi_compile_job_new(dev, desc, vert_words, vert_n, frag_words, frag_n,
                                               rhi_pipe_variant_current(), 0u,
                                               rhi_pipe_variant_epoch());
    batch_slot *b = job ? malloc(sizeof *b) : NULL;
    if (!b) {
        rhi_compile_job_discard(dev, job);
        *slot = failed;
        return -1;
    }
    *b = (batch_slot){ slot, failed };
    dev->batch.queued++;
    rhi_compile_pool_submit(dev, job, 0, batch_install, b);
    return 1;
}

/* ---- live rebuild (rhi.h) ------------------------------------------------ */

/* One member of a live rebuild: the pipeline, the create info it is rebuilt
 * to, and the compiled result once its job returns. */
struct rebuild_entry {
    rhi_pipeline *p;        /* NULL: the member left the set */
    uint64_t      key;      /* create-info key the member is rebuilt to */
    pipe_compiled ready;    /* the result; pipeline VK_NULL_HANDLE until it arrives */
    bool          waiting;  /* a job for key is outstanding */
};

/** @brief Return the create-info key a compile job compiles: its desc, stages and variant.
 *
 * @param dev Device.
 * @param job The job.
 * @return The key. */
static uint64_t job_variant_key(const rhi_device *dev, const rhi_compile_job *job)
{
    rhi_pc_variant v;
    pipe_variant_view(dev, &job->desc, &job->info, job->variant, job->slot_contest, &v);
    return rhi_pc_variant_key(&v);
}

/** @brief Destroy a member's compiled result (never linked or recorded) and stop waiting for its job.
 *
 * @param dev Device.
 * @param e The entry. */
static void rebuild_entry_clear(rhi_device *dev, struct rebuild_entry *e)
{
    if (e->ready.pipeline) vkDestroyPipeline(dev->device, e->ready.pipeline, NULL);
    if (e->ready.layout)   vkDestroyPipelineLayout(dev->device, e->ready.layout, NULL);
    e->ready = (pipe_compiled){0};
    if (e->waiting) dev->rb.waiting--;
    e->waiting = false;
}

/** @brief Remove a member from the rebuild set, destroying its result; a job still out for it is discarded when it returns.
 *
 * @param dev Device.
 * @param e The member's entry. */
static void rebuild_drop(rhi_device *dev, struct rebuild_entry *e)
{
    rebuild_entry_clear(dev, e);
    if (e->p) e->p->rb_gen = 0;
    e->p = NULL;
}

/** @brief Take a pipeline out of the live rebuild in flight, if it is a member: the retirement and destruction paths call it before freeing the pipeline.
 *
 * @param dev Device.
 * @param p The pipeline. */
static void rebuild_forget(rhi_device *dev, rhi_pipeline *p)
{
    struct rebuild_set *rb = &dev->rb;
    if (!rb->active || p->rb_gen != rb->gen || p->rb_idx >= rb->n) return;
    rebuild_drop(dev, &rb->e[p->rb_idx]);
    rb->retired++;
}

/** @brief Add a pipeline to the rebuild set.
 *
 * @param dev Device.
 * @param p The pipeline, not a member.
 * @return Its entry, or NULL on allocation failure. */
static struct rebuild_entry *rebuild_add(rhi_device *dev, rhi_pipeline *p)
{
    struct rebuild_set *rb = &dev->rb;
    if (rb->n == rb->cap) {
        const unsigned cap = rb->cap ? rb->cap * 2u : 256u;
        struct rebuild_entry *e = realloc(rb->e, cap * sizeof *e);
        if (!e) return NULL;
        rb->e = e;
        rb->cap = cap;
    }
    struct rebuild_entry *e = &rb->e[rb->n];
    *e = (struct rebuild_entry){ .p = p };
    p->rb_gen = rb->gen;
    p->rb_idx = rb->n++;
    return e;
}

/** @brief Take a rebuild job's result: keep it when its member is still in the set and still rebuilt to the job's create info, else destroy it.
 *
 * Display owner (an rhi_pool_install_fn).
 * @param dev Device.
 * @param job The job, consumed; its generation is the set's.
 * @param rc Its run's result, or RHI_POOL_CANCELLED.
 * @param tag The member's entry index. */
static void rebuild_install(rhi_device *dev, rhi_compile_job *job, int rc, void *tag)
{
    struct rebuild_set *rb = &dev->rb;
    const unsigned idx = (unsigned)(uintptr_t)tag;
    struct rebuild_entry *e = (rc != RHI_POOL_CANCELLED && rb->active &&
                               job->generation == rb->gen && idx < rb->n) ? &rb->e[idx] : NULL;
    if (e && (!e->p || !e->waiting || job_variant_key(dev, job) != e->key)) e = NULL;
    if (!e) {
        rb->stale += rc != RHI_POOL_CANCELLED;
        rhi_compile_job_discard(dev, job);
        return;
    }
    e->waiting = false;
    rb->waiting--;
    if (rc == 0) {
        e->ready = job->out;
        job->out = (pipe_compiled){0};
    }
    rhi_compile_job_discard(dev, job);
}

/** @brief Bring one registered pipeline's membership in line with a requested variant: keep, join with a new job, or leave.
 *
 * @param dev Device.
 * @param p The pipeline.
 * @param var The requested variant.
 * @param epoch The variant epoch of the request. */
static void rebuild_member(rhi_device *dev, rhi_pipeline *p, rhi_pipe_variant var, uint32_t epoch)
{
    struct rebuild_set *rb = &dev->rb;
    rhi_pc_variant v;
    pipe_variant_of(dev, p, var, &v);
    const uint64_t k = rhi_pc_variant_key(&v);
    const bool member = p->rb_gen == rb->gen && p->rb_idx < rb->n;
    if (member && rb->e[p->rb_idx].key == k) return;   /* its job or result is still right */
    if (k == p->vkey) {                                 /* back to its installed objects */
        if (member) rebuild_drop(dev, &rb->e[p->rb_idx]);
        return;
    }
    struct rebuild_entry *e = member ? &rb->e[p->rb_idx] : rebuild_add(dev, p);
    if (!e) return;   /* allocation failure: the swap compiles it (rebuild_late) */
    rebuild_entry_clear(dev, e);
    e->key = k;
    rhi_compile_job *job = rhi_compile_job_of(dev, p, var, rb->gen, epoch);
    if (!job) return;   /* left with no result: the swap keeps its old objects */
    e->waiting = true;
    rb->waiting++;
    rhi_compile_pool_submit(dev, job, 0, rebuild_install, (void *)(uintptr_t)(e - rb->e));
}

/** @brief Return whether two variants select the same creation-time state.
 *
 * @param a A variant.
 * @param b A variant.
 * @return true when equal. */
static bool variant_eq(rhi_pipe_variant a, rhi_pipe_variant b)
{
    return a.affine == b.affine && a.notex == b.notex && a.wireframe == b.wireframe &&
           a.truescope == b.truescope;
}

static void walk_cover_variant(rhi_device *dev, const rhi_pipeline *p, rhi_pipe_variant var);

int rhi_rebuild_request(rhi_device *dev, rhi_pipe_variant var, bool sync)
{
    if (!dev) return -1;
    seam_check("rebuild_request");   /* the registry is the display owner's */
    struct rebuild_set *rb = &dev->rb;
    const Uint64 t0 = SDL_GetTicksNS();  /* clock-guard: wall: rebuild timing for the log */
    if (!rb->active) {
        rb->gen = rb->gen + 1u ? rb->gen + 1u : 1u;
        rb->active = true;
        rb->n = rb->waiting = rb->requests = rb->stale = rb->retired = rb->joined = 0;
        rb->t0_ns = t0;
        rb->owner_ns = 0;
        rb->serial0 = s_frame_serial;
        rhi_pc_tally_get(RHI_PC_PHASE_REBUILD, &rb->hit0, &rb->miss0, &rb->unk0);
        rhi_pc_rebuild_visit();
    } else if (variant_eq(var, rb->target)) {
        return 0;
    }
    atomic_fetch_add(&s_variant_epoch, 1u);
    rb->target = var;
    rb->requests++;
    const uint32_t epoch = rhi_pipe_variant_epoch();
    t_in_rebuild = true;
    for (rhi_pipeline *p = dev->pipes_head; p; p = p->next) {
        walk_cover_variant(dev, p, var);
        rebuild_member(dev, p, var, epoch);
    }
    t_in_rebuild = false;
    if (sync) rhi_compile_pool_wait(dev, NULL, NULL);
    rb->owner_ns += SDL_GetTicksNS() - t0;  /* clock-guard: wall: rebuild timing for the log */
    return 0;
}

bool rhi_rebuild_pending(const rhi_device *dev)
{
    return dev && dev->rb.active;
}

/** @brief Bring a pipeline registered while a rebuild is in flight into its set: its create info for the requested variant compiles on the pool, and the swap waits for it.
 *
 * The pipeline was compiled for the variant still shown; when the
 * requested variant changes its create info it joins the set like a member
 * the request found.
 * @param dev Device.
 * @param p The pipeline, just linked. */
static void rebuild_join(rhi_device *dev, rhi_pipeline *p)
{
    struct rebuild_set *rb = &dev->rb;
    if (!rb->active) return;
    const unsigned n0 = rb->n;
    t_in_rebuild = true;
    rebuild_member(dev, p, rb->target, rhi_pipe_variant_epoch());
    t_in_rebuild = false;
    rb->joined += rb->n != n0;
}

/** @brief Compile, on the display owner, every registered pipeline outside the rebuild set whose create info the target variant changes: those that could not join it (an allocation failure).
 *
 * @param dev Device.
 * @param st Counts: rebuilt, failed and late are added to. */
static void rebuild_late(rhi_device *dev, rhi_rebuild_stats *st)
{
    struct rebuild_set *rb = &dev->rb;
    const uint32_t epoch = rhi_pipe_variant_epoch();
    for (rhi_pipeline *p = dev->pipes_head; p; p = p->next) {
        if (p->rb_gen == rb->gen) continue;
        rhi_pc_variant v;
        pipe_variant_of(dev, p, rb->target, &v);
        if (rhi_pc_variant_key(&v) == p->vkey) continue;
        st->late++;
        rhi_compile_job *job = rhi_compile_job_of(dev, p, rb->target, 0u, epoch);
        if (!job || rhi_compile_job_run(dev, job, 0) != 0) {
            st->failed++;   /* keep the old objects: stale spec constants beat a hole */
            rhi_compile_job_discard(dev, job);
            continue;
        }
        rebuild_swap(dev, p, &job->out);
        job->out = (pipe_compiled){0};
        rhi_compile_job_discard(dev, job);
        st->rebuilt++;
    }
}

/** @brief Make a variant the one rhi_pipe_variant_current returns, without bumping the epoch (the request bumped it).
 *
 * @param var The variant. */
static void variant_show(rhi_pipe_variant var)
{
    s_vs_affine_uv = var.affine ? VK_TRUE : VK_FALSE;
    s_fs_notex = var.notex ? VK_TRUE : VK_FALSE;
    s_wireframe = var.wireframe;
    s_fs_moby_truescope = var.truescope ? VK_TRUE : VK_FALSE;
}

/** @brief Swap a complete rebuild set: install every member's result at once, compile the late members, and show the target variant.
 *
 * @param dev Device.
 * @param st Receives the rebuild's counts. */
static void rebuild_swap_all(rhi_device *dev, rhi_rebuild_stats *st)
{
    struct rebuild_set *rb = &dev->rb;
    const Uint64 t0 = SDL_GetTicksNS();  /* clock-guard: wall: rebuild timing for the log */
    *st = (rhi_rebuild_stats){ .shown = rb->target, .stale = rb->stale, .retired = rb->retired,
                               .requests = rb->requests, .late = rb->joined };
    t_in_rebuild = true;
    rebuild_late(dev, st);
    t_in_rebuild = false;
    for (unsigned i = 0; i < rb->n; i++) {
        struct rebuild_entry *e = &rb->e[i];
        if (!e->p) continue;
        if (e->ready.pipeline) {
            rebuild_swap(dev, e->p, &e->ready);
            st->rebuilt++;
        } else {
            st->failed++;   /* keep the old objects: stale spec constants beat a hole */
        }
        e->ready = (pipe_compiled){0};
        e->p->rb_gen = 0;
    }
    unsigned members = 0;
    for (rhi_pipeline *p = dev->pipes_head; p; p = p->next) members++;
    st->unchanged = members - st->rebuilt - st->failed;
    variant_show(rb->target);
    rb->active = false;
    rb->n = 0;
    const Uint64 t1 = SDL_GetTicksNS();  /* clock-guard: wall: rebuild timing for the log */
    rb->owner_ns += t1 - t0;
    st->ms = (double)(t1 - rb->t0_ns) / 1e6;
    st->owner_ms = (double)rb->owner_ns / 1e6;
    st->presents = (unsigned)(s_frame_serial - rb->serial0);
    unsigned hit, miss, unk;
    rhi_pc_tally_get(RHI_PC_PHASE_REBUILD, &hit, &miss, &unk);
    st->hit = hit - rb->hit0;
    st->miss = miss - rb->miss0;
    st->unknown = unk - rb->unk0;
    RHI_LOG("rebuilt %u pipelines (%u failed, %u unchanged kept) in %.1f ms "
            "(cache hit %u, miss %u, unknown %u); display owner %.1f ms, %u presents, "
            "%u request(s), %u late, %u stale results, %u retired in flight\n",
            st->rebuilt, st->failed, st->unchanged, st->ms, st->hit, st->miss, st->unknown,
            st->owner_ms, st->presents, st->requests, st->late, st->stale, st->retired);
}

int rhi_rebuild_poll(rhi_device *dev, rhi_rebuild_stats *out)
{
    if (!dev) return 0;
    seam_check("rebuild_poll");
    rhi_compile_pool_drain(dev);
    if (!dev->rb.active || dev->rb.waiting) return 0;
    rhi_rebuild_stats st;
    rebuild_swap_all(dev, &st);
    if (out) *out = st;
    return 1;
}

/** @brief Abandon a rebuild in flight: destroy every compiled result and empty the set (pool stop, device destruction).
 *
 * @param dev Device. */
static void rebuild_abandon(rhi_device *dev)
{
    struct rebuild_set *rb = &dev->rb;
    for (unsigned i = 0; i < rb->n; i++) rebuild_drop(dev, &rb->e[i]);
    rb->active = false;
    rb->n = 0;
    free(rb->e);
    rb->e = NULL;
    rb->cap = 0;
}

/* ---- link-time optimization (rhi.h) -------------------------------------- */

static rhi_pipeline *registry_find(rhi_device *dev, uint64_t serial);
static void pcache_autosave_arm(rhi_device *dev);

/** @brief Return the create-info key of a pipeline's installed objects under the variant shown: the key a link-time-optimized link of it must carry.
 *
 * @param dev Device.
 * @param job A job made from the pipeline (rhi_compile_job_of).
 * @return The key. */
static uint64_t lto_job_key(const rhi_device *dev, const rhi_compile_job *job)
{
    rhi_pc_variant v;
    pipe_variant_view(dev, &job->desc, &job->info, job->variant, job->slot_contest, &v);
    return rhi_pc_variant_key(&v);
}

/** @brief Return whether a registered pipeline may take a link-time-optimized result for a key now: still fast linked to that key and outside a live rebuild's set.
 *
 * @param dev Device.
 * @param p The pipeline, or NULL.
 * @param key The result's create-info key.
 * @return true when the result may replace its objects. */
static bool lto_target_ok(const rhi_device *dev, const rhi_pipeline *p, uint64_t key)
{
    if (!p || p->link != PIPE_LINK_FAST || p->vkey != key) return false;
    return !(dev->rb.active && p->rb_gen == dev->rb.gen);
}

/** @brief Take a link-time-optimized link's result: park it for the next swap when its pipeline still wants it, else destroy it.
 *
 * A link that failed and was created whole instead (pipeline_compile) is
 * parked the same way: a whole pipeline is the optimized form too.
 * Display owner (an rhi_pool_install_fn, run by a pool drain at any point
 * of a present, so the swap waits for rhi_pipeline_lto_step).
 * @param dev Device.
 * @param job The job, consumed.
 * @param rc Its run's result, or RHI_POOL_CANCELLED.
 * @param tag The pipeline's registry serial, heap-allocated, freed here. */
static void lto_arrive(rhi_device *dev, rhi_compile_job *job, int rc, void *tag)
{
    const uint64_t serial = *(const uint64_t *)tag;
    free(tag);
    struct lto_state *l = &dev->lto;
    l->out--;
    l->returned++;
    rhi_pipeline *p = rc == RHI_POOL_CANCELLED ? NULL : registry_find(dev, serial);
    if (p) p->lto_out = false;
    struct lto_ready *r = NULL;
    if (rc == 0 && lto_target_ok(dev, p, job->out.key) && job->out.link != PIPE_LINK_FAST)
        r = malloc(sizeof *r);
    if (!r) {
        l->failed += rc != 0 && rc != RHI_POOL_CANCELLED;
        l->stale += rc == 0;
        if (p && rc == 0) l->owed = true;   /* reconsidered at the next queue pass */
        rhi_compile_job_discard(dev, job);
        return;
    }
    *r = (struct lto_ready){ .next = l->ready, .serial = serial, .c = job->out };
    l->ready = r;
    job->out = (pipe_compiled){0};
    rhi_compile_job_discard(dev, job);
}

/** @brief Log one swapped-in optimized result under the pipe census (rhi_pc_level): the pipeline's site, how the result was made and the vertex stage it compiled.
 *
 * @param p The pipeline, its new objects installed. */
static void lto_swap_log(const rhi_pipeline *p)
{
    if (!rhi_pc_level()) return;
    const char *vs = p->whole_stage ? p->desc.vert_spv_whole : p->desc.vert_spv;
    RHI_LOG("pipe-swap #%llu site=%s link=%s vs=%s\n", (unsigned long long)p->reg_serial,
            rhi_pc_site_name(rhi_pc_site_of(&p->desc, p->vert_words || p->frag_words)),
            p->link == PIPE_LINK_WHOLE ? "whole" : "lto", vs ? vs : "(words)");
}

/** @brief Swap every parked optimized result into its pipeline (rebuild_swap: the replaced objects retire by frame serial); a result its pipeline no longer wants is destroyed.
 *
 * @param dev Device. */
static void lto_swap_ready(rhi_device *dev)
{
    struct lto_state *l = &dev->lto;
    for (struct lto_ready *r = l->ready, *next; r; r = next) {
        next = r->next;
        rhi_pipeline *p = registry_find(dev, r->serial);
        if (lto_target_ok(dev, p, r->c.key)) {
            rebuild_swap(dev, p, &r->c);
            lto_swap_log(p);
            l->swapped++;
            l->swapped_whole += r->c.whole_stage;
            l->swaps_in_frame += dev->frame_active;
            if (!l->serial_first) l->serial_first = s_frame_serial;
            l->serial_last = s_frame_serial;
        } else {
            l->stale++;
            vkDestroyPipeline(dev->device, r->c.pipeline, NULL);
            vkDestroyPipelineLayout(dev->device, r->c.layout, NULL);
        }
        free(r);
    }
    l->ready = NULL;
}

/** @brief Submit a link-time-optimized link, on the pool's low queue, of every registered fast-linked pipeline that has none out.
 *
 * The job is the pipeline's create info under the variant shown, so its
 * result replaces objects of the same create info; the creates count in the
 * precompile phase, beside the background variant walk.
 * @param dev Device, with no live rebuild in flight. */
static void lto_queue(rhi_device *dev)
{
    struct lto_state *l = &dev->lto;
    l->owed = false;
    if (!l->t0_ns) l->t0_ns = SDL_GetTicksNS();  /* clock-guard: wall: optimization duration for the log */
    const rhi_pipe_phase ph = t_pipe_phase;
    t_pipe_phase = RHI_PIPE_PHASE_PRECOMPILE;
    for (rhi_pipeline *p = dev->pipes_head; p; p = p->next) {
        if (p->link != PIPE_LINK_FAST || p->lto_out) continue;
        rhi_compile_job *job = rhi_compile_job_of(dev, p, rhi_pipe_variant_current(), 0u,
                                                  rhi_pipe_variant_epoch());
        uint64_t *tag = job ? malloc(sizeof *tag) : NULL;
        if (!tag || lto_job_key(dev, job) != p->vkey) {
            free(tag);
            rhi_compile_job_discard(dev, job);
            continue;
        }
        job->lto = true;
        *tag = p->reg_serial;
        p->lto_out = true;
        l->out++;
        l->queued++;
        rhi_compile_pool_submit(dev, job, POOL_LOW, lto_arrive, tag);
    }
    t_pipe_phase = ph;
}

/** @brief Wait until every link-time-optimized link out has returned, draining the pool's staged results.
 *
 * @param dev Device. */
static void lto_wait(rhi_device *dev)
{
    struct compile_pool *pl = dev->cpool;
    while (pl && dev->lto.out && !pl->paused) {
        rhi_compile_pool_drain(dev);
        SDL_LockMutex(pl->mx);
        if (dev->lto.out && !pl->s_head) SDL_WaitConditionTimeout(pl->done, pl->mx, 10);
        SDL_UnlockMutex(pl->mx);
    }
}

/** @brief Log the optimization counts once every queued link has returned and been swapped, and arm the cache save so a warm start finds the links.
 *
 * @param dev Device. */
static void lto_report_drained(rhi_device *dev)
{
    struct lto_state *l = &dev->lto;
    if (l->drained || l->out || l->ready || l->owed || !l->queued) return;
    l->drained = true;
    RHI_LOG("pipeline lto: %u queued, %u returned, %u swapped (%u created whole from "
            "vert_spv_whole), %u stale, %u failed; "
            "%u made optimized at create (%u probe misses); swaps at frame serials "
            "%llu..%llu, %u inside a frame; %u binds of a pipeline with no objects; %.1f ms\n",
            l->queued, l->returned, l->swapped, l->swapped_whole, l->stale, l->failed,
            atomic_load(&l->direct), atomic_load(&l->probe_miss),
            (unsigned long long)l->serial_first, (unsigned long long)l->serial_last,
            l->swaps_in_frame, dev->null_binds,
            (double)(SDL_GetTicksNS() - l->t0_ns) / 1e6);  /* clock-guard: wall: optimization duration for the log */
    pcache_autosave_arm(dev);
}

void rhi_pipeline_lto_step(rhi_device *dev, bool sync)
{
    if (!dev || !dev->has_lto) return;
    seam_check("pipeline_lto_step");   /* the registry is the display owner's */
    rhi_compile_pool_drain(dev);
    lto_swap_ready(dev);
    lto_report_drained(dev);   /* the last link may return on a step that queues nothing */
    if (!dev->lto.owed || dev->rb.active || dev->cpool_stopped) return;
    if (!dev->headless && !atomic_load(&s_first_frame_done)) return;
    if (!pool_get(dev) || !dev->cpool->n) {   /* a pool of none would link on this thread */
        dev->lto.owed = false;
        RHI_LOG("pipeline lto: the compile pool has no worker; fast links kept\n");
        return;
    }
    lto_queue(dev);
    if (sync) {
        lto_wait(dev);
        lto_swap_ready(dev);
    }
    lto_report_drained(dev);
}

/** @brief Destroy the parked link-time-optimized results (device destruction, after the pool stopped).
 *
 * @param dev Device. */
static void lto_destroy_ready(rhi_device *dev)
{
    for (struct lto_ready *r = dev->lto.ready, *next; r; r = next) {
        next = r->next;
        vkDestroyPipeline(dev->device, r->c.pipeline, NULL);
        vkDestroyPipelineLayout(dev->device, r->c.layout, NULL);
        free(r);
    }
    dev->lto.ready = NULL;
}

/* ---- precompile ---------------------------------------------------------- */

/* Queue depth per worker the blocking walk keeps: bounds the jobs (each holds
 * its stage copies) alive at once. */
#define PRECOMPILE_QUEUE_PER_WORKER 4u

/** @brief Return the pool's variant outcomes (created, probe hits, failed) since a snapshot.
 *
 * @param dev Device.
 * @param p0 The snapshot.
 * @return The outcomes counted since it. */
static unsigned walk_returned(const rhi_device *dev, const rhi_compile_pool_stats *p0)
{
    rhi_compile_pool_stats p;
    rhi_compile_pool_stats_get(dev, &p);
    return (p.variant_created + p.variant_probe_hits + p.variant_failed) -
           (p0->variant_created + p0->variant_probe_hits + p0->variant_failed);
}

/** @brief Return whether a walk skips a variant because the key manifest lists it: only on a device without pipeline cache control whose manifest is trusted.
 *
 * Off the library path the create compiles the whole-create vertex stage
 * when the desc names one (pipeline_compile), so that stage's view is the
 * one looked up.
 * @param dev Device.
 * @param desc The pipeline's description.
 * @param info Its stage facts.
 * @param words Whether a stage is given as SPIR-V words.
 * @param var The variant.
 * @param sc SLOT_CONTEST's value.
 * @return true when the manifest lists the variant. */
static bool walk_manifest_lists(rhi_device *dev, const rhi_pipeline_desc *desc,
                                const pipe_stage_info *info, bool words, rhi_pipe_variant var,
                                VkBool32 sc)
{
    if (dev->has_cache_control || !dev->manifest_trusted) return false;
    const bool lib = pipe_uses_library(dev, desc, words);
    pipe_stage_info wi = *info;
    if (!lib && info->wvs_hash) { wi.vs_hash = info->wvs_hash; wi.vs_ids = info->wvs_ids; }
    rhi_pc_variant v;
    pipe_variant_view(dev, desc, &wi, var, sc, &v);
    const pipe_link link = lib ? PIPE_LINK_FAST : PIPE_LINK_WHOLE;
    SDL_LockMutex(dev->pc_mx);
    const int listed = rhi_pc_keyset_has(&dev->pc_keys, pipe_manifest_key_as(&v, link));
    SDL_UnlockMutex(dev->pc_mx);
    return listed != 0;
}

/** @brief Enumerate one view of a pipeline into the walk unless its key was seen or the manifest lists it.
 *
 * @param dev Device.
 * @param w The walk, items allocated for every pipeline and walked view.
 * @param seen Keys enumerated so far.
 * @param p The pipeline.
 * @param var The variant. */
static void walk_collect(rhi_device *dev, struct precompile_walk *w, rhi_pc_keyset *seen,
                         const rhi_pipeline *p, rhi_pipe_variant var)
{
    rhi_pc_variant v;
    pipe_variant_of(dev, p, var, &v);
    const uint64_t key = rhi_pc_variant_key(&v);
    if (!rhi_pc_keyset_insert(seen, key)) return;
    w->st.keys++;
    if (walk_manifest_lists(dev, &p->desc, &p->info, p->vert_words || p->frag_words, var,
                            s_fs_slot_contest)) {
        w->st.manifest_skips++;
        return;
    }
    w->items[w->n++] = (walk_item){ .serial = p->reg_serial, .ahead = -1, .var = var, .key = key };
}

/** @brief Begin a variant walk: enumerate the walked views of every registered pipeline, each key once, the installed keys excluded.
 *
 * The enumeration completes before the first submit, so the walk's total is
 * known from the start.
 * @param dev Device, with no walk running.
 * @param limit Jobs out at most.
 * @param background true for the background walk (one job per step, the end
 *        logged and the save armed). */
static void walk_begin(rhi_device *dev, unsigned limit, bool background)
{
    struct precompile_walk *w = &dev->walk;
    *w = (struct precompile_walk){ .state = RHI_WALK_RUNNING, .background = background,
                                   .limit = limit ? limit : 1u,
                                   .t0_ns = SDL_GetTicksNS() };  /* clock-guard: wall: walk duration for the log */
    (void)pool_get(dev);
    rhi_pc_keyset seen = {0};
    for (rhi_pipeline *p = dev->pipes_head; p; p = p->next) {
        w->st.bases++;
        if (rhi_pc_keyset_insert(&seen, p->vkey)) { w->st.keys++; w->st.current++; }
    }
    const unsigned cap = w->st.bases * (unsigned)PIPE_WALK_VIEWS;
    w->items = cap ? malloc(cap * sizeof *w->items) : NULL;
    for (rhi_pipeline *p = dev->pipes_head; w->items && p; p = p->next)
        for (int view = 0; view < PIPE_WALK_VIEWS; view++)
            walk_collect(dev, w, &seen, p, pipe_view_variant(view));
    rhi_pc_keyset_free(&seen);
    w->st.queued = w->n;
    w->serial_end = dev->pipe_serial;
    rhi_compile_pool_stats_get(dev, &w->p0);
}

/** @brief Return the registered pipeline with a registry serial.
 *
 * @param dev Device.
 * @param serial The serial.
 * @return The pipeline, or NULL when it was destroyed. */
static rhi_pipeline *registry_find(rhi_device *dev, uint64_t serial)
{
    for (rhi_pipeline *p = dev->pipes_head; p; p = p->next)
        if (p->reg_serial == serial) return p;
    return NULL;
}

/** @brief Make the compile job of an ahead walk item: its recorded description at its variant and SLOT_CONTEST value.
 *
 * @param dev Device.
 * @param it The item.
 * @return The job, or NULL on allocation failure or a stage with no source. */
static rhi_compile_job *walk_ahead_job(rhi_device *dev, const walk_item *it)
{
    rhi_compile_job *job = rhi_compile_job_new(dev, &dev->described[it->ahead].desc, NULL, 0,
                                               NULL, 0, it->var, 0u, rhi_pipe_variant_epoch());
    if (job) job->slot_contest = it->slot_contest;
    return job;
}

/** @brief Return whether every front rank of the ahead walk (AHEAD_FRONT_RANKS) holding an item has logged its return.
 *
 * @param w The ahead walk.
 * @return true when no front rank is outstanding. */
static bool walk_front_back(const struct precompile_walk *w)
{
    for (unsigned r = 0; r < AHEAD_FRONT_RANKS; r++)
        if (w->rank_n[r] && !((w->rank_logged >> r) & 1u)) return false;
    return true;
}

/** @brief Submit the walk's next variant that is neither covered by a live rebuild nor of a destroyed pipeline, built from its pipeline (or, on the ahead walk, its recorded description) now.
 *
 * The job carries the precompile create phase, so its creates never count as
 * first use. The ahead walk submits no item past its front ranks until
 * walk_front_back holds.
 * @param dev Device.
 * @param w The walk.
 * @return true when a job was submitted, false when the enumeration is spent
 *         or its next item waits for the front ranks. */
static bool walk_submit_next(rhi_device *dev, struct precompile_walk *w)
{
    while (w->next < w->n) {
        if (w->ahead && w->items[w->next].rank >= AHEAD_FRONT_RANKS && !walk_front_back(w)) return false;
        const walk_item *it = &w->items[w->next++];
        if (rhi_pc_keyset_has(&w->covered, it->key)) {
            w->st.covered++;
            w->skipped++;
            w->rank_skip[it->rank] += w->ahead;
            continue;
        }
        const rhi_pipeline *p = it->ahead < 0 ? registry_find(dev, it->serial) : NULL;
        rhi_compile_job *job = it->ahead >= 0 ? walk_ahead_job(dev, it)
                             : p ? rhi_compile_job_of(dev, p, it->var, 0u, rhi_pipe_variant_epoch())
                                 : NULL;
        if (!job) {
            w->st.gone += it->ahead < 0 && !p;
            w->st.failed += it->ahead >= 0 || p != NULL;
            w->skipped++;
            w->rank_skip[it->rank] += w->ahead;
            continue;
        }
        const rhi_pipe_phase ph = t_pipe_phase;
        t_pipe_phase = w->ahead ? RHI_PIPE_PHASE_LAUNCHER : RHI_PIPE_PHASE_PRECOMPILE;
        rhi_compile_pool_submit(dev, job,
                                RHI_COMPILE_VARIANT | (dev->has_cache_control ? RHI_COMPILE_PROBE : 0u) |
                                    (w->ahead ? POOL_AHEAD | (it->rank << POOL_RANK_SHIFT) : 0u) |
                                    (w->background ? POOL_BG : 0u),
                                NULL, NULL);
        t_pipe_phase = ph;
        w->submitted++;
        w->out_key = it->key;
        w->out_dup = false;
        return true;
    }
    return false;
}

/** @brief Set the debounced first-use save to run at the next begun frame unless a first-use miss restarts its quiet period.
 *
 * Display owner only.
 * @param dev Device. */
static void pcache_autosave_arm(rhi_device *dev)
{
    dev->pcache_fu_pending = true;
    dev->pcache_fu_serial = s_frame_serial >= PCACHE_SAVE_QUIET_FRAMES
                          ? s_frame_serial - PCACHE_SAVE_QUIET_FRAMES : 0u;
}

/** @brief End a walk: take the pool's outcomes into its counts and free its enumeration; a background walk logs its line and, unless cut short, arms the cache save.
 *
 * The ahead walk's line reads "launcher walk", the registry's "background
 * walk"; the ahead walk also frees the recorded descriptions.
 * @param dev Device.
 * @param w The walk.
 * @param cut Why the walk ended before every variant returned, or NULL. */
static void walk_end(rhi_device *dev, struct precompile_walk *w, const char *cut)
{
    rhi_compile_pool_stats p1;
    rhi_compile_pool_stats_get(dev, &p1);
    w->st.created = p1.variant_created - w->p0.variant_created;
    w->st.probe_hits = p1.variant_probe_hits - w->p0.variant_probe_hits;
    w->st.failed += p1.variant_failed - w->p0.variant_failed;
    w->st.ms = (double)(SDL_GetTicksNS() - w->t0_ns) / 1e6;  /* clock-guard: wall: walk duration for the log */
    if (w->background) {
        RHI_LOG("%s walk: keys %u, created %u, probe hits %u, duplicates %u, %.1f ms "
                "(%u installed, %u queued, %u manifest skips, %u covered by a rebuild, "
                "%u pipelines gone, %u failed%s%s)\n", w->ahead ? "launcher" : "background",
                w->st.keys, w->st.created, w->st.probe_hits, w->st.duplicates, w->st.ms,
                w->st.current, w->st.queued, w->st.manifest_skips, w->st.covered, w->st.gone,
                w->st.failed, cut ? "; " : "", cut ? cut : "");
        if (!cut) pcache_autosave_arm(dev);
    }
    if (w->ahead) {
        described_free(dev);
        if (dev->cpool) {
            SDL_LockMutex(dev->cpool->mx);
            dev->cpool->ahead_wake = NULL;
            SDL_UnlockMutex(dev->cpool->mx);
        }
    }
    free(w->items);
    w->items = NULL;
    rhi_pc_keyset_free(&w->covered);
    w->state = RHI_WALK_DONE;
}

/** @brief Log each group of the ahead walk's first texture mapping whose every item has returned or was skipped, once per group.
 *
 * The line reads "launcher walk: <mapping> group G returned: N create
 * infos, T ms after the start".
 * @param dev Device.
 * @param w The ahead walk.
 * @return true when the call logged a group. */
static bool walk_log_ranks(rhi_device *dev, struct precompile_walk *w)
{
    bool logged = false;
    unsigned back[AHEAD_RANKS];
    SDL_LockMutex(dev->cpool->mx);
    memcpy(back, dev->cpool->ahead_back, sizeof back);
    SDL_UnlockMutex(dev->cpool->mx);
    for (unsigned r = 0; r < RHI_AHEAD_GROUPS; r++) {
        if ((w->rank_logged >> r) & 1u || !w->rank_n[r] || back[r] + w->rank_skip[r] < w->rank_n[r])
            continue;
        w->rank_logged |= 1u << r;
        logged = true;
        RHI_LOG("launcher walk: %s group %u returned: %u create infos, %.1f ms after the start\n",
                w->affine_first ? "affine" : "perspective", r, w->rank_n[r],
                (double)(SDL_GetTicksNS() - w->t0_ns) / 1e6);  /* clock-guard: wall: walk progress for the log */
    }
    return logged;
}

/** @brief Queue the library parts of every ahead walk item of one rank not yet submitted, so the pool's workers build them side by side.
 *
 * A part is otherwise claimed only when the first item using it is
 * submitted (gpl_items_for), and an item waiting on a part holds one of the
 * walk's slots, so a rank of many items over few parts builds those parts
 * one after another. A part already claimed or queued is skipped. Display
 * owner only.
 * @param dev Device.
 * @param w The ahead walk.
 * @param rank The rank.
 * @return The parts queued. */
static unsigned walk_queue_rank_parts(rhi_device *dev, struct precompile_walk *w, unsigned rank)
{
    struct compile_pool *pl = dev->cpool;
    if (!pl || pl->n == 0) return 0;
    pool_item *head = NULL, *tail = NULL;
    unsigned n = 0;
    for (unsigned i = w->next; i < w->n; i++) {
        if (w->items[i].rank != rank) continue;
        rhi_compile_job *job = walk_ahead_job(dev, &w->items[i]);
        if (!job) continue;
        pool_item *libs = gpl_items_for(dev, job, RHI_PIPE_PHASE_LAUNCHER, t_in_rebuild);
        rhi_compile_job_discard(dev, job);
        for (pool_item *next; libs; libs = next, n++) {
            next = libs->next;
            libs->next = NULL;
            if (tail) tail->next = libs; else head = libs;
            tail = libs;
        }
    }
    if (!head) return 0;
    SDL_LockMutex(pl->mx);
    pool_queue_parts_locked(pl, head, true);
    if (pl->queued > pl->st.max_queued) pl->st.max_queued = pl->queued;
    pool_wake_locked(pl, true);
    SDL_UnlockMutex(pl->mx);
    return n;
}

/** @brief Log the ahead walk's returned groups, and queue each front rank's library parts once the rank before it returned (or held no item).
 *
 * The queue logs "launcher walk: N library parts of group G queued, T ms
 * after the start".
 * @param dev Device.
 * @param w The ahead walk.
 * @return true when a group's return was logged, which may release items
 *         walk_submit_next held. */
static bool walk_front_step(rhi_device *dev, struct precompile_walk *w)
{
    const bool logged = walk_log_ranks(dev, w);
    for (unsigned r = 1; r < AHEAD_FRONT_RANKS; r++) {
        const bool prev_back = !w->rank_n[r - 1] || ((w->rank_logged >> (r - 1)) & 1u);
        if (((w->rank_parts >> r) & 1u) || !prev_back) continue;
        w->rank_parts |= 1u << r;
        const unsigned n = walk_queue_rank_parts(dev, w, r);
        if (n)
            RHI_LOG("launcher walk: %u library parts of group %u queued, %.1f ms after the start\n", n, r,
                    (double)(SDL_GetTicksNS() - w->t0_ns) / 1e6);  /* clock-guard: wall: walk progress for the log */
    }
    return logged;
}

/** @brief Submit walk jobs up to the walk's limit (one per call for a background walk), and end the walk once every job has returned.
 *
 * @param dev Device.
 * @return The walk's state after the step. */
static rhi_walk_state walk_advance(rhi_device *dev)
{
    struct precompile_walk *w = &dev->walk;
    if (w->state != RHI_WALK_RUNNING) return w->state;
    if (dev->cpool_stopped || !dev->cpool) { walk_end(dev, w, "abandoned at the pool's stop"); return w->state; }
    do {
        while (w->submitted - walk_returned(dev, &w->p0) < w->limit) {
            if (!walk_submit_next(dev, w) || (w->background && !w->ahead)) break;
        }
    } while (w->ahead && walk_front_step(dev, w));
    if (w->next >= w->n && walk_returned(dev, &w->p0) >= w->submitted) walk_end(dev, w, NULL);
    return w->state;
}

/** @brief Record that a live rebuild request compiles a registered pipeline at a variant: the walk skips that key, and a walk job out for it counts one duplicate.
 *
 * @param dev Device.
 * @param p The pipeline.
 * @param var The requested variant. */
static void walk_cover_variant(rhi_device *dev, const rhi_pipeline *p, rhi_pipe_variant var)
{
    struct precompile_walk *w = &dev->walk;
    if (w->state != RHI_WALK_RUNNING || p->reg_serial > w->serial_end) return;
    rhi_pc_variant v;
    pipe_variant_of(dev, p, var, &v);
    const uint64_t key = rhi_pc_variant_key(&v);
    (void)rhi_pc_keyset_insert(&w->covered, key);
    if (!w->out_dup && w->submitted && key == w->out_key &&
        walk_returned(dev, &w->p0) < w->submitted) {
        w->st.duplicates++;
        w->out_dup = true;
    }
}

/** @brief Wait until a pool variant outcome arrives after a count, draining staged results and pumping.
 *
 * @param dev Device.
 * @param seen The walk's returned count the wait starts from.
 * @param pump Called between waits, or NULL.
 * @param ctx Passed to @p pump. */
static void walk_wait_return(rhi_device *dev, unsigned seen, void (*pump)(void *), void *ctx)
{
    struct compile_pool *pl = dev->cpool;
    while (pl && walk_returned(dev, &dev->walk.p0) == seen) {
        rhi_compile_pool_drain(dev);
        SDL_LockMutex(pl->mx);
        if (!pl->s_head) SDL_WaitConditionTimeout(pl->done, pl->mx, 10);
        SDL_UnlockMutex(pl->mx);
        if (pump) pump(ctx);
    }
}

int rhi_precompile_variants(rhi_device *dev, void (*pump)(void *ctx), void *ctx,
                            rhi_precompile_stats *out)
{
    if (!dev) return -1;
    seam_check("precompile_variants");   /* the registry is the display owner's */
    if (dev->walk.state == RHI_WALK_RUNNING) return -1;
    struct compile_pool *pl = pool_get(dev);
    struct precompile_walk *w = &dev->walk;
    walk_begin(dev, PRECOMPILE_QUEUE_PER_WORKER * (unsigned)(pl && pl->n ? pl->n : 1), false);
    while (walk_advance(dev) == RHI_WALK_RUNNING)
        walk_wait_return(dev, walk_returned(dev, &w->p0), pump, ctx);
    rhi_compile_pool_wait(dev, pump, ctx);
    rhi_compile_pool_stats p1;
    rhi_compile_pool_stats_get(dev, &p1);
    w->st.saved = pipeline_cache_save(dev, "precompile");
    w->st.ms = (double)(SDL_GetTicksNS() - w->t0_ns) / 1e6;  /* clock-guard: wall: precompile duration for the log */
    RHI_LOG("precompile: %u pipelines, %u distinct create infos (%u installed), %u created, "
            "%u probe hits, %u manifest skips, %u failed, cache %s, %.1f ms on %u worker(s) "
            "(%.1f ms of runs)\n",
            w->st.bases, w->st.keys, w->st.current, w->st.created, w->st.probe_hits,
            w->st.manifest_skips, w->st.failed, w->st.saved ? "saved" : "not saved", w->st.ms,
            p1.workers, p1.run_ms - w->p0.run_ms);
    if (out) *out = w->st;
    return 0;
}

int rhi_precompile_walk_start(rhi_device *dev)
{
    if (!dev || dev->walk.state == RHI_WALK_RUNNING) return -1;
    seam_check("precompile_walk_start");   /* the registry is the display owner's */
    walk_begin(dev, 1u, true);
    pcache_autosave_arm(dev);   /* the creates before the walk reach the file without waiting for it */
    return (int)dev->walk.n;
}

/** @brief Enumerate one recorded description at one variant into the ahead walk, at both SLOT_CONTEST values, each key once unless seen or listed in the manifest.
 *
 * @param dev Device.
 * @param w The ahead walk, items allocated for every description, view and value.
 * @param seen Keys enumerated so far.
 * @param i The description's index.
 * @param info Its stages' facts.
 * @param var The variant.
 * @param rank Its rank (walk_item.rank). */
static void walk_collect_ahead(rhi_device *dev, struct precompile_walk *w, rhi_pc_keyset *seen,
                               int i, const pipe_stage_info *info, rhi_pipe_variant var,
                               unsigned rank)
{
    const rhi_pipeline_desc *d = &dev->described[i].desc;
    for (int sc = 0; sc < 2; sc++) {
        rhi_pc_variant v;
        pipe_variant_view(dev, d, info, var, sc ? VK_TRUE : VK_FALSE, &v);
        const uint64_t key = rhi_pc_variant_key(&v);
        if (!rhi_pc_keyset_insert(seen, key)) continue;
        w->st.keys++;
        if (walk_manifest_lists(dev, d, info, false, var, sc ? VK_TRUE : VK_FALSE)) {
            w->st.manifest_skips++;
            continue;
        }
        w->items[w->n++] = (walk_item){ .ahead = i, .rank = rank,
                                        .slot_contest = sc ? VK_TRUE : VK_FALSE, .var = var, .key = key };
        w->rank_n[rank]++;
    }
}

/** @brief Resolve a recorded description's stages and record their facts, as a compile job of it would.
 *
 * @param dev Device.
 * @param i The description's index.
 * @param out Receives the facts.
 * @return false when a stage has no source or the job cannot be made. */
static bool described_info(rhi_device *dev, unsigned i, pipe_stage_info *out)
{
    rhi_compile_job *job = rhi_compile_job_new(dev, &dev->described[i].desc, NULL, 0, NULL, 0,
                                               rhi_pipe_variant_current(), 0u, 0u);
    if (!job) return false;
    *out = job->info;
    rhi_compile_job_discard(dev, job);
    return true;
}

int rhi_precompile_ahead_start(rhi_device *dev, bool affine_first, void (*wake)(void))
{
    if (!dev || dev->walk.state == RHI_WALK_RUNNING || !dev->described_n) return -1;
    seam_check("precompile_ahead_start");   /* job making reads the environment */
    const struct compile_pool *pl = pool_get(dev);
    struct precompile_walk *w = &dev->walk;
    *w = (struct precompile_walk){ .state = RHI_WALK_RUNNING, .background = true, .ahead = true,
                                   .limit = 2u * (pl && pl->n ? (unsigned)pl->n : 1u),
                                   .affine_first = affine_first,
                                   .t0_ns = SDL_GetTicksNS() };  /* clock-guard: wall: walk duration for the log */
    const unsigned nd = dev->described_n;
    pipe_stage_info *info = calloc(nd, sizeof *info);
    bool *ok = calloc(nd, sizeof *ok);
    w->items = (info && ok) ? malloc((size_t)nd * PIPE_WALK_VIEWS * 2u * sizeof *w->items) : NULL;
    for (unsigned i = 0; w->items && i < nd; i++) ok[i] = described_info(dev, i, &info[i]);
    rhi_pc_keyset seen = {0};
    /* Walk views 0 (perspective) and 1 (affine), the first drawn first; in
     * each, the groups in order, each group's descriptions as recorded. */
    for (unsigned k = 0; w->items && k < (unsigned)PIPE_WALK_VIEWS; k++) {
        const int view = affine_first ? PIPE_WALK_VIEWS - 1 - (int)k : (int)k;
        for (unsigned g = 0; g < RHI_AHEAD_GROUPS; g++)
            for (unsigned i = 0; i < nd; i++)
                if (ok[i] && dev->described[i].group == g)
                    walk_collect_ahead(dev, w, &seen, (int)i, &info[i], pipe_view_variant(view),
                                       k * RHI_AHEAD_GROUPS + g);
    }
    rhi_pc_keyset_free(&seen);
    free(info);
    free(ok);
    w->st.bases = nd;
    w->st.queued = w->n;
    rhi_compile_pool_stats_get(dev, &w->p0);
    if (dev->cpool) {
        SDL_LockMutex(dev->cpool->mx);
        memset(dev->cpool->ahead_back, 0, sizeof dev->cpool->ahead_back);
        dev->cpool->ahead_wake = wake;
        SDL_UnlockMutex(dev->cpool->mx);
    }
    RHI_LOG("launcher walk: %u descriptions (%u with SPIR-V words skipped), %u create infos, "
            "%u queued, %u manifest skips, up to %u jobs out, %s first; per group at the first "
            "mapping %u %u %u %u, at the second %u %u %u %u\n", nd,
            dev->described_words, w->st.keys, w->n, w->st.manifest_skips, w->limit,
            affine_first ? "affine" : "perspective", w->rank_n[0], w->rank_n[1], w->rank_n[2],
            w->rank_n[3], w->rank_n[4], w->rank_n[5], w->rank_n[6], w->rank_n[7]);
    return (int)w->n;
}

/** @brief Remove the ahead walk's jobs still queued on the pool and destroy them.
 *
 * @param dev Device.
 * @return The jobs dropped. */
static unsigned pool_drop_ahead(rhi_device *dev)
{
    struct compile_pool *pl = dev->cpool;
    if (!pl) return 0;
    pool_item *drop = NULL, *prev = NULL;
    SDL_LockMutex(pl->mx);
    for (pool_item **pp = &pl->q_head; *pp;) {
        pool_item *it = *pp;
        if (!it->ahead) { prev = it; pp = &it->next; continue; }
        *pp = it->next;
        if (pl->q_tail == it) pl->q_tail = prev;
        pl->queued--;
        it->next = drop;
        drop = it;
    }
    SDL_UnlockMutex(pl->mx);
    unsigned n = 0;
    for (pool_item *it = drop, *next; it; it = next, n++) {
        next = it->next;
        rhi_compile_job_discard(dev, it->job);
        free(it);
    }
    return n;
}

void rhi_precompile_ahead_stop(rhi_device *dev)
{
    struct precompile_walk *w = dev ? &dev->walk : NULL;
    if (!w || w->state != RHI_WALK_RUNNING || !w->ahead) return;
    seam_check("precompile_ahead_stop");
    const unsigned dropped = pool_drop_ahead(dev);
    const unsigned back = walk_returned(dev, &w->p0) + dropped;
    char cut[192];
    snprintf(cut, sizeof cut, "stopped at the pick: %u of %u not submitted, %u queued dropped, "
             "%u running", w->n - w->next, w->n, dropped,
             w->submitted > back ? w->submitted - back : 0u);
    walk_end(dev, w, cut);
}

unsigned rhi_precompile_ahead_returned(const rhi_device *dev)
{
    return dev && dev->walk.ahead ? dev->walk.rank_logged : 0u;
}

rhi_walk_state rhi_precompile_walk_step(rhi_device *dev)
{
    if (!dev || dev->walk.state != RHI_WALK_RUNNING) return dev ? dev->walk.state : RHI_WALK_IDLE;
    seam_check("precompile_walk_step");
    return walk_advance(dev);
}

rhi_walk_state rhi_precompile_walk_get(const rhi_device *dev, unsigned *done, unsigned *total,
                                       rhi_precompile_stats *st)
{
    const struct precompile_walk *w = dev ? &dev->walk : NULL;
    unsigned d = 0, t = 0;
    if (w && w->state == RHI_WALK_RUNNING) {
        t = w->n;
        d = w->skipped + walk_returned(dev, &w->p0);
        if (d > t) d = t;
    } else if (w && w->state == RHI_WALK_DONE) {
        t = d = w->st.queued;
    }
    if (done) *done = d;
    if (total) *total = t;
    if (st) *st = w ? w->st : (rhi_precompile_stats){0};
    return w ? w->state : RHI_WALK_IDLE;
}

void rhi_precompile_progress(const rhi_device *dev, unsigned *done, unsigned *total)
{
    unsigned d = 0, t = 0;
    if (dev) {
        const rhi_pipeline_batch_stats *b = &dev->batch;
        if (dev->batch_open) {
            t += b->queued;
            d += b->installed + b->failed + b->duplicate;
        }
        if (dev->walk.state == RHI_WALK_RUNNING && !dev->walk.background) {
            unsigned wd = 0, wt = 0;
            (void)rhi_precompile_walk_get(dev, &wd, &wt, NULL);
            t += wt;
            d += wd;
        }
    }
    if (done) *done = d;
    if (total) *total = t;
}

void rhi_pipe_precompile_done(void) { atomic_store(&s_precompile_done, true); }

/* ---- rhi_ubo: fixed-size uniform blocks × frame ring --------------- */

/* Generations per frame slot for a uniform block: a write to a block that a
 * draw in the same frame already bound moves to the next generation
 * (physical block = (frame * RHI_UBO_GENS + gen) * blocks + block), so the
 * recorded draw keeps the data it was bound to. A block written more than
 * RHI_UBO_GENS times after binds in one frame overwrites the last generation
 * in place (counted in rhi_ring_gen_overflows). */
#define RHI_UBO_GENS 4u

struct rhi_ubo {
    rhi_buffer     *buf;         /* HOST_VISIBLE|COHERENT, UNIFORM usage */
    uint32_t        block_size;  /* aligned to minUniformBufferOffsetAlignment */
    uint32_t        blocks;      /* per frame slot */
    uint8_t        *gen;         /* [blocks] current generation this frame */
    uint8_t        *bound;       /* [blocks] generation referenced by a draw */
    uint64_t        gen_serial;  /* frame serial gen[]/bound[] belong to */
    VkDescriptorSet set;         /* 2 dynamic-UBO bindings into buf */
    VkDescriptorSet set1;        /* 1 dynamic-UBO binding (rhi_ubo_bind_at) */
    VkDescriptorPool set_pool, set1_pool;   /* pools set and set1 came from */
    rhi_device     *dev;
};

/** @brief Write a contract-set descriptor set: bindings 0 and 1 view one block of a uniform block set, binding 2 the whole of a table buffer.
 *
 * The dynamic offsets at bind time select (frame slot, block) per UBO
 * binding.
 * @param dev Device.
 * @param set Descriptor set of ubo_set_layout.
 * @param u Uniform block set.
 * @param table Storage buffer shown at binding 2. */
static void contract_set_write(rhi_device *dev, VkDescriptorSet set, const rhi_ubo *u,
                               const rhi_buffer *table)
{
    VkDescriptorBufferInfo bi = {
        .buffer = u->buf->buffer, .offset = 0, .range = u->block_size,
    };
    VkDescriptorBufferInfo ti = { .buffer = table->buffer, .offset = 0, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet wr[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &bi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
          .pBufferInfo = &bi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = 2, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &ti },
    };
    vkUpdateDescriptorSets(dev->device, 3, wr, 0, NULL);
}

rhi_ubo *rhi_create_ubo(rhi_device *dev, uint32_t block_size, uint32_t blocks)
{
    if (!dev || block_size == 0 || blocks == 0) return NULL;
    rhi_ubo *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->dev = dev;
    u->blocks = blocks;
    u->block_size = (uint32_t)((block_size + dev->ubo_align - 1) &
                               ~(dev->ubo_align - 1));
    size_t total = (size_t)u->block_size * blocks * RHI_FRAMES_IN_FLIGHT * RHI_UBO_GENS;
    u->buf = create_host_buffer(dev, total, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                /*prefer_cached=*/false);
    u->gen = calloc(blocks, 1);
    u->bound = calloc(blocks, 1);
    if (!u->buf || !u->gen || !u->bound) {
        if (u->buf) rhi_destroy_buffer(dev, u->buf);
        free(u->gen); free(u->bound); free(u); return NULL;
    }
    memset(u->buf->mapped, 0, total);   /* unwritten blocks read as zeros */

    if (!dev->ubo_data_zero) {
        dev->ubo_data_zero = create_device_mapped_buffer(dev, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                         "zero table block");
        if (dev->ubo_data_zero) memset(dev->ubo_data_zero->mapped, 0, 16);
    }
    if (!dev->ubo_data_zero ||
        !desc_alloc(dev, dev->ubo_set_layout, &u->set, &u->set_pool)) {
        RHI_LOG("rhi_create_ubo: descriptor alloc failed\n");
        rhi_destroy_buffer(dev, u->buf);
        free(u->gen); free(u->bound);
        free(u); return NULL;
    }
    contract_set_write(dev, u->set, u, dev->ubo_data_zero);
    VkDescriptorBufferInfo bi = {
        .buffer = u->buf->buffer, .offset = 0, .range = u->block_size,
    };

    /* Second view of the same buffer through the single-binding layout, so a
     * block can also be bound at a set index outside the contract set. Its
     * absence is not fatal - rhi_ubo_bind_at simply becomes a no-op. */
    if (desc_alloc(dev, dev->ubo1_set_layout, &u->set1, &u->set1_pool)) {
        VkWriteDescriptorSet w1 = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = u->set1,
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .pBufferInfo = &bi,
        };
        vkUpdateDescriptorSets(dev->device, 1, &w1, 0, NULL);
    } else {
        RHI_LOG("rhi_create_ubo: single-binding descriptor alloc failed\n");
        u->set1 = VK_NULL_HANDLE;
    }
    return u;
}

/** @brief Reset a uniform block set's per-block generation and bound state on its first touch in a new frame.
 *
 * @param ubo Uniform block set. */
static void ubo_frame_sync(rhi_ubo *ubo)
{
    if (ubo->gen_serial == s_frame_serial) return;
    ubo->gen_serial = s_frame_serial;
    memset(ubo->gen, 0, ubo->blocks);
    memset(ubo->bound, 0, ubo->blocks);
}
/** @brief Return the block's physical index in the buffer for the current frame slot and generation.
 *
 * (frame slot x RHI_UBO_GENS + generation) x blocks + block; multiply by the
 * block size for the byte offset. The block index must be in range.
 * @param ubo Uniform block set on a device with a valid frame slot.
 * @param block Block index. */
static size_t ubo_phys(const rhi_ubo *ubo, uint32_t block)
{
    return ((size_t)ubo->dev->frame * RHI_UBO_GENS + ubo->gen[block]) * ubo->blocks + block;
}

void rhi_ubo_write(rhi_ubo *ubo, uint32_t block, const void *data, uint32_t len)
{
    if (!ubo || !data || block >= ubo->blocks) return;
    if (len > ubo->block_size) len = ubo->block_size;
    if (ubo->dev->frame_active) {
        ubo_frame_sync(ubo);
        if (ubo->bound[block]) {
            if (ubo->gen[block] + 1u < RHI_UBO_GENS) { ubo->gen[block]++; s_ring_gen_bumps++; }
            else {
                s_ring_gen_overflows++;
                static bool once; if (!once) { once = true;
                    RHI_LOG("ubo block %u: %u generations exhausted in frame %llu; overwriting bound data\n",
                            block, RHI_UBO_GENS, (unsigned long long)s_frame_serial); }
            }
            ubo->bound[block] = 0;
        }
        memcpy((uint8_t *)ubo->buf->mapped + ubo_phys(ubo, block) * ubo->block_size,
               data, len);
    } else {
        /* Outside a frame: fill every parity and generation (init-time writes). */
        for (uint32_t i = 0; i < RHI_FRAMES_IN_FLIGHT * RHI_UBO_GENS; i++)
            memcpy((uint8_t *)ubo->buf->mapped +
                       ((size_t)i * ubo->blocks + block) * ubo->block_size,
                   data, len);
    }
}

struct rhi_ubo_data {
    rhi_buffer     *buf;         /* the table block, STORAGE usage */
    VkDescriptorSet set;         /* contract set: the rhi_ubo's blocks + buf */
    VkDescriptorPool set_pool;
};

rhi_ubo_data *rhi_ubo_data_create(rhi_device *dev, rhi_ubo *ubo,
                                  const void *bytes, size_t len)
{
    if (!dev || !ubo || !bytes || len < 4) return NULL;
    rhi_ubo_data *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->buf = create_device_mapped_buffer(dev, len, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         "table block");
    if (!d->buf || !desc_alloc(dev, dev->ubo_set_layout, &d->set, &d->set_pool)) {
        RHI_LOG("rhi_ubo_data_create: %zu B table block allocation failed\n", len);
        if (d->buf) rhi_destroy_buffer(dev, d->buf);
        free(d);
        return NULL;
    }
    memcpy(d->buf->mapped, bytes, len);
    contract_set_write(dev, d->set, ubo, d->buf);
    return d;
}

void rhi_ubo_data_destroy(rhi_device *dev, rhi_ubo_data *data)
{
    if (!data) return;
    if (data->set) vkFreeDescriptorSets(dev->device, data->set_pool, 1, &data->set);
    rhi_destroy_buffer(dev, data->buf);
    free(data);
}

void rhi_ubo_bind_data(rhi_device *dev, rhi_ubo *ubo, uint32_t block_a,
                       uint32_t block_b, const rhi_ubo_data *data)
{
    if (!dev || !ubo || !dev->frame_active || !dev->cur_layout) return;
    if (block_a >= ubo->blocks || block_b >= ubo->blocks) return;
    ubo_frame_sync(ubo);
    ubo->bound[block_a] = ubo->bound[block_b] = 1;
    uint32_t offs[2] = {
        (uint32_t)ubo_phys(ubo, block_a) * ubo->block_size,
        (uint32_t)ubo_phys(ubo, block_b) * ubo->block_size,
    };
    const VkDescriptorSet set = data ? data->set : ubo->set;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, 3, 1, &set, 2, offs);
}

void rhi_ubo_bind(rhi_device *dev, rhi_ubo *ubo, uint32_t block_a,
                  uint32_t block_b)
{
    rhi_ubo_bind_data(dev, ubo, block_a, block_b, NULL);
}

rhi_ubo *rhi_create_ssbo(rhi_device *dev, uint32_t block_size, uint32_t blocks)
{
    if (!dev || block_size == 0 || blocks == 0 || !dev->ssbo1_set_layout) return NULL;
    rhi_ubo *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    const VkDeviceSize al = dev->ssbo_align > dev->ubo_align ? dev->ssbo_align : dev->ubo_align;
    u->dev = dev;
    u->blocks = blocks;
    u->block_size = (uint32_t)((block_size + al - 1) & ~(al - 1));
    size_t total = (size_t)u->block_size * blocks * RHI_FRAMES_IN_FLIGHT * RHI_UBO_GENS;
    u->buf = create_host_buffer(dev, total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                /*prefer_cached=*/false);
    u->gen = calloc(blocks, 1);
    u->bound = calloc(blocks, 1);
    if (!u->buf || !u->gen || !u->bound ||
        !desc_alloc(dev, dev->ssbo1_set_layout, &u->set1, &u->set1_pool)) {
        RHI_LOG("rhi_create_ssbo: %zu B buffer or descriptor allocation failed\n", total);
        if (u->buf) rhi_destroy_buffer(dev, u->buf);
        free(u->gen); free(u->bound); free(u); return NULL;
    }
    memset(u->buf->mapped, 0, total);   /* unwritten blocks read as zeros */
    VkDescriptorBufferInfo bi = { .buffer = u->buf->buffer, .offset = 0, .range = u->block_size };
    VkWriteDescriptorSet w1 = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = u->set1,
        .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &bi,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w1, 0, NULL);
    return u;
}

void rhi_ubo_bind_at(rhi_device *dev, rhi_ubo *ubo, uint32_t set, uint32_t block)
{
    if (!dev || !ubo || !ubo->set1 || !dev->frame_active || !dev->cur_layout) return;
    if (block >= ubo->blocks) return;
    ubo_frame_sync(ubo);
    ubo->bound[block] = 1;
    uint32_t off = (uint32_t)ubo_phys(ubo, block) * ubo->block_size;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &ubo->set1, 1, &off);
}

void rhi_destroy_ubo(rhi_device *dev, rhi_ubo *ubo)
{
    if (!ubo) return;
    if (ubo->set1) vkFreeDescriptorSets(dev->device, ubo->set1_pool, 1, &ubo->set1);
    if (ubo->set) vkFreeDescriptorSets(dev->device, ubo->set_pool, 1, &ubo->set);
    rhi_destroy_buffer(dev, ubo->buf);
    free(ubo->gen); free(ubo->bound);
    free(ubo);
}

/* ---- rhi_storage: retained read-only storage, versioned ------------ */

struct rhi_storage {
    rhi_buffer     *buf;        /* RHI_STORAGE_VERSIONS copies, `stride` apart */
    size_t          bytes;      /* contents size the shaders see */
    VkDeviceSize    stride;     /* bytes rounded up to the storage offset alignment */
    uint32_t        committed;  /* copy binds read */
    int32_t         mapped;     /* copy handed out by map, -1 when none */
    uint64_t        bound[RHI_STORAGE_VERSIONS];  /* last frame serial that bound each copy; 0 = never */
    VkDescriptorSet set;        /* the single dynamic storage binding over buf */
    VkDescriptorPool set_pool;
};

rhi_storage *rhi_storage_create(rhi_device *dev, size_t bytes)
{
    if (!dev || bytes < 4 || !dev->ssbo1_set_layout) return NULL;
    rhi_storage *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    const VkDeviceSize al = dev->ssbo_align;
    s->bytes = bytes;
    s->stride = ((VkDeviceSize)bytes + al - 1) & ~(al - 1);
    s->mapped = -1;
    const size_t total = (size_t)s->stride * RHI_STORAGE_VERSIONS;
    s->buf = create_device_mapped_buffer(dev, total, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         "retained storage");
    if (!s->buf || !desc_alloc(dev, dev->ssbo1_set_layout, &s->set, &s->set_pool)) {
        RHI_LOG("rhi_storage_create: %zu B buffer or descriptor allocation failed\n", total);
        if (s->buf) rhi_destroy_buffer(dev, s->buf);
        free(s);
        return NULL;
    }
    memset(s->buf->mapped, 0, total);
    VkDescriptorBufferInfo bi = { .buffer = s->buf->buffer, .offset = 0, .range = bytes };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set,
        .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &bi,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w, 0, NULL);
    return s;
}

/** @brief Return the copy rhi_storage_map hands out: the least recently bound copy other than the committed one.
 *
 * A copy never bound sorts before every bound one. The choice is free when
 * its serial plus RHI_FRAMES_IN_FLIGHT is at most the current frame serial
 * (rhi_begin_frame waited that frame's fence); otherwise every other copy is
 * in use and the caller counts the overwrite.
 * @param s Buffer.
 * @param free_out Receives whether the copy is retired.
 * @return The copy's index. */
static uint32_t storage_pick(const rhi_storage *s, bool *free_out)
{
    uint32_t best = UINT32_MAX;
    for (uint32_t v = 0; v < RHI_STORAGE_VERSIONS; v++) {
        if (v == s->committed) continue;
        if (best == UINT32_MAX || s->bound[v] < s->bound[best]) best = v;
    }
    *free_out = s->bound[best] + RHI_FRAMES_IN_FLIGHT <= s_frame_serial || s->bound[best] == 0;
    return best;
}

void *rhi_storage_map(rhi_storage *s)
{
    if (!s) return NULL;
    if (s->mapped < 0) {
        bool retired;
        const uint32_t v = storage_pick(s, &retired);
        if (!retired) {
            s_ring_gen_overflows++;
            static bool once;
            if (!once) {
                once = true;
                RHI_LOG("rhi_storage: %u copies in use in frame %llu; overwriting a bound copy\n",
                        RHI_STORAGE_VERSIONS, (unsigned long long)s_frame_serial);
            }
        }
        s->mapped = (int32_t)v;
    }
    return (uint8_t *)s->buf->mapped + (size_t)s->stride * (uint32_t)s->mapped;
}

void rhi_storage_commit(rhi_storage *s)
{
    if (!s || s->mapped < 0) return;
    s->committed = (uint32_t)s->mapped;
    s->mapped = -1;
}

void rhi_storage_bind(rhi_device *dev, rhi_storage *s, uint32_t set)
{
    if (!dev || !s || !dev->frame_active || !dev->cur_layout) return;
    s->bound[s->committed] = s_frame_serial;
    const uint32_t off = (uint32_t)(s->stride * s->committed);
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &s->set, 1, &off);
}

void rhi_storage_destroy(rhi_device *dev, rhi_storage *s)
{
    if (!s) return;
    if (s->set) vkFreeDescriptorSets(dev->device, s->set_pool, 1, &s->set);
    rhi_destroy_buffer(dev, s->buf);
    free(s);
}

/* ---- compute passes ------------------------------------------------- */

/* Stages and accesses every buffer use outside a compute pass may fall in:
 * the first scope of rhi_compute_begin's barrier and the second of
 * rhi_compute_end's. */
#define COMPUTE_USER_STAGES (VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | \
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | \
                             VK_PIPELINE_STAGE_TRANSFER_BIT)

struct rhi_compute_pipeline {
    VkPipeline            pipeline;
    VkPipelineLayout      layout;
    VkDescriptorSetLayout set_layout;   /* set 0: `bindings` dynamic storage buffers */
    uint32_t              bindings;
    uint32_t              push_size;
};

struct rhi_compute_set {
    VkDescriptorSet  set;
    VkDescriptorPool pool;
    uint32_t         n;
    rhi_storage     *storage[RHI_COMPUTE_MAX_BINDINGS];  /* NULL for a GPU-buffer binding */
};

struct rhi_gpu_buffer {
    VkBuffer         buffer;
    VkDeviceMemory   memory;
    VkDeviceSize     size;
    VkDescriptorSet  set;       /* ssbo1 layout, for rhi_gpu_buffer_bind */
    VkDescriptorPool set_pool;
};

struct rhi_readback {
    rhi_buffer *buf;                            /* RHI_FRAMES_IN_FLIGHT slots, `cap` apart */
    size_t      cap;
    size_t      len[RHI_FRAMES_IN_FLIGHT];      /* bytes queued in each slot; 0 = empty */
    uint64_t    serial[RHI_FRAMES_IN_FLIGHT];   /* frame serial each slot's copy was queued in */
};

/** @brief Decide whether the device runs compute passes and log the facts the decision read.
 *
 * Reads the graphics queue family's flags, the compute and storage limits
 * and OPENPETE_RHI_COMPUTE; enables nothing. Writes dev->compute_ok and,
 * when false, dev->compute_why.
 * @param dev Device after the physical device and queue family were chosen. */
static void compute_caps_init(rhi_device *dev)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev->phys, &props);
    const VkPhysicalDeviceLimits *l = &props.limits;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev->phys, &qn, NULL);
    VkQueueFamilyProperties *q = qn ? calloc(qn, sizeof(*q)) : NULL;
    if (q) vkGetPhysicalDeviceQueueFamilyProperties(dev->phys, &qn, q);
    const VkQueueFlags qf = q && dev->gfx_family < qn ? q[dev->gfx_family].queueFlags : 0;
    free(q);
    const char *knob = rhi_knob("OPENPETE_RHI_COMPUTE");
    const char *why = "";
    if (knob && *knob == '0') why = "OPENPETE_RHI_COMPUTE=0";
    else if (!(qf & VK_QUEUE_COMPUTE_BIT)) why = "the graphics queue family has no compute bit";
    else if (l->maxComputeWorkGroupInvocations < RHI_COMPUTE_MIN_INVOCATIONS ||
             l->maxComputeWorkGroupSize[0] < RHI_COMPUTE_MIN_INVOCATIONS)
        why = "workgroup size limit below 64";
    else if (l->maxStorageBufferRange < RHI_COMPUTE_MIN_STORAGE_RANGE)
        why = "maxStorageBufferRange below 2^27";
    else if (l->maxPerStageDescriptorStorageBuffers < RHI_COMPUTE_MAX_BINDINGS ||
             l->maxDescriptorSetStorageBuffersDynamic < RHI_COMPUTE_MAX_BINDINGS)
        why = "fewer than 4 storage-buffer bindings per stage or dynamic per layout";
    dev->compute_ok = why[0] == '\0';
    dev->compute_why = why;
    snprintf(dev->compute_facts, sizeof dev->compute_facts,
             "queue_family=%u flags=0x%x invocations=%u size_x=%u groups_x=%u "
             "storage_range=%u stage_storage=%u dynamic_storage=%u storage_align=%llu "
             "timestamps_compute=%u",
             dev->gfx_family, (unsigned)qf, l->maxComputeWorkGroupInvocations,
             l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupCount[0],
             l->maxStorageBufferRange, l->maxPerStageDescriptorStorageBuffers,
             l->maxDescriptorSetStorageBuffersDynamic,
             (unsigned long long)l->minStorageBufferOffsetAlignment,
             (unsigned)l->timestampComputeAndGraphics);
    RHI_LOG("compute: %s%s (%s)\n", dev->compute_ok ? "available" : "unavailable: ",
            dev->compute_ok ? "" : why, dev->compute_facts);
}

bool rhi_compute_supported(const rhi_device *dev)
{
    return dev && dev->compute_ok;
}

const char *rhi_compute_unsupported_reason(const rhi_device *dev)
{
    if (!dev) return "no device";
    return dev->compute_ok ? "" : dev->compute_why;
}

rhi_compute_pipeline *rhi_create_compute_pipeline(rhi_device *dev,
                                                  const rhi_compute_pipeline_desc *desc)
{
    if (!dev || !dev->compute_ok || !desc || !desc->comp_spv || desc->bindings == 0 ||
        desc->bindings > RHI_COMPUTE_MAX_BINDINGS || desc->push_const_size > 128 ||
        (desc->push_const_size & 3u))
        return NULL;
    rhi_compute_pipeline *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->bindings = desc->bindings;
    p->push_size = desc->push_const_size;
    VkDescriptorSetLayoutBinding b[RHI_COMPUTE_MAX_BINDINGS];
    for (uint32_t i = 0; i < desc->bindings; i++)
        b[i] = (VkDescriptorSetLayoutBinding){
            .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
    const VkDescriptorSetLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = desc->bindings, .pBindings = b,
    };
    const VkPushConstantRange pr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = desc->push_const_size,
    };
    size_t n = 0;
    void *owned = NULL;
    const void *spv = stage_bytes(NULL, 0, desc->comp_spv, &n, &owned);
    VkShaderModule m = spv ? create_shader_module(dev, spv, n, desc->comp_spv, 0) : VK_NULL_HANDLE;
    free(owned);
    bool ok = m != VK_NULL_HANDLE &&
              vkCreateDescriptorSetLayout(dev->device, &li, NULL, &p->set_layout) == VK_SUCCESS;
    if (ok) {
        const VkPipelineLayoutCreateInfo pli = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &p->set_layout,
            .pushConstantRangeCount = desc->push_const_size ? 1u : 0u, .pPushConstantRanges = &pr,
        };
        ok = vkCreatePipelineLayout(dev->device, &pli, NULL, &p->layout) == VK_SUCCESS;
    }
    if (ok) {
        const VkComputePipelineCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                       .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main" },
            .layout = p->layout,
        };
        ok = vkCreateComputePipelines(dev->device, dev->pcache, 1, &ci, NULL, &p->pipeline) == VK_SUCCESS;
    }
    if (m) vkDestroyShaderModule(dev->device, m, NULL);
    if (!ok) {
        RHI_LOG("rhi_create_compute_pipeline: %s failed\n", desc->comp_spv);
        rhi_destroy_compute_pipeline(dev, p);
        return NULL;
    }
    return p;
}

void rhi_destroy_compute_pipeline(rhi_device *dev, rhi_compute_pipeline *pipe)
{
    if (!pipe) return;
    if (pipe->pipeline) vkDestroyPipeline(dev->device, pipe->pipeline, NULL);
    if (pipe->layout) vkDestroyPipelineLayout(dev->device, pipe->layout, NULL);
    if (pipe->set_layout) vkDestroyDescriptorSetLayout(dev->device, pipe->set_layout, NULL);
    free(pipe);
}

/** @brief Record one global memory barrier into the current frame's command buffer.
 * @param dev Device inside an active frame.
 * @param src_stage Stages of the earlier accesses.
 * @param src_access Earlier writes made available.
 * @param dst_stage Stages of the later accesses.
 * @param dst_access Later accesses the writes are made visible to. */
static void memory_barrier(rhi_device *dev, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                           VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    const VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
    };
    vkCmdPipelineBarrier(dev->cmd[dev->frame], src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

rhi_gpu_buffer *rhi_gpu_buffer_create(rhi_device *dev, size_t bytes)
{
    if (!dev || bytes < 4 || !dev->ssbo1_set_layout) return NULL;
    rhi_gpu_buffer *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->size = (VkDeviceSize)bytes;
    const VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    bool ok = vkCreateBuffer(dev->device, &bi, NULL, &b->buffer) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev->device, b->buffer, &mr);
        uint32_t mt = find_memtype(dev->phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) mt = find_memtype(dev->phys, mr.memoryTypeBits, 0);
        const VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = mt,
        };
        ok = mt != UINT32_MAX && vkAllocateMemory(dev->device, &mai, NULL, &b->memory) == VK_SUCCESS &&
             vkBindBufferMemory(dev->device, b->buffer, b->memory, 0) == VK_SUCCESS;
    }
    ok = ok && desc_alloc(dev, dev->ssbo1_set_layout, &b->set, &b->set_pool);
    VkCommandBuffer cb = ok ? one_shot_begin(dev) : VK_NULL_HANDLE;
    if (!cb) {
        RHI_LOG("rhi_gpu_buffer_create: %zu B buffer, memory or descriptor allocation failed\n", bytes);
        rhi_gpu_buffer_destroy(dev, b);
        return NULL;
    }
    vkCmdFillBuffer(cb, b->buffer, 0, VK_WHOLE_SIZE, 0);
    const VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, COMPUTE_USER_STAGES, 0, 1, &mb,
                         0, NULL, 0, NULL);
    one_shot_end(dev, cb);
    const VkDescriptorBufferInfo dbi = { .buffer = b->buffer, .offset = 0, .range = VK_WHOLE_SIZE };
    const VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = b->set,
        .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &dbi,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w, 0, NULL);
    return b;
}

size_t rhi_gpu_buffer_size(const rhi_gpu_buffer *b)
{
    return b ? (size_t)b->size : 0;
}

void rhi_gpu_buffer_bind(rhi_device *dev, rhi_gpu_buffer *b, uint32_t set)
{
    if (!dev || !b || !dev->frame_active || !dev->cur_layout) return;
    const uint32_t off = 0;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &b->set, 1, &off);
}

void rhi_gpu_buffer_destroy(rhi_device *dev, rhi_gpu_buffer *b)
{
    if (!b) return;
    if (b->set) vkFreeDescriptorSets(dev->device, b->set_pool, 1, &b->set);
    if (b->buffer) vkDestroyBuffer(dev->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(dev->device, b->memory, NULL);
    free(b);
}

rhi_compute_set *rhi_compute_set_create(rhi_device *dev, const rhi_compute_pipeline *pipe,
                                        const rhi_compute_binding *b, uint32_t n)
{
    if (!dev || !pipe || !b || n != pipe->bindings) return NULL;
    VkDescriptorBufferInfo bi[RHI_COMPUTE_MAX_BINDINGS];
    VkWriteDescriptorSet w[RHI_COMPUTE_MAX_BINDINGS];
    rhi_compute_set *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->n = n;
    for (uint32_t i = 0; i < n; i++) {
        if (!b[i].storage == !b[i].gpu) {
            RHI_LOG("rhi_compute_set_create: binding %u names %s buffer\n", i,
                    b[i].storage ? "more than one" : "no");
            free(s);
            return NULL;
        }
        s->storage[i] = b[i].storage;
        bi[i] = b[i].storage
            ? (VkDescriptorBufferInfo){ .buffer = b[i].storage->buf->buffer, .range = b[i].storage->bytes }
            : (VkDescriptorBufferInfo){ .buffer = b[i].gpu->buffer, .range = VK_WHOLE_SIZE };
    }
    if (!desc_alloc(dev, pipe->set_layout, &s->set, &s->pool)) {
        RHI_LOG("rhi_compute_set_create: descriptor allocation failed\n");
        free(s);
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++)
        w[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set,
            .dstBinding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &bi[i],
        };
    vkUpdateDescriptorSets(dev->device, n, w, 0, NULL);
    return s;
}

void rhi_compute_set_destroy(rhi_device *dev, rhi_compute_set *set)
{
    if (!set) return;
    if (set->set) vkFreeDescriptorSets(dev->device, set->pool, 1, &set->set);
    free(set);
}

bool rhi_compute_begin(rhi_device *dev)
{
    seam_check("compute_begin");
    if (!dev || !dev->compute_ok || !dev->frame_active || dev->pass_open || dev->compute_open)
        return false;
    memory_barrier(dev, COMPUTE_USER_STAGES,
                   VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    dev->compute_open = true;
    dev->compute_pipe = NULL;
    dev->compute_set_bound = false;
    return true;
}

void rhi_compute_bind(rhi_device *dev, rhi_compute_pipeline *pipe, rhi_compute_set *set)
{
    if (!dev || !dev->compute_open || !pipe || !set || set->n != pipe->bindings) return;
    uint32_t off[RHI_COMPUTE_MAX_BINDINGS] = { 0 };
    for (uint32_t i = 0; i < set->n; i++) {
        rhi_storage *st = set->storage[i];
        if (!st) continue;
        st->bound[st->committed] = s_frame_serial;
        off[i] = (uint32_t)(st->stride * st->committed);
    }
    VkCommandBuffer cb = dev->cmd[dev->frame];
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->layout, 0, 1, &set->set,
                            set->n, off);
    dev->compute_pipe = pipe;
    dev->compute_set_bound = true;
}

void rhi_compute_push(rhi_device *dev, const void *data, uint32_t size)
{
    if (!dev || !dev->compute_open || !dev->compute_pipe || !data || size == 0 ||
        size > dev->compute_pipe->push_size)
        return;
    vkCmdPushConstants(dev->cmd[dev->frame], dev->compute_pipe->layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, size, data);
}

void rhi_compute_dispatch(rhi_device *dev, uint32_t gx, uint32_t gy, uint32_t gz)
{
    if (!dev || !dev->compute_open || !dev->compute_set_bound || !gx || !gy || !gz) return;
    dev->compute_dispatches++;
    vkCmdDispatch(dev->cmd[dev->frame], gx, gy, gz);
}

void rhi_compute_barrier(rhi_device *dev)
{
    if (!dev || !dev->compute_open) return;
    memory_barrier(dev, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void rhi_compute_end(rhi_device *dev)
{
    if (!dev || !dev->compute_open) return;
    dev->compute_open = false;
    if (!dev->frame_active) return;
    memory_barrier(dev, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   COMPUTE_USER_STAGES,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                   VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
}

uint32_t rhi_compute_dispatches(const rhi_device *dev)
{
    return dev ? dev->compute_dispatches : 0;
}

rhi_readback *rhi_readback_create(rhi_device *dev, size_t bytes)
{
    if (!dev || bytes == 0) return NULL;
    rhi_readback *rb = calloc(1, sizeof(*rb));
    if (!rb) return NULL;
    rb->cap = bytes;
    rb->buf = create_host_buffer(dev, bytes * RHI_FRAMES_IN_FLIGHT,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, /*prefer_cached=*/true);
    if (!rb->buf || !rb->buf->mapped) {
        RHI_LOG("rhi_readback_create: %zu B ring allocation failed\n", bytes * RHI_FRAMES_IN_FLIGHT);
        if (rb->buf) rhi_destroy_buffer(dev, rb->buf);
        free(rb);
        return NULL;
    }
    return rb;
}

/** @brief Copy a ring slot's capture to the caller and empty the slot.
 * @param rb Ring.
 * @param slot Slot index.
 * @param out Destination, or NULL to drop the capture.
 * @param out_cap Capacity of @p out.
 * @param out_serial Receives the capture's frame serial; may be NULL.
 * @return Bytes written; 0 when the slot was empty or the capture did not fit. */
static size_t readback_take(rhi_readback *rb, uint32_t slot, void *out, size_t out_cap,
                            uint64_t *out_serial)
{
    const size_t n = rb->len[slot];
    rb->len[slot] = 0;
    if (n == 0 || !out || n > out_cap) return 0;
    memcpy(out, (const uint8_t *)rb->buf->mapped + rb->cap * slot, n);
    if (out_serial) *out_serial = rb->serial[slot];
    return n;
}

size_t rhi_readback_cycle(rhi_device *dev, rhi_readback *rb, rhi_gpu_buffer *src,
                          size_t offset, size_t bytes, void *out, size_t out_cap,
                          uint64_t *out_serial)
{
    if (!dev || !rb || !dev->frame_active || dev->pass_open || dev->compute_open) return 0;
    const uint32_t slot = dev->frame;
    const size_t got = readback_take(rb, slot, out, out_cap, out_serial);
    if (!src || bytes == 0 || bytes > rb->cap || offset > src->size || bytes > src->size - offset)
        return got;
    const VkBufferCopy region = { .srcOffset = offset, .dstOffset = rb->cap * slot, .size = bytes };
    vkCmdCopyBuffer(dev->cmd[slot], src->buffer, rb->buf->buffer, 1, &region);
    memory_barrier(dev, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    rb->len[slot] = bytes;
    rb->serial[slot] = s_frame_serial;
    return got;
}

size_t rhi_readback_drain(rhi_device *dev, rhi_readback *rb, void *out, size_t out_cap,
                          uint64_t *out_serial)
{
    if (!dev || !rb || dev->frame_active) return 0;
    for (;;) {
        uint32_t oldest = UINT32_MAX;
        for (uint32_t i = 0; i < RHI_FRAMES_IN_FLIGHT; i++)
            if (rb->len[i] && (oldest == UINT32_MAX || rb->serial[i] < rb->serial[oldest])) oldest = i;
        if (oldest == UINT32_MAX) return 0;
        const size_t got = readback_take(rb, oldest, out, out_cap, out_serial);
        if (got) return got;
    }
}

void rhi_readback_destroy(rhi_device *dev, rhi_readback *rb)
{
    if (!rb) return;
    rhi_destroy_buffer(dev, rb->buf);
    free(rb);
}

/* ---- offscreen-as-input: the COMPOSITE postfx point ---------------- */

void rhi_offscreen_to_sampled(rhi_device *dev)
{
    if (!dev || !dev->frame_active) return;
    /* off_rp left the image TRANSFER_SRC (its finalLayout); its subpass->
     * external dependency covers TRANSFER reads only, so raise our own
     * color-write -> fragment-read barrier with the layout flip. */
    image_barrier(dev->cmd[dev->frame], dev->off_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void rhi_bind_offscreen_tex(rhi_device *dev, uint32_t set)
{
    if (!dev || !dev->frame_active || !dev->cur_layout) return;
    uint32_t slot = dev->frame;
    if (dev->off_sampled_set[slot] == VK_NULL_HANDLE) {
        if (!desc_alloc(dev, dev->vram_set_layout, &dev->off_sampled_set[slot], NULL)) {
            RHI_LOG("off_sampled_set alloc failed\n");
            return;
        }
        dev->off_sampled_gen[slot] = 0;   /* force the write below */
    }
    if (dev->off_sampled_gen[slot] != dev->off_gen) {
        /* Safe to rewrite THIS slot's set: begin_frame fenced this slot, so
         * no in-flight use remains (the other slot keeps its own set). */
        VkDescriptorImageInfo ii = {
            .sampler = dev->linear_sampler,
            .imageView = dev->off_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet wr = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dev->off_sampled_set[slot],
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &ii,
        };
        vkUpdateDescriptorSets(dev->device, 1, &wr, 0, NULL);
        dev->off_sampled_gen[slot] = dev->off_gen;
    }
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1,
                            &dev->off_sampled_set[slot], 0, NULL);
}

/* Undo rhi_offscreen_to_sampled WITHOUT re-rendering (a chain that
 * built no runnable pass): flip off_image straight back to TRANSFER_SRC
 * so the present blit can read it. */
void rhi_offscreen_resume_blit_src(rhi_device *dev)
{
    if (!dev || !dev->frame_active) return;
    image_barrier(dev->cmd[dev->frame], dev->off_image,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void rhi_offscreen_resume_attachment(rhi_device *dev)
{
    if (!dev || !dev->frame_active) return;
    /* Fragment reads of off_image must finish before the re-begun offscreen
     * pass writes it (off_rp initialLayout is UNDEFINED + loadOp CLEAR, so
     * the layout value here only names the source scope). */
    image_barrier(dev->cmd[dev->frame], dev->off_image,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}

uint32_t rhi_storage_offset_align(const rhi_device *dev)
{
    return dev && dev->ssbo_align ? (uint32_t)dev->ssbo_align : 1u;
}

/** @brief Pick the texture's descriptor-set entry for a (store, range) pair: its own, else an empty or retired one.
 * @param tex Texture.
 * @param uid The store's uid.
 * @param range Bytes binding 1 spans.
 * @param made Receives 1 when the entry already describes the pair.
 * @return The entry index, or -1 when every entry is bound by an unretired frame. */
static int texture_store_slot(const rhi_texture *tex, uint64_t uid, uint32_t range, int *made)
{
    int pick = -1;
    *made = 0;
    for (int i = 0; i < RHI_TEX_STORE_SETS; i++) {
        if (tex->store_set[i] && tex->store_uid[i] == uid && tex->store_range[i] == range) {
            *made = 1;
            return i;
        }
        const bool free_entry = !tex->store_set[i] ||
                                tex->store_serial[i] + RHI_FRAMES_IN_FLIGHT <= s_frame_serial;
        if (free_entry && (pick < 0 || !tex->store_set[i])) pick = i;
    }
    return pick;
}

/** @brief Make or reuse the texture's image-plus-storage descriptor set for one (store, range) pair.
 *
 * Each texture keeps RHI_TEX_STORE_SETS sets keyed by the store's uid and
 * the range. A pair without a set takes an empty entry, or one no unretired
 * frame bound (its set is freed first); with every entry in flight the pair
 * is refused (logged once).
 * @param dev Device.
 * @param tex Texture.
 * @param store Storage buffer.
 * @param range Bytes binding 1 spans.
 * @return The entry describing the pair, or -1. */
static int texture_store_set(rhi_device *dev, rhi_texture *tex, const rhi_buffer *store,
                             uint32_t range)
{
    int made;
    const int i = texture_store_slot(tex, store->uid, range, &made);
    if (made) return i;
    if (i < 0) {
        static bool once;
        if (!once) { once = true; RHI_LOG("rhi_bind_texture_storage: every (store, range) set of the texture is in flight; refused\n"); }
        return -1;
    }
    if (tex->store_set[i]) {
        vkFreeDescriptorSets(dev->device, tex->store_pool[i], 1, &tex->store_set[i]);
        tex->store_set[i] = VK_NULL_HANDLE;
    }
    if (!desc_alloc(dev, dev->texssbo_set_layout, &tex->store_set[i], &tex->store_pool[i])) {
        RHI_LOG("rhi_bind_texture_storage: descriptor set allocation failed\n");
        return -1;
    }
    VkDescriptorImageInfo ii = {
        .sampler = dev->vram_sampler,
        .imageView = tex->rt_depth_sample_view ? tex->rt_depth_sample_view : tex->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkDescriptorBufferInfo bi = { .buffer = store->buffer, .offset = 0, .range = range };
    VkWriteDescriptorSet w[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = tex->store_set[i],
          .dstBinding = 0, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = tex->store_set[i],
          .dstBinding = 1, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &bi },
    };
    vkUpdateDescriptorSets(dev->device, 2, w, 0, NULL);
    tex->store_uid[i] = store->uid;
    tex->store_range[i] = range;
    return i;
}

int rhi_bind_texture_storage(rhi_device *dev, rhi_texture *tex, rhi_buffer *store,
                             uint32_t range, uint32_t set, uint32_t offset)
{
    if (!dev || !tex || !store || !dev->frame_active || !dev->cur_layout) return 0;
    const int i = texture_store_set(dev, tex, store, range);
    if (i < 0) return -1;
    tex->store_serial[i] = s_frame_serial;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &tex->store_set[i], 1, &offset);
    return 0;
}

void rhi_bind_texture_set(rhi_device *dev, rhi_texture *tex, uint32_t set)
{
    if (!dev || !tex || !dev->frame_active || !dev->cur_layout) return;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &tex->set, 0, NULL);
}

void rhi_blit_rt_to_offscreen(rhi_device *dev, rhi_texture *src)
{
    if (!dev || !src || !src->is_rt || !dev->frame_active) return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    image_barrier(cb, src->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cb, dev->off_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0, 0, 0}, {(int32_t)src->width, (int32_t)src->height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0, 0, 0}, {(int32_t)dev->off_extent.width,
                                    (int32_t)dev->off_extent.height, 1} },
    };
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);
    image_barrier(cb, dev->off_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    src->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* Same write-back for a render-target compose path (the screenshot-all
 * native_rtt dump): dst ends SHADER_READ (readable by
 * rhi_read_render_target), src restored to SHADER_READ. */
void rhi_blit_rt_to_rt(rhi_device *dev, rhi_texture *dst, rhi_texture *src)
{
    if (!dev || !dst || !src || !dst->is_rt || !src->is_rt || !dev->frame_active)
        return;
    VkCommandBuffer cb = dev->cmd[dev->frame];
    image_barrier(cb, src->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(cb, dst->image, dst->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .srcOffsets = { {0, 0, 0}, {(int32_t)src->width, (int32_t)src->height, 1} },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .dstOffsets = { {0, 0, 0}, {(int32_t)dst->width, (int32_t)dst->height, 1} },
    };
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_NEAREST);
    image_barrier(cb, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    image_barrier(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    dst->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    src->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void rhi_destroy_pipeline(rhi_device *dev, rhi_pipeline *pipe)
{
    if (!pipe) return;
    rebuild_forget(dev, pipe);
    registry_unlink(dev, pipe);
    if (pipe->pipeline) vkDestroyPipeline(dev->device, pipe->pipeline, NULL);
    if (pipe->layout)   vkDestroyPipelineLayout(dev->device, pipe->layout, NULL);
    pipeline_free(pipe);
}

void rhi_bind_pipeline(rhi_device *dev, rhi_pipeline *pipe)
{
    if (!dev->frame_active) return;
    dev->null_binds += pipe->pipeline == VK_NULL_HANDLE;
    dev->cur_layout = pipe->layout; /* for bind_vram / push_constants */
    dev->cur_pc_stages = pipe->pc_stages;
    dev->cur_depth_rev = dev->depth_rev[pipe->desc.depth_class];
    vkCmdBindPipeline(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
    /* A pipeline holding depth bias static invalidates the dynamic value, so
     * the next depth_bias pipeline records the last factors again. */
    dev->bias_owed |= !pipe->desc.depth_bias;
    dev->bias_bound = pipe->desc.depth_bias;
    if (dev->bias_bound && dev->bias_owed) {
        vkCmdSetDepthBias(dev->cmd[dev->frame], dev->bias_pending[0], dev->bias_pending[1],
                          dev->bias_pending[2]);
        dev->bias_owed = false;
    }
}

void rhi_bind_vertex_buffer(rhi_device *dev, rhi_buffer *buf)
{
    if (!dev->frame_active) return;
    /* Dynamic ring buffers bind the current frame's slot and generation
     * (see rhi_buffer); a static buffer has ring_stride 0. */
    VkDeviceSize off = buf->ring_stride ? buf->ring_stride * ring_bind_slot(buf) : 0;
    vkCmdBindVertexBuffers(dev->cmd[dev->frame], 0, 1, &buf->buffer, &off);
}

void rhi_bind_vertex_buffer2(rhi_device *dev, rhi_buffer *buf, rhi_buffer *prev)
{
    if (!dev->frame_active) return;
    VkBuffer bufs[2] = { buf->buffer, prev->buffer };
    VkDeviceSize offs[2] = { buf->ring_stride ? buf->ring_stride * ring_bind_slot(buf) : 0,
                             prev->ring_stride ? prev->ring_stride * ring_bind_slot(prev) : 0 };
    vkCmdBindVertexBuffers(dev->cmd[dev->frame], 0, 2, bufs, offs);
}

void rhi_texture_set_filter_linear(rhi_device *dev, rhi_texture *tex)
{
    /* Rewrite the texture's (view, sampler) descriptor with the LINEAR
     * sampler. Only safe while the set is not in flight - call at load time,
     * before the texture's first frame. */
    VkDescriptorImageInfo ii = {
        .sampler = dev->linear_sampler,
        .imageView = tex->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = tex->set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &ii,
    };
    vkUpdateDescriptorSets(dev->device, 1, &w, 0, NULL);
}

void rhi_bind_vram(rhi_device *dev, rhi_texture *tex)
{
    if (!dev->frame_active) return;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, 0, 1, &tex->set, 0, NULL);
}

void rhi_bind_vram2(rhi_device *dev, rhi_texture *tex)
{
    if (!dev->frame_active) return;
    vkCmdBindDescriptorSets(dev->cmd[dev->frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, 1, 1, &tex->set, 0, NULL);
}

/** @brief Append one allocated set to a frame slot's list of the pair (kind 0), trio (kind 1) or quad (kind 2) layout, growing the list's arrays by doubling.
 *
 * @param dev Device.
 * @param kind 0 for the pair layout, 1 for the trio layout, 2 for the quad layout.
 * @param f Frame slot.
 * @return false when the arrays cannot grow or the set cannot be allocated. */
static bool view_sets_grow(rhi_device *dev, int kind, uint32_t f)
{
    rhi_view_sets *L = &dev->vsets[kind][f];
    if (L->alloc == L->cap) {
        const uint32_t cap = L->cap ? L->cap * 2 : 32;
        VkDescriptorSet *sets = realloc(L->sets, cap * sizeof(*sets));
        if (!sets) return false;
        L->sets = sets;
        VkImageView (*views)[4] = realloc(L->views, cap * sizeof(*views));
        if (!views) return false;
        L->views = views;
        L->cap = cap;
    }
    if (!desc_alloc(dev, kind == 2 ? dev->quad_set_layout
                       : kind ? dev->trio_set_layout : dev->pair_set_layout,
                    &L->sets[L->alloc], NULL))
        return false;
    L->alloc++;
    return true;
}

/** @brief Bind a sampler set holding views v[0..n-1] at bindings 0..n-1 at `set`, n 2 (the pair layout) or 3 (the trio layout).
 *
 * The frame slot reuses its set already written with the same views, else
 * writes the next set of the slot's list for that layout, allocating one
 * when the list is exhausted (view_sets_grow); the list has no fixed
 * length. A bind whose allocation fails is dropped with a log. Every view
 * is sampled through vram_sampler in SHADER_READ_ONLY_OPTIMAL layout.
 * @param dev Device whose current frame records the bind.
 * @param v The views.
 * @param n View count, 2 or 3.
 * @param set Descriptor set index in the bound pipeline's layout. */
static void bind_view_set(rhi_device *dev, const VkImageView *v, uint32_t n, uint32_t set)
{
    const uint32_t f = dev->frame;
    const int kind = n == 4 ? 2 : n == 3 ? 1 : 0;
    rhi_view_sets *L = &dev->vsets[kind][f];
    for (uint32_t i = 0; i < L->used; i++) {
        if (L->views[i][0] == v[0] && L->views[i][1] == v[1] && (n < 3 || L->views[i][2] == v[2]) &&
            (n < 4 || L->views[i][3] == v[3])) {
            vkCmdBindDescriptorSets(dev->cmd[f], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    dev->cur_layout, set, 1, &L->sets[i], 0, NULL);
            return;
        }
    }
    if (L->used == L->alloc && !view_sets_grow(dev, kind, f)) {
        RHI_LOG("sampler set: allocation failed; the bind is dropped\n");
        return;
    }
    const uint32_t slot = L->used++;
    VkDescriptorSet ds = L->sets[slot];
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w[4];
    for (uint32_t k = 0; k < n; k++) {
        L->views[slot][k] = v[k];
        ii[k] = (VkDescriptorImageInfo){ .sampler = dev->vram_sampler, .imageView = v[k],
                                         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[k] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = k,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &ii[k] };
    }
    vkUpdateDescriptorSets(dev->device, n, w, 0, NULL);
    vkCmdBindDescriptorSets(dev->cmd[f], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            dev->cur_layout, set, 1, &ds, 0, NULL);
}

/** @brief Bind a two-sampler pair set holding views v0 (binding 0) and v1 (binding 1) at `set` (bind_view_set).
 * @param dev Device whose current frame records the bind.
 * @param v0 View for binding 0.
 * @param v1 View for binding 1.
 * @param set Descriptor set index in the bound pipeline's layout. */
static void bind_view_pair(rhi_device *dev, VkImageView v0, VkImageView v1, uint32_t set)
{
    const VkImageView v[2] = { v0, v1 };
    bind_view_set(dev, v, 2, set);
}

void rhi_bind_terrain_key_set(rhi_device *dev, rhi_texture *key_rt, rhi_texture *drain,
                              uint32_t set)
{
    if (!dev->frame_active || !key_rt || !drain || !key_rt->rt_depth_sample_view) return;
    const VkImageView v[3] = { key_rt->rt_depth_sample_view, drain->view, key_rt->view };
    bind_view_set(dev, v, 3, set);
}

void rhi_bind_vram_depth_pair(rhi_device *dev, rhi_texture *tex, rhi_texture *depth_rt,
                              uint32_t set)
{
    if (!dev->frame_active || !tex || !depth_rt || !depth_rt->rt_depth_sample_view) return;
    bind_view_pair(dev, tex->view, depth_rt->rt_depth_sample_view, set);
}

void rhi_bind_depth_tex_pair(rhi_device *dev, rhi_texture *depth_rt, rhi_texture *tex,
                             uint32_t set)
{
    if (!dev->frame_active || !tex || !depth_rt || !depth_rt->rt_depth_sample_view) return;
    bind_view_pair(dev, depth_rt->rt_depth_sample_view, tex->view, set);
}

void rhi_bind_depth_rt_quad(rhi_device *dev, rhi_texture *a, rhi_texture *b, uint32_t set)
{
    if (!dev->frame_active || !a || !b || !a->rt_depth_sample_view || !b->rt_depth_sample_view)
        return;
    const VkImageView v[4] = { a->rt_depth_sample_view, a->view, b->rt_depth_sample_view, b->view };
    bind_view_set(dev, v, 4, set);
}

void rhi_push_constants(rhi_device *dev, const void *data, uint32_t size)
{
    if (!dev->frame_active) return;
    vkCmdPushConstants(dev->cmd[dev->frame], dev->cur_layout,
                       dev->cur_pc_stages ? dev->cur_pc_stages
                                          : VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, size, data);
}

void rhi_draw(rhi_device *dev, uint32_t vertex_count, uint32_t first_vertex)
{
    if (!dev->frame_active) return;
    dev->draw_calls++;
    vkCmdDraw(dev->cmd[dev->frame], vertex_count, 1, first_vertex, 0);
}

void rhi_draw_instanced(rhi_device *dev, uint32_t vertex_count, uint32_t instance_count,
                        uint32_t first_vertex, uint32_t first_instance)
{
    if (!dev->frame_active) return;
    dev->draw_calls++;
    vkCmdDraw(dev->cmd[dev->frame], vertex_count, instance_count, first_vertex,
              first_instance);
}

uint32_t rhi_draw_calls(rhi_device *dev)
{
    return dev->draw_calls;
}

void rhi_set_scissor(rhi_device *dev, int x, int y, int w, int h)
{
    if (!dev->frame_active) return;
    /* Callers pass swapchain/window pixels. When the bound pass targets a
     * larger render area (the SSAA supersampled scene RT, whose extent is an
     * integer multiple of the swapchain), scale the rect by that factor so the
     * panel scissor still confines the N×-resolution scene. (No-op when the
     * current pass matches the swapchain extent, i.e. the 1× offscreen.) */
    if (dev->off_extent.width &&
        dev->cur_extent.width  > dev->off_extent.width &&
        dev->cur_extent.width  % dev->off_extent.width == 0) {
        int eff = (int)(dev->cur_extent.width / dev->off_extent.width);
        x *= eff; y *= eff; w *= eff; h *= eff;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (x + w > (int)dev->cur_extent.width)  w = (int)dev->cur_extent.width  - x;
    if (y + h > (int)dev->cur_extent.height) h = (int)dev->cur_extent.height - y;
    VkRect2D s = { .offset = { x, y }, .extent = { (uint32_t)(w > 0 ? w : 0),
                                                   (uint32_t)(h > 0 ? h : 0) } };
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &s);
}

void rhi_reset_scissor(rhi_device *dev)
{
    if (!dev->frame_active) return;
    VkRect2D s = { .offset = { 0, 0 }, .extent = dev->cur_extent };
    vkCmdSetScissor(dev->cmd[dev->frame], 0, 1, &s);
}

void rhi_set_depth_bias(rhi_device *dev, float constant, float clamp, float slope)
{
    if (!dev->frame_active) return;
    /* Only consumed by pipelines created with desc.depth_bias (the dynamic
     * state is per-pipeline); setting it with another pipeline bound is legal
     * and inert. Parameter order matches vkCmdSetDepthBias. Without the
     * depthBiasClamp feature the clamp param MUST be 0.0 (spec 00790) - the
     * pull is then bounded only by the slope term; acceptable degradation. */
    if (!dev->has_depth_bias_clamp) clamp = 0.0f;
    /* The caller's sign convention is forward: negative pulls toward the
     * camera. Under the reversed scene convention nearer is larger, so all
     * three factors flip. The slope and clamp keep their view-Z meaning
     * exactly (|dz/dvz| = 1/vz^2 in both encodings); the constant is in
     * units of the stored value's resolvable step r, 2^-24 near z = 1
     * forward and about z * 2^-23.5 reversed. */
    const float sgn = dev->depth_rev[RHI_DEPTH_SCENE] ? -1.0f : 1.0f;
    depth_bias_set(dev, sgn * constant, sgn * clamp, sgn * slope);
}
