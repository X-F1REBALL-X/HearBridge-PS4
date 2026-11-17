/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HearBridge launcher: installs GoldHEN plugins for game audio (14.02 path).
 * Copies game_tap, boot, daemon, and libSceUsbd into /data/GoldHEN/plugins/,
 * ensures plugins.ini [default] lists game_tap + boot, and turns on
 * PluginLoader_Enabled. Original plugins.ini / config.ini kept as .bak once. */
#include "install.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef HB_ROOT
#define HB_ROOT      ""
#endif
#define GH_DIR       HB_ROOT "/data/GoldHEN"
#define PLUGIN_DIR   GH_DIR "/plugins"
#define TAP_LOG_DIR  GH_DIR "/hearbridge"
#define PLUGINS_INI  GH_DIR "/plugins.ini"
#define CONFIG_INI   GH_DIR "/config.ini"

struct install_file {
    const char *src;
    const char *dst;
    const char *tag;
};

static const struct install_file k_files[] = {
    { HB_ROOT "/app0/hearbridge_game_tap.prx", PLUGIN_DIR "/hearbridge_game_tap.prx", "game_tap" },
    { HB_ROOT "/app0/hearbridge_boot.prx",     PLUGIN_DIR "/hearbridge_boot.prx",     "boot" },
    { HB_ROOT "/app0/hearbridge_daemon.prx",   PLUGIN_DIR "/hearbridge_daemon.prx",   "daemon" },
    { HB_ROOT "/app0/libSceUsbd.sprx",         PLUGIN_DIR "/libSceUsbd.sprx",         "usbd" },
};

static const char *const k_ini_lines[] = {
    "/data/GoldHEN/plugins/hearbridge_game_tap.prx",
    "/data/GoldHEN/plugins/hearbridge_boot.prx",
};

static void (*L)(const char *fmt, ...);

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL, *nb;
    size_t cap = 0, n = 0, r;
    if (!f) return NULL;
    do {
        if (n + 1 >= cap) {
            cap = cap ? cap * 2 : 65536;
            if (!(nb = realloc(buf, cap))) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
    } while (r > 0);
    fclose(f);
    buf[n] = 0;
    *len = n;
    return buf;
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int write_atomic(const char *path, const void *d, size_t n)
{
    char tmp[256];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!(f = fopen(tmp, "wb"))) return -1;
    ok = fwrite(d, 1, n, f) == n;
    if (ok && fflush(f) != 0) ok = 0;
    if (ok && fsync((fileno)(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    /* LoadStartModule needs +x; fopen creates 0666 and games get EACCES (0x8002000d). */
    chmod(path, 0777);
    return 0;
}

static void backup_once(const char *path, const unsigned char *d, size_t n)
{
    char bak[256];
    snprintf(bak, sizeof bak, "%s.bak", path);
    if (file_exists(bak)) {
        L("backup %s already there, kept", bak);
        return;
    }
    L("backup %s: %s", bak, write_atomic(bak, d, n) == 0 ? "ok" : "FAILED");
}

static unsigned fnv1a(const unsigned char *d, size_t n)
{
    unsigned h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ d[i]) * 16777619u;
    return h;
}

static int install_one_file(const struct install_file *f)
{
    size_t sl = 0, dl = 0;
    unsigned char *src = slurp(f->src, &sl), *dst;
    int rc;
    if (!src || sl == 0) {
        L("%s: cannot read %s", f->tag, f->src);
        free(src);
        return -1;
    }
    dst = slurp(f->dst, &dl);
    if (dst && dl == sl && memcmp(src, dst, sl) == 0) {
        L("%s: %s up to date (%zu bytes, fnv %08x)", f->tag, f->dst, sl, fnv1a(src, sl));
        chmod(f->dst, 0777);
        free(src);
        free(dst);
        return 0;
    }
    if (dst) L("%s: %s differs (%zu -> %zu bytes), replacing", f->tag, f->dst, dl, sl);
    else L("%s: %s missing, installing (%zu bytes, fnv %08x)", f->tag, f->dst, sl, fnv1a(src, sl));
    rc = write_atomic(f->dst, src, sl);
    L("%s: write %s", f->tag, rc == 0 ? "ok" : "FAILED");
    free(src);
    free(dst);
    return rc == 0 ? 1 : -1;
}

static int install_plugin_files(void)
{
    int i, worst = 0, rc;
    for (i = 0; i < (int)(sizeof k_files / sizeof k_files[0]); i++) {
        rc = install_one_file(&k_files[i]);
        if (rc < 0) worst = -1;
        else if (rc > 0 && worst >= 0) worst = 1;
    }
    return worst;
}

static void trim(const char *b, const char *e, const char **ob, const char **oe)
{
    while (b < e && (*b == ' ' || *b == '\t' || *b == '\r' || (unsigned char)*b == 0xEF ||
                     (unsigned char)*b == 0xBB || (unsigned char)*b == 0xBF))
        b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    *ob = b;
    *oe = e;
}

static int span_eq(const char *b, const char *e, const char *s)
{
    size_t n = strlen(s);
    return (size_t)(e - b) == n && memcmp(b, s, n) == 0;
}

static int span_ieq(const char *b, const char *e, const char *s)
{
    size_t n = strlen(s), i;
    if ((size_t)(e - b) != n) return 0;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)b[i]) != tolower((unsigned char)s[i])) return 0;
    return 1;
}

