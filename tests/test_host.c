/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host tests for the pure parts: class filter (cod.h) and resampler
 * (resample.h). Run with: make test */
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cod.h"
#include "../src/resample.h"
#include "../src/cmdq.h"
#include "../src/hciframe.h"
#include "../plugin/tap_rate.h"
#include "../plugin/tap_convert.h"
#include "../plugin/tap_diag.h"
#include "../src/boost.h"
#include "../src/pager.h"
#include "../src/eq.h"
#include "../src/gprof.h"
#include "../launcher/addr.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void test_cod(void)
{
    /* devices seen in the field */
    CHECK(cod_is_audio_sink(0x240404));     /* earbuds style: headset */
    CHECK(cod_is_audio_sink(0x240418));     /* Coral CM835 speaker: headphones minor */
    CHECK(cod_is_audio_sink(0x240414));     /* loudspeaker */
    CHECK(cod_is_audio_sink(0x240408));     /* hands-free */
    CHECK(cod_is_audio_sink(0x24041C));     /* portable audio */
    CHECK(cod_is_audio_sink(0x240428));     /* hi-fi */
    CHECK(cod_is_audio_sink(0x200420));     /* real car audio: no capturing bit */
    /* not audio sinks */
    CHECK(!cod_is_audio_sink(0x3c0420));    /* HOT Streamer claims car audio */
    CHECK(!cod_is_audio_sink(0x2c0420));    /* TCL TV claims car audio */
    CHECK(!cod_is_audio_sink(0x240424));    /* set-top box */
    CHECK(!cod_is_audio_sink(0x24043C));    /* video display and loudspeaker */
    CHECK(!cod_is_audio_sink(0x240438));    /* video monitor */
    CHECK(!cod_is_audio_sink(0x24042C));    /* video camera */
    CHECK(!cod_is_audio_sink(0x5a020c));    /* phone */
    CHECK(!cod_is_audio_sink(0x002508));    /* game pad */
    CHECK(!cod_is_audio_sink(0x002540));    /* keyboard */
    CHECK(!cod_is_audio_sink(0x240400));    /* A/V uncategorized */
    CHECK(!cod_is_audio_sink(0x240410));    /* microphone */
    CHECK(!cod_is_audio_sink(0));
    CHECK(!cod_is_audio_sink(0x200000));    /* audio service bit alone is not enough */
    /* device decision: unknown class only for a saved audio device */
    CHECK(!cod_device_allowed(0, 0, 0));
    CHECK(cod_device_allowed(0, 1, 0x240404));
    CHECK(cod_device_allowed(0, 1, 0));
    CHECK(!cod_device_allowed(0, 1, 0x3c0420));
    CHECK(cod_device_allowed(0x240404, 0, 0));
    CHECK(!cod_device_allowed(0x3c0420, 1, 0x240404)); /* known class wins */
}

/* Reference lerp in 64 bits */
static int ref_lerp(int a, int b, unsigned f)
{
    return a + (int)(((long long)(b - a) * (long long)f) >> 16);
}

static int16_t at_lin(void *ctx, int frame, int ch)
{
    return ((const int16_t *)ctx)[frame * 2 + ch];
}

static void test_lerp(void)
{
    int a, b;
    unsigned f;
    static const int ext[] = { -32768, -30000, -1, 0, 1, 30000, 32767 };
    int ia, ib;
    for (ia = 0; ia < 7; ia++)
        for (ib = 0; ib < 7; ib++)
            for (f = 0; f < 65536; f += 4093) {
                a = ext[ia]; b = ext[ib];
                if (hb_lerp16(a, b, f) != ref_lerp(a, b, f)) { CHECK(0); return; }
            }
    /* the old 32-bit product overflowed here */
    CHECK(hb_lerp16(-32768, 32767, 65000) > 30000);
    CHECK(hb_lerp16(32767, -32768, 65000) < -30000);
    CHECK(hb_lerp16(-32768, 32767, 0) == -32768);
}

static void test_resample(void)
{
    enum { N = 4800 };
    static int16_t in[N * 2], out[N * 2];
    int i, got, rates[3] = { 48000, 44100, 32000 }, r;
    for (i = 0; i < N; i++) {
        in[i * 2] = (int16_t)(20000.0 * sin(2 * M_PI * 1000.0 * i / 48000.0));
        in[i * 2 + 1] = in[i * 2];
    }
    for (r = 0; r < 3; r++) {
        uint32_t pos = 0, step = hb_rs_step(rates[r], 0.0);
        double err = 0;
        int used;
        got = hb_rs_run(at_lin, in, N, &pos, step, out, N);
        used = (int)(pos >> 16);
        /* frames produced = frames consumed * rate / 48000 */
        CHECK(abs(got - (int)((double)used * rates[r] / 48000.0)) <= 2);
        CHECK(got > 0 && used <= N);
        /* output keeps a 1 kHz sine at the new rate */
        for (i = 4; i < got - 4; i++) {
            double want = 20000.0 * sin(2 * M_PI * 1000.0 * i / (double)rates[r]);
            double d = fabs(want - out[i * 2]);
            if (d > err) err = d;
        }
        printf("  %d Hz: %d out from %d in, max error %.0f\n", rates[r], got, used, err);
        CHECK(err < 1500.0);   /* linear interpolation of a 1 kHz tone */
    }
    /* 48 kHz must be a copy */
    {
        uint32_t pos = 0;
        got = hb_rs_run(at_lin, in, 100, &pos, hb_rs_step(48000, 0.0), out, 50);
        CHECK(got == 50 && memcmp(out, in, 50 * 4) == 0 && (pos >> 16) == 50);
    }
    /* 44.1 kHz: 48000 in per second gives 44100 out per second, so a 8 % slow
     * playback of the old code (no resample) cannot happen */
    {
        uint32_t step = hb_rs_step(44100, 0.0);
        CHECK(step > 65536 + 5000 && step < 65536 + 6000);
    }
    /* drift correction moves the step by ppm */
    CHECK(hb_rs_step(48000, 1000.0) > hb_rs_step(48000, 0.0));
}


