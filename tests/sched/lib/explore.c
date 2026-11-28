/* explore.c - the explorer: forks one child per schedule and enumerates
 * choice strings.
 *
 * DFS mode is stateless search over choice prefixes. A child runs its
 * prefix, then takes the default at every later choice and reports each
 * such choice's alternatives with their preemption and deviation costs.
 * Every alternative at a reported position becomes a new prefix (the
 * prefix, the defaults up to that position, the alternative), so each
 * complete choice string is run exactly once. The bound pairs (p, d) are
 * visited in order of p + d, then p; a prefix whose cost exceeds the pair
 * under exploration but fits the configured limits is deferred to the
 * first later pair that covers it, so no schedule runs twice and a failure
 * is found at the smallest pair that covers its cost.
 *
 * DFS runs its schedules through fork servers: worker processes forked once
 * from the explorer, each of which forks one child per schedule it is sent
 * and forwards the child's report, framed with the child's pid and wait
 * status. The explorer's memory (a created device, say) is copied once per
 * worker, and the per-schedule forks run in parallel. */
#define _GNU_SOURCE
#include "op_sched_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/** A choice prefix waiting to run, with its accumulated cost. */
typedef struct prefix {
    uint16_t *ch;
    int len;
    int p, d;
} prefix;

/** A growable stack of prefixes. */
typedef struct pstack { prefix *v; long n, cap; } pstack;

/** A running child and the report it has sent so far. */
typedef struct child {
    pid_t pid;
    int fd;
    prefix pre;
    uint8_t *buf;
    size_t len, cap;
} child;

/** @brief Push a prefix onto a stack, growing it; exits on allocation failure.
 * @param st The stack.
 * @param p The prefix (ownership moves to the stack). */
static void ps_push(pstack *st, prefix p)
{
    if (st->n == st->cap) {
        const long cap = st->cap ? st->cap * 2 : 1024;
        prefix *v = realloc(st->v, (size_t)cap * sizeof *st->v);
        if (!v) { perror("sched: realloc"); exit(2); }
        st->v = v;
        st->cap = cap;
    }
    st->v[st->n++] = p;
}

/** @brief Build the prefix base[0..pos) (defaults beyond base's length) followed by alt.
 * @param base The parent prefix.
 * @param pos Position of the alternative.
 * @param alt The alternative.
 * @param dp Its preemption cost.
 * @param dd Its deviation cost.
 * @return The new prefix. */
static prefix prefix_extend(const prefix *base, int pos, int alt, int dp, int dd)
{
    prefix p = { calloc((size_t)pos + 1, sizeof(uint16_t)), pos + 1, base->p + dp, base->d + dd };
    if (!p.ch) { perror("sched: calloc"); exit(2); }
    memcpy(p.ch, base->ch, (size_t)base->len * sizeof(uint16_t));
    p.ch[pos] = (uint16_t)alt;
    return p;
}

/** @brief Parse a choice string ("-" or comma-separated indices) into a prefix.
 * @param s The string.
 * @param out The prefix.
 * @return 0 on success, -1 on a malformed string. */
static int prefix_parse(const char *s, prefix *out)
{
    memset(out, 0, sizeof *out);
    out->ch = calloc(SCHED_MAX_CHOICES, sizeof(uint16_t));
    if (!out->ch) return -1;
    if (!s || !strcmp(s, "-") || !*s) return 0;
    while (*s) {
        char *end;
        const long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v >= SCHED_MAX_ALTS || out->len >= SCHED_MAX_CHOICES) return -1;
        out->ch[out->len++] = (uint16_t)v;
        s = end;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return 0;
}

/** @brief Fork a child that runs one schedule; the parent keeps the read end of its report pipe.
 * @param cfg The configuration.
 * @param pre Its prefix.
 * @param pct Nonzero to draw choices by PCT after the prefix.
 * @param seed PCT seed.
 * @param trace_always Nonzero to write the trace on a pass too.
 * @param c Receives the child.
 * @return 0, or -1 when fork or pipe failed. */
