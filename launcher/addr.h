/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HearBridge launcher helpers that need no console APIs (host tests include
 * this file): reading a string field out of the daemon's /api/status reply and
 * the text of the in-app message that tells where the page is. */
#ifndef HB_LAUNCHER_ADDR_H
#define HB_LAUNCHER_ADDR_H

#include <stdio.h>
#include <string.h>

/* Copy the value of "key":"value" from a JSON text into out (cut at n-1).
 * 1 if found and not empty. Only plain strings without escapes, which is all
 * the daemon puts in the version and ip fields. */
static inline int hb_json_str(const char *json, const char *key, char *out, size_t n)
{
    char pat[40];
    const char *v;
    size_t len;
    if (n == 0) return 0;
    out[0] = 0;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    if (!(v = strstr(json, pat))) return 0;
    v += strlen(pat);
    len = strcspn(v, "\"");
    if (len == 0) return 0;
    if (len > n - 1) len = n - 1;
    memcpy(out, v, len);
    out[len] = 0;
    return 1;
}

/* A dotted IPv4 address that is worth showing (not empty, not loopback). */
static inline int hb_ip_ok(const char *ip)
{
    unsigned a, b, c, d;
    char tail;
    if (!ip || sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return a != 127 && a != 0;
}

/* Message of the tile's dialog. note (may be empty) comes first, e.g. the
 * plugin notice. ver is the launcher version. open_l / close_l are the names of the
 * buttons that open the browser and close the message ("X" / "Circle"). */
static inline void hb_ui_text(char *out, size_t n, const char *note, const char *ip, int port, const char *ver,
                                const char *open_l, const char *close_l)
{
    size_t k = 0;
    out[0] = 0;
    if (note && note[0]) k = (size_t)snprintf(out, n, "%s\n\n", note);
    if (k >= n) return;
    if (hb_ip_ok(ip))
        snprintf(out + k, n - k,
                 "HearBridge %s is running.\nA CSR8510 or Realtek RTL8761BU USB dongle must be plugged into the PS4.\n\nOpen http://%s:%d on a phone or PC on the same network.\n"
                 "On the PS4 itself use the web browser at http://127.0.0.1:%d.\n\n"
                 "First press OK. Then:\nPress %s to open the page in the PS4 web browser.\nPress %s to close, HearBridge keeps running.",
                 ver, ip, port, port, open_l, close_l);
    else
        snprintf(out + k, n - k,
                 "HearBridge %s is running.\nA CSR8510 or Realtek RTL8761BU USB dongle must be plugged into the PS4.\n\nOpen http://<console address>:%d on a phone or PC on the same "
                 "network (the address is in the HearBridge notification).\n"
                 "On the PS4 itself use the web browser at http://127.0.0.1:%d.\n\n"
                 "First press OK. Then:\nPress %s to open the page in the PS4 web browser.\nPress %s to close, HearBridge keeps running.",
                 ver, port, port, open_l, close_l);
}

#endif
