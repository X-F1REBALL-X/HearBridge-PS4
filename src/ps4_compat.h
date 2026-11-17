/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Payload build only: keep the daemon on the same set of system imports
 * as 1.0.0 (no malloc/free/usleep/fputs/strcspn/strncat from the SDK). */
#ifndef HEARBRIDGE_PS4_COMPAT_H
#define HEARBRIDGE_PS4_COMPAT_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int sceKernelUsleep(unsigned int usec);

#define HB_ARENA_SIZE (3u * 1024u * 1024u)
static unsigned char hb_arena[HB_ARENA_SIZE] __attribute__((aligned(64), unused));
static size_t hb_arena_top __attribute__((unused));
static int hb_arena_live __attribute__((unused));

static inline __attribute__((unused)) void *hb_malloc(size_t n)
{
    size_t at = (hb_arena_top + 63u) & ~(size_t)63u;
    if (n == 0) n = 1;
    if (at > HB_ARENA_SIZE || n > HB_ARENA_SIZE - at) return NULL;
    hb_arena_top = at + n;
    hb_arena_live++;
    return hb_arena + at;
}
static inline __attribute__((unused)) void hb_free(void *p)
{
    if (!p) return;
    if (--hb_arena_live <= 0) { hb_arena_live = 0; hb_arena_top = 0; }
}
static inline __attribute__((unused)) size_t hb_strcspn(const char *s, const char *rej)
{
    size_t i;
    for (i = 0; s[i]; i++)
        if (strchr(rej, s[i])) break;
    return i;
}
static inline __attribute__((unused)) char *hb_strncat(char *d, const char *s, size_t n)
{
    size_t k = strlen(d), i = 0;
    while (i < n && s[i]) { d[k + i] = s[i]; i++; }
    d[k + i] = 0;
    return d;
}
static inline __attribute__((unused)) int hb_fputs(const char *s, FILE *f)
{
    return fprintf(f, "%s", s);
}
#define malloc(n) hb_malloc(n)
#define free(p) hb_free(p)
#define usleep(u) sceKernelUsleep(u)
#define strcspn(a, b) hb_strcspn(a, b)
#define strncat(a, b, n) hb_strncat(a, b, n)
#define fputs(s, f) hb_fputs(s, f)
#endif