static int child_spawn(const sched_config *cfg, prefix pre, int pct, uint64_t seed, int trace_always, child *c)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fflush(NULL);
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        sched_child_plan plan = { cfg, pre.ch, pre.len, fds[1], pct, seed, cfg->trace_path, trace_always };
        sched_child_run(&plan);
    }
    close(fds[1]);
    memset(c, 0, sizeof *c);
    c->pid = pid; c->fd = fds[0]; c->pre = pre;
    return 0;
}

/** @brief Read what a child's pipe holds into its buffer.
 * @param c The child.
 * @return 1 at end of file, 0 when more may come, -1 on error. */
static int child_read(child *c)
{
    if (c->cap - c->len < 65536) {
        const size_t cap = c->cap ? c->cap * 2 : 131072;
        uint8_t *buf = realloc(c->buf, cap);
        if (!buf) return -1;
        c->buf = buf;
        c->cap = cap;
    }
    const ssize_t r = read(c->fd, c->buf + c->len, c->cap - c->len);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 1;
    c->len += (size_t)r;
    return 0;
}

/** What one finished child reported. */
typedef struct child_report {
    int status;           /* SCHED_CHILD_*, or -1 when the child died without an end record */
    long steps;
    int nchoices;
    char msg[512];
    char choices[4096];
} child_report;

/** @brief Decode a child's end record at buf[at..len).
 * @param b The buffer.
 * @param at Offset of the record's first byte after the tag.
 * @param len Buffer length.
 * @param rep Receives the record.
 * @return 0, or -1 when truncated. */
static int decode_end(const uint8_t *b, size_t at, size_t len, child_report *rep)
{
    int32_t st, nc; int64_t steps; uint16_t ml, cl;
    if (at + 18 > len) return -1;
    memcpy(&st, b + at, 4); memcpy(&steps, b + at + 4, 8); memcpy(&nc, b + at + 12, 4); memcpy(&ml, b + at + 16, 2);
    at += 18;
    if (at + ml + 2 > len) return -1;
    memcpy(rep->msg, b + at, ml < 511 ? ml : 511); rep->msg[ml < 511 ? ml : 511] = 0;
    at += ml;
    memcpy(&cl, b + at, 2); at += 2;
    if (at + cl > len) return -1;
    memcpy(rep->choices, b + at, cl < 4095 ? cl : 4095); rep->choices[cl < 4095 ? cl : 4095] = 0;
    rep->status = st; rep->steps = (long)steps; rep->nchoices = nc;
    return 0;
}

/** Bounds of a DFS exploration: the pair under exploration and the configured limits. */
typedef struct bounds { int p, d, max_p, max_d; } bounds;

/** @brief Expand a finished child's choice records into new prefixes: run now, deferred, or pruned.
 * @param c The child.
 * @param bd Bounds.
 * @param now Prefixes within the current pair.
 * @param later Prefixes deferred to a later pair.
 * @param r Result counters.
 * @param rep Receives the end record.
 * @return 0, or -1 on a malformed report. */
static int child_expand(const child *c, const bounds *bd, pstack *now, pstack *later,
                        sched_result *r, child_report *rep)
{
    size_t at = 0;
    int pos = c->pre.len;
    rep->status = -1;
    while (at < c->len) {
        const uint8_t tag = c->buf[at++];
        if (tag == SCHED_REC_END) return decode_end(c->buf, at, c->len, rep);
        if (tag != SCHED_REC_CHOICE || at + 2 > c->len) return -1;
        uint16_t n; memcpy(&n, c->buf + at, 2); at += 2;
        if (at + 2u * n > c->len) return -1;
        for (int k = 1; k < n; k++) {
            const int dp = c->buf[at + 2 * k], dd = c->buf[at + 2 * k + 1];
            const int p = c->pre.p + dp, d = c->pre.d + dd;
            if (p > bd->max_p || d > bd->max_d) { r->pruned++; continue; }
            ps_push(p <= bd->p && d <= bd->d ? now : later, prefix_extend(&c->pre, pos, k, dp, dd));
        }
        at += 2u * n;
        pos++;
    }
    return 0;
}

