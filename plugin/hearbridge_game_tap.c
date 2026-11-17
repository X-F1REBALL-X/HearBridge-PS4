/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hearbridge_game_tap - GoldHEN game plugin: tee game AudioOut PCM to the
 * HearBridge PS4 service (UDP 127.0.0.1:9500, 48 kHz s16le stereo).
 *
 * Layout follows the public GoldHEN game plugins (plugin info globals,
 * plugin_load / plugin_unload, empty module_start / module_stop, x32 detours).
 *
 * Audio thread rules:
 *  - The original AudioOut call always runs FIRST; its return value is
 *    returned unchanged. The hook never alters blocking / error semantics.
 *  - After the original returns: O(1) bookkeeping, optional convert + ring
 *    push. No sendto, no file I/O, no locks, no sleep on the audio thread.
 *  - Rate-limit accepts to ~48 kHz + burst so a spinning Output loop cannot
 *    burn the CPU in convert(). A call whose original returned an error did
 *    not queue its buffer and is not copied once the port has returned
 *    success; a port that never returns success (after 1000 calls and 2 s)
 *    is not tapped. Rules in tap_rate.h, diagnostics in tap_diag.h, sample
 *    conversion in tap_convert.h; host tests cover them (make test).
 *  - Latest MAIN port becomes master immediately; older MAIN ports are
 *    untapped (spin on them is ignored). When the newest MAIN port closes
 *    (video, cut scene) the newest older MAIN port that is still open is
 *    tapped again. BGM still mixes in the worker.
 *  - Worker thread sleeps, drains rings, and does all UDP sendto + logging.
 *
 * Load-time rules (plugin_load runs very early, from libc _init_env):
 *  - The PRX imports only libkernel, libSceLibcInternal and libSceSysmodule,
 *    like GoldHEN's plugin_template. AudioOut and Net functions are resolved
 *    at runtime with sceKernelDlsym, so a missing library can never stop the
 *    game from starting.
 *  - plugin_load writes one log line, resolves the AudioOut functions and
 *    installs the hooks. Nothing else.
 *  - The worker thread is started from the first sceAudioOutOpen call (the
 *    game is initialized by then). It loads libSceNet through sysmodule if
 *    needed and creates the non-blocking socket.
 *  - Some games load libSceAudioOut after plugin_load. Then plugin_load
 *    hooks the module-load entry points (libkernel sceKernelLoadStartModule,
 *    libSceSysmodule sceSysmoduleLoadModule and
 *    sceSysmoduleLoadModuleInternal). Each hook calls the original first,
 *    then installs the AudioOut hooks once the audio module is present. A
 *    poll thread (500 ms, 60 s max, lowest priority) is the safety net.
 *    Install is once-only (atomic state).
 *  - Module lookup by name is normalized (basename, case-insensitive,
 *    .sprx/.prx stripped) and falls back to sceKernelDlsym on every loaded
 *    handle. Module enumeration uses the dynlib syscalls directly (592
 *    get_list, 593 get_info, 591 dlsym) and falls back to the libkernel
 *    wrappers, because in some game processes sceKernelGetModuleList only
 *    reports app modules.
 *
 * Which titles and when:
 *  - Runs only in retail game titles (CUSA/PCAS/PCJS/PLJS/PLJM/PLAS/PCKS/
 *    PLKS prefixes), never in NPXS system apps or homebrew IDs; honors
 *    /data/GoldHEN/hearbridge_skip.txt. Skipped titles return from
 *    plugin_load at once with no hooks, threads or files.
 *  - /data/GoldHEN/hearbridge_tap_off ("Game audio" switch) makes the tap
 *    pass nothing while the file exists; hooks stay installed and the worker
 *    re-checks it every 2 s, so the switch works live.
 *  - Tapping is gated on the daemon state file (/data/hearbridge/state ==
 *    "streaming"), re-read by the worker every 2 s.
 *  - Heartbeat "HBTAP <TITLE> <pkts>" to the daemon every 2 s (shown as the
 *    game in the HearBridge page), "<TITLE> <pkts>" in
 *    /data/GoldHEN/hearbridge/tap_status every 5 s, and the UDP port is read
 *    from /data/hearbridge/pcm_port.
 *  - Logs go to /data/GoldHEN/hearbridge/<TITLEID>.log (truncated per
 *    launch, capped at 64 KB), or /data/GoldHEN/hearbridge_game_tap.log when
 *    that directory does not exist (the plugin never creates directories).
 *
 * Mixing: only MAIN and BGM ports are tapped. The latest MAIN is master.
 * Every tapped port pushes into its own ring (rate-limited). The worker
 * pulls the master ring, mixes BGM, and sends.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>

#include <Common.h>
#include "hb_plugin_common.h"
#include <orbis/Sysmodule.h>
#include <Syscall.h>
#include <orbis/_types/net.h>
#include <orbis/_types/audio_out.h>
#include <orbis/_types/pad.h>
#include <HDE64.h>


#define PLUGIN_NAME "hearbridge_game_tap"
#define PLUGIN_VER  0x00000100u /* 1.0.0 */

#define PCM_PORT       9500
#include "tap_rate.h"
#include "tap_convert.h"
#include "tap_diag.h"

#define MAX_PORTS      16
#define MAX_FRAMES     2048      /* AudioOut len is 256..2048 */
#define PKT_FRAMES     1024      /* 4096 bytes, daemon MAX_PKT */
#define RING_FRAMES    4096      /* power of two, about 85 ms */
#define NET_RETRY_US   5000000u
#define MSG_DONTWAIT_ORBIS 0x80

/* /data/GoldHEN is proven writable from game processes (plugin_loader
 * writes plugins.ini there with sceKernelOpen). */
#define TAP_LOG_FALLBACK "/data/GoldHEN/hearbridge_game_tap.log"
#define TAP_LOG_DIR "/data/GoldHEN/hearbridge/"
#define LOG_MAX_BYTES (64 * 1024)
#define SKIP_FILE "/data/GoldHEN/hearbridge_skip.txt"
#define DAEMON_STATE "/data/hearbridge/state"
#define DAEMON_STATE_ALT "/user/data/hearbridge/state"
#define DAEMON_CHECK_US 2000000ull
#define PCM_PORT_FILE "/data/hearbridge/pcm_port"
#define TAP_STATUS_FILE "/data/GoldHEN/hearbridge/tap_status"
static int g_pcm_port = PCM_PORT;    /* worker only */
static char g_log_path[96] = TAP_LOG_FALLBACK;
static int g_daemon_on = 1;          /* atomic; worker refreshes from the state file */
#define TAP_OFF_FLAG "/data/GoldHEN/hearbridge_tap_off"

#define SYS_LIB_DIR "/system/common/lib/"

/* sceKernelOpen flags (FreeBSD values). */
#define K_O_WRONLY 0x0001
#define K_O_APPEND 0x0008
#define K_O_CREAT  0x0200
#define K_O_TRUNC  0x0400

/* sceAudioOutOpen port types (PS4 SDK values). */
#define PORT_MAIN     0
#define PORT_BGM      1
#define PORT_VOICE    2
#define PORT_PERSONAL 3
#define PORT_PADSPK   4
#define PORT_AUDIO3D  126
#define PORT_AUX      127

#ifndef SCE_NET_SOL_SOCKET
#define SCE_NET_SOL_SOCKET 0xffff
#endif
#ifndef SCE_NET_SO_NBIO
#define SCE_NET_SO_NBIO 0x1200
#endif

attr_public const char *g_pluginName = PLUGIN_NAME;
attr_public const char *g_pluginDesc = "Tee game AudioOut (MAIN/BGM) to HearBridge UDP 127.0.0.1:9500";
attr_public const char *g_pluginAuth = "HearBridge";
attr_public uint32_t g_pluginVersion = PLUGIN_VER;

enum { SLOT_FREE = 0, SLOT_CLAIM = 1, SLOT_OPEN = 2 };

struct port_slot {
    int state;               /* atomic: SLOT_* */
    int32_t handle;
    int32_t type;
    uint32_t frames;
    uint32_t freq;
    uint32_t param;
    uint32_t fmt;
    uint32_t channels;
    volatile int tapped;     /* MAIN/BGM, known format, 48 kHz, and currently tapped */
    volatile int eligible;   /* could be tapped (same test as at open); a MAIN that was
                              * untapped for a newer MAIN comes back when that one closes */
    uint32_t open_seq;       /* order of opens, newest = highest */
    int open_logged;         /* atomic: 0 pending, 1 written */
    int hit;                 /* atomic: 0 none, 1 pending log, 2 logged */
    uint64_t last_us;        /* atomic */
    uint32_t ring_wr;        /* atomic, producer = this port */
    uint32_t ring_rd;        /* atomic, consumer = worker */
    /* Rate limit / diagnostics (audio thread writes, worker reads). */
    struct hb_tapgate gate;  /* rate limit and error handling (tap_rate.h) */
    uint64_t err_calls;      /* atomic: original returned an error, buffer not copied */
    uint64_t ok_calls;       /* atomic: original returned success */
    uint64_t out_calls;      /* atomic */
    uint64_t out_accepts;    /* atomic: converted + pushed */
    uint64_t rate_drops;     /* atomic */
    int32_t last_rc;         /* atomic: last original Output return */
    struct hb_portdiag diag; /* return codes, buffer checksum and peak of failing calls */
    int16_t conv[MAX_FRAMES * 2];
    int16_t ring[RING_FRAMES * 2];
};

