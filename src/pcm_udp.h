/* SPDX-License-Identifier: GPL-3.0-or-later */
/* UDP PCM ingress for the HearBridge PS4 daemon (48 kHz stereo s16le). */
#ifndef HEARBRIDGE_PCM_UDP_H
#define HEARBRIDGE_PCM_UDP_H

#include <stddef.h>
#include <stdint.h>

#define PCM_UDP_PORT 9500
#define PCM_UDP_PORT_ALT 9501                       /* when 9500 stays held */
#define PCM_PORT_FILE "/data/hearbridge/pcm_port"   /* port in use, read by the tap */
#define TAP_STATUS_FILE "/data/GoldHEN/hearbridge/tap_status" /* "<TITLE> <pkts>", written by the tap */

/* Open a non-blocking UDP socket bound to 127.0.0.1:9500. Returns 0 on ok. */
int pcm_udp_open(void);
/* 0 ok, else errno of socket/bind (no SO_REUSEADDR: a held port fails). */
int pcm_udp_open_port(int port);
void pcm_udp_send_stop_port(int port);
void pcm_udp_close(void);

/* Drain the socket into an internal ring. Returns 1 if a "stop" command
 * arrived, 0 otherwise. */
int pcm_udp_poll(void);

/* Same-process feed from game_tap (bypasses UDP). nbytes = frames*4. */
void pcm_udp_feed(const void *data, int nbytes);

/* Send "stop" to 127.0.0.1:9500 (ask a running daemon to exit). */
void pcm_udp_send_stop(void);

/* Copy up to max_frames interleaved stereo s16le frames at the output rate
 * (see pcm_udp_set_out_rate). Returns frames. */
int pcm_udp_read_s16(int16_t *out, int max_frames);

/* Sample rate of the frames pcm_udp_read_s16 returns (default 48000). The
 * 48 kHz input is resampled to it. */
void pcm_udp_set_out_rate(int hz);

/* Jitter-buffer target from the UI latency control (20..190 ms). */
void pcm_udp_set_target_ms(int ms);
int  pcm_udp_target_ms(void);

/* Frames currently buffered. */
int pcm_udp_buffered(void);
int pcm_udp_overflows(void);
int pcm_udp_underruns(void);
/* Drift compensation: applied resample correction in ppm (positive:
 * the game delivers faster than we send), its slow (integral) part, and
 * the smoothed buffer fill in frames. */
int pcm_udp_drift_ppm(void);
int pcm_udp_drift_base_ppm(void);
int pcm_udp_fill_avg(void);
/* Port in use, PCM packets / frames received since open, tap
 * heartbeats ("HBTAP <title> <pkts>") seen, last tap title and its own
 * packet counter. */
int pcm_udp_port(void);
unsigned long pcm_udp_rx_pkts(void);
unsigned long pcm_udp_rx_frames(void);
unsigned long pcm_udp_hellos(void);
unsigned long pcm_udp_tap_pkts(void);
const char *pcm_udp_tap_title(void);

#endif