/** @brief Rename a failing child's trace file to the configured path, or delete a later one.
 * @param cfg The configuration.
 * @param pid The child.
 * @param keep Nonzero for the first failure. */
static void collect_trace(const sched_config *cfg, pid_t pid, int keep)
{
    if (!cfg->trace_path) return;
    char part[1024];
    snprintf(part, sizeof part, "%s.%ld.part", cfg->trace_path, (long)pid);
    if (keep) rename(part, cfg->trace_path);
    else unlink(part);
}

/** @brief Account a finished child's end record in the result.
 * @param cfg The configuration.
 * @param c The child.
 * @param rep Its end record.
 * @param bd The pair under exploration.
 * @param r The result. */
static void account(const sched_config *cfg, const child *c, const child_report *rep,
                    const bounds *bd, sched_result *r)
{
    r->schedules++;
    r->steps_total += rep->steps;
    if (rep->steps > r->steps_max) r->steps_max = rep->steps;
    if (rep->nchoices > r->choices_max) r->choices_max = rep->nchoices;
    if (rep->status == SCHED_CHILD_PASS) return;
    r->failures++;
    const int first = !r->found;
    collect_trace(cfg, c->pid, first);
    if (!first) return;
    r->found = 1;
    r->found_p = bd->p; r->found_d = bd->d;
    snprintf(r->choices, sizeof r->choices, "%s", rep->choices);
    snprintf(r->message, sizeof r->message, "%s",
             rep->status < 0 ? "child ended without a report (crash or abort)" : rep->msg);
}

/** @brief Report whether exploration must stop: a failure under stop_on_fail, or the schedule cap.
 * @param cfg The configuration.
 * @param r The result so far.
 * @return 1 to stop. */
static int must_stop(const sched_config *cfg, const sched_result *r)
{
    if (cfg->stop_on_fail && r->found) return 1;
    return cfg->max_schedules > 0 && r->schedules >= cfg->max_schedules;
}

/** A fork server and the schedule it is running. */
typedef struct worker {
    pid_t pid;
    int cmd_fd, rep_fd;
    int busy;
    child c;              /* the schedule in flight: its prefix, and its report once framed */
    uint8_t *raw; size_t rlen, rcap;
} worker;

/** @brief Read exactly n bytes, retrying short reads.
 * @param fd Descriptor.
 * @param buf Destination.
 * @param n Length.
 * @return 0, or -1 at end of file or on error. */
static int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        const ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/** @brief Body of a fork server: for each prefix received, run it in a child (child_spawn), collect its report, and send the report framed with the child's pid and wait status.
 * @param cfg The configuration.
 * @param cmd_fd Prefixes arrive here (int32 length, then the choices; -1 ends the server).
 * @param rep_fd Frames go here (int32 pid, int32 wait status, uint64 length, report bytes). */
static void worker_main(const sched_config *cfg, int cmd_fd, int rep_fd) __attribute__((noreturn));
static void worker_main(const sched_config *cfg, int cmd_fd, int rep_fd)
{
    prefix pre = { calloc(SCHED_MAX_CHOICES, sizeof(uint16_t)), 0, 0, 0 };
    if (!pre.ch) _exit(2);
    for (;;) {
        int32_t n;
        if (read_all(cmd_fd, &n, 4) != 0 || n < 0 || n > SCHED_MAX_CHOICES) _exit(0);
        if (n && read_all(cmd_fd, pre.ch, (size_t)n * sizeof(uint16_t)) != 0) _exit(0);
        pre.len = n;
        child c;
        if (child_spawn(cfg, pre, 0, 0, 0, &c) != 0) _exit(2);
        int e;
        while ((e = child_read(&c)) == 0) {}
        close(c.fd);
        int ws = 0;
        while (waitpid(c.pid, &ws, 0) < 0 && errno == EINTR) {}
        const int32_t hdr[2] = { (int32_t)c.pid, e < 0 ? -1 : (int32_t)ws };
        const uint64_t len = c.len;
        if (sched_write_all(rep_fd, hdr, sizeof hdr) || sched_write_all(rep_fd, &len, 8) ||
            sched_write_all(rep_fd, c.buf, c.len))
            _exit(2);
        free(c.buf);
    }
}