struct sockaddr_in_orbis {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    OrbisNetInAddr sin_addr;
    char sin_zero[8];
};

static struct port_slot g_ports[MAX_PORTS];
static int g_master = -1;            /* atomic slot index */
static uint32_t g_open_seq;          /* atomic, counts port opens */

typedef int (*net_socket_fn)(const char *, int, int, int);
typedef int (*net_setsockopt_fn)(int, int, int, const void *, uint32_t);
typedef int (*net_sendto_fn)(int, const void *, size_t, int, const void *, uint32_t);
typedef int (*net_close_fn)(int);

static net_socket_fn p_sceNetSocket;
static net_setsockopt_fn p_sceNetSetsockopt;
static net_sendto_fn p_sceNetSendto;
static net_close_fn p_sceNetSocketClose;
typedef int32_t (*hb_pcm_feed_fn)(const void *, size_t);
static hb_pcm_feed_fn p_hb_pcm_feed;
static uint8_t g_feed_neg[1024];
static uint64_t g_feed_last_try;

static int g_sock = -1;
static int g_sock_state;             /* atomic: 0 none, 2 ok, 3 failed (worker retries) */
static int32_t g_sock_err;
static char g_sock_why[64];
static struct sockaddr_in_orbis g_dst;

/* Counters (atomic, read by the logger thread). */
static uint64_t g_pkts_sent;
static uint64_t g_frames_sent;
static uint64_t g_pkts_dropped;
static uint64_t g_ring_drops;
static uint64_t g_unknown_calls;
static uint64_t g_skipped_calls;
static uint64_t g_rate_drops;
static int32_t g_last_send_err;
static uint64_t g_wakes;             /* worker loop iterations */
static uint64_t g_short_sleeps;      /* usleep(5000) returned in < 1 ms */
static uint64_t g_tap_us;            /* total hook work time (after original) */
static uint64_t g_tap_n;
static uint32_t g_tap_max_us;
static int g_tap_off;                /* atomic: "Game audio" switch off (flag file) */

static char g_titleid[16];
static int g_hooked;                 /* atomic: 0 no, 1 installing, 2 installed, -1 failed for good */
static volatile int g_logger_run;
static OrbisPthread g_logger;
static int g_logger_started;         /* atomic: 0 no, 1 starting/started, -1 failed */
static int32_t g_logger_err;

HOOK_INIT(sceAudioOutOpen);
HOOK_INIT(sceAudioOutClose);
HOOK_INIT(sceAudioOutOutput);
HOOK_INIT(sceAudioOutOutputs);

/* Which detours were installed (for unload). */
static int g_det_ok[4];

/* Module-load hooks (deferred install). */
HOOK_INIT(sceKernelLoadStartModule);
HOOK_INIT(sceSysmoduleLoadModule);
HOOK_INIT(sceSysmoduleLoadModuleInternal);
static int g_ldet_ok[3];
static int g_deferred;               /* 1 when load hooks / poller are armed */
static int g_poll_started;           /* atomic: 0 no, 1 started, -1 failed */
static volatile int g_poll_run;
static OrbisPthread g_poll;
static uint32_t g_load_calls;        /* atomic */
static uint8_t g_audio_neg[1024];    /* handles known not to export sceAudioOutOpen */
static char g_audio_from[160];
static int32_t g_audio_hint = -1;    /* AudioOut handle obtained by loading */

#define A_LOAD(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define A_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define A_ADD(p, v)   __atomic_fetch_add((p), (v), __ATOMIC_RELAXED)

/* ------------------------------------------------------------------ */
/* Format helpers                                                       */

static uint32_t fmt_channels(uint32_t fmt)
{
    switch (fmt) {
    case FMT_S16_MONO:
    case FMT_FLOAT_MONO:
        return 1;
    case FMT_S16_STEREO:
    case FMT_FLOAT_STEREO:
        return 2;
    case FMT_S16_8CH:
    case FMT_FLOAT_8CH:
    case FMT_S16_8CH_STD:
    case FMT_FLOAT_8CH_STD:
        return 8;
    default:
        return 0;
    }
}

static const char *fmt_name(uint32_t fmt)
{
    switch (fmt) {
    case FMT_S16_MONO: return "s16_mono";
    case FMT_S16_STEREO: return "s16_stereo";
    case FMT_S16_8CH: return "s16_8ch";
    case FMT_FLOAT_MONO: return "float_mono";
    case FMT_FLOAT_STEREO: return "float_stereo";
    case FMT_FLOAT_8CH: return "float_8ch";
    case FMT_S16_8CH_STD: return "s16_8ch_std";
    case FMT_FLOAT_8CH_STD: return "float_8ch_std";
    default: return "unknown";
    }
}

static const char *type_name(int32_t type)
{
    switch (type) {
    case PORT_MAIN: return "main";
    case PORT_BGM: return "bgm";
    case PORT_VOICE: return "voice";
    case PORT_PERSONAL: return "personal";
    case PORT_PADSPK: return "padspk";
    case PORT_AUDIO3D: return "audio3d";
    case PORT_AUX: return "aux";
    default: return "other";
    }
}

/* Convert one port buffer into dst as s16 stereo. Returns frames written. */
static uint32_t convert(const struct port_slot *p, const void *src, int16_t *dst)
{
    uint32_t n = p->frames;

    if (n == 0 || n > MAX_FRAMES)
        n = (n == 0) ? 256 : MAX_FRAMES;
    return hb_convert(p->fmt, n, src, dst);
}

/* ------------------------------------------------------------------ */
/* Port table                                                           */

static int slot_find(int32_t handle)
{
    int i;
    for (i = 0; i < MAX_PORTS; i++)
        if (A_LOAD(&g_ports[i].state) == SLOT_OPEN && g_ports[i].handle == handle)
            return i;
    return -1;
}