static void test_cmdq(void)
{
    struct cmdq q;
    unsigned char a[6] = { 1, 2, 3, 4, 5, 6 }, b[6] = { 9, 9, 9, 9, 9, 9 }, o[6];
    int t, i;
    memset(&q, 0, sizeof q);
    CHECK(!cmdq_pop(&q, &t, o));
    /* order is kept, nothing is lost for quick clicks */
    cmdq_push(&q, 3, NULL);              /* disconnect */
    cmdq_push(&q, 2, a);                 /* connect a */
    cmdq_push(&q, 4, b);                 /* forget b */
    CHECK(cmdq_pop(&q, &t, o) && t == 3);
    CHECK(cmdq_pop(&q, &t, o) && t == 2 && memcmp(o, a, 6) == 0);
    CHECK(cmdq_pop(&q, &t, o) && t == 4 && memcmp(o, b, 6) == 0);
    CHECK(!cmdq_pop(&q, &t, o));
    /* the same command twice in a row is one */
    cmdq_push(&q, 1, NULL); cmdq_push(&q, 1, NULL);
    CHECK(q.count == 1);
    cmdq_push(&q, 4, a); cmdq_push(&q, 4, b);   /* different address: two */
    CHECK(q.count == 3);
    while (cmdq_pop(&q, &t, o)) {}
    /* full: the oldest goes, the newest stays */
    for (i = 0; i < CMDQ_N + 3; i++) { unsigned char x[6] = { (unsigned char)i, 0, 0, 0, 0, 0 }; cmdq_push(&q, 2, x); }
    CHECK(q.count == CMDQ_N);
    CHECK(cmdq_pop(&q, &t, o) && o[0] == 3);
    while (q.count > 1) cmdq_pop(&q, &t, o);
    CHECK(cmdq_pop(&q, &t, o) && o[0] == CMDQ_N + 2);
}

/* Feed bytes through the same rule ev_drain uses: count events, drop a byte
 * when the header cannot be real. */
static int frame_run(const unsigned char *in, int n, int *dropped)
{
    unsigned char buf[600];
    int fill = 0, ev = 0, i;
    *dropped = 0;
    for (i = 0; i < n; i++) {
        buf[fill++] = in[i];
        while (fill >= 2) {
            int need;
            if (!hci_evt_plausible(buf[0], buf[1])) { memmove(buf, buf + 1, (size_t)--fill); (*dropped)++; continue; }
            need = 2 + buf[1];
            if (fill < need) break;
            ev++;
            memmove(buf, buf + need, (size_t)(fill - need));
            fill -= need;
        }
    }
    return ev;
}

/* The saved headset's own call, as the console log shows it (12 bytes). */
static void test_conn_request(void)
{
    static const unsigned char req[] = { 0x04,0x0a,0x7c,0x3b,0x63,0x62,0x18,0x58,0x04,0x04,0x24,0x01 };
    static const unsigned char cc[] = { 0x03,0x0b,0x00,0x2a,0x00,0x7c,0x3b,0x63,0x62,0x18,0x58,0x01,0x00 };
    CHECK(sizeof req == HCI_EVT_CONN_REQUEST_LEN);
    CHECK(hci_evt_plausible(req[0], req[1]));            /* the framing check lets it through ... */
    CHECK(hci_conn_request_ok(req, (int)sizeof req));    /* ... and so does the handler (it wanted 13) */
    CHECK(!hci_conn_request_ok(req, (int)sizeof req - 1));
    CHECK(!hci_conn_request_ok(cc, (int)sizeof cc));     /* other events are not requests */
    CHECK(hci_create_conn_busy(0x0C) && hci_create_conn_busy(0x0B));
    CHECK(!hci_create_conn_busy(0x00) && !hci_create_conn_busy(0x04) && !hci_create_conn_busy(0x0D));
}

static void test_hciframe(void)
{
    /* Real events from the console log */
    static const unsigned char inq[] = { 0x22,0x0f,0x01,0xdf,0x48,0x55,0xbe,0x22,0x22,0x01,0x02,0x24,0x04,0x28,0xa4,0x36,0xc8 };
    static const unsigned char cs[] = { 0x0f,0x04,0x00,0x01,0x04,0x0c };      /* command status */
    static const unsigned char done[] = { 0x01,0x01,0x00 };                    /* inquiry complete */
    unsigned char s[200];
    int n = 0, dropped, ev;
    CHECK(hci_evt_plausible(0x22, 15) && hci_evt_plausible(0x0F, 4) && hci_evt_plausible(0x0E, 4));
    CHECK(!hci_evt_plausible(0xC8, 34));   /* the byte that started the desync */
    CHECK(!hci_evt_plausible(0x0F, 1));    /* the tail seen after it */
    CHECK(!hci_evt_plausible(0, 3) && !hci_evt_plausible(0x22, 16));
    memcpy(s + n, inq, sizeof inq); n += (int)sizeof inq;
    memcpy(s + n, cs, sizeof cs); n += (int)sizeof cs;
    memcpy(s + n, done, sizeof done); n += (int)sizeof done;
    ev = frame_run(s, n, &dropped);
    CHECK(ev == 3 && dropped == 0);
    /* The first 16 bytes of an event are lost, its last byte arrives alone:
     * without the check the next events are eaten, with it they all come. */
    n = 0;
    s[n++] = 0xC8;                                    /* stray 17th byte */
    memcpy(s + n, inq, sizeof inq); n += (int)sizeof inq;
    memcpy(s + n, inq, sizeof inq); n += (int)sizeof inq;
    memcpy(s + n, done, sizeof done); n += (int)sizeof done;
    ev = frame_run(s, n, &dropped);
    CHECK(ev == 3 && dropped == 1);
}

/* ---- tap call patterns (plugin/tap_rate.h) -------------------------------
 * A simulated game: buffer k holds frames [k*n, (k+1)*n). Every call carries
 * (time, return code, buffer id). The tap copies what hb_gate_pass lets
 * through. A call that returns an error did not queue its buffer, so the
 * reference stream is exactly the buffers whose call returned success, once
 * each and in order. */
#define NOT_INIT ((int32_t)0x8026000Fu)
#define SIM_MAX 400000

static int sim_ref[SIM_MAX], sim_got[SIM_MAX];
static int sim_nref, sim_ngot;
static struct hb_tapgate sim_gate;
static uint64_t sim_frames_in;     /* frames let through */
static uint32_t sim_cap = 4096;

static void sim_reset(void)
{
    memset(&sim_gate, 0, sizeof sim_gate);
    sim_nref = sim_ngot = 0;
    sim_frames_in = 0;
}

/* One call: t microseconds, returned rc, the buffer it carried. */
static void sim_call(uint64_t t, int32_t rc, int buf, uint32_t n)
{
    int r = hb_gate_pass(&sim_gate, rc, n, t, sim_cap);
    if (rc >= 0 && sim_nref < SIM_MAX) sim_ref[sim_nref++] = buf;
    if (r == HB_GATE_PASS) {
        if (sim_ngot < SIM_MAX) sim_got[sim_ngot++] = buf;
        sim_frames_in += n;
    }
}

static int sim_same(void)
{
    return sim_nref == sim_ngot && memcmp(sim_ref, sim_got, (size_t)sim_nref * sizeof(int)) == 0;
}

static unsigned sim_rnd;
static unsigned rnd(void) { sim_rnd = sim_rnd * 1664525u + 1013904223u; return sim_rnd >> 8; }

/* paced calls at the audio rate, optionally with the game clock off by ppm
 * and a timing jitter in microseconds */
