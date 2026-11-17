/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Preference for which USB Bluetooth stick HearBridge should open.
 * File: /data/hearbridge/adapter  one word: auto | usb | VID:PID (e.g. 0A12:0001). */
#ifndef HEARBRIDGE_ADAPTER_PREF_H
#define HEARBRIDGE_ADAPTER_PREF_H

#define HB_ADAPTER_PATH "/data/hearbridge/adapter"
#define HB_AD_MAX 8

enum {
    HB_SEL_AUTO = 0,
    HB_SEL_USB,      /* first USB stick */
    HB_SEL_ID        /* exact VID:PID in out_id */
};

/* Read preference. Writes "AAAA:BBBB" into out_id when HB_SEL_ID. */
int hb_adapter_selection(char *out_id, int cap);

/* Normalize and save. word: auto|usb|0A12:0001. 1 ok. */
int hb_adapter_pref_set(const char *word);

/* Current preference word for the status JSON ("auto", "usb", or "0A12:0001"). */
void hb_adapter_pref_word(char *out, int cap);

/* Format VID:PID uppercase into out. */
void hb_adapter_id_fmt(unsigned vid, unsigned pid, char *out, int cap);

#endif
