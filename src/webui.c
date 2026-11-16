/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HearBridge PS4 web UI server. See ui.h. */
#include "ui.h"
#include "devices.h"
#include "linkkeys.h"
#include "cod.h"
#include "boost.h"
#include "cmdq.h"
#include "eq.h"
#include "gprof.h"
#include "atomicfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#ifdef HEARBRIDGE_PAYLOAD
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include "webui_html.h"
#include "webui_assets.h"

int32_t scePthreadCreate(void **thr, const void *attr, void *(*fn)(void *), void *arg,
                         const char *name);
int32_t scePthreadJoin(void *thr, void **ret);
int32_t sceKernelUsleep(uint32_t usec);
uint64_t sceKernelGetProcessTime(void);

#ifndef HB_BUILD
#define HB_BUILD "dev"      /* id of this daemon build, set by the Makefile */
#endif

#ifndef SETTINGS_PATH
#define SETTINGS_PATH "/data/hearbridge/settings.txt"
#endif
#ifndef TAP_OFF_PATH
#define TAP_OFF_PATH  "/data/GoldHEN/hearbridge_tap_off"
#endif
#ifndef SKIP_PATH
#define SKIP_PATH     "/data/GoldHEN/hearbridge_skip.txt"
#endif
#define SKIP_MAX      64

/* ---- shared state ------------------------------------------------------ */

static volatile int g_lock;

static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        ;
}

static void unlock(void)
{
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

static struct {
    char version[16];
    char state[32];
    int have_dev;
    unsigned char dev_addr[6];
    char dev_name[64];
    char codec[32];
    int rate, bitpool, bitpool_max, pcm_ms, media_drops, resyncs;
    int streaming, scanning;
    uint64_t scan_t0;          /* process time (us) when the scan started */
    int scan_names;            /* 1 while names are being asked */
    int nscan;
    struct ui_scan_dev scan[UI_SCAN_MAX];
    char log[UI_LOG_MAX][120];
    int log_n, log_head;
    struct cmdq cmdq;               /* commands from the page, oldest first */
    unsigned cmd_n;                 /* entries in cmdq (read without the lock as a hint) */
    int volume;                /* boost percent 0..HB_LOUD_MAX (boost.h) */
    int hs_avail, hs_vol, hs_req;   /* headphone volume (AVRCP): available, level 0..127, page request+1 */
    uint64_t stream_t0;        /* process time (us) when streaming began */
    char error[192];           /* last FAIL line of the daemon, cleared at streaming */
    unsigned long pcm_rx;            /* PCM packets from the tap this stream */
    long pcm_idle_s;
    char tap_title[16];
    int pcm_lost;
    /* equalizer (the page sets it, the audio loop reads it through eq_gen) */
    int eq_on, eq_db[HB_EQ_NB];
    unsigned eq_gen;
    /* game mode: profiles, the running game, the profile now on, the usual sound */
    hb_gps gps;
    int gmode;
    char run[10], applied[10], run_name[HB_GP_NAME];
    int base_boost, base_eq_on, base_eq_db[HB_EQ_NB];
    int save_due;              /* settings or games changed inside a locked section: write them */
} S;

static void settings_save(void);
static void settings_save_soon(void);
static void settings_flush_due(int force);
static void game_tick_locked(const char *title, long idle_s, int streaming);
static void game_name_read(const char *id, char *out, size_t outn);

void ui_set_state(const char *st)
{
    lock();
    snprintf(S.state, sizeof S.state, "%s", st ? st : "");
    unlock();
}

void ui_set_device(const unsigned char addr[6], const char *name)
{
    lock();
    S.have_dev = addr != NULL;
    if (addr) memcpy(S.dev_addr, addr, 6);
    snprintf(S.dev_name, sizeof S.dev_name, "%s", name ? name : "");
    unlock();
}

void ui_set_stream(const char *codec, int rate, int bitpool, int bitpool_max,
                   int pcm_ms, int media_drops, int resyncs)
{
    lock();
    snprintf(S.codec, sizeof S.codec, "%s", codec ? codec : "");
    S.rate = rate;
    S.bitpool = bitpool;
    S.bitpool_max = bitpool_max;
    S.pcm_ms = pcm_ms;
    S.media_drops = media_drops;
    S.resyncs = resyncs;
    unlock();
}

void ui_set_streaming(int on)
{
    int save;
    lock();
    S.streaming = on;
    if (!on) game_tick_locked("", 0, 0);
    save = S.save_due;
    if (on) {
        S.pcm_rx = 0; S.pcm_idle_s = 0; S.tap_title[0] = 0; S.pcm_lost = 0;
        S.stream_t0 = sceKernelGetProcessTime();
        S.error[0] = 0;
    }
    unlock();
    if (save) settings_save();
}

void ui_set_hs_volume(int avail, int vol127)
{
    lock();
    S.hs_avail = avail;
    S.hs_vol = vol127;
    unlock();
}

int ui_take_hs_volume(void)
{
    /* stored +1 so the zeroed state means "nothing asked" */
    return __atomic_exchange_n(&S.hs_req, 0, __ATOMIC_ACQ_REL) - 1;
}

void ui_set_error(const char *text)
{
    lock();
    snprintf(S.error, sizeof S.error, "%s", text ? text : "");
    unlock();
}

void ui_set_pcm(unsigned long rx_pkts, long idle_s, const char *tap_title, int lost)
{
    static char named_id[10], named[HB_GP_NAME];
    int save;
    /* The name of a new game is read before the lock (file access). */
    if (hb_gp_id_ok(tap_title) && strcmp(named_id, tap_title) != 0) {
        game_name_read(tap_title, named, sizeof named);
        snprintf(named_id, sizeof named_id, "%s", tap_title);
    }
    lock();
    S.pcm_rx = rx_pkts;
    S.pcm_idle_s = idle_s;
    snprintf(S.tap_title, sizeof S.tap_title, "%s", tap_title ? tap_title : "");
    S.pcm_lost = lost;
    game_tick_locked(S.tap_title, idle_s, S.streaming);
    if (S.run[0] && strcmp(S.run, named_id) == 0) snprintf(S.run_name, sizeof S.run_name, "%s", named);
    save = S.save_due;
    unlock();
    if (save) settings_save();
}
void ui_set_scanning(int on)
{
    lock();
    if (on && (!S.scanning || on == 1)) S.scan_t0 = sceKernelGetProcessTime();
    S.scan_names = on == 2;
    S.scanning = on != 0;
    unlock();
}

void ui_set_scan_results(const struct ui_scan_dev *d, int n)
{
    lock();
    if (n > UI_SCAN_MAX) n = UI_SCAN_MAX;
    if (n < 0) n = 0;
    if (n) memcpy(S.scan, d, sizeof(*d) * (size_t)n);
    S.nscan = n;
    unlock();
}

void ui_log(const char *line)
{
    lock();
    snprintf(S.log[S.log_head], sizeof S.log[0], "%s", line ? line : "");
    S.log_head = (S.log_head + 1) % UI_LOG_MAX;
    if (S.log_n < UI_LOG_MAX) S.log_n++;
    unlock();
}

/* Log ring for /log: wall-clock seconds plus the text of the newest lines. */
#define LOGR_LINES 300
#define LOGR_LEN   128
static struct { long t; char s[LOGR_LEN]; } g_lr[LOGR_LINES];
static unsigned g_lr_n;                 /* lines ever added */

void ui_logbuf_add(const char *line)
{
    unsigned i;
    long now = (long)time(NULL);
    lock();
    i = g_lr_n % LOGR_LINES;
    g_lr[i].t = now;
    snprintf(g_lr[i].s, LOGR_LEN, "%s", line ? line : "");
    g_lr_n++;
    unlock();
}

int ui_take_cmd(struct ui_cmd *out)
{
    int r = 0;
    settings_flush_due(0);
    if (!__atomic_load_n(&S.cmd_n, __ATOMIC_ACQUIRE)) return 0;
    lock();
    r = cmdq_pop(&S.cmdq, &out->type, out->addr);
    __atomic_store_n(&S.cmd_n, S.cmdq.count, __ATOMIC_RELEASE);
    unlock();
    return r;
}

int ui_volume(void)
{
    return __atomic_load_n(&S.volume, __ATOMIC_RELAXED);
}

static void put_cmd(int type, const unsigned char *addr)
{
    lock();
    cmdq_push(&S.cmdq, type, addr);
    __atomic_store_n(&S.cmd_n, S.cmdq.count, __ATOMIC_RELEASE);
    unlock();
}

/* ---- settings and tap files ------------------------------------------- */

static void games_load(void)
{
    char buf[4096];
    size_t n = 0;
    FILE *f = fopen(HB_GAMES_PATH, "r");
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    hb_gps_parse(&S.gps, buf);
}

static void settings_load(void)
{
    FILE *f = fopen(SETTINGS_PATH, "r");
    char line[64];
    S.volume = HB_LOUD_DEFAULT;
    S.hs_vol = -1;
    S.gmode = 1;
    S.eq_gen = 1;
    games_load();
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        /* volume= and boost= (from older files) are not used, only boost= maps
         * to the nearest loudness so a chosen Loud or Louder carries over. */
        if (strncmp(line, "loudness=", 9) == 0) {
            int v = atoi(line + 9);
            if (v >= 0) S.volume = v > HB_LOUD_MAX ? HB_LOUD_MAX : v;
        } else if (strncmp(line, "eq=", 3) == 0) {
            S.eq_on = atoi(line + 3) != 0;
        } else if (strncmp(line, "eqdb=", 5) == 0) {
            hb_eq_parse(line + 5, S.eq_db);
        } else if (strncmp(line, "gamemode=", 9) == 0) {
            S.gmode = atoi(line + 9) != 0;
        } else if (strncmp(line, "boost=", 6) == 0) {
            int v = atoi(line + 6);
            if (v == 0) S.volume = 100;
            else if (v == 2) S.volume = 280;
        }
    }
    fclose(f);
}