static void run_paced(uint32_t n, int calls, int ppm, int jitter_us)
{
    uint64_t t = 1000000;
    double period = (double)n * 1e6 / 48000.0 * (1.0 - ppm * 1e-6);
    int k;
    sim_reset();
    for (k = 0; k < calls; k++) {
        int j = jitter_us ? (int)(rnd() % (unsigned)(2 * jitter_us + 1)) - jitter_us : 0;
        sim_call(t + (uint64_t)(j + jitter_us), (int32_t)n, k, n);
        t += (uint64_t)(period + 0.5);
    }
}

static void test_tap_patterns(void)
{
    static const uint32_t sizes[] = { 256, 480, 1024, 2048 };
    int i, k;
    /* 1) paced, exact audio rate, every buffer size */
    for (i = 0; i < 4; i++) {
        run_paced(sizes[i], 4000, 0, 0);
        CHECK(sim_same());
    }
    /* 2) paced with jitter of +-4 ms */
    for (i = 0; i < 4; i++) {
        run_paced(sizes[i], 4000, 0, 4000);
        CHECK(sim_same());
    }
    /* 3) the game clock is 1 % fast and 1 % slow: still nothing lost */
    run_paced(1024, 6000, 10000, 500);
    CHECK(sim_same());
    run_paced(2048, 3000, -10000, 500);
    CHECK(sim_same());
    /* 4) tight loop: a failing call every 19 us, a real buffer every 2048
     * frames. Spinning-loop shaped. Before any success the port is unknown
     * (bounded by the 48 kHz limit), afterwards errors are never copied. */
    {
        const uint32_t n = 2048;
        uint64_t t = 1000, next = 0, step = 19, end = 1000 + 12000000ull;
        int buf = 0, real = 0;
        uint64_t period = (uint64_t)((double)n * 1e6 / 48000.0);
        sim_reset();
        /* first real buffer after 300 ms of errors only */
        next = t + 300000;
        while (t < end) {
            if (t >= next) { sim_call(t, (int32_t)n, buf++, n); real++; next += period; }
            else sim_call(t, NOT_INIT, buf++, n);   /* the same buffer retried: new id each call */
            t += step;
        }
        /* the real buffers all arrived, in order, none duplicated */
        {
            int cnt = 0, dup = 0, miss = 0;
            int idx = 0;
            for (k = 0; k < sim_nref; k++) {
                while (idx < sim_ngot && sim_got[idx] < sim_ref[k]) idx++;   /* skip pre-success extras */
                if (idx < sim_ngot && sim_got[idx] == sim_ref[k]) { cnt++; idx++; } else miss++;
            }
            for (k = 1; k < sim_ngot; k++) if (sim_got[k] <= sim_got[k - 1]) dup++;
            CHECK(miss == 0 && dup == 0 && cnt == sim_nref);
        }
        /* extras are only the unknown phase before the first success, at most
         * 48 kHz worth of frames (300 ms) */
        {
            int extra = sim_ngot - sim_nref;
            CHECK(extra >= 0 && (uint64_t)extra * n <= 300000ull * 48000ull / 1000000ull + 2 * n);
        }
    }
    /* 5) tight loop, a real buffer first, errors in between: exact */
    {
        const uint32_t n = 1024;
        uint64_t t = 1000, next = 1000, period = (uint64_t)((double)n * 1e6 / 48000.0);
        int buf = 0;
        sim_reset();
        while (t < 1000 + 20000000ull) {
            if (t >= next) { sim_call(t, (int32_t)n, buf++, n); next += period; }
            else sim_call(t, NOT_INIT, buf++, n);
            t += 19;
        }
        CHECK(sim_nref > 900 && sim_same());
    }
    /* 6) mixed: random errors between real buffers, now and then two real
     * buffers back to back (a game filling its queue), random sizes */
    for (i = 0; i < 4; i++) {
        uint32_t n = sizes[i];
        uint64_t t = 5000, period = (uint64_t)((double)n * 1e6 / 48000.0);
        int buf = 0;
        sim_reset();
        for (k = 0; k < 3000; k++) {
            int e = k ? (int)(rnd() % 40) : 0, j;          /* the first call is a real one: the port has shown it works */
            for (j = 0; j < e; j++) sim_call(t + (uint64_t)j * 7, NOT_INIT, buf++, n);
            sim_call(t + 400, (int32_t)n, buf++, n);
            if (k == 1 || k % 97 == 60) {                 /* queue fill: two at once, then one period of quiet */
                sim_call(t + 410, (int32_t)n, buf++, n);
                t += period;
            }
            t += period;
        }
        CHECK(sim_same());
    }
    /* 7) a game that only ever returns errors (never a success): copied
     * under the 48 kHz limit (exact, no lost fractions) until 1000 calls and
     * 2 s have passed, then the port is dead: nothing is copied any more */
    {
        const uint32_t n = 2048;
        uint64_t t = 1000, end = 1000 + 10000000ull;
        double got, want;
        sim_reset();
        while (t < end) { sim_call(t, NOT_INIT, 0, n); t += 19; }
        got = (double)sim_frames_in;
        want = 2.0 * 48000.0;
        printf("  errors only: %.0f frames in 10 s (about 2 s worth, then silence, want %.0f)\n", got, want);
        CHECK(sim_gate.dead);
        CHECK(got >= want * 0.99 && got <= want * 1.01 + 3 * n);
    }
    /* 7b) too few calls or too short: never dead. 999 calls over 5 s, then
     * 5000 calls within 1 s of the first. */
    {
        const uint32_t n = 1024;
        int k2;
        sim_reset();
        for (k2 = 0; k2 < 999; k2++) sim_call(1000 + (uint64_t)k2 * 5000, NOT_INIT, 0, n);
        CHECK(!sim_gate.dead);
        sim_reset();
        for (k2 = 0; k2 < 5000; k2++) sim_call(1000 + (uint64_t)k2 * 100, NOT_INIT, 0, n);
        CHECK(!sim_gate.dead);
        sim_call(1000 + 2000000ull + 200, NOT_INIT, 0, n);      /* 2 s after the first call, 5001st call */
        CHECK(sim_gate.dead);
    }
    /* 7c) a dead port comes back with its first success and then behaves as
     * a normal one: every successful buffer exactly once, errors skipped */
    {
        const uint32_t n = 1024;
        uint64_t t = 1000, next, period = (uint64_t)((double)n * 1e6 / 48000.0);
        int buf = 0, extra;
        sim_reset();
        while (t < 1000 + 4000000ull) { sim_call(t, NOT_INIT, buf++, n); t += 19; }
        CHECK(sim_gate.dead);
        extra = sim_ngot;                                     /* what the unknown phase let through */
        CHECK(sim_gate.dead && sim_ngot == extra);
        next = t;
        {
            int before = sim_ngot, real = 0, k2;
            for (k2 = 0; k2 < 2000; k2++) {
                while (t < next) { sim_call(t, NOT_INIT, buf++, n); t += 19; }
                sim_call(next, (int32_t)n, buf++, n); real++;
                next += period; t = next - period + 19;
            }
            CHECK(!sim_gate.dead && sim_gate.ok_seen);
            CHECK(sim_ngot - before == real);
        }
    }
    /* 8) a long pause (menu, suspend) does not bank an unbounded budget */
    {
        const uint32_t n = 1024;
        int burst = 0;
        sim_reset();
        sim_call(1000, (int32_t)n, 0, n);
        for (k = 0; k < 200; k++)                     /* 200 buffers right after 60 s of nothing */
            { uint64_t t = 60000000ull + (uint64_t)k; int b0 = sim_ngot; sim_call(t, (int32_t)n, k + 1, n); burst += sim_ngot - b0; }
        CHECK(burst <= (int)(sim_cap / n) + 1);
    }
    /* error codes are negative as int32 */
    CHECK(NOT_INIT < 0 && (int32_t)0x80260004u < 0 && 0x800 > 0);
}