/** @brief Start the fork servers.
 * @param cfg The configuration.
 * @param w The servers.
 * @param n How many.
 * @return 0, or -1 when a fork or pipe failed. */
static int workers_start(const sched_config *cfg, worker *w, int n)
{
    for (int i = 0; i < n; i++) {
        int cmd[2], rep[2];
        if (pipe(cmd) != 0) return -1;
        if (pipe(rep) != 0) return -1;
        fflush(NULL);
        const pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            close(cmd[1]); close(rep[0]);
            for (int k = 0; k < i; k++) { close(w[k].cmd_fd); close(w[k].rep_fd); }
            worker_main(cfg, cmd[0], rep[1]);
        }
        close(cmd[0]); close(rep[1]);
        memset(&w[i], 0, sizeof w[i]);
        w[i].pid = pid; w[i].cmd_fd = cmd[1]; w[i].rep_fd = rep[0];
    }
    return 0;
}

/** @brief End the fork servers and reap them.
 * @param w The servers.
 * @param n How many. */
static void workers_stop(worker *w, int n)
{
    for (int i = 0; i < n; i++) {
        const int32_t q = -1;
        (void)sched_write_all(w[i].cmd_fd, &q, 4);
        close(w[i].cmd_fd);
    }
    for (int i = 0; i < n; i++) {
        int ws;
        while (waitpid(w[i].pid, &ws, 0) < 0 && errno == EINTR) {}
        close(w[i].rep_fd);
        free(w[i].raw);
    }
}

/** @brief Send a prefix to an idle fork server.
 * @param w The server.
 * @param pre The prefix (ownership moves to the server's record).
 * @return 0, or -1 on a write error. */
static int worker_send(worker *w, prefix pre)
{
    const int32_t n = pre.len;
    if (sched_write_all(w->cmd_fd, &n, 4) || (n && sched_write_all(w->cmd_fd, pre.ch, (size_t)n * sizeof(uint16_t))))
        return -1;
    memset(&w->c, 0, sizeof w->c);
    w->c.pre = pre;
    w->busy = 1;
    w->rlen = 0;
    return 0;
}

/** @brief Read what a busy server sent; when a whole frame is in, decode it into the stacks and the result.
 * @param cfg The configuration.
 * @param w The server.
 * @param bd Bounds.
 * @param now Current-pair stack.
 * @param later Deferred stack.
 * @param r Result.
 * @return 0, or -1 on an explorer error. */
static int worker_read(const sched_config *cfg, worker *w, const bounds *bd, pstack *now, pstack *later, sched_result *r)
{
    if (w->rcap - w->rlen < 65536) {
        const size_t cap = w->rcap ? w->rcap * 2 : 131072;
        uint8_t *raw = realloc(w->raw, cap);
        if (!raw) return -1;
        w->raw = raw;
        w->rcap = cap;
    }
    const ssize_t got = read(w->rep_fd, w->raw + w->rlen, w->rcap - w->rlen);
    if (got < 0) return errno == EINTR ? 0 : -1;
    if (got == 0) return -1;
    w->rlen += (size_t)got;
    if (w->rlen < 16) return 0;
    int32_t hdr[2]; uint64_t len;
    memcpy(hdr, w->raw, 8); memcpy(&len, w->raw + 8, 8);
    if (w->rlen < 16 + len) return 0;
    child *c = &w->c;
    c->pid = hdr[0];
    c->buf = w->raw + 16; c->len = (size_t)len;
    child_report rep;
    memset(&rep, 0, sizeof rep);
    if (child_expand(c, bd, now, later, r, &rep) != 0) rep.status = -1;
    const int ws = hdr[1];
    if (rep.status == SCHED_CHILD_PASS && !(WIFEXITED(ws) && WEXITSTATUS(ws) == 0)) rep.status = -1;
    account(cfg, c, &rep, bd, r);
    free(c->pre.ch);
    c->buf = NULL;
    w->busy = 0;
    w->rlen = 0;
    return 0;
}