static int line_in_default(const char *txt, size_t n, const char *want)
{
    const char *p;
    int in_default = 0;
    for (p = txt; p < txt + n;) {
        const char *le = memchr(p, '\n', (size_t)(txt + n - p));
        const char *next = le ? le + 1 : txt + n, *b, *e;
        if (!le) le = txt + n;
        trim(p, le, &b, &e);
        if (b < e && *b == '[') {
            in_default = 0;
            {
                const char *cb = b + 1, *ce = memchr(b, ']', (size_t)(e - b));
                const char *tb, *te;
                if (ce) {
                    trim(cb, ce, &tb, &te);
                    if (span_ieq(tb, te, "default")) in_default = 1;
                }
            }
        } else if (in_default && b < e && *b != ';' && *b != '#') {
            const char *ke = memchr(b, '=', (size_t)(e - b)), *tb, *te;
            if (!ke) ke = e;
            trim(b, ke, &tb, &te);
            if (span_eq(tb, te, want)) return 1;
        }
        p = next;
    }
    return 0;
}

static int ensure_ini_line(const char *want)
{
    size_t n = 0;
    char *txt = (char *)slurp(PLUGINS_INI, &n), *out;
    const char *p, *eol = "\n";
    const char *ins_at = NULL;
    int in_default = 0, have_default = 0;
    size_t on = 0, cap;
    int rc;

    if (!txt) {
        char fresh[256];
        int fn = snprintf(fresh, sizeof fresh, "[default]\n%s\n", want);
        L("plugins.ini: missing, creating with %s", want);
        rc = write_atomic(PLUGINS_INI, fresh, (size_t)fn);
        L("plugins.ini: write %s", rc == 0 ? "ok" : "FAILED");
        return rc == 0 ? 1 : -1;
    }
    if (line_in_default(txt, n, want)) {
        L("plugins.ini: [default] already has %s", want);
        free(txt);
        return 0;
    }
    if (strstr(txt, "\r\n")) eol = "\r\n";
    for (p = txt; p < txt + n;) {
        const char *le = memchr(p, '\n', (size_t)(txt + n - p));
        const char *next = le ? le + 1 : txt + n, *b, *e;
        if (!le) le = txt + n;
        trim(p, le, &b, &e);
        if (b < e && *b == '[') {
            if (in_default) in_default = 0;
            {
                const char *cb = b + 1, *ce = memchr(b, ']', (size_t)(e - b));
                const char *tb, *te;
                if (ce) {
                    trim(cb, ce, &tb, &te);
                    if (span_ieq(tb, te, "default")) {
                        in_default = 1;
                        have_default = 1;
                        ins_at = next;
                    }
                }
            }
        } else if (in_default) {
            if (b < e && *b != ';' && *b != '#')
                ins_at = next;
        }
        p = next;
    }
    backup_once(PLUGINS_INI, (unsigned char *)txt, n);
    cap = n + strlen(want) + 64;
    if (!(out = malloc(cap))) { free(txt); return -1; }
    if (have_default) {
        size_t head = (size_t)(ins_at - txt);
        memcpy(out, txt, head);
        on = head;
        if (head > 0 && out[head - 1] != '\n') on += (size_t)sprintf(out + on, "%s", eol);
        on += (size_t)sprintf(out + on, "%s%s", want, eol);
        memcpy(out + on, txt + head, n - head);
        on += n - head;
        L("plugins.ini: added %s to [default]", want);
    } else {
        memcpy(out, txt, n);
        on = n;
        if (n > 0 && out[n - 1] != '\n') on += (size_t)sprintf(out + on, "%s", eol);
        on += (size_t)sprintf(out + on, "%s[default]%s%s%s", n > 0 ? eol : "", eol, want, eol);
        L("plugins.ini: appended [default] with %s", want);
    }
    rc = write_atomic(PLUGINS_INI, out, on);
    L("plugins.ini: write %s", rc == 0 ? "ok" : "FAILED");
    free(out);
    free(txt);
    return rc == 0 ? 1 : -1;
}