/* ---- failing port diagnostics (plugin/tap_diag.h) ----------------------- */
static void test_launcher_addr(void)
{
    char v[32], t[1024];
    const char *st = "{\"version\":\"1.0.0\",\"ip\":\"192.0.2.10\",\"state\":\"listening\",\"streaming\":0}";
    CHECK(hb_json_str(st, "version", v, sizeof v) && strcmp(v, "1.0.0") == 0);
    CHECK(hb_json_str(st, "ip", v, sizeof v) && strcmp(v, "192.0.2.10") == 0);
    CHECK(!hb_json_str(st, "missing", v, sizeof v) && v[0] == 0);
    CHECK(!hb_json_str("{\"ip\":\"\"}", "ip", v, sizeof v));
    CHECK(hb_json_str(st, "ip", v, 8) && strcmp(v, "192.0.2") == 0);          /* cut, still terminated */
    CHECK(hb_ip_ok("192.0.2.10") && hb_ip_ok("192.0.2.7"));
    CHECK(!hb_ip_ok("127.0.0.1") && !hb_ip_ok("0.0.0.0") && !hb_ip_ok("") && !hb_ip_ok("abc") &&
          !hb_ip_ok("300.1.1.1") && !hb_ip_ok("1.2.3") && !hb_ip_ok("1.2.3.4.5"));
    hb_ui_text(t, sizeof t, "", "192.0.2.10", 6767, "1.0.0", "X", "Circle");
    CHECK(strstr(t, "http://192.0.2.10:6767") && strstr(t, "127.0.0.1:6767") && t[0] == 'H');
    CHECK(strstr(t, "Press X to open the page in the PS4 web browser.\nPress Circle to close, HearBridge keeps running."));
    CHECK(strstr(t, "Yes") == NULL && strstr(t, "No closes") == NULL);
    CHECK(strstr(t, "\nA CSR8510 or Realtek RTL8761BU USB dongle must be plugged into the PS4.\n") != NULL && strlen(t) < 450);
    CHECK(strstr(t, "First press OK. Then:\nPress X to open") != NULL);       /* the pad is blind while the message is up */
    CHECK(strncmp(t, "HearBridge 1.0.0 is running.\n", 29) == 0);                 /* version in the first line */
    CHECK(strstr(t, "Developed by") == NULL && strstr(t, "F1REBALL") == NULL);   /* the name is on the splash only */
    hb_ui_text(t, sizeof t, "Game audio plugin installed.", "192.0.2.10", 6768, "1.0.0", "X", "Circle");
    CHECK(strncmp(t, "Game audio plugin installed.\n\n", 30) == 0 && strstr(t, ":6768"));
    hb_ui_text(t, sizeof t, "", "127.0.0.1", 6767, "1.0.0", "X", "Circle");
    CHECK(strstr(t, "<console address>:6767") != NULL && strstr(t, "HearBridge 1.0.0 is running") &&
          strstr(t, "Developed by") == NULL);                          /* no usable address: say so */
    hb_ui_text(t, 40, "x", "192.0.2.10", 6767, "1.0.0", "X", "Circle");                              /* tiny buffer: no overflow */
    CHECK(strlen(t) < 40);
    hb_ui_text(t, sizeof t, "", "192.0.2.10", 6767, "1.0.0", "Circle", "X");              /* Asian button assignment */
    CHECK(strstr(t, "Press Circle to open the page in the PS4 web browser.\nPress X to close") != NULL);
}

static void test_pager(void)
{
    struct hb_pager p = { HB_PAGE_TRIES };
    int i, pages = 0;
    long total = 0;

    /* daemon start: a short window, then silence for good */
    for (i = 0; i < 1000000; i++) pages += hb_pager_take(&p);
    CHECK(pages == HB_PAGE_TRIES);
    CHECK(!hb_pager_take(&p));
    /* every trigger (Reconnect, Try now, scan that stopped a stream, stream ended) opens one window */
    for (i = 0; i < 5; i++) {
        pages = 0;
        hb_pager_trigger(&p);
        while (hb_pager_take(&p)) pages++;
        CHECK(pages == HB_PAGE_TRIES);
    }
    /* a scan from idle pages nothing */
    hb_pager_quiet(&p);
    CHECK(!hb_pager_take(&p));
    /* a saved headset with a key (5.12 s page timeout): the whole window is under 30 s */
    total = hb_pager_window_ms(5120);
    CHECK(total > 20000 && total <= 30000);
    CHECK(hb_pager_gap_ms(0) == 1000 && hb_pager_gap_ms(9) == 3000 && hb_pager_gap_ms(-1) == 1000);
    /* stream ended or link dropped, any reason: no paging at all, only the start page and user triggers page */
    {
        static const int reasons[] = { 0x13, 0x15, 0x08, 0x22, 0x3D, 0x14, 0x00, -1 };
        for (i = 0; i < 8; i++) {
            hb_pager_trigger(&p);
            CHECK(hb_pager_after_stream(&p, reasons[i]) == 0);
            CHECK(!hb_pager_take(&p) && !hb_pager_take(&p));
        }
        /* daemon start: one page of 5.12 s, then quiet */
        {
            struct hb_pager start = { HB_PAGE_TRIES_START };
            CHECK(hb_pager_take(&start) && !hb_pager_take(&start));
            CHECK(hb_pager_pages_ms(HB_PAGE_TRIES_START, 5120) >= 5000 && hb_pager_pages_ms(HB_PAGE_TRIES_START, 5120) <= 10000);
        }
        /* Connect / Try now still pages, after the quiet phase too */
        hb_pager_trigger(&p);
        pages = 0;
        while (hb_pager_take(&p)) pages++;
        CHECK(pages == HB_PAGE_TRIES);
        printf("  start window %ld ms, Connect / Try now %ld ms, after a stream or a scan: none\n",
               hb_pager_pages_ms(HB_PAGE_TRIES_START, 5120), hb_pager_window_ms(5120));
    }
    printf("  paging window: %d pages, %ld ms at most, then quiet\n", HB_PAGE_TRIES, total);
}