/** @brief Give every idle fork server a prefix from the current-pair stack, unless exploration is stopping.
 * @param w The servers.
 * @param nw How many.
 * @param now Current-pair stack.
 * @param stopped Nonzero once no new schedule may start.
 * @return The servers busy afterwards, or -1 on a write error. */
static int servers_feed(worker *w, int nw, pstack *now, int stopped)
{
    int busy = 0;
    for (int i = 0; i < nw; i++) {
        if (!w[i].busy && !stopped && now->n > 0 && worker_send(&w[i], now->v[--now->n]) != 0) return -1;
        busy += w[i].busy;
    }
    return busy;
}

/** @brief Wait until a busy server has sent data, and read it from every server that has.
 * @param cfg The configuration.
 * @param w The servers.
 * @param nw How many.
 * @param bd Bounds.
 * @param now Current-pair stack.
 * @param later Deferred stack.
 * @param r Result.
 * @return 0, or -1 on an explorer error. */
static int servers_collect(const sched_config *cfg, worker *w, int nw, const bounds *bd, pstack *now, pstack *later,
                           sched_result *r)
{
    struct pollfd pf[64];
    int np = 0, idx[64];
    for (int i = 0; i < nw; i++)
        if (w[i].busy) { pf[np] = (struct pollfd){ w[i].rep_fd, POLLIN, 0 }; idx[np++] = i; }
    if (poll(pf, (nfds_t)np, -1) < 0) return errno == EINTR ? 0 : -1;
    for (int k = 0; k < np; k++)
        if (pf[k].revents && worker_read(cfg, &w[idx[k]], bd, now, later, r) != 0) return -1;
    return 0;
}

/** @brief Run every prefix on the current-pair stack (and the prefixes they spawn) to completion on the fork servers.
 * @param cfg The configuration.
 * @param w The servers.
 * @param nw How many.
 * @param bd Bounds.
 * @param now Current-pair stack.
 * @param later Deferred stack.
 * @param r Result.
 * @return 0, 1 when stopped early, -1 on an explorer error. */
static int run_pair_servers(const sched_config *cfg, worker *w, int nw, const bounds *bd, pstack *now, pstack *later,
                            sched_result *r)
{
    for (;;) {
        const int busy = servers_feed(w, nw, now, must_stop(cfg, r));
        if (busy < 0) return -1;
        if (busy == 0) return must_stop(cfg, r);
        if (servers_collect(cfg, w, nw, bd, now, later, r) != 0) return -1;
    }
}

/** @brief Move every deferred prefix the pair covers onto the current-pair stack.
 * @param bd The pair.
 * @param now Current-pair stack.
 * @param later Deferred stack. */
static void undefer(const bounds *bd, pstack *now, pstack *later)
{
    long keep = 0;
    for (long i = 0; i < later->n; i++) {
        if (later->v[i].p <= bd->p && later->v[i].d <= bd->d) ps_push(now, later->v[i]);
        else later->v[keep++] = later->v[i];
    }
    later->n = keep;
}

/** @brief DFS over the joint bound: pairs in order of p + d, then p.
 * @param cfg The configuration.
 * @param r The result.
 * @return 0, or -1 on an explorer error. */
static int explore_dfs(const sched_config *cfg, sched_result *r)
{
    pstack now = {0}, later = {0};
    ps_push(&now, (prefix){ NULL, 0, 0, 0 });
    const int mp = cfg->max_preemptions, md = cfg->max_deviations;
    const int nw = cfg->jobs > 1 ? (cfg->jobs < 64 ? cfg->jobs : 64) : 1;
    worker w[64];
    if (workers_start(cfg, w, nw) != 0) return -1;
    r->reached_p = r->reached_d = -1;
    int e = 0;
    for (int sum = 0; sum <= mp + md && e == 0; sum++) {
        for (int p = 0; p <= sum && p <= mp; p++) {
            const int d = sum - p;
            if (d > md) continue;
            const bounds bd = { p, d, mp, md };
            undefer(&bd, &now, &later);
            e = run_pair_servers(cfg, w, nw, &bd, &now, &later, r);
            if (e != 0) break;
            r->reached_p = p; r->reached_d = d;
        }
    }
    workers_stop(w, nw);
    if (e < 0) return -1;
    if (e == 0) r->complete = later.n == 0;
    free(now.v); free(later.v);
    return 0;
}