/* Writes the usual sound: while a game profile is on that is the sound
 * from before it, not the profile (the profile lives in games.txt). */
static void settings_save(void)
{
    int boost, eq_on, db[HB_EQ_NB], gmode, n;
    char dbs[40], txt[160];
    lock();
    if (S.applied[0]) {
        boost = S.base_boost; eq_on = S.base_eq_on; memcpy(db, S.base_eq_db, sizeof db);
    } else {
        boost = S.volume; eq_on = S.eq_on; memcpy(db, S.eq_db, sizeof db);
    }
    gmode = S.gmode;
    S.save_due = 0;
    unlock();
    hb_eq_format(db, dbs, sizeof dbs);
    mkdir("/data/hearbridge", 0777);
    n = snprintf(txt, sizeof txt, "loudness=%d\neq=%d\neqdb=%s\ngamemode=%d\n", boost, eq_on, dbs, gmode);
    if (hb_file_write_atomic(SETTINGS_PATH, txt, (size_t)n) != 0) printf("[hb-ui] could not save %s\n", SETTINGS_PATH);
}

/* A dragged slider sends many requests: write the settings once it has been
 * quiet for a moment (ui_take_cmd, polled by the daemon, does the write). */
static uint64_t g_save_at;

static void settings_save_soon(void)
{
    __atomic_store_n(&g_save_at, sceKernelGetProcessTime() + 1500000u, __ATOMIC_RELEASE);
}