static void test_diag(void)
{
    struct hb_portdiag d;
    static int16_t a[2048], b[2048];
    int k;
    memset(&d, 0, sizeof d);
    hb_diag_rc(&d, (int32_t)0x8026000Fu);
    hb_diag_rc(&d, (int32_t)0x8026000Fu);
    hb_diag_rc(&d, 0x100);
    CHECK(d.nrc == 2 && d.rc_n[0] == 2 && d.rc_n[1] == 1);
    hb_diag_rc(&d, 1); hb_diag_rc(&d, 2); hb_diag_rc(&d, 3); hb_diag_rc(&d, 4);
    CHECK(d.nrc == HB_DIAG_RC && d.rc_other == 2);
    /* identical silent buffers: one checksum value, no change, peak 0 */
    memset(&d, 0, sizeof d);
    memset(a, 0, sizeof a);
    for (k = 0; k < 1000; k++) hb_diag_err(&d, a, 2048);
    CHECK(d.err_n == 1000 && d.samples == 256 && d.changes == 0 && d.peak == 0);
    /* a buffer that changes every call, loud */
    memset(&d, 0, sizeof d);
    for (k = 0; k < 100; k++) { a[7] = (int16_t)(1000 + k); a[100] = -20000; hb_diag_err(&d, a, 2048); }
    CHECK(d.samples == 100 && d.changes == 99 && d.peak == 20000);
    /* the sampler is cheap: 256 checks, then one in 1024 */
    memset(&d, 0, sizeof d);
    memcpy(b, a, sizeof b);
    for (k = 0; k < 52000; k++) hb_diag_err(&d, b, 2048);
    CHECK(d.samples == 256 + 50);                      /* 52000 failing calls: 306 buffers checksummed */
    CHECK(hb_diag_hash(a, 2048) == hb_diag_hash(a, 2048) && hb_diag_hash(a, 2048) != hb_diag_hash(a, 2047));
}

/* ---- sample formats (plugin/tap_convert.h) ------------------------------ */
static void test_convert(void)
{
    enum { N = 2048 };
    static int16_t s16[N * 8], dst[N * 2];
    static float fl[N * 8];
    uint32_t i, n;
    int ok;

    /* s16 stereo: a straight copy, all sizes */
    for (n = 256; n <= N; n *= 2) {
        for (i = 0; i < n * 2; i++) s16[i] = (int16_t)(i * 7 - 9000);
        CHECK(hb_convert(FMT_S16_STEREO, n, s16, dst) == n);
        CHECK(memcmp(dst, s16, n * 4) == 0);
    }
    /* s16 mono: both channels equal to the input */
    for (i = 0; i < N; i++) s16[i] = (int16_t)(i * 13 - 12000);
    CHECK(hb_convert(FMT_S16_MONO, N, s16, dst) == N);
    for (ok = 1, i = 0; i < N; i++) if (dst[i * 2] != s16[i] || dst[i * 2 + 1] != s16[i]) ok = 0;
    CHECK(ok);
    /* float mono and stereo: in range stays, over range is limited, NaN is silence */
    fl[0] = 0.5f; fl[1] = -0.5f; fl[2] = 4.0f; fl[3] = -4.0f; fl[4] = NAN; fl[5] = 0.0f;
    CHECK(hb_convert(FMT_FLOAT_STEREO, 3, fl, dst) == 3);
    CHECK(abs(dst[0] - 16383) < 40 && abs(dst[1] + 16383) < 40);
    CHECK(dst[2] > 30000 && dst[3] < -30000);       /* limited, no wrap around */
    CHECK(dst[4] == 0);
    CHECK(hb_convert(FMT_FLOAT_MONO, 2, fl, dst) == 2);
    CHECK(dst[0] == dst[1] && dst[2] == dst[3] && abs(dst[0] - 16383) < 40);
    /* float far out of range: limited at both ends, never wraps */
    for (i = 0; i < N * 2; i++) fl[i] = (float)((int)(i % 41) - 20) * 0.37f;
    hb_convert(FMT_FLOAT_STEREO, N, fl, dst);
    for (ok = 1, i = 0; i < N * 2; i++) {
        if (fl[i] > 1.0f && dst[i] < 30000) ok = 0;
        if (fl[i] < -1.0f && dst[i] > -30000) ok = 0;
    }
    CHECK(ok);
    /* 8 channels, both layouts and both sample types: front pair only gives
     * the same L/R, center and surrounds are added, nothing wraps */
    {
        uint32_t f;
        static const uint32_t fm_s16[2] = { FMT_S16_8CH, FMT_S16_8CH_STD };
        static const uint32_t fm_fl[2] = { FMT_FLOAT_8CH, FMT_FLOAT_8CH_STD };
        for (f = 0; f < 2; f++) {
            memset(s16, 0, sizeof s16);
            for (i = 0; i < N; i++) { s16[i * 8] = 10000; s16[i * 8 + 1] = -10000; }
            CHECK(hb_convert(fm_s16[f], N, s16, dst) == N);
            CHECK(abs(dst[0] - 10000) <= 2 && abs(dst[1] + 10000) <= 2 && abs(dst[(N - 1) * 2] - 10000) <= 2);
            for (i = 0; i < N; i++) { s16[i * 8 + 2] = 20000; s16[i * 8 + 4] = 20000; s16[i * 8 + 6] = 20000; }
            hb_convert(fm_s16[f], N, s16, dst);
            CHECK(dst[0] > 10000 && dst[0] <= 32767 && dst[1] > -10000 + 1000);    /* L got louder, R got center and surround */
            for (i = 0; i < N * 8; i++) s16[i] = 32767;                          /* full scale everywhere */
            hb_convert(fm_s16[f], N, s16, dst);
            CHECK(dst[0] > 30000 && dst[1] > 30000);                               /* all positive peaks: limited at the top */
            memset(fl, 0, sizeof fl);
            for (i = 0; i < N; i++) { fl[i * 8] = 0.25f; fl[i * 8 + 1] = -0.25f; }
            CHECK(hb_convert(fm_fl[f], N, fl, dst) == N);
            CHECK(abs(dst[0] - 8191) < 40 && abs(dst[1] + 8191) < 40);
            for (i = 0; i < N * 8; i++) fl[i] = 1.0f;                              /* everything full: limiter, not wrap */
            hb_convert(fm_fl[f], N, fl, dst);
            CHECK(dst[0] > 30000 && dst[1] > 30000);
        }
    }
    /* an unknown format returns 0 and writes nothing */
    CHECK(hb_convert(99, 256, s16, dst) == 0);
}

