/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "adapter_pref.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void lower_copy(char *dst, int cap, const char *src)
{
    int i = 0;
    while (src[i] && i < cap - 1) {
        char c = src[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        i++;
    }
    dst[i] = 0;
}

void hb_adapter_id_fmt(unsigned vid, unsigned pid, char *out, int cap)
{
    if (!out || cap < 10) return;
    snprintf(out, (size_t)cap, "%04X:%04X", vid & 0xffffu, pid & 0xffffu);
}

static int parse_vidpid(const char *s, char *out_id, int cap)
{
    unsigned v = 0, p = 0;
    int i, n = 0;
    char buf[16];
    if (!s || !s[0]) return 0;
    for (i = 0; s[i] && n < (int)sizeof buf - 1; i++) {
        if (s[i] == '-' || s[i] == ' ') continue;
        buf[n++] = s[i];
    }
    buf[n] = 0;
    if (n != 9 || buf[4] != ':') return 0;
    for (i = 0; i < 4; i++) {
        int h = hexv(buf[i]);
        if (h < 0) return 0;
        v = (v << 4) | (unsigned)h;
    }
    for (i = 5; i < 9; i++) {
        int h = hexv(buf[i]);
        if (h < 0) return 0;
        p = (p << 4) | (unsigned)h;
    }
    hb_adapter_id_fmt(v, p, out_id, cap);
    return 1;
}

int hb_adapter_selection(char *out_id, int cap)
{
    FILE *f;
    char line[64], word[32];
    int n = 0, i;

    if (out_id && cap > 0) out_id[0] = 0;
    f = fopen(HB_ADAPTER_PATH, "r");
    if (!f) return HB_SEL_AUTO;
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    while (line[n] == ' ' || line[n] == '\t') n++;
    for (i = 0; line[n] && line[n] != ' ' && line[n] != '\t' && i < (int)sizeof word - 1; n++)
        word[i++] = line[n];
    word[i] = 0;
    lower_copy(line, (int)sizeof line, word);
    if (!line[0] || !strcmp(line, "auto")) return HB_SEL_AUTO;
    if (!strcmp(line, "usb") || !strcmp(line, "external") || !strcmp(line, "adapter"))
        return HB_SEL_USB;
    if (parse_vidpid(word, out_id, cap)) return HB_SEL_ID;
    return HB_SEL_AUTO;
}

void hb_adapter_pref_word(char *out, int cap)
{
    char id[16];
    int sel;
    if (!out || cap < 2) return;
    sel = hb_adapter_selection(id, (int)sizeof id);
    if (sel == HB_SEL_USB) snprintf(out, (size_t)cap, "usb");
    else if (sel == HB_SEL_ID) snprintf(out, (size_t)cap, "%s", id);
    else snprintf(out, (size_t)cap, "auto");
}

int hb_adapter_pref_set(const char *word)
{
    char norm[32], line[40], tmp[80], got[40];
    FILE *f;
    int i = 0;
    char id[16];

    if (!word || !word[0]) return 0;
    while (word[i] == ' ' || word[i] == '\t') i++;
    lower_copy(norm, (int)sizeof norm, word + i);
    if (!strcmp(norm, "auto"))
        snprintf(line, sizeof line, "auto\n");
    else if (!strcmp(norm, "usb") || !strcmp(norm, "external") || !strcmp(norm, "adapter"))
        snprintf(line, sizeof line, "usb\n");
    else if (parse_vidpid(word + i, id, (int)sizeof id))
        snprintf(line, sizeof line, "%s\n", id);
    else
        return 0;

    mkdir("/data/hearbridge", 0777);
    snprintf(tmp, sizeof tmp, "%s.tmp", HB_ADAPTER_PATH);
    f = fopen(tmp, "w");
    if (!f) return 0;
    if (fputs(line, f) < 0) { fclose(f); unlink(tmp); return 0; }
    if (fclose(f) != 0) { unlink(tmp); return 0; }
    if (rename(tmp, HB_ADAPTER_PATH) != 0) { unlink(tmp); return 0; }
    f = fopen(HB_ADAPTER_PATH, "r");
    if (!f) return 0;
    if (!fgets(got, sizeof got, f)) got[0] = 0;
    fclose(f);
    return got[0] && !strncmp(got, line, strlen(line) - 1);
}