static void settings_flush_due(int force)
{
    uint64_t at = __atomic_load_n(&g_save_at, __ATOMIC_ACQUIRE);
    if (at && (force || sceKernelGetProcessTime() >= at) &&
        __atomic_compare_exchange_n(&g_save_at, &at, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        settings_save();
}

static void games_save(void)
{
    static char buf[4096];
    lock();
    hb_gps_format(&S.gps, buf, sizeof buf);
    unlock();
    mkdir("/data/hearbridge", 0777);
    if (hb_file_write_atomic(HB_GAMES_PATH, buf, strlen(buf)) != 0) printf("[hb-ui] could not save %s\n", HB_GAMES_PATH);
}

/* ---- equalizer and game mode (called with the lock held where noted) --- */

static void eq_bump_locked(void) { __atomic_add_fetch(&S.eq_gen, 1, __ATOMIC_RELEASE); }

static void game_apply_locked(const hb_gp *g)
{
    S.volume = g->boost;
    S.eq_on = g->eq_on;
    memcpy(S.eq_db, g->eq_db, sizeof S.eq_db);
    snprintf(S.applied, sizeof S.applied, "%s", g->id);
    eq_bump_locked();
}

static void game_restore_locked(void)
{
    S.volume = S.base_boost;
    S.eq_on = S.base_eq_on;
    memcpy(S.eq_db, S.base_eq_db, sizeof S.eq_db);
    S.applied[0] = 0;
    eq_bump_locked();
}

static void game_take_base_locked(void)
{
    S.base_boost = S.volume;
    S.base_eq_on = S.eq_on;
    memcpy(S.base_eq_db, S.eq_db, sizeof S.base_eq_db);
}

/* The game running now ("" none) changed or not. Streaming off or a game that
 * has been silent for 30 s counts as no game. */
static void game_tick_locked(const char *title, long idle_s, int streaming)
{
    char cur[10] = "";
    int d, i;
    if (streaming && hb_gp_id_ok(title) && idle_s < 30) snprintf(cur, sizeof cur, "%s", title);
    d = hb_gm_decide(S.run, cur);
    if (d == HB_GM_KEEP) return;
    if ((d == HB_GM_STOP || d == HB_GM_SWITCH) && S.applied[0]) {
        game_restore_locked();
        S.save_due = 1;
    }
    if (d == HB_GM_START || d == HB_GM_SWITCH) game_take_base_locked();
    snprintf(S.run, sizeof S.run, "%s", cur);
    if (!cur[0]) S.run_name[0] = 0;
    if (cur[0] && S.gmode && (i = hb_gps_find(&S.gps, cur)) >= 0) {
        game_apply_locked(&S.gps.g[i]);
        S.save_due = 1;
    }
}

/* Audio loop: equalizer (when on) and Boost on interleaved int16 stereo. */
void ui_eq_apply(int16_t *pcm, int frames, int32_t gain_q8)
{
    static hb_eq eq;
    static unsigned seen;
    static int inited;
    unsigned gen = __atomic_load_n(&S.eq_gen, __ATOMIC_ACQUIRE);
    if (!inited) { hb_eq_init(&eq); inited = 1; }
    if (gen != seen) {
        int on, db[HB_EQ_NB];
        lock();
        on = S.eq_on;
        memcpy(db, S.eq_db, sizeof db);
        unlock();
        hb_eq_set(&eq, on, db, 48000);
        seen = gen;
    }
    hb_eq_apply(&eq, pcm, frames, gain_q8);
}

/* Name of a game from its param.sfo ("" when unknown). */
static void game_name_read(const char *id, char *out, size_t outn)
{
    static unsigned char buf[8192];
    char path[96];
    size_t n = 0;
    FILE *f;
    out[0] = 0;
    snprintf(path, sizeof path, "/system_data/priv/appmeta/%s/param.sfo", id);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof path, "/user/appmeta/%s/param.sfo", id);
        f = fopen(path, "rb");
    }
    if (!f) return;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (!hb_sfo_title(buf, n, out, outn)) out[0] = 0;
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int valid_title(const char *t)
{
    int i;
    if (!t || strlen(t) != 9) return 0;
    for (i = 0; i < 4; i++)
        if (t[i] < 'A' || t[i] > 'Z') return 0;
    for (i = 4; i < 9; i++)
        if (t[i] < '0' || t[i] > '9') return 0;
    return 1;
}

static int skip_load(char ids[SKIP_MAX][10])
{
    FILE *f = fopen(SKIP_PATH, "r");
    char line[64];
    int n = 0;
    if (!f) return 0;
    while (n < SKIP_MAX && fgets(line, sizeof line, f)) {
        char id[10];
        if (sscanf(line, "%9s", id) == 1 && valid_title(id))
            snprintf(ids[n++], 10, "%s", id);
    }
    fclose(f);
    return n;
}

static int skip_save(char ids[SKIP_MAX][10], int n)
{
    static char buf[SKIP_MAX * 10 + 100];
    int i, k;
    k = snprintf(buf, sizeof buf, "# HearBridge: games where the game audio tap does not run\n");
    for (i = 0; i < n && k + 11 < (int)sizeof buf; i++) k += snprintf(buf + k, sizeof buf - (size_t)k, "%s\n", ids[i]);
    return hb_file_write_atomic(SKIP_PATH, buf, (size_t)k);
}

/* ---- JSON -------------------------------------------------------------- */

struct sb { char *p; size_t n, cap; int ov; };   /* ov: output was cut */

static void sb_add(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void sb_add(struct sb *b, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (b->n >= b->cap) return;
    va_start(ap, fmt);
    r = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (r > 0) b->n += (size_t)r;
    if (b->n >= b->cap) {
        b->n = b->cap - 1;
        b->ov = 1;
    }
}

static void sb_str(struct sb *b, const char *s)
{
    sb_add(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') sb_add(b, "\\%c", c);
        else if (c < 0x20 || c == 0x7f) sb_add(b, " ");   /* names come from other devices */
        else sb_add(b, "%c", c);
    }
    sb_add(b, "\"");
}

/* Per-start secret that state-changing requests must carry (see handle()).
 * The page learns it from /api/status, which a page on another origin cannot
 * read, so a web page opened on another device cannot drive the daemon. */
static char g_ui_tok[33];

static void token_init(void)
{
    unsigned char r[16];
    size_t got = 0, i;
    FILE *f;
    if (g_ui_tok[0]) return;
    if ((f = fopen("/dev/urandom", "rb"))) {
        got = fread(r, 1, sizeof r, f);
        fclose(f);
    }
    if (got < sizeof r) {   /* no random device: mix clocks and addresses */
        uint64_t x = __builtin_ia32_rdtsc() ^ (sceKernelGetProcessTime() << 20) ^ (uint64_t)(uintptr_t)&got;
        for (i = got; i < sizeof r; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            r[i] = (unsigned char)(x >> 24);
        }
    }
    for (i = 0; i < sizeof r; i++) snprintf(g_ui_tok + 2 * i, 3, "%02x", r[i]);
}

/* Assigned Numbers, Audio/Video minor classes, for the label of a device
 * that already passed the class filter (cod.h). TVs and set-top boxes are
 * filtered out before they reach the lists; "tv" stays only as a label for
 * old data. */
static const char *cod_kind(uint32_t cod)
{
    int major = (int)((cod >> 8) & 0x1F), minor = (int)((cod >> 2) & 0x3F);
    if (major == 0x04) {
        switch (minor) {
        case 0x01: return "headset";
        case 0x02: return "handsfree";
        case 0x05: return "speaker";
        case 0x06: return "headphones";
        case 0x07: return "portable";
        case 0x0A: return "hifi";
        case 0x08: return "car";
        case 0x09: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10:
            return "tv";
        default: return "audio";
        }
    }
    if (cod & 0x200000) return "audio";
    return "other";
}

/* 0 headphones / headsets, 1 speakers and Hi-Fi, 2 other audio, 3 TV-like. */
int ui_cod_rank(uint32_t cod)
{
    const char *k = cod_kind(cod);
    if (!strcmp(k, "headset") || !strcmp(k, "headphones") || !strcmp(k, "handsfree")) return 0;
    if (!strcmp(k, "speaker") || !strcmp(k, "portable") || !strcmp(k, "hifi")) return 1;
    if (!strcmp(k, "tv")) return 3;
    return 2;
}

static void build_status(struct sb *b)
{
    struct saved_dev sd[DEVICES_MAX];
    char skip[SKIP_MAX][10];
    int nsd = devices_load(sd), nskip = skip_load(skip), i, k;
    char a[18];

    {   /* console address for the launcher's message, refreshed every 30 s */
        static char ip[24];
        static uint64_t ip_t;
        uint64_t now = sceKernelGetProcessTime();
        if (!ip[0] || now - ip_t > 30000000u) {
            ui_local_ip(ip, sizeof ip);
            ip_t = now;
        }
        lock();
        sb_add(b, "{\"version\":");
        sb_str(b, S.version);
        sb_add(b, ",\"build\":\"%s\"", HB_BUILD);
        sb_add(b, ",\"token\":");
        sb_str(b, g_ui_tok);
        sb_add(b, ",\"ip\":");
        sb_str(b, ip);
    }
    sb_add(b, ",\"state\":");
    sb_str(b, S.state);
    sb_add(b, ",\"streaming\":%d,\"scanning\":%d,\"scan_s\":%d,\"scan_phase\":\"%s\"",
           S.streaming, S.scanning,
           S.scanning ? (int)((sceKernelGetProcessTime() - S.scan_t0) / 1000000u) : 0,
           !S.scanning ? "" : S.scan_names ? "names" : "inquiry");
    if (S.have_dev) {
        devices_addr_str(S.dev_addr, a);
        sb_add(b, ",\"device\":{\"addr\":");
        sb_str(b, a);
        sb_add(b, ",\"name\":");
        sb_str(b, S.dev_name);
        sb_add(b, "}");
    } else {
        sb_add(b, ",\"device\":null");
    }
    sb_add(b, ",\"codec\":");
    sb_str(b, S.codec);
    sb_add(b, ",\"rate\":%d,\"bitpool\":%d,\"bitpool_max\":%d,\"buffer_ms\":%d,"
              "\"drops\":%d,\"resyncs\":%d,\"volume\":%d",
           S.rate, S.bitpool, S.bitpool_max, S.pcm_ms, S.media_drops, S.resyncs, S.volume);
    sb_add(b, ",\"pcm_rx\":%lu,\"pcm_idle_s\":%ld,\"pcm_lost\":%d,\"tap_title\":",
           S.pcm_rx, S.pcm_idle_s, S.pcm_lost);
    sb_str(b, S.tap_title);
    /* Headphone volume in percent (AVRCP 0..127 -> 0..100). */
    sb_add(b, ",\"hs_volume\":{\"avail\":%d,\"pct\":%d}", S.hs_avail,
           S.hs_vol < 0 ? -1 : (S.hs_vol * 100 + 63) / 127);
    sb_add(b, ",\"stream_s\":%d,\"error\":", S.streaming && S.stream_t0 ?
           (int)((sceKernelGetProcessTime() - S.stream_t0) / 1000000u) : 0);
    sb_str(b, S.streaming ? "" : S.error);
    sb_add(b, ",\"scan\":[");
    for (i = 0; i < S.nscan; i++) {
        devices_addr_str(S.scan[i].addr, a);
        sb_add(b, "%s{\"addr\":", i ? "," : "");
        sb_str(b, a);
        sb_add(b, ",\"name\":");
        sb_str(b, S.scan[i].name);
        sb_add(b, ",\"kind\":\"%s\",\"rank\":%d,\"rssi\":%d}", cod_kind(S.scan[i].cod),
               ui_cod_rank(S.scan[i].cod), S.scan[i].rssi == 127 ? 0 : S.scan[i].rssi);
    }
    sb_add(b, "],\"log\":[");
    for (i = 0; i < S.log_n; i++) {
        k = (S.log_head - S.log_n + i + UI_LOG_MAX) % UI_LOG_MAX;
        if (i) sb_add(b, ",");
        sb_str(b, S.log[k]);
    }
    sb_add(b, "]");
    unlock();

    sb_add(b, ",\"saved\":[");
    {
        /* most recent first */
        int used[DEVICES_MAX] = { 0 }, out = 0;
        /* Entries for a TV or streamer stay in the
         * file but are not listed and cannot be connected. */
        for (i = 0; i < nsd; i++)
            if (!cod_device_allowed(0, 1, sd[i].cod)) used[i] = 1;
        for (;;) {
            int best = -1;
            for (i = 0; i < nsd; i++)
                if (!used[i] && (best < 0 || sd[i].seq > sd[best].seq)) best = i;
            if (best < 0) break;
            used[best] = 1;
            devices_addr_str(sd[best].addr, a);
            sb_add(b, "%s{\"addr\":", out ? "," : "");
            sb_str(b, a);
            sb_add(b, ",\"name\":");
            sb_str(b, sd[best].name);
            sb_add(b, ",\"kind\":\"%s\",\"last\":%d}", cod_kind(sd[best].cod), out == 0);
            out++;
        }
    }
    sb_add(b, "],\"tap\":%d,\"skip\":[", !file_exists(TAP_OFF_PATH));
    for (i = 0; i < nskip; i++) {
        if (i) sb_add(b, ",");
        sb_str(b, skip[i]);
    }
    sb_add(b, "]");
    lock();
    sb_add(b, ",\"eq\":{\"on\":%d,\"db\":[%d,%d,%d,%d,%d]}", S.eq_on, S.eq_db[0], S.eq_db[1],
           S.eq_db[2], S.eq_db[3], S.eq_db[4]);
    {
        int own = S.run[0] ? hb_gps_find(&S.gps, S.run) : -1;
        sb_add(b, ",\"game\":{\"mode\":%d,\"run\":", S.gmode);
        sb_str(b, S.run);
        sb_add(b, ",\"name\":");
        sb_str(b, S.run_name);
        sb_add(b, ",\"has\":%d,\"active\":%d,\"dirty\":%d,\"saved\":[", own >= 0,
               S.applied[0] != 0 && own >= 0 && !strcmp(S.applied, S.run),
               own >= 0 ? hb_gp_differs(&S.gps.g[own], S.eq_on, S.eq_db, S.volume) : 0);
        for (i = 0; i < S.gps.n; i++) {
            const hb_gp *g = &S.gps.g[i];
            sb_add(b, "%s{\"id\":", i ? "," : "");
            sb_str(b, g->id);
            sb_add(b, ",\"name\":");
            sb_str(b, g->name);
            sb_add(b, ",\"eq\":{\"on\":%d,\"db\":[%d,%d,%d,%d,%d]},\"boost\":%d}", g->eq_on,
                   g->eq_db[0], g->eq_db[1], g->eq_db[2], g->eq_db[3], g->eq_db[4], g->boost);
        }
        sb_add(b, "]}");
    }
    unlock();
    sb_add(b, "}");
}

#ifdef HEARBRIDGE_PAYLOAD

/* ---- HTTP -------------------------------------------------------------- */

/* One connection gets this long in all (reading the request and sending the
 * answer); a client that trickles bytes cannot hold the single server thread. */
#define REQ_READ_US   4000000ull
#define CONN_TOTAL_US 20000000ull
static uint64_t g_conn_end;

static void send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w;
        if (sceKernelGetProcessTime() > g_conn_end) return;
        w = send(fd, p, n, 0);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

static void respond(int fd, const char *type, const char *body, size_t len)
{
    char hdr[256];
    int h = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     type, (unsigned)len);
    send_all(fd, hdr, (size_t)h);
    send_all(fd, body, len);
}

static void respond_403(int fd)
{
    static const char r[] = "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n"
                            "Content-Length: 24\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
                            "{\"ok\":false,\"error\":403}";
    send_all(fd, r, sizeof r - 1);
}

static void respond_503(int fd)
{
    static const char r[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                            "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    send_all(fd, r, sizeof r - 1);
}

static void respond_404(int fd)
{
    static const char r[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(fd, r, sizeof r - 1);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Value of key in a query string ("a=1&b=2"), URL-decoded into out. */
static int qget(const char *q, const char *key, char *out, size_t outlen)
{
    size_t kl = strlen(key);
    while (q && *q) {
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            const char *v = q + kl + 1;
            size_t n = 0;
            while (*v && *v != '&' && n + 1 < outlen) {
                if (*v == '%') {
                    int hi = hexval(v[1]), lo = v[1] ? hexval(v[2]) : -1;
                    if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return 0;   /* malformed or %00 */
                    out[n++] = (char)(hi * 16 + lo);
                    v += 3;
                } else {
                    out[n++] = (*v == '+') ? ' ' : *v;
                    v++;
                }
            }
            out[n] = 0;
            return 1;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
    return 0;
}

static void handle_cmd(int fd, const char *q)
{
    char c[24], v[32];
    unsigned char addr[6];
    const char *ok = "{\"ok\":true}", *bad = "{\"ok\":false}";
    const char *r = ok;

    if (!qget(q, "c", c, sizeof c)) c[0] = 0;
    if (strcmp(c, "scan") == 0) {
        put_cmd(UI_CMD_SCAN, NULL);
    } else if (strcmp(c, "connect") == 0 || strcmp(c, "forget") == 0) {
        if (qget(q, "addr", v, sizeof v) && devices_parse_addr(v, addr)) {
            if (c[0] == 'f') {
                /* The daemon thread removes the entry and the key (one writer). */
                put_cmd(UI_CMD_FORGET, addr);
            } else {
                put_cmd(UI_CMD_CONNECT, addr);
            }
        } else {
            r = bad;
        }
    } else if (strcmp(c, "disconnect") == 0) {
        put_cmd(UI_CMD_DISCONNECT, NULL);
    } else if (strcmp(c, "stop") == 0) {
        put_cmd(UI_CMD_STOP, NULL);
    } else if (strcmp(c, "volume") == 0) {
        int x;
        if (qget(q, "v", v, sizeof v)) {
            x = atoi(v);
            if (x < 0) x = 0;
            if (x > HB_LOUD_MAX) x = HB_LOUD_MAX;
            __atomic_store_n(&S.volume, x, __ATOMIC_RELAXED);
            settings_save_soon();
        } else {
            r = bad;
        }
    } else if (strcmp(c, "hsvol") == 0) {
        int x;
        if (qget(q, "v", v, sizeof v)) {
            x = atoi(v);
            if (x < 0) x = 0;
            if (x > 100) x = 100;
            x = (x * 127 + 50) / 100;
            lock();
            S.hs_req = x + 1;
            S.hs_vol = x;            /* shown right away; the daemon confirms */
            unlock();
        } else {
            r = bad;
        }
    } else if (strcmp(c, "eq") == 0) {
        char key[4];
        int i, db[HB_EQ_NB], on = 0;
        lock();
        for (i = 0; i < HB_EQ_NB; i++) db[i] = S.eq_db[i];
        on = S.eq_on;
        unlock();
        if (qget(q, "on", v, sizeof v)) on = atoi(v) != 0;
        for (i = 0; i < HB_EQ_NB; i++) {
            snprintf(key, sizeof key, "b%d", i);
            if (qget(q, key, v, sizeof v)) db[i] = hb_gp_clamp(atoi(v), -12, 12);
        }
        lock();
        S.eq_on = on;
        for (i = 0; i < HB_EQ_NB; i++) S.eq_db[i] = db[i];
        eq_bump_locked();
        unlock();
        settings_save_soon();
    } else if (strcmp(c, "gamemode") == 0) {
        int on = qget(q, "on", v, sizeof v) && v[0] == '1', i;
        lock();
        S.gmode = on;
        if (!on && S.applied[0]) game_restore_locked();
        else if (on && S.run[0] && !S.applied[0] && (i = hb_gps_find(&S.gps, S.run)) >= 0)
            game_apply_locked(&S.gps.g[i]);
        unlock();
        settings_save();
    } else if (strcmp(c, "gsave") == 0) {
        int ok = 0;
        lock();
        if (S.run[0]) {
            hb_gp g;
            memset(&g, 0, sizeof g);
            snprintf(g.id, sizeof g.id, "%s", S.run);
            snprintf(g.name, sizeof g.name, "%s", S.run_name);
            g.eq_on = S.eq_on;
            memcpy(g.eq_db, S.eq_db, sizeof g.eq_db);
            g.boost = S.volume;
            hb_gps_put(&S.gps, &g);
            if (S.gmode) snprintf(S.applied, sizeof S.applied, "%s", S.run);
            ok = 1;
        }
        unlock();
        if (ok) { games_save(); settings_save(); } else r = bad;
    } else if (strcmp(c, "gdel") == 0) {
        char id[10];
        int ok = 0;
        lock();
        if (qget(q, "id", v, sizeof v) && hb_gp_id_ok(v)) snprintf(id, sizeof id, "%s", v);
        else snprintf(id, sizeof id, "%s", S.run);
        if (id[0] && hb_gps_drop(&S.gps, id)) {
            if (!strcmp(S.applied, id)) game_restore_locked();
            ok = 1;
        }
        unlock();
        if (ok) { games_save(); settings_save(); } else r = bad;
    } else if (strcmp(c, "tap") == 0) {
        if (qget(q, "on", v, sizeof v) && v[0] == '1') {
            unlink(TAP_OFF_PATH);
        } else {
            FILE *f = fopen(TAP_OFF_PATH, "w");
            if (f) fclose(f);
        }
    } else if (strcmp(c, "skip_add") == 0 || strcmp(c, "skip_del") == 0) {
        char ids[SKIP_MAX][10];
        int n = skip_load(ids), i, j = 0;
        if (!qget(q, "id", v, sizeof v) || !valid_title(v)) {
            r = bad;
        } else if (c[5] == 'a') {
            for (i = 0; i < n; i++)
                if (strcmp(ids[i], v) == 0) break;
            if (i == n) {
                if (n >= SKIP_MAX) {
                    r = bad;                              /* the list is full */
                } else {
                    snprintf(ids[n++], 10, "%s", v);
                    if (skip_save(ids, n) != 0) r = bad;
                }
            }
        } else {
            for (i = 0; i < n; i++)
                if (strcmp(ids[i], v) != 0) snprintf(ids[j++], 10, "%s", ids[i]);
            if (skip_save(ids, j) != 0) r = bad;
        }
    } else {
        r = bad;
    }
    /* Plain link (no JS, e.g. a browser that ignores the page's click
     * handler): go back to the page instead of showing JSON. */
    if (qget(q, "back", v, sizeof v) && v[0] == '1') {
        static const char k_back[] = "HTTP/1.1 303 See Other\r\nLocation: /\r\n"
                                     "Cache-Control: no-store\r\nContent-Length: 0\r\n"
                                     "Connection: close\r\n\r\n";
        send_all(fd, k_back, sizeof k_back - 1);
        return;
    }
    respond(fd, "application/json", r, strlen(r));
}


/* ---- /log page ---------------------------------------------------------- */

static const char k_log_html[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>HearBridge PS4 log</title><style>"
"*{box-sizing:border-box}html,body{height:100%}"
"body{margin:0;background:#121218;color:#eceef2;font-family:Arial,Helvetica,sans-serif;"
"display:-webkit-flex;display:flex;-webkit-flex-direction:column;flex-direction:column;overflow-x:hidden}"
".bar{-webkit-flex:none;flex:none;padding:10px 14px;background:#1b1c24;border-bottom:2px solid #2a2c36}"
".bar b{font-size:20px;margin-right:14px}"
".b{display:inline-block;margin:3px 6px 3px 0;padding:10px 18px;background:#262833;border:2px solid #3a3d48;color:#eceef2;"
"font-size:17px;font-weight:700;text-decoration:none;cursor:pointer}"
".b:focus{outline:none;border-color:#fff;box-shadow:0 0 0 3px #c6f432}.b.on{border-color:#c6f432;color:#c6f432}"
"#st{color:#8a8f9c;font-size:14px;margin-left:8px}"
"#log{-webkit-flex:1;flex:1;min-height:0;margin:0;padding:10px 14px;font:14px/19px Consolas,Menlo,monospace;color:#b8ff6a;"
"white-space:pre-wrap;word-wrap:break-word;overflow-wrap:break-word;overflow-y:auto}"
"@media (max-width:860px){.bar b{display:block;margin-bottom:4px}.b{padding:12px 16px;min-width:76px;text-align:center}"
"#st{display:block;margin:4px 0 0}#log{font-size:13px;line-height:18px;padding:8px 10px}}"
"</style></head><body><div class=\"bar\"><b>Log</b>"
"<a class=\"b\" id=\"bk\" href=\"/\">Back</a>"
"<a class=\"b\" id=\"pz\" href=\"#\">Pause</a>"
"<a class=\"b\" id=\"cp\" href=\"#\">Copy</a>"
"<a class=\"b\" id=\"dl\" href=\"/log.txt?dl=1\">Download</a>"
"<span id=\"st\"></span></div><pre id=\"log\">Loading...</pre><script>"
"var paused=0,tz=new Date().getTimezoneOffset(),box=document.getElementById('log'),st=document.getElementById('st');"
"document.getElementById('dl').href='/log.txt?dl=1&tz='+tz;"
"function load(){if(paused)return;var x=new XMLHttpRequest();"
"x.onreadystatechange=function(){if(x.readyState!==4)return;"
"if(x.status===200){var atEnd=box.scrollTop+box.clientHeight>=box.scrollHeight-30;"
"if(box.textContent!==x.responseText){box.textContent=x.responseText;if(atEnd)box.scrollTop=box.scrollHeight;}"
"st.innerHTML='updated '+new Date().toTimeString().substr(0,8);}else{st.innerHTML='no answer';}};"
"x.open('GET','/log.txt?tz='+tz+'&t='+new Date().getTime(),true);x.send();}"
"document.getElementById('pz').onclick=function(){paused=!paused;this.innerHTML=paused?'Resume':'Pause';this.className='b'+(paused?' on':'');load();return false;};"
"document.getElementById('cp').onclick=function(){var t=document.createElement('textarea');t.value=box.textContent;"
"document.body.appendChild(t);t.select();var ok=false;try{ok=document.execCommand('copy');}catch(e){}document.body.removeChild(t);"
"st.innerHTML=ok?'copied':'copy not allowed here, use Download';return false;};"
"load();setInterval(load,2000);"
"</script></body></html>";

/* Newest lines (up to n) as "HH:MM:SS text", oldest first. tz is the
 * browser's getTimezoneOffset() in minutes (UTC minus local). */
static void send_log_text(int fd, int tz, int n, int download)
{
    char hdr[256], tmp[LOGR_LEN + 16];
    unsigned total, first, i;
    int h = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Cache-Control: no-store\r\n%sConnection: close\r\n\r\n",
                     download ? "Content-Disposition: attachment; filename=\"hearbridge-ps4-log.txt\"\r\n" : "");
    send_all(fd, hdr, (size_t)h);
    lock();
    total = g_lr_n;
    unlock();
    if (n < 1 || n > LOGR_LINES) n = LOGR_LINES;
    first = total > (unsigned)n ? total - (unsigned)n : 0;
    if (total > LOGR_LINES && first < total - LOGR_LINES) first = total - LOGR_LINES;
    for (i = first; i < total; i++) {
        long t;
        int len;
        lock();
        if (g_lr_n - i > LOGR_LINES) {      /* overwritten while sending */
            unlock();
            continue;
        }
        t = g_lr[i % LOGR_LINES].t - (long)tz * 60;
        len = snprintf(tmp, sizeof tmp, "%02ld:%02ld:%02ld %s\n", (t / 3600) % 24, (t / 60) % 60,
                       t % 60, g_lr[i % LOGR_LINES].s);
        unlock();
        if (len > (int)sizeof tmp - 1) len = (int)sizeof tmp - 1;
        send_all(fd, tmp, (size_t)len);
    }
}

/* Web app manifest: lets a phone add the page to its home screen as an app. */
static const char k_manifest[] =
"{\"name\":\"HearBridge\",\"short_name\":\"HearBridge\","
"\"description\":\"Bluetooth audio for PS4\","
"\"start_url\":\"/\",\"scope\":\"/\",\"display\":\"standalone\","
"\"orientation\":\"portrait\","
"\"theme_color\":\"#0e0f14\",\"background_color\":\"#0e0f14\","
"\"icons\":["
"{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any\"},"
"{\"src\":\"/icon-512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\",\"purpose\":\"any\"}"
"]}";

/* Value of a request header (name without the colon, case-insensitive). */
static int hdr_get(const char *hdrs, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name), i;
    const char *p = hdrs;
    out[0] = 0;
    while (p && (p = strstr(p, "\r\n")) != NULL) {
        size_t k = 0;
        const char *v;
        p += 2;
        if (!*p || *p == '\r') break;
        for (i = 0; i < nl; i++) {
            char a = p[i], b = name[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (i < nl || p[nl] != ':') continue;
        v = p + nl + 1;
        while (*v == ' ' || *v == '\t') v++;
        while (*v && *v != '\r' && *v != '\n' && k + 1 < n) out[k++] = *v++;
        while (k && (out[k - 1] == ' ' || out[k - 1] == '\t')) k--;
        out[k] = 0;
        return 1;
    }
    return 0;
}

static int name_ends(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a > b && strcmp(s + a - b, suf) == 0;
}

/* Host header: an IP address, or a name that cannot be reached over the public
 * DNS (no dot, .local, .lan, .home.arpa). A page on some other domain that was
 * pointed at the console (DNS rebinding) has a public name and is refused. */
static int host_ok(const char *h)
{
    char b[64];
    size_t n = 0;
    unsigned a1, a2, a3, a4;
    char tail;
    if (!h[0]) return 1;                           /* HTTP/1.0 client without Host */
    if (h[0] == '[') return 1;                     /* IPv6 literal */
    while (h[n] && h[n] != ':' && n + 1 < sizeof b) {
        b[n] = (h[n] >= 'A' && h[n] <= 'Z') ? (char)(h[n] + 32) : h[n];
        n++;
    }
    b[n] = 0;
    if (h[n] && h[n] != ':') return 0;             /* overlong */
    if (sscanf(b, "%u.%u.%u.%u%c", &a1, &a2, &a3, &a4, &tail) == 4) return 1;
    if (!strchr(b, '.')) return 1;
    return name_ends(b, ".local") || name_ends(b, ".lan") || name_ends(b, ".home.arpa");
}

/* State-changing request: the token goes in the X-HB-Token header (the page's
 * XHR) or in the k= query value (plain links), and a browser's Origin, when
 * there is one, must be this very page. */
static int token_ok(const char *tok, const char *host, const char *origin)
{
    size_t i, l = strlen(tok);
    unsigned diff = 0;
    char want[80];
    if (!g_ui_tok[0] || l != strlen(g_ui_tok)) return 0;
    for (i = 0; i < l; i++) diff |= (unsigned)(tok[i] ^ g_ui_tok[i]);
    if (diff) return 0;
    if (origin[0]) {
        snprintf(want, sizeof want, "http://%s", host);
        if (!host[0] || strcmp(origin, want) != 0) return 0;
    }
    return 1;
}

static void handle(int fd, int peer_loopback)
{
    static char req[4096];
    static char body[32768];
    char host[72], origin[96], tok[48], qtok[48];
    size_t n = 0;
    char *path, *sp, *q, *hdrs;
    int post;
    uint64_t t0 = sceKernelGetProcessTime();

    g_conn_end = t0 + CONN_TOTAL_US;
    while (n + 1 < sizeof req && sceKernelGetProcessTime() - t0 < REQ_READ_US) {
        ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[n] = 0;
    if (!strstr(req, "\r\n\r\n")) return;           /* incomplete or too slow */
    post = strncmp(req, "POST ", 5) == 0;
    if (strncmp(req, "GET ", 4) != 0 && !post) return;
    hdrs = strstr(req, "\r\n");
    hdr_get(hdrs, "Host", host, sizeof host);
    hdr_get(hdrs, "Origin", origin, sizeof origin);
    hdr_get(hdrs, "X-HB-Token", tok, sizeof tok);
    path = strchr(req, ' ') + 1;
    sp = strchr(path, ' ');
    if (sp) *sp = 0;
    q = strchr(path, '?');
    if (q) *q++ = 0;
    if (!host_ok(host)) {
        respond_403(fd);
        return;
    }
    qtok[0] = 0;
    if (!tok[0] && q && !qget(q, "k", qtok, sizeof qtok)) qtok[0] = 0;
    if (!tok[0]) snprintf(tok, sizeof tok, "%s", qtok);

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        respond(fd, "text/html; charset=utf-8", webui_html, sizeof webui_html - 1);
    } else if (strcmp(path, "/manifest.webmanifest") == 0) {
        respond(fd, "application/manifest+json", k_manifest, sizeof k_manifest - 1);
    } else if (strcmp(path, "/icon-192.png") == 0) {
        respond(fd, "image/png", (const char *)webui_icon192, sizeof webui_icon192);
    } else if (strcmp(path, "/icon-512.png") == 0) {
        respond(fd, "image/png", (const char *)webui_icon512, sizeof webui_icon512);
    } else if (strcmp(path, "/apple-touch-icon.png") == 0 || strcmp(path, "/apple-touch-icon-precomposed.png") == 0) {
        respond(fd, "image/png", (const char *)webui_icon180, sizeof webui_icon180);
    } else if (strcmp(path, "/log") == 0) {
        respond(fd, "text/html; charset=utf-8", k_log_html, sizeof k_log_html - 1);
    } else if (strcmp(path, "/log.txt") == 0) {
        char v[16];
        int tz = 0, n = 0;
        if (q && qget(q, "tz", v, sizeof v)) tz = atoi(v);
        if (tz < -840 || tz > 840) tz = 0;
        if (q && qget(q, "n", v, sizeof v)) n = atoi(v);
        send_log_text(fd, tz, n, q && qget(q, "dl", v, sizeof v) && v[0] == '1');
    } else if (strcmp(path, "/api/status") == 0) {
        struct sb b = { body, 0, sizeof body, 0 };
        build_status(&b);
        if (b.ov) respond_503(fd);
        else respond(fd, "application/json", body, b.n);
    } else if (strcmp(path, "/api/cmd") == 0) {
        if (!token_ok(tok, host, origin)) respond_403(fd);
        else handle_cmd(fd, q ? q : "");
    } else if (strcmp(path, "/api/quit") == 0) {
        /* A newer instance takes over: stop like "Stop HearBridge"; main
         * then closes this server (ui_stop) before it returns. The newer
         * instance runs on this console and asks over loopback; anything else
         * needs the token. */
        char r[96];
        if (!peer_loopback && !token_ok(tok, host, origin)) {
            respond_403(fd);
            return;
        }
        int n = snprintf(r, sizeof r, "{\"ok\":true,\"version\":\"%s\"}", S.version);
        respond(fd, "application/json", r, (size_t)n);
        put_cmd(UI_CMD_STOP, NULL);
    } else {
        respond_404(fd);
    }
}

void ui_local_ip(char *out, int outlen)
{
    struct sockaddr_in a, me;
    socklen_t ml = sizeof me;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    snprintf(out, (size_t)outlen, "127.0.0.1");
    if (fd < 0) return;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x08080808u); /* no packet is sent by connect() */
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(fd, (struct sockaddr *)&me, &ml) == 0 && me.sin_addr.s_addr != 0) {
        unsigned char *b = (unsigned char *)&me.sin_addr.s_addr;
        snprintf(out, (size_t)outlen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    close(fd);
}

static int g_lsock = -1;
static int g_port;
static volatile int g_srv_quit;
static void *g_srv_thr;
static int g_srv_running;

static void *server_main(void *arg)
{
    (void)arg;
    while (!g_srv_quit) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int fd = accept(g_lsock, (struct sockaddr *)&ca, &cl);
        if (g_srv_quit) {
            if (fd >= 0) close(fd);
            break;
        }
        if (fd < 0) {
            sceKernelUsleep(100000);
            continue;
        }
        {
            struct timeval tv = { 1, 0 };   /* short, handle() keeps an overall deadline */
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }
        handle(fd, ((unsigned char *)&ca.sin_addr.s_addr)[0] == 127);
        close(fd);
    }
    return NULL;
}

int ui_port(void)
{
    return g_srv_running ? g_port : 0;
}

int ui_start(const char *version, int port)
{
    struct sockaddr_in a;
    int one = 1, err;

    snprintf(S.version, sizeof S.version, "%s", version);
    token_init();
    if (!S.state[0]) snprintf(S.state, sizeof S.state, "starting");
    if (!g_srv_running) settings_load();
    if (g_srv_running) return 0;

    g_lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_lsock < 0) return errno ? errno : -1;
    setsockopt(g_lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_lsock, (struct sockaddr *)&a, sizeof a) != 0 || listen(g_lsock, 8) != 0) {
        err = errno ? errno : -1;
        close(g_lsock);
        g_lsock = -1;
        return err;
    }
    g_srv_quit = 0;
    if (scePthreadCreate(&g_srv_thr, NULL, server_main, NULL, "hb_webui") != 0) {
        close(g_lsock);
        g_lsock = -1;
        return -1;
    }
    g_port = port;
    g_srv_running = 1;
    {
        FILE *f;
        mkdir("/data/hearbridge", 0777);
        if ((f = fopen(UI_PORT_FILE, "w"))) {
            fprintf(f, "%d\n", port);
            fclose(f);
        }
    }
    return 0;
}

void ui_stop(void)
{
    struct sockaddr_in a;
    int fd;
    settings_flush_due(1);
    if (!g_srv_running) return;
    g_srv_quit = 1;
    /* Wake accept(): shut the socket down, and connect once in case this
     * kernel keeps accept() blocked on a shut-down socket. */
    shutdown(g_lsock, SHUT_RDWR);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct timeval tv = { 1, 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        memset(&a, 0, sizeof a);
        a.sin_len = sizeof a;
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)g_port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(fd, (struct sockaddr *)&a, sizeof a);
        close(fd);
    }
    scePthreadJoin(g_srv_thr, NULL);  /* a client in progress ends within ~4 s */
    close(g_lsock);
    g_lsock = -1;
    g_srv_running = 0;
    unlink(UI_PORT_FILE);
}

#endif /* HEARBRIDGE_PAYLOAD */