/* ---- output boost and soft limiter (src/boost.h) ----------------------- */
static void test_boost(void)
{
    enum { N = 4800 };
    static int16_t in[N], out[N], b[N];
    int i, ok, flat;
    int32_t v, prev;
    double rin, rout;

    /* slider percent to gain */
    CHECK(hb_gain_q8(100) == 256 && hb_gain_q8(0) == 0 && hb_gain_q8(50) == 128);
    CHECK(HB_LOUD_DEFAULT == 100 && hb_gain_q8(HB_LOUD_DEFAULT) == 256); /* the meter starts at 100% */
    CHECK(HB_LOUD_MAX == 500 && hb_gain_q8(HB_LOUD_MAX) == 1280 && hb_gain_q8(999) == 1280 && hb_gain_q8(-5) == 0);
    {   /* x5 on a full-scale square wave: limited, never wraps, stays below full scale */
        static int16_t sq[64];
        int j, good = 1;
        for (j = 0; j < 64; j++) sq[j] = (j & 1) ? -32767 : 32767;
        hb_apply_gain(sq, 64, hb_gain_q8(HB_LOUD_MAX));
        for (j = 0; j < 64; j++) if (sq[j] == 0 || (j & 1 ? sq[j] > -30000 : sq[j] < 30000) || (int)sq[j] < -32767) good = 0;
        CHECK(good);
    }
    for (ok = 1, i = 1; i <= HB_LOUD_MAX; i++) if (hb_gain_q8(i) < hb_gain_q8(i - 1)) ok = 0;
    CHECK(ok);
    /* below the knee the limiter does nothing */
    for (ok = 1, v = -HB_KNEE; v <= HB_KNEE; v += 37) if (hb_soft_limit(v) != v) ok = 0;
    CHECK(ok);
    /* never reaches full scale, whatever the input, odd symmetric, monotonic */
    prev = -40000;
    for (ok = 1, v = -300000; v <= 300000; v += 7) {
        int16_t y = hb_soft_limit(v);
        if ((int)y < -32767) ok = 0;
        if (hb_soft_limit(-v) != -y) ok = 0;
        if (y < prev) ok = 0;
        prev = y;
    }
    CHECK(ok);
    /* continuous at the knee with slope 1, then bending */
    CHECK(abs(hb_soft_limit(HB_KNEE + 1) - HB_KNEE) <= 1);
    CHECK(hb_soft_limit(HB_KNEE + 1000) > HB_KNEE + 900 && hb_soft_limit(HB_KNEE + 1000) < HB_KNEE + 1000);
    CHECK(hb_soft_limit(100000) < HB_FULL && hb_soft_limit(100000) > 28000);
    /* unity gain: bit exact; below unity: plain scale, never louder */
    for (i = 0; i < N; i++) in[i] = (int16_t)(20000.0 * sin(2 * M_PI * 440.0 * i / 48000.0));
    memcpy(out, in, sizeof in);
    hb_apply_gain(out, N, 256);
    CHECK(memcmp(out, in, sizeof in) == 0);
    memcpy(out, in, sizeof in);
    hb_apply_gain(out, N, 128);
    for (ok = 1, i = 0; i < N; i++) if (abs(out[i] - in[i] / 2) > 1) ok = 0;
    CHECK(ok);
    /* a quiet game signal gets clearly louder at 180 % and at 280 %, undistorted
     * (amplitude 6000 x 1.8 stays under the knee: exactly linear) */
    for (i = 0; i < N; i++) b[i] = (int16_t)(6000.0 * sin(2 * M_PI * 1000.0 * i / 48000.0));
    memcpy(out, b, sizeof b);
    hb_apply_gain(out, N, hb_gain_q8(180));
    for (ok = 1, i = 0; i < N; i++) if (out[i] != (int16_t)(((int32_t)b[i] * hb_gain_q8(180)) >> 8)) ok = 0;
    CHECK(ok);
    for (rin = rout = 0, i = 0; i < N; i++) { rin += (double)b[i] * b[i]; rout += (double)out[i] * out[i]; }
    CHECK(sqrt(rout / rin) > 1.75);
    memcpy(out, b, sizeof b);
    hb_apply_gain(out, N, hb_gain_q8(280));
    for (rout = 0, i = 0; i < N; i++) rout += (double)out[i] * out[i];
    CHECK(sqrt(rout / rin) > 2.6);
    /* a full scale sine at the highest boost: no sample at full scale and no
     * flat top (hard clipping would give runs of 32767) */
    for (i = 0; i < N; i++) in[i] = (int16_t)(32767.0 * sin(2 * M_PI * 1000.0 * i / 48000.0));
    memcpy(out, in, sizeof in);
    hb_apply_gain(out, N, hb_gain_q8(280));
    flat = 0;
    for (ok = 1, i = 0; i < N; i++) {
        if (out[i] >= 32767 || out[i] <= -32767) ok = 0;
        if (i >= 2 && out[i] == out[i - 1] && out[i] == out[i - 2] && abs(out[i]) > 20000) flat++;
    }
    CHECK(ok && flat == 0);
    /* still loud: the peak is well above the input of a quiet track */
    {
        int pk = 0;
        for (i = 0; i < N; i++) if (abs(out[i]) > pk) pk = abs(out[i]);
        CHECK(pk > 29000);
    }
    /* stereo interleaved: both channels get the same treatment */
    for (i = 0; i < N; i += 2) { in[i] = (int16_t)(15000.0 * sin(i * 0.05)); in[i + 1] = in[i]; }
    hb_apply_gain(in, N, hb_gain_q8(280));
    for (ok = 1, i = 0; i < N; i += 2) if (in[i] != in[i + 1]) ok = 0;
    CHECK(ok);
}


/* ---- equalizer ---- */
static double rms_of(const int16_t *p, int n, int skip)
{
    double a = 0;
    int i;
    for (i = skip; i < n; i++) a += (double)p[i] * p[i];
    return sqrt(a / (n - skip));
}