static int slot_claim(void)
{
    int i;
    for (i = 0; i < MAX_PORTS; i++) {
        int expect = SLOT_FREE;
        if (__atomic_compare_exchange_n(&g_ports[i].state, &expect, SLOT_CLAIM,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Network (worker thread only)                                         */

static void send_pcm(const int16_t *pcm, uint32_t frames)
{
    uint32_t off = 0;

    /* Prefer in-process feed into hearbridge_daemon (same game process). */
    if (p_hb_pcm_feed) {
        while (off < frames) {
            uint32_t n = frames - off;
            int32_t rc;
            if (n > PKT_FRAMES)
                n = PKT_FRAMES;
            rc = p_hb_pcm_feed(pcm + off * 2, (size_t)n * 4u);
            if (rc < 0) {
                A_ADD(&g_pkts_dropped, 1);
                g_last_send_err = rc;
            } else {
                A_ADD(&g_pkts_sent, 1);
                A_ADD(&g_frames_sent, n);
            }
            off += n;
        }
        return;
    }

    if (A_LOAD(&g_sock_state) != 2 || !p_sceNetSendto)
        return;
    while (off < frames) {
        uint32_t n = frames - off;
        int rc;
        if (n > PKT_FRAMES)
            n = PKT_FRAMES;
        rc = p_sceNetSendto(g_sock, pcm + off * 2, (size_t)n * 4u, MSG_DONTWAIT_ORBIS,
                            &g_dst, sizeof g_dst);
        if (rc < 0) {
            A_ADD(&g_pkts_dropped, 1);
            g_last_send_err = rc;
        } else {
            A_ADD(&g_pkts_sent, 1);
            A_ADD(&g_frames_sent, n);
        }
        off += n;
    }
}

/* ------------------------------------------------------------------ */
/* Ring (SPSC per port: audio producer, worker consumer)                */

static void ring_push(struct port_slot *p, const int16_t *pcm, uint32_t n)
{
    uint32_t wr = __atomic_load_n(&p->ring_wr, __ATOMIC_RELAXED);
    uint32_t rd = A_LOAD(&p->ring_rd);
    uint32_t used = wr - rd;
    uint32_t i;

    if (used > RING_FRAMES) {
        A_STORE(&p->ring_rd, wr);
        used = 0;
    }
    if (RING_FRAMES - used < n) {
        A_ADD(&g_ring_drops, 1);
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t k = ((wr + i) & (RING_FRAMES - 1)) * 2;
        p->ring[k] = pcm[i * 2];
        p->ring[k + 1] = pcm[i * 2 + 1];
    }
    A_STORE(&p->ring_wr, wr + n);
}

/* Pull up to n frames into dst (zeros the rest). Returns frames pulled. */
static uint32_t ring_pull(struct port_slot *p, int16_t *dst, uint32_t n)
{
    uint32_t wr = A_LOAD(&p->ring_wr);
    uint32_t rd = __atomic_load_n(&p->ring_rd, __ATOMIC_RELAXED);
    uint32_t avail = wr - rd;
    uint32_t i;

    if (avail > RING_FRAMES) {
        A_STORE(&p->ring_rd, wr);
        memset(dst, 0, (size_t)n * 4u);
        return 0;
    }
    if (avail > n)
        avail = n;
    for (i = 0; i < avail; i++) {
        uint32_t k = ((rd + i) & (RING_FRAMES - 1)) * 2;
        dst[i * 2] = p->ring[k];
        dst[i * 2 + 1] = p->ring[k + 1];
    }
    for (; i < n; i++) {
        dst[i * 2] = 0;
        dst[i * 2 + 1] = 0;
    }
    A_STORE(&p->ring_rd, rd + avail);
    return avail;
}

static void ring_mix_into(struct port_slot *p, int16_t *dst, uint32_t n)
{
    uint32_t wr = A_LOAD(&p->ring_wr);
    uint32_t rd = __atomic_load_n(&p->ring_rd, __ATOMIC_RELAXED);
    uint32_t avail = wr - rd;
    uint32_t i;

    if (avail > RING_FRAMES) {
        A_STORE(&p->ring_rd, wr);
        return;
    }
    if (avail > n)
        avail = n;
    for (i = 0; i < avail; i++) {
        uint32_t k = ((rd + i) & (RING_FRAMES - 1)) * 2;
        dst[i * 2] = clamp16((int32_t)dst[i * 2] + p->ring[k]);
        dst[i * 2 + 1] = clamp16((int32_t)dst[i * 2 + 1] + p->ring[k + 1]);
    }
    A_STORE(&p->ring_rd, rd + avail);
}

/* ------------------------------------------------------------------ */
/* Tap core (audio thread, after original Output returns)               */

/* Newest MAIN wins immediately. Older MAIN ports are untapped so a spin
 * on a leftover menu port is ignored (O(1) counter only). */
static void claim_master_main(int idx)
{
    int i;

    A_STORE(&g_master, idx);
    for (i = 0; i < MAX_PORTS; i++) {
        struct port_slot *o = &g_ports[i];
        if (i == idx || A_LOAD(&o->state) != SLOT_OPEN)
            continue;
        if (o->type == PORT_MAIN && o->tapped)
            o->tapped = 0;
    }
}

/* The master MAIN port closed: tap the newest older MAIN port that is
 * still open, so the game audio does not vanish (a video or cut scene
 * opens its own MAIN port, the game keeps the earlier one). */
static void retap_previous_main(int closed_idx)
{
    int i, best = -1;
    uint32_t best_seq = 0;

    for (i = 0; i < MAX_PORTS; i++) {
        struct port_slot *o = &g_ports[i];
        if (i == closed_idx || A_LOAD(&o->state) != SLOT_OPEN)
            continue;
        if (o->type != PORT_MAIN || !o->eligible)
            continue;
        if (best < 0 || (int32_t)(o->open_seq - best_seq) > 0) {
            best = i;
            best_seq = o->open_seq;
        }
    }
    if (best < 0)
        return;
    {
        struct port_slot *b = &g_ports[best];
        /* Start clean: drop stale samples and the old rate budget. */
        A_STORE(&b->ring_rd, A_LOAD(&b->ring_wr));
        hb_gate_reset(&b->gate);
        b->tapped = 1;
    }
    A_STORE(&g_master, best);
}

static void tap_one(int32_t handle, const void *ptr, int32_t rc)
{
    int idx;
    struct port_slot *p;
    uint64_t now;
    uint32_t n;
    int gate;

    if (!ptr)
        return;
    idx = slot_find(handle);
    if (idx < 0) {
        A_ADD(&g_unknown_calls, 1);
        return;
    }
    p = &g_ports[idx];
    A_ADD(&p->out_calls, 1);
    A_STORE(&p->last_rc, rc);
    hb_diag_rc(&p->diag, rc);
    if (rc < 0 && hb_diag_due(p->diag.err_n + 1) && p->channels != 0 && p->frames != 0 &&
        p->frames <= MAX_FRAMES) {
        /* a failing call: now and then checksum and measure its buffer (no copy is made) */
        uint32_t dn = convert(p, ptr, p->conv);
        hb_diag_err(&p->diag, p->conv, dn * 2);
    } else if (rc < 0) {
        p->diag.err_n++;
    }
    if (A_LOAD(&p->hit) == 0)
        A_STORE(&p->hit, 1);
    if (!p->tapped || !A_LOAD(&g_daemon_on) || A_LOAD(&g_tap_off)) {
        A_ADD(&g_skipped_calls, 1);
        return;
    }

    if (rc < 0 && (p->gate.ok_seen || p->gate.dead)) {   /* failed call: nothing was queued (cheap path, no clock read) */
        A_ADD(&p->err_calls, 1);
        return;
    }

    now = sceKernelGetProcessTime();
    A_STORE(&p->last_us, now);

    n = p->frames;
    if (n == 0 || n > MAX_FRAMES)
        n = (n == 0) ? 256 : MAX_FRAMES;
    if (rc >= 0)
        A_ADD(&p->ok_calls, 1);
    gate = hb_gate_pass(&p->gate, rc, n, now, RING_FRAMES);
    if (gate != HB_GATE_PASS) {
        if (gate == HB_GATE_ERR_SKIP) {
            A_ADD(&p->err_calls, 1);
        } else {
            A_ADD(&g_rate_drops, 1);
            A_ADD(&p->rate_drops, 1);
        }
        return;
    }

    n = convert(p, ptr, p->conv);
    if (n == 0)
        return;
    ring_push(p, p->conv, n);
    A_ADD(&p->out_accepts, 1);
    {
        uint64_t d = sceKernelGetProcessTime() - now;
        A_ADD(&g_tap_us, d);
        A_ADD(&g_tap_n, 1);
        if (d > A_LOAD(&g_tap_max_us))
            A_STORE(&g_tap_max_us, (uint32_t)d);
    }
}

/* ------------------------------------------------------------------ */
/* Hooks                                                                */

static void logger_start(void);

static int32_t sceAudioOutOpen_hook(int32_t userId, int32_t type,
                                    int32_t index, uint32_t len, uint32_t freq,
                                    uint32_t param)
{
    int32_t h = HOOK_CONTINUE(sceAudioOutOpen,
                              int32_t (*)(int32_t, int32_t, int32_t,
                                          uint32_t, uint32_t, uint32_t),
                              userId, type, index, len, freq, param);
    if (h > 0) {
        int idx = slot_find(h);
        if (idx < 0)
            idx = slot_claim();
        if (idx >= 0) {
            struct port_slot *p = &g_ports[idx];
            uint32_t fmt = param & 0xffu;
            uint32_t ch = fmt_channels(fmt);
            A_STORE(&p->state, SLOT_CLAIM);
            p->handle = h;
            p->type = type;
            p->frames = len;
            p->freq = freq;
            p->param = param;
            p->fmt = fmt;
            p->channels = ch;
            p->eligible = (type == PORT_MAIN || type == PORT_BGM) && ch != 0 &&
                          freq == 48000 && len > 0 && len <= MAX_FRAMES;
            p->tapped = p->eligible;
            p->open_seq = __atomic_add_fetch(&g_open_seq, 1u, __ATOMIC_RELAXED);
            p->last_us = 0;
            p->ring_wr = 0;
            p->ring_rd = 0;
            memset(&p->gate, 0, sizeof p->gate);
            p->err_calls = 0;
            p->ok_calls = 0;
            p->out_calls = 0;
            p->out_accepts = 0;
            p->rate_drops = 0;
            p->last_rc = 0;
            memset(&p->diag, 0, sizeof p->diag);
            p->hit = 0;
            p->open_logged = 0;
            A_STORE(&p->state, SLOT_OPEN);
            if (p->tapped && type == PORT_MAIN)
                claim_master_main(idx);
        }
    }
    logger_start();
    return h;
}

static int32_t sceAudioOutClose_hook(int32_t handle)
{
    /* The game may open a new port with the same handle value right after the
     * real close returns: only release the slot we saw before the call. */
    int before = slot_find(handle);
    uint32_t seq = before >= 0 ? g_ports[before].open_seq : 0;
    int32_t rc = HOOK_CONTINUE(sceAudioOutClose, int32_t (*)(int32_t), handle);
    int idx = slot_find(handle);
    if (idx >= 0 && idx == before && g_ports[idx].open_seq == seq) {
        int m = idx;
        int was_main = g_ports[idx].type == PORT_MAIN && g_ports[idx].tapped;
        g_ports[idx].tapped = 0;
        g_ports[idx].eligible = 0;
        if (__atomic_compare_exchange_n(&g_master, &m, -1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) || was_main)
            retap_previous_main(idx);
        A_STORE(&g_ports[idx].state, SLOT_FREE);
    }
    return rc;
}

static int32_t sceAudioOutOutput_hook(int32_t handle, const void *ptr)
{
    int32_t rc = HOOK_CONTINUE(sceAudioOutOutput,
                               int32_t (*)(int32_t, const void *),
                               handle, ptr);
    tap_one(handle, ptr, rc);
    return rc;
}

static int32_t sceAudioOutOutputs_hook(OrbisAudioOutOutputParam *param, uint32_t num)
{
    int32_t rc = HOOK_CONTINUE(sceAudioOutOutputs,
                               int32_t (*)(OrbisAudioOutOutputParam *, uint32_t),
                               param, num);
    if (param && num > 0) {
        uint32_t i;
        for (i = 0; i < num; i++)
            tap_one(param[i].handle, param[i].pointer, rc);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Logger thread                                                        */

static void log_line(const char *s, int truncate)
{
    int fd;
    int flags = K_O_WRONLY | K_O_CREAT | (truncate ? K_O_TRUNC : K_O_APPEND);

    fd = sceKernelOpen(g_log_path, flags, 0666);
    if (fd < 0)
        return;
    if (truncate || sceKernelLseek(fd, 0, SEEK_END) < LOG_MAX_BYTES)
        (void)sceKernelWrite(fd, s, strlen(s));
    sceKernelClose(fd);
}

#define MOD_MAX        256
#define MOD_CACHE      1024   /* handles below this are cached as "checked" */
#define MOD_BRUTE      256    /* handle range probed if GetModuleList fails */

/* Normalized module name: basename, lowercase, without .sprx/.prx/.elf/.self. */
static void mod_norm(const char *in, char *out, size_t outlen)
{
    const char *b = in;
    const char *q;
    size_t n = 0;
    size_t i;

    for (q = in; *q; q++)
        if (*q == '/' || *q == '\\')
            b = q + 1;
    while (b[n] && n + 1 < outlen) {
        char c = b[n];
        out[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        n++;
    }
    out[n] = 0;
    {
        static const char *const ext[] = { ".sprx", ".prx", ".self", ".elf" };
        for (i = 0; i < sizeof ext / sizeof ext[0]; i++) {
            size_t el = strlen(ext[i]);
            if (n > el && strcmp(out + n - el, ext[i]) == 0) {
                out[n - el] = 0;
                break;
            }
        }
    }
}

static int mod_name_eq(const char *modname, const char *want)
{
    char a[64], b[64];
    mod_norm(modname, a, sizeof a);
    mod_norm(want, b, sizeof b);
    return strcmp(a, b) == 0;
}

/* Fills list; on GetModuleList failure returns rc and *count = 0. */
static int mod_list(OrbisKernelModule *list, size_t *count)
{
    int rc;
    *count = 0;
    /* sys_dynlib_get_list lists every handle incl. system sprx. */
    rc = orbis_syscall(592, list, (size_t)MOD_MAX, count);
    if (rc != 0 || *count == 0) {
        *count = 0;
        rc = sceKernelGetModuleList(list, MOD_MAX, count); /* size = entry count */
        if (rc != 0)
            *count = 0;
    }
    if (*count > MOD_MAX)
        *count = MOD_MAX;
    return rc;
}

static int mod_info_name(int32_t h, char *name, size_t namelen)
{
    OrbisKernelModuleInfo info;
    int rc;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    rc = sceKernelGetModuleInfo((OrbisKernelModule)h, &info);
    if (rc != 0) {
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        rc = orbis_syscall(593, (int)h, &info); /* sys_dynlib_get_info */
    }
    name[0] = 0;
    if (rc == 0) {
        strncpy(name, info.name, namelen - 1);
        name[namelen - 1] = 0;
    }
    return rc;
}

static int mod_dlsym(int32_t h, const char *sym, void **a)
{
    *a = NULL;
    if (sceKernelDlsym(h, sym, a) == 0 && *a)
        return 0;
    *a = NULL;
    if (sys_dynlib_dlsym((int)h, sym, a) == 0 && *a)
        return 0;
    *a = NULL;
    return -1;
}

/* Find a loaded module by (normalized) name, -1 if not loaded. */
static int32_t module_handle(const char *name)
{
    OrbisKernelModule list[MOD_MAX];
    size_t count = 0;
    size_t i;
    char nm[256];

    if (mod_list(list, &count) != 0)
        return -1;
    for (i = 0; i < count; i++)
        if (mod_info_name((int32_t)list[i], nm, sizeof nm) == 0 && mod_name_eq(nm, name))
            return (int32_t)list[i];
    return -1;
}

/* Find whichever loaded module exports sym (dlsym on every handle).
 * neg (optional) caches handles already known not to export it, so a
 * repeated search only touches new modules. Returns handle or -1. */
static int32_t find_export(const char *sym, void **addr, uint8_t *neg)
{
    OrbisKernelModule list[MOD_MAX];
    size_t count = 0;
    size_t i;
    int brute = 0;

    *addr = NULL;
    if (mod_list(list, &count) != 0 || count == 0) {
        brute = 1;
        count = MOD_BRUTE;
    }
    for (i = 0; i < count; i++) {
        int32_t h = brute ? (int32_t)i : (int32_t)list[i];
        void *a = NULL;
        if (neg && h >= 0 && h < MOD_CACHE && neg[h])
            continue;
        if (mod_dlsym(h, sym, &a) == 0) {
            *addr = a;
            return h;
        }
        if (neg && h >= 0 && h < MOD_CACHE && !brute)
            neg[h] = 1;
    }
    return -1;
}

/* Name first, then by export. */
static int32_t module_or_export(const char *const *names, const char *sym,
                                const char **how)
{
    void *a = NULL;
    int32_t h;
    for (; *names; names++) {
        h = module_handle(*names);
        if (h >= 0) {
            *how = *names;
            return h;
        }
    }
    h = find_export(sym, &a, NULL);
    if (h >= 0) {
        *how = "by export";
        return h;
    }
    return -1;
}

/* Handle of a system sprx by loading it (returns the existing handle when it
 * is already loaded). Logs every attempt. -1 if all attempts fail. */
static int32_t sys_lib_handle(const char *file, const char *sym)
{
    char path[96];
    char buf[200];
    void *a = NULL;
    int32_t h;
    int res = 0;
    int id = -1;
    int rc;

    h = (int32_t)sceKernelLoadStartModule(file, 0, NULL, 0, NULL, &res);
    snprintf(buf, sizeof buf, "  sceKernelLoadStartModule(%s) = 0x%08x\n", file, (unsigned)h);
    log_line(buf, 0);
    if (h >= 0 && mod_dlsym(h, sym, &a) == 0)
        return h;
    snprintf(path, sizeof path, SYS_LIB_DIR "%s", file);
    h = (int32_t)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
    snprintf(buf, sizeof buf, "  sceKernelLoadStartModule(%s) = 0x%08x\n", path, (unsigned)h);
    log_line(buf, 0);
    if (h >= 0 && mod_dlsym(h, sym, &a) == 0)
        return h;
    rc = sys_dynlib_load_prx(path, &id);
    snprintf(buf, sizeof buf, "  dynlib_load_prx(%s) rc=0x%08x id=0x%x\n", path, (unsigned)rc,
             (unsigned)id);
    log_line(buf, 0);
    if (rc == 0 && id >= 0 && mod_dlsym(id, sym, &a) == 0)
        return id;
    return -1;
}

/* Log every loaded module (handle, info rc, name). */
static void log_modules(const char *tag)
{
    OrbisKernelModule list[MOD_MAX];
    size_t count = 0;
    size_t i;
    char nm[256];
    char buf[384];
    int rc = mod_list(list, &count);

    snprintf(buf, sizeof buf, "[%llu ms] modules (%s): dynlib_get_list rc=0x%08x count=%u\n",
             (unsigned long long)(sceKernelGetProcessTime() / 1000), tag, (unsigned)rc,
             (unsigned)count);
    log_line(buf, 0);
    if (rc != 0 || count == 0) {
        /* Probe handles directly so we still see what is there. */
        for (i = 0; i < MOD_BRUTE; i++) {
            if (mod_info_name((int32_t)i, nm, sizeof nm) == 0) {
                snprintf(buf, sizeof buf, "  probe h=0x%x name=%s\n", (unsigned)i, nm);
                log_line(buf, 0);
            }
        }
        return;
    }
    for (i = 0; i < count; i++) {
        int irc = mod_info_name((int32_t)list[i], nm, sizeof nm);
        snprintf(buf, sizeof buf, "  h=0x%x info_rc=0x%08x name=%s\n", (unsigned)list[i],
                 (unsigned)irc, irc == 0 ? nm : "?");
        log_line(buf, 0);
    }
}

/* Worker thread only. Returns 1 when the socket is ready. */
static int net_setup(void)
{
    int32_t h;
    int nbio = 1;
    int sock;

    if (!p_sceNetSendto) {
        static const char *const k_net[] = { "libSceNet", NULL };
        const char *how = "";
        h = module_or_export(k_net, "sceNetSendto", &how);
        if (h < 0) {
            int32_t r = (int32_t)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
            h = module_or_export(k_net, "sceNetSendto", &how);
            if (h < 0) {
                g_sock_err = r;
                snprintf(g_sock_why, sizeof g_sock_why, "libSceNet not loaded (sysmodule 0x%08x)", (unsigned)r);
                return 0;
            }
        }
        if (sceKernelDlsym(h, "sceNetSocket", (void **)&p_sceNetSocket) != 0 ||
            sceKernelDlsym(h, "sceNetSetsockopt", (void **)&p_sceNetSetsockopt) != 0 ||
            sceKernelDlsym(h, "sceNetSocketClose", (void **)&p_sceNetSocketClose) != 0 ||
            sceKernelDlsym(h, "sceNetSendto", (void **)&p_sceNetSendto) != 0 ||
            !p_sceNetSocket || !p_sceNetSetsockopt || !p_sceNetSocketClose || !p_sceNetSendto) {
            p_sceNetSendto = NULL;
            g_sock_err = 0;
            snprintf(g_sock_why, sizeof g_sock_why, "sceNet symbols not found");
            return 0;
        }
    }

    sock = p_sceNetSocket("hb_game_tap", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_DGRAM, 0);
    if (sock < 0) {
        g_sock_err = sock;
        snprintf(g_sock_why, sizeof g_sock_why, "sceNetSocket 0x%08x", (unsigned)sock);
        return 0;
    }
    (void)p_sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nbio, sizeof nbio);
    memset(&g_dst, 0, sizeof g_dst);
    g_dst.sin_len = (uint8_t)sizeof g_dst;
    g_dst.sin_family = ORBIS_NET_AF_INET;
    g_dst.sin_port = __builtin_bswap16((uint16_t)g_pcm_port);
    g_dst.sin_addr.s_addr = __builtin_bswap32(0x7f000001u);
    g_sock = sock;
    A_STORE(&g_sock_state, 2);
    return 1;
}

static void log_ports(uint64_t now)
{
    char buf[256];
    int i;

    for (i = 0; i < MAX_PORTS; i++) {
        struct port_slot *p = &g_ports[i];
        int st = A_LOAD(&p->state);
        if (st == SLOT_FREE)
            continue;
        if (!A_LOAD(&p->open_logged) && st == SLOT_OPEN) {
            snprintf(buf, sizeof buf,
                     "[%llu ms] open handle=%d type=%s(%d) fmt=%s(0x%x) ch=%u len=%u freq=%u %s\n",
                     (unsigned long long)(now / 1000), (int)p->handle, type_name(p->type),
                     (int)p->type, fmt_name(p->fmt), p->param, p->channels, p->frames,
                     p->freq, p->tapped ? "tapped" : "ignored");
            log_line(buf, 0);
            A_STORE(&p->open_logged, 1);
        }
        if (A_LOAD(&p->hit) == 1) {
            snprintf(buf, sizeof buf,
                     "[%llu ms] first output handle=%d type=%s fmt=%s len=%u %s\n",
                     (unsigned long long)(now / 1000), (int)p->handle, type_name(p->type),
                     fmt_name(p->fmt), p->frames, p->tapped ? "tapped" : "ignored");
            log_line(buf, 0);
            A_STORE(&p->hit, 2);
        }
    }
}

/* Read a small file with kernel calls. Returns bytes read or -1. */
static int read_small_k(const char *path, char *out, int outlen)
{
    int fd = sceKernelOpen(path, 0 /* O_RDONLY */, 0);
    int n;
    if (fd < 0)
        return -1;
    n = (int)sceKernelRead(fd, out, (size_t)(outlen - 1));
    sceKernelClose(fd);
    if (n < 0)
        n = 0;
    out[n] = 0;
    return n;
}

/* 1 if the HearBridge daemon reports "streaming". */
static int daemon_streaming(void)
{
    char st[32];
    int n = read_small_k(DAEMON_STATE, st, sizeof st);
    if (n < 0)
        n = read_small_k(DAEMON_STATE_ALT, st, sizeof st);
    if (n <= 0)
        return 0;
    return strncmp(st, "streaming", 9) == 0;
}

#define WORKER_PRIO 740   /* PS4: 256 highest .. 767 lowest */

static void *logger_main(void *arg)
{
    char buf[384];
    uint64_t start = sceKernelGetProcessTime();
    uint64_t last_stats = start;
    uint64_t last_sum = 0;
    int last_sock = 0;
    (void)arg;

    uint64_t last_net_try = 0;
    uint64_t last_daemon_check = 0;
    uint64_t last_hello = 0, last_status = 0;

    (void)scePthreadSetprio(scePthreadSelf(), WORKER_PRIO);
    snprintf(buf, sizeof buf, "[%llu ms] worker started (prio %d)\n",
             (unsigned long long)(start / 1000), WORKER_PRIO);
    log_line(buf, 0);

    while (g_logger_run) {
        uint64_t now = sceKernelGetProcessTime();
        uint64_t elapsed = now - start;
        uint64_t every = (elapsed < 60000000ull) ? 10000000ull : 60000000ull;
        int sock;
        int m;
        int i;
        static int16_t mix[PKT_FRAMES * 2];
        uint32_t got;

        if (last_daemon_check == 0 || now - last_daemon_check >= DAEMON_CHECK_US) {
            int on = daemon_streaming();
            last_daemon_check = now;
            {
                int fd = sceKernelOpen(TAP_OFF_FLAG, 0 /* O_RDONLY */, 0);
                int off = fd >= 0;
                if (fd >= 0) sceKernelClose(fd);
                if (off != A_LOAD(&g_tap_off)) {
                    A_STORE(&g_tap_off, off);
                    snprintf(buf, sizeof buf, "[%llu ms] game audio switched %s\n",
                             (unsigned long long)(now / 1000), off ? "OFF" : "ON");
                    log_line(buf, 0);
                }
            }
            {
                char ps[16];
                int pp = PCM_PORT;
                if (read_small_k(PCM_PORT_FILE, ps, sizeof ps) > 0) {
                    pp = atoi(ps);
                    if (pp <= 0 || pp > 65535) pp = PCM_PORT;
                }
                if (pp != g_pcm_port) {
                    g_pcm_port = pp;
                    g_dst.sin_port = __builtin_bswap16((uint16_t)pp);
                    snprintf(buf, sizeof buf, "[%llu ms] daemon PCM port now %d (%s)\n",
                             (unsigned long long)(now / 1000), pp, PCM_PORT_FILE);
                    log_line(buf, 0);
                }
            }
            if (on != A_LOAD(&g_daemon_on)) {
                A_STORE(&g_daemon_on, on);
                snprintf(buf, sizeof buf, "[%llu ms] daemon %s: tap %s\n",
                         (unsigned long long)(now / 1000), on ? "streaming" : "not streaming",
                         on ? "active" : "paused");
                log_line(buf, 0);
            }
        }
        /* Resolve hb_pcm_feed from hearbridge_daemon once it is loaded. */
        if (A_LOAD(&g_daemon_on) && !p_hb_pcm_feed &&
            (g_feed_last_try == 0 || now - g_feed_last_try >= 2000000ull)) {
            void *a = NULL;
            int32_t h;
            g_feed_last_try = now;
            h = find_export("hb_pcm_feed", &a, g_feed_neg);
            if (h >= 0 && a) {
                p_hb_pcm_feed = (hb_pcm_feed_fn)a;
                snprintf(buf, sizeof buf, "[%llu ms] hb_pcm_feed resolved in module 0x%x\n",
                         (unsigned long long)(now / 1000), (unsigned)h);
                log_line(buf, 0);
            }
        }
        if (!A_LOAD(&g_daemon_on)) {
            /* Idle: nothing to send; check again in ~100 ms. */
            sceKernelUsleep(100000);
            continue;
        }
        if (A_LOAD(&g_sock_state) != 2 &&
            (last_net_try == 0 || now - last_net_try >= NET_RETRY_US)) {
            last_net_try = now;
            if (!net_setup())
                A_STORE(&g_sock_state, 3);
        }
        sock = A_LOAD(&g_sock_state);

        /* Heartbeat for the daemon (not PCM: starts with "HBTAP ", < 64 bytes). */
        if (sock == 2 && (last_hello == 0 || now - last_hello >= 2000000ull)) {
            char hb[48];
            int hn = snprintf(hb, sizeof hb, "HBTAP %s %llu", g_titleid,
                              (unsigned long long)A_LOAD(&g_pkts_sent));
            last_hello = now;
            if (hn > 0 && hn < (int)sizeof hb && (hn & 3) == 0)
                hb[hn++] = ' ';        /* never a multiple of 4 bytes, unlike PCM */
            (void)p_sceNetSendto(g_sock, hb, (size_t)hn, MSG_DONTWAIT_ORBIS, &g_dst, sizeof g_dst);
        }
        if (last_status == 0 || now - last_status >= 5000000ull) {
            char st[48];
            int fd, sn = snprintf(st, sizeof st, "%s %llu\n", g_titleid,
                                  (unsigned long long)A_LOAD(&g_pkts_sent));
            last_status = now;
            fd = sceKernelOpen(TAP_STATUS_FILE, K_O_WRONLY | K_O_CREAT | K_O_TRUNC, 0666);
            if (fd >= 0) {
                if (sn > 0) sceKernelWrite(fd, st, (size_t)sn);
                sceKernelClose(fd);
            }
        }

        /* Drain rings at real-time pace: one packet per wake when data exists. */
        m = A_LOAD(&g_master);
        if (m < 0 || A_LOAD(&g_ports[m].state) != SLOT_OPEN || !g_ports[m].tapped) {
            m = -1;
            for (i = 0; i < MAX_PORTS; i++) {
                struct port_slot *p = &g_ports[i];
                if (A_LOAD(&p->state) == SLOT_OPEN && p->tapped && p->type == PORT_MAIN) {
                    m = i;
                    A_STORE(&g_master, i);
                    break;
                }
            }
            if (m < 0) {
                for (i = 0; i < MAX_PORTS; i++) {
                    struct port_slot *p = &g_ports[i];
                    if (A_LOAD(&p->state) == SLOT_OPEN && p->tapped) {
                        m = i;
                        A_STORE(&g_master, i);
                        break;
                    }
                }
            }
        }
        if (m >= 0 && sock == 2) {
            got = ring_pull(&g_ports[m], mix, PKT_FRAMES);
            if (got > 0) {
                for (i = 0; i < MAX_PORTS; i++) {
                    struct port_slot *o = &g_ports[i];
                    if (i == m || A_LOAD(&o->state) != SLOT_OPEN || !o->tapped)
                        continue;
                    if (o->type == PORT_BGM)
                        ring_mix_into(o, mix, got);
                }
                send_pcm(mix, got);
            }
        }

        log_ports(now);

        if (sock != last_sock && (sock == 2 || sock == 3)) {
            if (sock == 2)
                snprintf(buf, sizeof buf, "[%llu ms] udp socket ready\n",
                         (unsigned long long)(now / 1000));
            else
                snprintf(buf, sizeof buf, "[%llu ms] udp socket failed: %s (retry every 5 s)\n",
                         (unsigned long long)(now / 1000), g_sock_why);
            log_line(buf, 0);
            last_sock = sock;
        }

        if (now - last_stats >= every) {
            uint64_t pk = A_LOAD(&g_pkts_sent);
            uint64_t dr = A_LOAD(&g_pkts_dropped);
            uint64_t rd = A_LOAD(&g_ring_drops);
            uint64_t uk = A_LOAD(&g_unknown_calls);
            uint64_t ig = A_LOAD(&g_skipped_calls);
            uint64_t rt = A_LOAD(&g_rate_drops);
            uint64_t sum = pk + dr + rd + uk + ig + rt;
            if (sum != last_sum) {
                m = A_LOAD(&g_master);
                snprintf(buf, sizeof buf,
                         "[%llu ms] pkts=%llu frames=%llu send_drops=%llu last_err=0x%08x "
                         "ring_drops=%llu rate_drops=%llu unknown=%llu ignored=%llu master=%d\n",
                         (unsigned long long)(now / 1000), (unsigned long long)pk,
                         (unsigned long long)A_LOAD(&g_frames_sent), (unsigned long long)dr,
                         (unsigned)g_last_send_err, (unsigned long long)rd,
                         (unsigned long long)rt, (unsigned long long)uk,
                         (unsigned long long)ig,
                         m >= 0 ? (int)g_ports[m].handle : -1);
                log_line(buf, 0);
                {
                    static uint64_t prev_wakes, prev_t;
                    uint64_t w = A_LOAD(&g_wakes);
                    uint64_t tn = A_LOAD(&g_tap_n);
                    uint64_t dt = now - prev_t;
                    snprintf(buf, sizeof buf,
                             "[%llu ms] worker wakes/s=%llu short_sleeps=%llu hook_avg_us=%llu "
                             "hook_max_us=%u\n",
                             (unsigned long long)(now / 1000),
                             (unsigned long long)(dt ? (w - prev_wakes) * 1000000ull / dt : 0),
                             (unsigned long long)A_LOAD(&g_short_sleeps),
                             (unsigned long long)(tn ? A_LOAD(&g_tap_us) / tn : 0),
                             (unsigned)A_LOAD(&g_tap_max_us));
                    log_line(buf, 0);
                    prev_wakes = w;
                    prev_t = now;
                }
                for (i = 0; i < MAX_PORTS; i++) {
                    struct port_slot *p = &g_ports[i];
                    uint64_t calls, acc, rdrop, okc, errc;
                    int32_t lrc;
                    if (A_LOAD(&p->state) != SLOT_OPEN)
                        continue;
                    calls = A_LOAD(&p->out_calls);
                    acc = A_LOAD(&p->out_accepts);
                    rdrop = A_LOAD(&p->rate_drops);
                    okc = A_LOAD(&p->ok_calls);
                    errc = A_LOAD(&p->err_calls);
                    lrc = A_LOAD(&p->last_rc);
                    if (calls == 0 && !p->tapped)
                        continue;
                    snprintf(buf, sizeof buf,
                             "[%llu ms] port handle=%d type=%s fmt=%s len=%u tapped=%d "
                             "calls=%llu ok=%llu err_skipped=%llu accepts=%llu rate_drops=%llu last_rc=0x%08x\n",
                             (unsigned long long)(now / 1000), (int)p->handle,
                             type_name(p->type), fmt_name(p->fmt), p->frames,
                             p->tapped, (unsigned long long)calls,
                             (unsigned long long)okc, (unsigned long long)errc,
                             (unsigned long long)acc, (unsigned long long)rdrop,
                             (unsigned)lrc);
                    log_line(buf, 0);
                    if (p->diag.nrc && (p->gate.dead || p->diag.err_n)) {
                        char *w = buf;
                        int room = (int)sizeof buf, k, nn;
                        nn = snprintf(w, (size_t)room, "[%llu ms] port handle=%d dead=%d rc:",
                                      (unsigned long long)(now / 1000), (int)p->handle, p->gate.dead);
                        for (k = 0; k < (int)p->diag.nrc && nn > 0 && nn < room; k++)
                            nn += snprintf(w + nn, (size_t)(room - nn), " 0x%08x=%llu",
                                           (unsigned)p->diag.rc[k], (unsigned long long)p->diag.rc_n[k]);
                        if (nn > 0 && nn < room)
                            nn += snprintf(w + nn, (size_t)(room - nn),
                                           " other=%llu failing_calls=%llu buf_samples=%u buf_changed=%u peak=%u hash=%08x\n",
                                           (unsigned long long)p->diag.rc_other,
                                           (unsigned long long)p->diag.err_n, p->diag.samples,
                                           p->diag.changes, p->diag.peak, p->diag.last_hash);
                        if (nn > 0 && nn < room)
                            log_line(buf, 0);
                    }
                }
                last_sum = sum;
            }
            last_stats = now;
        }
        /* ~5 ms: keep send paced near 48 kHz without busy-looping. If usleep
         * ever returns early (would be a busy spin), fall back to nanosleep. */
        A_ADD(&g_wakes, 1);
        {
            uint64_t t1 = sceKernelGetProcessTime();
            sceKernelUsleep(5000);
            if (sceKernelGetProcessTime() - t1 < 1000) {
                OrbisKernelTimespec ts = { 0, 5000000 };
                A_ADD(&g_short_sleeps, 1);
                sceKernelNanosleep(&ts, NULL);
            }
        }
    }
    log_line("unloaded\n", 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Plugin entry points                                                  */

static void logger_start(void)
{
    int expect = 0;
    int r;

    if (!__atomic_compare_exchange_n(&g_logger_started, &expect, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
    g_logger_run = 1;
    r = scePthreadCreate(&g_logger, NULL, logger_main, NULL, "hb_game_tap");
    if (r != 0) {
        g_logger_run = 0;
        g_logger_err = r;
        A_STORE(&g_logger_started, -1);
    }
}

/* Retail PS4 title ID prefixes. NPXS (system), homebrew IDs and anything
 * else is skipped. */
static int title_is_game(const char *t)
{
    static const char *const pre[] = { "CUSA", "PCAS", "PCJS", "PLJS", "PLJM", "PLAS",
                                       "PCKS", "PLKS", "PCES", "PLES", NULL };
    int i;
    if (!t || strlen(t) != 9)
        return 0;
    for (i = 0; pre[i]; i++)
        if (strncmp(t, pre[i], 4) == 0)
            break;
    if (!pre[i])
        return 0;
    for (i = 4; i < 9; i++)
        if (t[i] < '0' || t[i] > '9')
            return 0;
    return 1;
}

/* 1 if t is listed in SKIP_FILE (one ID per line, '#'/';' comments). */
static int title_in_skip_file(const char *t)
{
    static char buf[4096];
    char *p, *e;
    int n = read_small_k(SKIP_FILE, buf, sizeof buf);
    if (n <= 0)
        return 0;
    p = buf;
    while (*p) {
        size_t len;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        e = p;
        while (*e && *e != '\n' && *e != '\r' && *e != ' ' && *e != '\t' &&
               *e != '#' && *e != ';')
            e++;
        len = (size_t)(e - p);
        if (len == 9 && strncmp(p, t, 9) == 0)
            return 1;
        while (*e && *e != '\n')
            e++;
        p = e;
    }
    return 0;
}

static const char *const k_audio_syms[4] = {
    "sceAudioOutOpen", "sceAudioOutClose", "sceAudioOutOutput", "sceAudioOutOutputs"
};

static void remove_hooks(void)
{
    Detour *det[4] = { &Detour_sceAudioOutOpen, &Detour_sceAudioOutClose,
                       &Detour_sceAudioOutOutput, &Detour_sceAudioOutOutputs };
    int i;
    for (i = 3; i >= 0; i--) {
        if (g_det_ok[i]) {
            Detour_Destroy(det[i]);
            g_det_ok[i] = 0;
        }
    }
}

/* Returns 0 installed, 1 module not loaded yet (retry later), -1 failed. */
static int install_hooks_once(char *why, size_t whylen)
{
    Detour *det[4] = { &Detour_sceAudioOutOpen, &Detour_sceAudioOutClose,
                       &Detour_sceAudioOutOutput, &Detour_sceAudioOutOutputs };
    void *hooks[4] = { (void *)sceAudioOutOpen_hook, (void *)sceAudioOutClose_hook,
                       (void *)sceAudioOutOutput_hook, (void *)sceAudioOutOutputs_hook };
    void *addr[4] = { NULL, NULL, NULL, NULL };
    int32_t h;
    int i;

    /* Cheap per call: one GetModuleList + dlsym on handles not seen before
     * (a module that lacks sceAudioOutOpen never gains it). */
    h = -1;
    if (g_audio_hint >= 0 && mod_dlsym(g_audio_hint, "sceAudioOutOpen", &addr[0]) == 0)
        h = g_audio_hint;
    if (h < 0)
        h = find_export("sceAudioOutOpen", &addr[0], g_audio_neg);
    if (h < 0) {
        snprintf(why, whylen, "no loaded module exports sceAudioOutOpen");
        return 1;
    }
    {
        char nm[128];
        if (mod_info_name(h, nm, sizeof nm) != 0)
            snprintf(nm, sizeof nm, "?");
        snprintf(g_audio_from, sizeof g_audio_from, "handle 0x%x %s", (unsigned)h, nm);
    }
    for (i = 1; i < 4; i++) {
        if (mod_dlsym(h, k_audio_syms[i], &addr[i]) != 0) {
            /* Not in the same module: search all handles. */
            if (find_export(k_audio_syms[i], &addr[i], NULL) < 0 || !addr[i]) {
                snprintf(why, whylen, "dlsym %s failed", k_audio_syms[i]);
                return -1;
            }
        }
    }
    /* Close and Open first so ports are tracked before output is tapped. */
    for (i = 0; i < 4; i++) {
        Detour_Construct(det[i], DetourMode_x32);
        if (!Detour_DetourFunction(det[i], (uint64_t)addr[i], hooks[i])) {
            snprintf(why, whylen, "detour %s failed", k_audio_syms[i]);
            return -1;
        }
        g_det_ok[i] = 1;
    }
    return 0;
}

#define PRIO_LOWEST   767

/* Thread-safe, once-only. src names the caller for the log.
 * Returns 2 installed (now or earlier), 0 not yet, -1 failed for good. */
static int try_install(const char *src, int quiet_absent)
{
    char why[96];
    char buf[256];
    int expect = 0;
    int r;
    int st = A_LOAD(&g_hooked);

    if (st == 2 || st == -1)
        return st;
    if (!__atomic_compare_exchange_n(&g_hooked, &expect, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return 0; /* another thread is installing right now */
    why[0] = 0;
    r = install_hooks_once(why, sizeof why);
    if (r == 0) {
        A_STORE(&g_hooked, 2);
        snprintf(buf, sizeof buf,
                 "[%llu ms] hooks installed via %s, AudioOut in %s (worker starts on first sceAudioOutOpen)\n",
                 (unsigned long long)(sceKernelGetProcessTime() / 1000), src, g_audio_from);
        log_line(buf, 0);
        return 2;
    }
    if (r < 0) {
        remove_hooks();
        A_STORE(&g_hooked, -1);
        snprintf(buf, sizeof buf, "[%llu ms] hooks NOT installed via %s: %s (giving up)\n",
                 (unsigned long long)(sceKernelGetProcessTime() / 1000), src, why);
        log_line(buf, 0);
        final_printf("[GoldHEN] %s: %s", g_pluginName, buf);
        return -1;
    }
    A_STORE(&g_hooked, 0);
    if (!quiet_absent) {
        snprintf(buf, sizeof buf, "[%llu ms] %s: audio not loaded yet\n",
                 (unsigned long long)(sceKernelGetProcessTime() / 1000), src);
        log_line(buf, 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Deferred install: poll thread + module-load hooks                    */

#define POLL_US       500000u
#define POLL_MAX_US   60000000ull
#define POLL_LOG_EVERY 10       /* ~5 s */

static void *poll_main(void *arg)
{
    char buf[160];
    uint64_t t0;
    uint32_t n = 0;
    int dumped5 = 0, dumped20 = 0;
    (void)arg;

    (void)scePthreadSetprio(scePthreadSelf(), PRIO_LOWEST);
    t0 = sceKernelGetProcessTime();
    snprintf(buf, sizeof buf, "[%llu ms] poll: started (every %u ms, up to %llu s, prio %d)\n",
             (unsigned long long)(t0 / 1000), POLL_US / 1000,
             (unsigned long long)(POLL_MAX_US / 1000000ull), PRIO_LOWEST);
    log_line(buf, 0);
    log_modules("poll start");
    while (g_poll_run) {
        uint64_t el;
        int r;
        n++;
        r = try_install("poll", !(n == 1 || n % POLL_LOG_EVERY == 0));
        if (r != 0)
            break;
        el = sceKernelGetProcessTime() - t0;
        if (!dumped5 && el >= 5000000ull) {
            dumped5 = 1;
            log_modules("poll 5 s");
        }
        if (!dumped20 && el >= 20000000ull) {
            dumped20 = 1;
            log_modules("poll 20 s");
        }
        if (el >= POLL_MAX_US) {
            snprintf(buf, sizeof buf,
                     "[%llu ms] poll: gave up after %u attempts, no module exports sceAudioOutOpen\n",
                     (unsigned long long)(sceKernelGetProcessTime() / 1000), n);
            log_line(buf, 0);
            return NULL;
        }
        sceKernelUsleep(POLL_US);
    }
    snprintf(buf, sizeof buf, "[%llu ms] poll: done after %u attempts (state=%d)\n",
             (unsigned long long)(sceKernelGetProcessTime() / 1000), n, A_LOAD(&g_hooked));
    log_line(buf, 0);
    return NULL;
}

static void poll_start(const char *src)
{
    char buf[128];
    int expect = 0;
    int r;

    if (A_LOAD(&g_hooked) != 0)
        return;
    if (!__atomic_compare_exchange_n(&g_poll_started, &expect, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
    g_poll_run = 1;
    r = scePthreadCreate(&g_poll, NULL, poll_main, NULL, "hb_tap_poll");
    if (r != 0) {
        g_poll_run = 0;
        A_STORE(&g_poll_started, -1);
        snprintf(buf, sizeof buf, "poll: thread create from %s failed 0x%08x\n", src, (unsigned)r);
        log_line(buf, 0);
    }
}

/* Called on the game thread right after a module load returned. */
static void after_load(const char *fn, const char *what, int32_t rc)
{
    char buf[256];
    uint32_t n;

    if (A_LOAD(&g_hooked) != 0)
        return; /* installed or given up: pure pass-through */
    n = A_ADD(&g_load_calls, 1) + 1;
    snprintf(buf, sizeof buf, "[%llu ms] load #%u %s(%s) rc=0x%08x\n",
             (unsigned long long)(sceKernelGetProcessTime() / 1000), n, fn,
             what ? what : "?", (unsigned)rc);
    log_line(buf, 0);
    if (try_install(fn, 0) == 0)
        poll_start(fn);
}

static int32_t sceKernelLoadStartModule_hook(const char *path, size_t argc, const void *argp,
                                             uint32_t flags, void *opt, int *res)
{
    int32_t rc = HOOK_CONTINUE(sceKernelLoadStartModule,
                               int32_t (*)(const char *, size_t, const void *, uint32_t,
                                           void *, int *),
                               path, argc, argp, flags, opt, res);
    after_load("sceKernelLoadStartModule", path, rc);
    return rc;
}

static int32_t sceSysmoduleLoadModule_hook(uint32_t id)
{
    char what[16];
    int32_t rc = HOOK_CONTINUE(sceSysmoduleLoadModule, int32_t (*)(uint32_t), id);
    snprintf(what, sizeof what, "0x%04x", (unsigned)id);
    after_load("sceSysmoduleLoadModule", what, rc);
    return rc;
}

static int32_t sceSysmoduleLoadModuleInternal_hook(uint32_t id)
{
    char what[16];
    int32_t rc = HOOK_CONTINUE(sceSysmoduleLoadModuleInternal, int32_t (*)(uint32_t), id);
    snprintf(what, sizeof what, "0x%08x", (unsigned)id);
    after_load("sceSysmoduleLoadModuleInternal", what, rc);
    return rc;
}

/* Installs the module-load hooks. Returns how many were installed. */
static int install_load_hooks(void)
{
    static const char *const k_kernel[] = { "libkernel", "libkernel_web",
                                            "libkernel_sys", NULL };
    static const char *const k_sysmod[] = { "libSceSysmodule", NULL };
    Detour *det[3] = { &Detour_sceKernelLoadStartModule, &Detour_sceSysmoduleLoadModule,
                       &Detour_sceSysmoduleLoadModuleInternal };
    void *hooks[3] = { (void *)sceKernelLoadStartModule_hook, (void *)sceSysmoduleLoadModule_hook,
                       (void *)sceSysmoduleLoadModuleInternal_hook };
    const char *syms[3] = { "sceKernelLoadStartModule", "sceSysmoduleLoadModule",
                            "sceSysmoduleLoadModuleInternal" };
    const char *const *mods[3] = { k_kernel, k_sysmod, k_sysmod };
    char buf[256];
    int ok = 0;
    int i;

    for (i = 0; i < 3; i++) {
        const char *mname = "?";
        void *addr = NULL;
        int32_t h = module_or_export(mods[i], syms[i], &mname);
        if (h < 0) {
            h = sys_lib_handle(i == 0 ? "libkernel.sprx" : "libSceSysmodule.sprx", syms[i]);
            mname = "by load";
        }
        if (h < 0) {
            snprintf(buf, sizeof buf, "load hook %s: module not found (name or export)\n", syms[i]);
        } else if (mod_dlsym(h, syms[i], &addr) != 0) {
            snprintf(buf, sizeof buf, "load hook %s: dlsym in %s failed\n", syms[i], mname);
        } else {
            Detour_Construct(det[i], DetourMode_x32);
            if (!Detour_DetourFunction(det[i], (uint64_t)addr, hooks[i])) {
                snprintf(buf, sizeof buf, "load hook %s: detour failed\n", syms[i]);
            } else {
                g_ldet_ok[i] = 1;
                ok++;
                snprintf(buf, sizeof buf, "load hook %s (%s) installed\n", syms[i], mname);
            }
        }
        log_line(buf, 0);
    }
    return ok;
}

static void remove_load_hooks(void)
{
    Detour *det[3] = { &Detour_sceKernelLoadStartModule, &Detour_sceSysmoduleLoadModule,
                       &Detour_sceSysmoduleLoadModuleInternal };
    int i;
    for (i = 2; i >= 0; i--) {
        if (g_ldet_ok[i]) {
            Detour_Destroy(det[i]);
            g_ldet_ok[i] = 0;
        }
    }
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    char buf[256];
    int rc;
    int n;

    static int s_loaded;

    (void)argc;
    (void)argv;
    /* Listed twice (e.g. [default] and a title section): run once. */
    if (s_loaded)
        return 0;
    s_loaded = 1;
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    rc = hb_title_id(g_titleid, sizeof g_titleid);

    /* Skip fast, with no files, hooks or threads, where we must not run. */
    if (rc != 0 || !title_is_game(g_titleid)) {
        final_printf("[GoldHEN] %s: skip non-game title %s\n", g_pluginName,
                     g_titleid[0] ? g_titleid : "unknown");
        return 0;
    }
    {
        int fd = sceKernelOpen(TAP_OFF_FLAG, 0 /* O_RDONLY */, 0);
        if (fd >= 0) {
            sceKernelClose(fd);
            A_STORE(&g_tap_off, 1);
            final_printf("[GoldHEN] %s: game audio off (%s), waiting for it to be switched on\n",
                         g_pluginName, TAP_OFF_FLAG);
        }
    }
    if (title_in_skip_file(g_titleid)) {
        final_printf("[GoldHEN] %s: %s listed in %s\n", g_pluginName, g_titleid, SKIP_FILE);
        return 0;
    }

    /* Per-title log if the directory exists (never created here). */
    {
        char pth[96];
        int fd;
        snprintf(pth, sizeof pth, TAP_LOG_DIR "%s.log", g_titleid);
        fd = sceKernelOpen(pth, K_O_WRONLY | K_O_CREAT | K_O_TRUNC, 0666);
        if (fd >= 0) {
            sceKernelClose(fd);
            snprintf(g_log_path, sizeof g_log_path, "%s", pth);
        }
    }
    A_STORE(&g_daemon_on, daemon_streaming());

    snprintf(buf, sizeof buf,
             "%s 0x%08x loaded title=%s proc_info_rc=%d target=udp 127.0.0.1:%d daemon=%s\n",
             PLUGIN_NAME, PLUGIN_VER, g_titleid, rc, PCM_PORT,
             A_LOAD(&g_daemon_on) ? "streaming" : "not streaming (tap paused until it is)");
    log_line(buf, 1);
    if (A_LOAD(&g_tap_off)) {
        snprintf(buf, sizeof buf, "game audio is OFF (%s exists): nothing is sent until it is "
                 "switched on in HearBridge (checked every 2 s)\n", TAP_OFF_FLAG);
        log_line(buf, 0);
    }
    log_modules("plugin_load");

    /* Audio already loaded: install now, no deferral. */
    if (try_install("plugin_load", 0) != 0)
        return 0; /* installed, or failed for good (stay loaded but inert) */

    /* Not visible by enumeration: ask the system for it. The game links
     * AudioOut anyway; when it is already loaded these return its handle. */
    {
        int32_t r = (int32_t)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
        snprintf(buf, sizeof buf, "  sceSysmoduleLoadModuleInternal(AUDIOOUT) = 0x%08x\n",
                 (unsigned)r);
        log_line(buf, 0);
        if (try_install("plugin_load after sysmodule", 0) != 0)
            return 0;
        g_audio_hint = sys_lib_handle("libSceAudioOut.sprx", "sceAudioOutOpen");
        if (try_install("plugin_load after LoadStartModule", 0) != 0)
            return 0;
    }

    /* Deferred: hook the module-load path; poll thread as safety net. */
    g_deferred = 1;
    n = install_load_hooks();
    snprintf(buf, sizeof buf, "deferred install armed: %d/3 load hooks%s\n", n,
             n ? ", poll starts on first module load" : ", starting poll now");
    log_line(buf, 0);
    if (n == 0)
        poll_start("plugin_load");
    /* A module may have loaded between the check and the hooks. */
    (void)try_install("plugin_load recheck", 1);
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    if (A_LOAD(&g_poll_started) == 1) {
        g_poll_run = 0;
        scePthreadJoin(g_poll, NULL);
        A_STORE(&g_poll_started, 0);
    }
    if (g_deferred) {
        remove_load_hooks();
        g_deferred = 0;
    }
    if (A_LOAD(&g_hooked) == 2) {
        remove_hooks();
        A_STORE(&g_hooked, 0);
    }
    if (A_LOAD(&g_logger_started) == 1) {
        g_logger_run = 0;
        scePthreadJoin(g_logger, NULL);
        A_STORE(&g_logger_started, 0);
    }
    if (A_LOAD(&g_sock_state) == 2 && g_sock >= 0) {
        A_STORE(&g_sock_state, 0);
        p_sceNetSocketClose(g_sock);
        g_sock = -1;
    }
    return 0;
}

/* GoldHEN calls plugin_load after loading; module_start stays empty like
 * plugin_template. */
int32_t attr_module_hidden module_start(int64_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}

int32_t attr_module_hidden module_stop(int64_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}