static int ensure_plugins_ini(void)
{
    int i, worst = 0, rc;
    for (i = 0; i < (int)(sizeof k_ini_lines / sizeof k_ini_lines[0]); i++) {
        rc = ensure_ini_line(k_ini_lines[i]);
        if (rc < 0) worst = -1;
        else if (rc > 0 && worst >= 0) worst = 1;
    }
    return worst;
}

static int ensure_plugin_loader(void)
{
    size_t n = 0;
    char *txt = (char *)slurp(CONFIG_INI, &n), *out;
    const char *p;
    int rc;

    if (!txt) {
        L("config.ini: %s not found (GoldHEN defaults in use), not created", CONFIG_INI);
        return 0;
    }
    for (p = txt; p < txt + n;) {
        const char *le = memchr(p, '\n', (size_t)(txt + n - p));
        const char *next = le ? le + 1 : txt + n, *b, *e, *eq, *kb, *ke, *vb, *ve;
        if (!le) le = txt + n;
        trim(p, le, &b, &e);
        eq = b < e ? memchr(b, '=', (size_t)(e - b)) : NULL;
        if (eq && *b != ';' && *b != '#') {
            trim(b, eq, &kb, &ke);
            trim(eq + 1, e, &vb, &ve);
            if (span_ieq(kb, ke, "PluginLoader_Enabled")) {
                const char *nv;
                size_t head, tail;
                L("config.ini: PluginLoader_Enabled=%.*s", (int)(ve - vb), vb);
                if (span_ieq(vb, ve, "1") || span_ieq(vb, ve, "true")) {
                    free(txt);
                    return 0;
                }
                nv = (ve > vb && isalpha((unsigned char)*vb)) ? "true" : "1";
                backup_once(CONFIG_INI, (unsigned char *)txt, n);
                head = (size_t)(vb - txt);
                tail = n - (size_t)(ve - txt);
                if (!(out = malloc(head + strlen(nv) + tail + 1))) {
                    free(txt);
                    return -1;
                }
                memcpy(out, txt, head);
                memcpy(out + head, nv, strlen(nv));
                memcpy(out + head + strlen(nv), ve, tail);
                rc = write_atomic(CONFIG_INI, out, head + strlen(nv) + tail);
                L("config.ini: PluginLoader_Enabled -> %s: %s", nv, rc == 0 ? "ok" : "FAILED");
                free(out);
                free(txt);
                return rc == 0 ? 1 : -1;
            }
        }
        p = next;
    }
    L("config.ini: no PluginLoader_Enabled key, left unchanged");
    free(txt);
    return 0;
}

void install_all(void (*logf)(const char *fmt, ...), struct install_result *r)
{
    L = logf;
    memset(r, 0, sizeof *r);
    mkdir(GH_DIR, 0777);
    mkdir(PLUGIN_DIR, 0777);
    mkdir(TAP_LOG_DIR, 0777);
    mkdir(HB_ROOT "/data/hearbridge", 0777);
    r->plugin = install_plugin_files();
    r->plugins_ini = ensure_plugins_ini();
    r->loader = ensure_plugin_loader();
    L("install: files %d, plugins.ini %d, plugin loader %d "
      "(1 changed, 0 ok, -1 failed)", r->plugin, r->plugins_ini, r->loader);
}