static void test_eq(void)
{
    enum { F = 4800 };
    static int16_t a[F * 2], b[F * 2], c[F * 2];
    hb_eq e;
    int i, ok, pk;
    int flat[5] = {0, 0, 0, 0, 0}, bass[5] = {6, 3, 0, 0, 0}, loud[5] = {12, 12, 12, 12, 12};
    int gains[4] = {64, 256, 460, 1280};

    for (i = 0; i < F * 2; i += 2) {
        a[i] = (int16_t)(9000.0 * sin(i * 0.013) + 4000.0 * sin(i * 0.41));
        a[i + 1] = (int16_t)(7000.0 * sin(i * 0.021));
    }
    /* off, and all bands at 0 dB: bit exact with hb_apply_gain at every gain */
    hb_eq_init(&e);
    for (ok = 1, i = 0; i < 4; i++) {
        memcpy(b, a, sizeof a); memcpy(c, a, sizeof a);
        hb_eq_set(&e, 0, bass, 48000);
        hb_eq_apply(&e, b, F, gains[i]);
        hb_apply_gain(c, F * 2, gains[i]);
        if (memcmp(b, c, sizeof b)) ok = 0;
        memcpy(b, a, sizeof a);
        hb_eq_set(&e, 1, flat, 48000);
        hb_eq_apply(&e, b, F, gains[i]);
        if (memcmp(b, c, sizeof b)) ok = 0;
    }
    CHECK(ok);
    CHECK(!e.on);

    /* response of the RBJ filters: bass boost lifts 40 Hz by about 6 dB, leaves 1 kHz and 10 kHz alone */
    hb_eq_set(&e, 1, bass, 48000);
    CHECK(e.on);
    CHECK(hb_eq_response_db(&e, 40.f, 48000) > 5.0f && hb_eq_response_db(&e, 40.f, 48000) < 9.5f);
    CHECK(fabs(hb_eq_response_db(&e, 1000.f, 48000)) < 1.0f);
    CHECK(fabs(hb_eq_response_db(&e, 10000.f, 48000)) < 0.5f);
    {
        int cut[5] = {-6, -3, 0, 0, 0};
        hb_eq_set(&e, 1, cut, 48000);
        CHECK(hb_eq_response_db(&e, 40.f, 48000) < -5.0f);
    }
    /* measured on a signal: 40 Hz sine, bass boost ends up louder, 6 kHz untouched */
    hb_eq_set(&e, 1, bass, 48000);
    for (i = 0; i < F * 2; i += 2) {
        b[i] = b[i + 1] = (int16_t)(6000.0 * sin(2 * M_PI * 40.0 * (i / 2) / 48000.0));
        c[i] = c[i + 1] = (int16_t)(6000.0 * sin(2 * M_PI * 6000.0 * (i / 2) / 48000.0));
    }
    {
        double r0b = rms_of(b, F * 2, 2000), r0c = rms_of(c, F * 2, 2000);
        hb_eq_apply(&e, b, F, 256);
        hb_eq_init(&e); hb_eq_set(&e, 1, bass, 48000);
        hb_eq_apply(&e, c, F, 256);
        CHECK(rms_of(b, F * 2, 2000) / r0b > 1.75 && rms_of(b, F * 2, 2000) / r0b < 2.1);   /* about +5.7 dB at 40 Hz */
        CHECK(fabs(rms_of(c, F * 2, 2000) / r0c - 1.0) < 0.05);
    }
    /* never past full scale, even at +12 dB on every band and a full scale input */
    hb_eq_init(&e);
    hb_eq_set(&e, 1, loud, 48000);
    for (i = 0; i < F * 2; i++) b[i] = (int16_t)(32000.0 * sin(i * 0.05));
    hb_eq_apply(&e, b, F, 256);
    for (pk = 0, i = 0; i < F * 2; i++) if (abs(b[i]) > pk) pk = abs(b[i]);
    CHECK(pk <= 32767 && pk > 20000);
    /* block size does not matter: one call or ragged calls give the same samples */
    {
        int mid[5] = {3, -2, 4, -5, 2};
        int part[7] = {1, 255, 256, 257, 700, 1, 4000}, off = 0, k;
        hb_eq e2;
        hb_eq_init(&e); hb_eq_set(&e, 1, mid, 48000);
        hb_eq_init(&e2); hb_eq_set(&e2, 1, mid, 48000);
        memcpy(b, a, sizeof a); memcpy(c, a, sizeof a);
        hb_eq_apply(&e, b, F, 300);
        for (k = 0; k < 7 && off < F; k++) {
            int n = part[k] > F - off ? F - off : part[k];
            hb_eq_apply(&e2, c + off * 2, n, 300);
            off += n;
        }
        CHECK(off == F && memcmp(b, c, sizeof b) == 0);
    }
    /* changing the bands while running keeps the state: no jump larger than the signal */
    {
        int x1[5] = {0, 0, 0, 0, 0}, x2[5] = {5, 5, 5, 5, 5};
        int worst = 0;
        hb_eq_init(&e); hb_eq_set(&e, 1, x2, 48000);
        for (i = 0; i < F * 2; i += 2) b[i] = b[i + 1] = (int16_t)(8000.0 * sin(i * 0.02));
        hb_eq_apply(&e, b, 2400, 256);
        hb_eq_set(&e, 1, x1, 48000);
        hb_eq_set(&e, 1, x2, 48000);
        for (i = 1; i < 2400; i++) { int d = abs(b[i * 2] - b[(i - 1) * 2]); if (d > worst) worst = d; }
        CHECK(worst < 6000);
    }
    /* cost: 5 active bands on 10 s of audio, informational (printed once) */
    {
        enum { SEC = 480000 };
        static int16_t big[SEC * 2];
        clock_t t0;
        double ms;
        int five[5] = {3, -2, 4, -5, 2};
        for (i = 0; i < SEC * 2; i++) big[i] = (int16_t)(8000.0 * sin(i * 0.011));
        hb_eq_init(&e); hb_eq_set(&e, 1, five, 48000);
        t0 = clock();
        hb_eq_apply(&e, big, SEC, 256);
        ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        printf("  eq cost: 10 s of audio, 5 bands: %.1f ms (%.2f %% of one core on this machine)\n", ms, ms / 100.0);
        CHECK(ms < 2000.0);
    }
}

