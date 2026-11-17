/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Game mode: the sound (equalizer and Boost) saved for one game, applied while
 * that game runs and put back to the usual sound when it closes. Pure helpers
 * (parsing, the profile list, what to do when the running game changes, the
 * game name from a param.sfo), host tested; webui.c holds the state.
 * Idea from HearBridge-PS5 (per-game profiles, X-F1REBALL-X), simplified:
 * one profile per game, no per-headset copies, no headphone volume. */
#ifndef HEARBRIDGE_GPROF_H
#define HEARBRIDGE_GPROF_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HB_GAMES_PATH
#define HB_GAMES_PATH  "/data/hearbridge/games.txt"
#endif
#define HB_GP_MAX      16                 /* games with a profile */
#define HB_GP_NAME     48
#define HB_GP_EQ       5

typedef struct {
    char id[10];                          /* CUSA01234 */
    char name[HB_GP_NAME];                /* may be empty */
    int eq_on;
    int eq_db[HB_GP_EQ];                  /* -12..12 */
    int boost;                            /* 0..500 */
} hb_gp;

typedef struct {
    hb_gp g[HB_GP_MAX];                   /* newest first */
    int n;
} hb_gps;

/* Four capital letters and five digits. */
static inline int hb_gp_id_ok(const char *t)
{
    int i;
    if (!t || strlen(t) != 9) return 0;
    for (i = 0; i < 4; i++) if (t[i] < 'A' || t[i] > 'Z') return 0;
    for (i = 4; i < 9; i++) if (t[i] < '0' || t[i] > '9') return 0;
    return 1;
}

static inline int hb_gp_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* "a,b,c,d,e" into five values (clamped -12..12). 1 when all five were there. */
static inline int hb_eq_parse(const char *s, int db[HB_GP_EQ])
{
    int i;
    for (i = 0; i < HB_GP_EQ; i++) db[i] = 0;
    for (i = 0; i < HB_GP_EQ; i++) {
        char *end;
        long v;
        if (!s || !*s) return 0;
        v = strtol(s, &end, 10);
        if (end == s) return 0;
        db[i] = hb_gp_clamp((int)v, -12, 12);
        s = end;
        if (i < HB_GP_EQ - 1) {
            if (*s != ',') return 0;
            s++;
        }
    }
    return 1;
}

static inline void hb_eq_format(const int db[HB_GP_EQ], char *out, size_t n)
{
    snprintf(out, n, "%d,%d,%d,%d,%d", db[0], db[1], db[2], db[3], db[4]);
}

/* One line: "CUSA01234 eq=1 db=0,0,0,0,0 boost=180 name=Some Game". name last,
 * to the end of the line. Unknown words are ignored. 1 for a usable line. */
static inline int hb_gp_parse_line(const char *line, hb_gp *g)
{
    char id[16];
    const char *p;
    memset(g, 0, sizeof *g);
    g->boost = 100;
    if (sscanf(line, "%15s", id) != 1 || !hb_gp_id_ok(id)) return 0;
    snprintf(g->id, sizeof g->id, "%s", id);
    p = line + strlen(id);
    while (*p) {
        while (*p == ' ') p++;
        if (strncmp(p, "name=", 5) == 0) {
            size_t k = 0;
            p += 5;
            while (*p && *p != '\n' && *p != '\r' && k + 1 < sizeof g->name) g->name[k++] = *p++;
            g->name[k] = 0;
            break;
        } else if (strncmp(p, "eq=", 3) == 0) {
            g->eq_on = atoi(p + 3) != 0;
        } else if (strncmp(p, "db=", 3) == 0) {
            hb_eq_parse(p + 3, g->eq_db);
        } else if (strncmp(p, "boost=", 6) == 0) {
            g->boost = hb_gp_clamp(atoi(p + 6), 0, 500);
        }
        while (*p && *p != ' ') p++;
    }
    return 1;
}

static inline void hb_gps_parse(hb_gps *gs, const char *text)
{
    char line[160];
    gs->n = 0;
    while (text && *text && gs->n < HB_GP_MAX) {
        size_t k = 0;
        hb_gp g;
        while (*text && *text != '\n' && k + 1 < sizeof line) line[k++] = *text++;
        line[k] = 0;
        while (*text && *text != '\n') text++;       /* a too long line loses its tail */
        if (*text == '\n') text++;
        if (hb_gp_parse_line(line, &g)) {
            int i, dup = 0;
            for (i = 0; i < gs->n; i++) if (!strcmp(gs->g[i].id, g.id)) dup = 1;
            if (!dup) gs->g[gs->n++] = g;
        }
    }
}

