/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Output gain with a soft limiter. The PCM sent to the headphones is often
 * quiet (games leave a lot of headroom), so the page has a Boost slider:
 * 100 % leaves the signal as it is, below that it is turned down, above it
 * the gain goes up to x5. The gain is applied in 32 bits and then passed
 * through a soft knee: samples below the knee are untouched, above it the curve bends
 * smoothly toward full scale and never reaches it, so a loud passage is
 * compressed instead of hard clipped (hard clipping is what sounds like
 * crackle). Header only, host tests include it. */
#ifndef HEARBRIDGE_BOOST_H
#define HEARBRIDGE_BOOST_H

#include <stdint.h>

#define HB_LOUD_MAX      500         /* boost slider, percent; 100 = unchanged, 500 = x5 */
#define HB_LOUD_DEFAULT  100         /* unified volume meter starts at 100% */

/* Gain in 1/256 steps for a loudness percentage (0..HB_LOUD_MAX). */
static inline int32_t hb_gain_q8(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > HB_LOUD_MAX) pct = HB_LOUD_MAX;
    return (int32_t)pct * 256 / 100;
}

#define HB_KNEE   19661              /* 0.6 of full scale: linear below this */
#define HB_KNEE_EQ 29491             /* 0.9 of full scale: knee used with the equalizer at unity gain */
#define HB_FULL   32767

/* Soft limit of one sample already multiplied by the gain (may be far over
 * full scale). Output is within -32767..32767, odd-symmetric, monotonic, with
 * slope 1 at the knee. */
static inline int16_t hb_soft_limit_knee(int32_t v, int32_t knee)
{
    int32_t a = v < 0 ? -v : v;
    int32_t y;
    if (a <= knee) return (int16_t)v;
    {
        int64_t e = (int64_t)a - knee;
        int64_t r = HB_FULL - knee;
        y = (int32_t)(knee + e * r / (r + e));
    }
    if (y > HB_FULL) y = HB_FULL;
    return (int16_t)(v < 0 ? -y : y);
}

static inline int16_t hb_soft_limit(int32_t v)
{
    return hb_soft_limit_knee(v, HB_KNEE);
}

/* gain_q8: total gain (the loudness), 256 = unchanged. At or below
 * unity it is a plain scale (nothing can clip), above it the soft limiter
 * follows. n is the number of samples (frames times channels). */
static inline void hb_apply_gain(int16_t *pcm, int n, int32_t gain_q8)
{
    int i;
    if (gain_q8 == 256) return;
    if (gain_q8 < 256) {
        for (i = 0; i < n; i++) pcm[i] = (int16_t)(((int32_t)pcm[i] * gain_q8) >> 8);
        return;
    }
    for (i = 0; i < n; i++) pcm[i] = hb_soft_limit(((int32_t)pcm[i] * gain_q8) >> 8);
}

#endif
