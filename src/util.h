/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Small little-endian helpers used by the Realtek firmware loader.
 * now_ms() lives in hci_cmd_sync.c (host tests) or probe.c (the daemon). */
#ifndef HEARBRIDGE_UTIL_H
#define HEARBRIDGE_UTIL_H

#include <stdint.h>

static inline unsigned le16(const unsigned char *b)
{
    return (unsigned)(b[1] << 8 | b[0]);
}

static inline void put16(unsigned char *b, unsigned val)
{
    b[0] = (unsigned char)val;
    b[1] = (unsigned char)(val >> 8);
}

#endif