static inline int hb_gps_format(const hb_gps *gs, char *out, int max)
{
    int i, k = 0;
    out[0] = 0;
    for (i = 0; i < gs->n; i++) {
        char db[40], name[HB_GP_NAME], *q;
        int w;
        hb_eq_format(gs->g[i].eq_db, db, sizeof db);
        snprintf(name, sizeof name, "%s", gs->g[i].name);
        for (q = name; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
        w = snprintf(out + k, (size_t)(max - k), "%s eq=%d db=%s boost=%d name=%s\n",
                     gs->g[i].id, gs->g[i].eq_on ? 1 : 0, db, gs->g[i].boost, name);
        if (w < 0 || k + w >= max) { out[k] = 0; break; }
        k += w;
    }
    return k;
}

static inline int hb_gps_find(const hb_gps *gs, const char *id)
{
    int i;
    for (i = 0; i < gs->n; i++) if (!strcmp(gs->g[i].id, id)) return i;
    return -1;
}

/* Add or replace, newest first; past HB_GP_MAX the oldest goes. */
static inline void hb_gps_put(hb_gps *gs, const hb_gp *g)
{
    int i = hb_gps_find(gs, g->id), k;
    if (i < 0) {
        if (gs->n < HB_GP_MAX) gs->n++;
        i = gs->n - 1;
    }
    for (k = i; k > 0; k--) gs->g[k] = gs->g[k - 1];
    gs->g[0] = *g;
}

static inline int hb_gps_drop(hb_gps *gs, const char *id)
{
    int i = hb_gps_find(gs, id), k;
    if (i < 0) return 0;
    for (k = i; k + 1 < gs->n; k++) gs->g[k] = gs->g[k + 1];
    gs->n--;
    return 1;
}

/* 1 when the live sound is not what the profile saved. */
static inline int hb_gp_differs(const hb_gp *g, int eq_on, const int db[HB_GP_EQ], int boost)
{
    int i;
    if (g->boost != boost || (g->eq_on != 0) != (eq_on != 0)) return 1;
    /* Compare bands even when EQ is off, so Quick presets / Custom light
     * Update Game Profile as soon as the saved curve differs. */
    for (i = 0; i < HB_GP_EQ; i++) if (g->eq_db[i] != db[i]) return 1;
    return 0;
}

/* What a change of the running game means for the sound. run = game seen on
 * the last tick ("" none), cur = game now.
 * START: a game began. STOP: it ended. SWITCH: another game took over
 * (STOP then START). KEEP: no change. The caller takes the usual sound as it
 * is at START, applies the game's profile if it has one, and puts the usual
 * sound back at STOP when a profile was on. */
enum { HB_GM_KEEP = 0, HB_GM_START = 1, HB_GM_STOP = 2, HB_GM_SWITCH = 3 };
static inline int hb_gm_decide(const char *run, const char *cur)
{
    if (!strcmp(run, cur)) return HB_GM_KEEP;
    if (!run[0]) return HB_GM_START;
    if (!cur[0]) return HB_GM_STOP;
    return HB_GM_SWITCH;
}

/* Game title (TITLE) of a param.sfo image; 1 when found. Plain SFO: magic
 * "\0PSF", key table offset at 8, data table offset at 12, entry count at 16,
 * 16 byte entries (key offset u16, format u16, length u32, max u32, data
 * offset u32). */
static inline int hb_sfo_title(const unsigned char *b, size_t n, char *out, size_t outn)
{
    uint32_t kt, dt, cnt, i;
    if (n < 20 || memcmp(b, "\0PSF", 4)) return 0;
    kt = (uint32_t)b[8] | (uint32_t)b[9] << 8 | (uint32_t)b[10] << 16 | (uint32_t)b[11] << 24;
    dt = (uint32_t)b[12] | (uint32_t)b[13] << 8 | (uint32_t)b[14] << 16 | (uint32_t)b[15] << 24;
    cnt = (uint32_t)b[16] | (uint32_t)b[17] << 8 | (uint32_t)b[18] << 16 | (uint32_t)b[19] << 24;
    if (kt >= n || dt >= n || cnt > 64) return 0;
    for (i = 0; i < cnt; i++) {
        const unsigned char *e = b + 20 + i * 16;
        uint32_t ko, len, doff, k;
        if (20 + (i + 1) * 16 > n) return 0;
        ko = (uint32_t)e[0] | (uint32_t)e[1] << 8;
        len = (uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
        doff = (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
        if (kt + ko >= n || strncmp((const char *)b + kt + ko, "TITLE", 6) != 0) continue;
        if (dt + doff >= n) return 0;
        for (k = 0; k < len && dt + doff + k < n && k + 1 < outn && b[dt + doff + k]; k++)
            out[k] = (char)b[dt + doff + k];
        out[k] = 0;
        return k > 0;
    }
    return 0;
}

#endif