/** @brief Run one child to completion and decode its end record (PCT and replay).
 * @param cfg The configuration.
 * @param pre Its prefix.
 * @param pct Nonzero for PCT.
 * @param seed PCT seed.
 * @param trace_always Write the trace on a pass too.
 * @param r Result.
 * @return 0, or -1 on an explorer error. */
static int run_single(const sched_config *cfg, prefix pre, int pct, uint64_t seed, int trace_always, sched_result *r)
{
    child c;
    if (child_spawn(cfg, pre, pct, seed, trace_always, &c) != 0) return -1;
    int e;
    while ((e = child_read(&c)) == 0) {}
    close(c.fd);
    int ws = 0;
    waitpid(c.pid, &ws, 0);
    if (e < 0) return -1;
    child_report rep;
    memset(&rep, 0, sizeof rep);
    rep.status = -1;
    size_t at = 0;
    while (at < c.len) {
        const uint8_t tag = c.buf[at++];
        if (tag == SCHED_REC_END) { if (decode_end(c.buf, at, c.len, &rep) != 0) rep.status = -1; break; }
        if (tag != SCHED_REC_CHOICE || at + 2 > c.len) return -1;
        uint16_t n; memcpy(&n, c.buf + at, 2);
        at += 2 + 2u * n;   /* a replay reports the choices past its string; nothing to expand */
    }
    if (trace_always && rep.status == SCHED_CHILD_PASS && cfg->trace_path) collect_trace(cfg, c.pid, 1);
    const bounds bd = { 0, 0, 0, 0 };
    account(cfg, &c, &rep, &bd, r);
    free(c.buf);
    return 0;
}

/** @brief PCT sampling: pct_runs schedules, run i seeded from seed and i.
 * @param cfg The configuration.
 * @param r The result.
 * @return 0, or -1 on an explorer error. */
static int explore_pct(const sched_config *cfg, sched_result *r)
{
    uint64_t st = cfg->seed;
    for (long i = 0; i < cfg->pct_runs; i++) {
        st += 0x9e3779b97f4a7c15ull;
        uint64_t z = st;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        if (run_single(cfg, (prefix){ NULL, 0, 0, 0 }, 1, z ^ (z >> 31), 0, r) != 0) return -1;
        if (must_stop(cfg, r)) break;
    }
    r->complete = 0;
    return 0;
}

int sched_explore(const sched_config *cfg, sched_result *out)
{
    memset(out, 0, sizeof *out);
    if (cfg->trace_path) unlink(cfg->trace_path);
    int e = -1;
    if (cfg->mode == SCHED_MODE_DFS) e = explore_dfs(cfg, out);
    else if (cfg->mode == SCHED_MODE_PCT) e = explore_pct(cfg, out);
    else {
        prefix pre;
        if (prefix_parse(cfg->replay, &pre) != 0) { fprintf(stderr, "sched: malformed choice string\n"); return 2; }
        e = run_single(cfg, pre, 0, 0, 1, out);
        free(pre.ch);
    }
    if (e < 0) { perror("sched: explorer"); return 2; }
    return out->found ? 1 : 0;
}

void sched_print_result(const char *label, const sched_result *r)
{
    printf("sched: %s: schedules=%ld failures=%ld pruned=%ld steps/schedule max=%ld mean=%.1f "
           "choices max=%ld bound reached=(%d,%d)%s",
           label, r->schedules, r->failures, r->pruned, r->steps_max,
           r->schedules ? (double)r->steps_total / (double)r->schedules : 0.0, r->choices_max,
           r->reached_p, r->reached_d, r->complete ? " complete" : "");
    if (r->found)
        printf(" FOUND at (%d,%d): %s; choices %s", r->found_p, r->found_d, r->message, r->choices);
    printf("\n");
    fflush(stdout);
}