/* ---- game mode ---- */
static void test_gprof(void)
{
    hb_gps gs, g2;
    hb_gp g;
    char txt[4096], title[64];
    int db[5], i;
    /* ids */
    CHECK(hb_gp_id_ok("CUSA55885") && hb_gp_id_ok("CUSA00001"));
    CHECK(!hb_gp_id_ok("cusa55885") && !hb_gp_id_ok("CUSA5588") && !hb_gp_id_ok("CUSA558851") && !hb_gp_id_ok("") && !hb_gp_id_ok(NULL));
    /* eq strings */
    CHECK(hb_eq_parse("6,3,0,-2,12", db) && db[0] == 6 && db[3] == -2 && db[4] == 12);
    CHECK(hb_eq_parse("99,-99,0,0,0", db) && db[0] == 12 && db[1] == -12);
    CHECK(!hb_eq_parse("1,2,3", db) && !hb_eq_parse("", db) && !hb_eq_parse("a,b,c,d,e", db));
    /* line parse: name last, with spaces; defaults; junk */
    CHECK(hb_gp_parse_line("CUSA55885 eq=1 db=6,3,0,0,-4 boost=220 name=Sample Game\n", &g));
    CHECK(g.eq_on == 1 && g.eq_db[0] == 6 && g.eq_db[4] == -4 && g.boost == 220 && !strcmp(g.name, "Sample Game"));
    CHECK(hb_gp_parse_line("CUSA00001", &g) && g.boost == 100 && !g.eq_on && g.name[0] == 0);
    CHECK(hb_gp_parse_line("CUSA00001 boost=9999 foo=1 eq=1", &g) && g.boost == 500 && g.eq_on);
    CHECK(!hb_gp_parse_line("hello world", &g) && !hb_gp_parse_line("", &g) && !hb_gp_parse_line("CUSA0123 eq=1", &g));
    /* put, find, newest first, replace */
    memset(&gs, 0, sizeof gs);
    memset(&g, 0, sizeof g);
    snprintf(g.id, sizeof g.id, "CUSA00001"); snprintf(g.name, sizeof g.name, "One"); g.boost = 200; g.eq_on = 1; g.eq_db[0] = 6;
    hb_gps_put(&gs, &g);
    snprintf(g.id, sizeof g.id, "CUSA00002"); snprintf(g.name, sizeof g.name, "Two"); g.boost = 150; g.eq_on = 0;
    hb_gps_put(&gs, &g);
    CHECK(gs.n == 2 && !strcmp(gs.g[0].id, "CUSA00002") && hb_gps_find(&gs, "CUSA00001") == 1);
    snprintf(g.id, sizeof g.id, "CUSA00001"); snprintf(g.name, sizeof g.name, "One"); g.boost = 333; g.eq_on = 1; g.eq_db[0] = 6;
    hb_gps_put(&gs, &g);
    CHECK(gs.n == 2 && !strcmp(gs.g[0].id, "CUSA00001") && gs.g[0].boost == 333);
    /* file round trip */
    hb_gps_format(&gs, txt, sizeof txt);
    hb_gps_parse(&g2, txt);
    CHECK(g2.n == 2 && !strcmp(g2.g[0].id, "CUSA00001") && g2.g[0].boost == 333 && g2.g[0].eq_on == 1 && g2.g[0].eq_db[0] == 6 &&
          !strcmp(g2.g[1].name, "Two"));
    /* duplicate lines and garbage lines in a file */
    hb_gps_parse(&g2, "CUSA00001 boost=100\nnope\nCUSA00001 boost=300\nCUSA00003 eq=1 db=1,2,3,4,5\n");
    CHECK(g2.n == 2 && g2.g[0].boost == 100);
    /* capacity: the oldest goes */
    memset(&gs, 0, sizeof gs);
    for (i = 0; i < HB_GP_MAX + 5; i++) {
        memset(&g, 0, sizeof g);
        snprintf(g.id, sizeof g.id, "CUSA%05d", i + 1);
        hb_gps_put(&gs, &g);
    }
    CHECK(gs.n == HB_GP_MAX && hb_gps_find(&gs, "CUSA00001") < 0 && hb_gps_find(&gs, "CUSA00021") == 0);
    CHECK(hb_gps_drop(&gs, "CUSA00021") && gs.n == HB_GP_MAX - 1 && !hb_gps_drop(&gs, "CUSA00021"));
    /* a long name is cut, a newline in a name does not break the file */
    memset(&g, 0, sizeof g);
    snprintf(g.id, sizeof g.id, "CUSA00009");
    snprintf(g.name, sizeof g.name, "Line\nBreak");
    memset(&gs, 0, sizeof gs);
    hb_gps_put(&gs, &g);
    hb_gps_format(&gs, txt, sizeof txt);
    CHECK(strchr(txt, '\n') == txt + strlen(txt) - 1);
    /* differs */
    memset(&g, 0, sizeof g);
    g.boost = 180; g.eq_on = 1; g.eq_db[2] = 3;
    {
        int same[5] = {0, 0, 3, 0, 0}, other[5] = {0, 0, 4, 0, 0};
        CHECK(!hb_gp_differs(&g, 1, same, 180));
        CHECK(hb_gp_differs(&g, 1, other, 180) && hb_gp_differs(&g, 1, same, 190) && hb_gp_differs(&g, 0, same, 180));
        g.eq_on = 0;
        CHECK(hb_gp_differs(&g, 0, other, 180) && !hb_gp_differs(&g, 0, same, 180)); /* saved bands count even with the EQ off */
    }
    /* what a change of the running game means */
    CHECK(hb_gm_decide("", "") == HB_GM_KEEP && hb_gm_decide("CUSA00001", "CUSA00001") == HB_GM_KEEP);
    CHECK(hb_gm_decide("", "CUSA00001") == HB_GM_START && hb_gm_decide("CUSA00001", "") == HB_GM_STOP);
    CHECK(hb_gm_decide("CUSA00001", "CUSA00002") == HB_GM_SWITCH);
    /* TITLE from a param.sfo image (two entries: TITLE_ID first, then TITLE) */
    {
        unsigned char sfo[256];
        const char keys[] = "TITLE\0TITLE_ID\0";
        const char data[] = "HearBridge\0\0\0\0\0\0HRBG40001\0\0\0\0\0\0\0";
        memset(sfo, 0, sizeof sfo);
        memcpy(sfo, "\0PSF", 4);
        sfo[4] = 1; sfo[5] = 1;
        sfo[8] = 52; sfo[12] = 80; sfo[16] = 2;
        /* entry 0: TITLE_ID key off 6, utf8, len 10, max 16, data off 16 */
        sfo[20] = 6; sfo[22] = 4; sfo[23] = 2; sfo[24] = 10; sfo[28] = 16; sfo[32] = 16;
        /* entry 1: TITLE key off 0, len 11, max 16, data off 0 */
        sfo[36] = 0; sfo[38] = 4; sfo[39] = 2; sfo[40] = 11; sfo[44] = 16; sfo[48] = 0;
        memcpy(sfo + 52, keys, sizeof keys);
        memcpy(sfo + 80, data, 16);
        memcpy(sfo + 96, "HRBG40001", 10);
        CHECK(hb_sfo_title(sfo, 128, title, sizeof title) && !strcmp(title, "HearBridge"));
        CHECK(!hb_sfo_title(sfo, 10, title, sizeof title));
        sfo[0] = 'X';
        CHECK(!hb_sfo_title(sfo, 128, title, sizeof title));
    }
}

int main(void)
{
    test_cod();
    test_lerp();
    test_resample();
    test_cmdq();
    test_hciframe();
    test_conn_request();
    test_launcher_addr();
    test_pager();
    test_diag();
    test_tap_patterns();
    test_convert();
    test_boost();
    test_eq();
    test_gprof();
    printf(fails ? "FAILED: %d\n" : "ALL OK (0 failures)\n", fails);
    return fails != 0;
}
