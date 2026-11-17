/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hearbridge_ps4 - HearBridge PS4 daemon (see PROBE_VERSION).
 *
 * Drives a USB Bluetooth adapter (CSR8510 0A12:0001, or Realtek RTL8761BU
 * and its rebadges) through libSceUsbd. Realtek sticks that boot in ROM
 * mode get their patch uploaded the same way HearBridge PS5 does. Then it
 * keeps an A2DP Source stream open to a headset, and encodes PCM received
 * over UDP 127.0.0.1:9500 (48 kHz stereo s16le) to SBC. Silence is sent
 * when no PCM arrives. Link keys are persisted under /data/hearbridge/.
 * Send the ASCII command "stop" to the same UDP port for a clean exit.
 *
 * The PCM comes from the game tap plugin (plugin/hearbridge_game_tap.c,
 * GoldHEN plugin loaded into games). Home screen / system audio is not
 * captured. capture_diag and pcm_sweep (10 s UDP test tone) are separate
 * helper payloads.
 *
 * Behaviour: HCI events are read by a dedicated thread with a long USB
 * timeout, media frames are dropped (not forced) when the controller has no
 * ACL credits, pacing resyncs instead of bursting after a stall, and the
 * bitpool is capped at 45 and lowered on congestion. A new daemon asks a
 * running one to stop (UDP 9500, /data/hearbridge/quit, /api/quit) and takes
 * over. The web UI (port 8090, or 8091) covers status, scan, pair, saved
 * devices, volume, Boost, equalizer and game profiles. The target is the
 * pick in the UI, else the override file, else the last used saved device.
 * The current state is kept in /data/hearbridge/state.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "sbc_enc.h"
#include "sdp_server.h"
#include "linkkeys.h"
#include "pcm_udp.h"
#include "devices.h"
#include "cod.h"
#include "boost.h"
#include "hciframe.h"
#include "ui.h"
#include "avrcp.h"
#include "hfp.h"
#include "pager.h"
#include "hci.h"
#include "btchip.h"
#include "adapter_pref.h"
#include "realtek_fw.h"
#include "host_bdaddr.h"

#define PROBE_VERSION "1.2.0"
#define LOG_PATH      "/data/hearbridge/daemon.log"
#define LOG_PATH_ALT  "/user/data/hearbridge/daemon.log"
#define TARGET_PATH   "/data/hearbridge_target.txt"
#define TARGET_PATH_ALT "/user/data/hearbridge_target.txt"
#define STATE_PATH    "/data/hearbridge/state"
#define STATE_PATH_ALT "/user/data/hearbridge/state"

#define CSR_VID 0x0A12
#define CSR_PID 0x0001
#define OP_SET_EVENT_FILTER   0x0C05
#define OP_READ_LOCAL_FEATURES 0x1003


#define POLL_MS          20      /* per endpoint read inside the pump */
#define CMD_TIMEOUT_MS   3000
#define CMD_STATUS_SHORT_MS 800  /* for cancel / name-req status */
#define CTRL_TIMEOUT_MS  1000
#define BULK_OUT_TIMEOUT 500
#define INQUIRY_LEN      8       /* x 1.28 s = 10.24 s */
#define INQUIRY_WAIT_MS  14000
#define NAME_WAIT_MS     4000
#define PAGE_TIMEOUT     0x4000  /* x 0.625 ms = 10.24 s */
#define PAGE_TIMEOUT_SAVED 0x2000 /* 5.12 s for a saved device with a key */
#define SUPERVISION_UNITS 0x3E80 /* x 0.625 ms = 10 s link supervision */
#define CONN_WAIT_MS     15000
#define AUTH_WAIT_MS     30000
#define ENC_WAIT_MS      6000
#define L2_CONN_WAIT_MS  10000
#define L2_PEND_WAIT_MS  20000
#define L2_CFG_WAIT_MS   6000
#define AVDTP_RSP_MS     5000
#define MAX_DEV          16
#define MAX_SEP          8

#define LIBUSB_ERROR_TIMEOUT  (-7)
#define LIBUSB_ERROR_BUSY     (-6)
#define LIBUSB_ERROR_ACCESS   (-3)
#define LIBUSB_ERROR_OVERFLOW (-8)
/* sceUsbd on PS4 returns this instead of libusb -7 when an IN poll has no data. */
#define SCE_USBD_ERROR_TIMEOUT ((int)0x80240007)

/* HCI opcodes (OGF << 10 | OCF) */
#define OP_INQUIRY            0x0401
#define OP_INQUIRY_CANCEL     0x0402
#define OP_CREATE_CONN        0x0405
#define OP_DISCONNECT         0x0406
#define OP_CREATE_CONN_CANCEL 0x0408
#define OP_ACCEPT_CONN        0x0409
#define OP_REJECT_CONN        0x040A
#define OP_REJECT_SYNC_CONN   0x042A   /* Reject Synchronous Connection Request */
#define OP_LINK_KEY_REPLY     0x040B
#define OP_LINK_KEY_NEG_REPLY 0x040C
#define OP_PIN_CODE_REPLY     0x040D
#define OP_AUTH_REQUESTED     0x0411
#define OP_SET_ENCRYPTION     0x0413
#define OP_REMOTE_NAME_REQ    0x0419
#define OP_IO_CAP_REPLY       0x042B
#define OP_USER_CONFIRM_REPLY 0x042C
#define OP_PASSKEY_NEG_REPLY  0x042F
#define OP_SET_EVENT_MASK     0x0C01
#define OP_RESET              0x0C03
#define OP_WRITE_LOCAL_NAME   0x0C13
#define OP_WRITE_CLASS_OF_DEV 0x0C24   /* Audio/Video COD like PS5 */
#define OP_WRITE_PAGE_TIMEOUT 0x0C18
#define OP_WRITE_SCAN_ENABLE  0x0C1A
#define OP_WRITE_LINK_SUPV    0x0C37
#define OP_WRITE_INQUIRY_MODE 0x0C45
#define OP_WRITE_SSP_MODE     0x0C56
#define OP_READ_LOCAL_VER     0x1001
#define OP_READ_BUFFER_SIZE   0x1005
#define OP_READ_BD_ADDR       0x1009

/* L2CAP */
#define CID_SIGNALING 0x0001
#define PSM_SDP       0x0001
#define PSM_AVDTP     0x0019
#define PSM_AVCTP     0x0017   /* AVRCP control (absolute volume) */
#define PSM_RFCOMM    0x0003   /* HFP AG battery (see hfp.c) */
#define L2_CMD_REJ    0x01
#define L2_CONN_REQ   0x02
#define L2_CONN_RSP   0x03
#define L2_CFG_REQ    0x04
#define L2_CFG_RSP    0x05
#define L2_DISC_REQ   0x06
#define L2_DISC_RSP   0x07
#define L2_ECHO_REQ   0x08
#define L2_ECHO_RSP   0x09
#define L2_INFO_REQ   0x0A
#define L2_INFO_RSP   0x0B
#define L2_MTU        672

/* AVDTP signaling (single-packet messages only) */
#define AV_DISCOVER         0x01
#define AV_GET_CAPABILITIES 0x02
#define AV_SET_CONFIG       0x03
#define AV_GET_CONFIG       0x04
#define AV_RECONFIGURE      0x05
#define AV_OPEN             0x06
#define AV_START            0x07
#define AV_CLOSE            0x08
#define AV_SUSPEND          0x09
#define AV_ABORT            0x0A
#define AV_SECURITY         0x0B
#define AV_GET_ALL_CAPS     0x0C
#define AV_DELAY_REPORT     0x0D
#define AV_MSG_COMMAND      0
#define AV_MSG_GEN_REJECT   1
#define AV_MSG_ACCEPT       2
#define AV_MSG_REJECT       3
#define AV_HDR(label, msg)  ((unsigned char)((((label) & 0x0F) << 4) | ((msg) & 0x03)))
#define AV_ERR_BAD_ACP_SEID  0x12
#define AV_ERR_SEP_IN_USE    0x13
#define AV_ERR_NOT_SUPPORTED 0x19
#define AV_ERR_BAD_STATE     0x31
#define AV_DISCOVER_MS      3000    /* our DISCOVER is retried once after this */

/* Local stream end point: A2DP Source, audio, SBC. */
#define LOCAL_SEID   1
#define SBC_IE0      0x31    /* 48 kHz | 44.1 kHz | joint stereo */
#define SBC_IE1      0x15    /* blocks 16 | subbands 8 | loudness */
#define SBC_MIN_BP   2
#define SBC_MAX_BP   45      /* CSR8510 + PS4 2.4 GHz coexistence */
#define SBC_ADAPT_MIN_BP 29  /* lowest bitpool used by congestion adaptation */

/* Test tone */
#define STREAM_POLL_MS 2     /* USB poll timeout while streaming */
#define STREAM_AHEAD_US 40000 /* how far ahead of real time we may send */
#define STREAM_LATE_US  300000 /* further behind than this: resync (real stall) */
/* Up to STREAM_LATE_US behind we catch up (one packet per loop, only
 * with free ACL credits) instead of skipping ahead. Skipping 100+ ms every
 * few seconds made the game PCM pile up (pcm_over / resyncs every ~5 s). */
#define CREDIT_WAIT_MEDIA_MS 30   /* media: wait this long for credits, else drop */
#define CREDIT_WAIT_SIG_MS   250  /* signaling: wait this long (was 1000: stalled the stream) */
#define CREDIT_LEAK_MS       3000 /* no completion at all for this long: resync */
#define QUIT_PATH     "/data/hearbridge/quit"
#define QUIT_PATH_ALT "/user/data/hearbridge/quit"
#define SILENCE_KEEPALIVE 1   /* send silent SBC when UDP is empty */
#define PAIR_RETRY_MS    4000   /* quiet retry while waiting for pairing mode */
#define PAIR_WINDOW_MS   180000 /* how long to keep the quick pairing retries */
#define PAIR_GIVEUP_MS   10000  /* retry interval after the pairing window */
#define NOTIFY_GAP_MS    10000  /* at most one notification per 10 s */
#define INQ_STOP_WAIT_MS 3000   /* wait for Inquiry Complete after cancel */
#define EV_BUF           1024   /* event assembler; one event is <= 257 bytes */
#define ACL_HOLD_MAX     8      /* ACL packets held for a not-yet-known handle */
#define ACL_HOLD_MS      2000

/* Session results */
#define SES_RETRY         0
#define SES_STOP          1
#define SES_NEED_PAIRING  2
#define SES_IDLE          3   /* nothing to connect to: wait for the UI */

/* L2CAP channel roles */
#define ROLE_SDP    1
#define ROLE_AVSIG  2
#define ROLE_MEDIA  3
#define ROLE_AVCTP  4
#define ROLE_RFCOMM 5
#define MAX_CH      8

/* ---- libkernel (linked directly in both toolchains) ------------------ */

int32_t  sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                                  uint32_t flags, void *opt, int *res);
int32_t  sceKernelDlsym(int32_t handle, const char *sym, void **addr);
int32_t  sceKernelSendNotificationRequest(int32_t dev, void *req, size_t size,
                                          int32_t blocking);
uint64_t sceKernelGetProcessTime(void);
int32_t  sceKernelUsleep(uint32_t usec);
int32_t  scePthreadCreate(void **thr, const void *attr, void *(*fn)(void *), void *arg,
                          const char *name);
int32_t  scePthreadJoin(void *thr, void **ret);

/* OrbisNotificationRequest. Natural alignment, NOT packed: the fields add up
 * to 0xC2D bytes and the struct pads to 0xC30, the size the notification
 * device expects. Declared packed (0xC2D bytes) the request is refused
 * and no notification appears. */
struct notify_req {
    int32_t type;
    int32_t req_id;
    int32_t priority;
    int32_t msg_id;
    int32_t target_id;
    int32_t user_id;
    int32_t unk1;
    int32_t unk2;
    int32_t app_id;
    int32_t error_num;
    int32_t unk3;
    char    use_icon_uri;
    char    message[1024];
    char    icon_uri[1024];
    char    unk[1024];
};
_Static_assert(sizeof(struct notify_req) == 0xC30, "notification request must be 0xC30 bytes");
_Static_assert(offsetof(struct notify_req, message) == 0x2D, "message must start at 0x2D");
_Static_assert(offsetof(struct notify_req, icon_uri) == 0x42D, "icon_uri must start at 0x42D");

/* ---- libSceUsbd, resolved at run time --------------------------------- */

typedef void usb_dev;   /* libusb_device */
typedef void usb_hnd;   /* libusb_device_handle */

struct usb_dev_desc {   /* libusb_device_descriptor, natural layout */
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

static struct {
    int32_t (*Init)(void);
    int32_t (*Exit)(void);
    int32_t (*GetDeviceList)(usb_dev ***list);
    int32_t (*FreeDeviceList)(usb_dev **list);
    int32_t (*GetDeviceDescriptor)(usb_dev *d, struct usb_dev_desc *out);
    uint8_t (*GetBusNumber)(usb_dev *d);
    uint8_t (*GetDeviceAddress)(usb_dev *d);
    int32_t (*Open)(usb_dev *d, usb_hnd **h);
    void    (*Close)(usb_hnd *h);
    int32_t (*GetDescriptor)(usb_hnd *h, uint8_t type, uint8_t idx,
                             unsigned char *data, int32_t len);
    int32_t (*KernelDriverActive)(usb_hnd *h, int32_t iface);
    int32_t (*DetachKernelDriver)(usb_hnd *h, int32_t iface);
    int32_t (*AttachKernelDriver)(usb_hnd *h, int32_t iface);
    int32_t (*ClaimInterface)(usb_hnd *h, int32_t iface);
    int32_t (*ReleaseInterface)(usb_hnd *h, int32_t iface);
    int32_t (*ControlTransfer)(usb_hnd *h, uint8_t rt, uint8_t req, uint16_t val,
                               uint16_t idx, unsigned char *data, uint16_t len,
                               uint32_t timeout);
    int32_t (*InterruptTransfer)(usb_hnd *h, unsigned char ep, unsigned char *data,
                                 int32_t len, int32_t *done, uint32_t timeout);
    int32_t (*BulkTransfer)(usb_hnd *h, unsigned char ep, unsigned char *data,
                            int32_t len, int32_t *done, uint32_t timeout);
} U;

/* ---- output: log file, stdout, notifications -------------------------- */

static FILE *g_log;
static const char *g_log_path;      /* where g_log was opened */
static long g_log_bytes;
static char  g_summary[600];
#define LOG_CAP_BYTES (512 * 1024)  /* then the log restarts; the last full one stays as .old */

static long now_ms(void)
{
    return (long)(sceKernelGetProcessTime() / 1000u);
}

/* to_page: also keep the line in the web page's log ring (ui_logbuf_add).
 * Raw hex dumps stay in the log file only. */
static void out_x(int to_page, const char *fmt, va_list ap)
{
    char line[600];
    vsnprintf(line, sizeof line, fmt, ap);
    printf("[hb-probe] %s\n", line);
    if (g_log) {
        if (g_log_bytes > LOG_CAP_BYTES && g_log_path) {
            char old[96];
            fclose(g_log);
            snprintf(old, sizeof old, "%s.old", g_log_path);
            rename(g_log_path, old);
            g_log = fopen(g_log_path, "w");
            g_log_bytes = 0;
        }
        if (g_log) {
            int w = fprintf(g_log, "[%7ld] %s\n", now_ms(), line);
            if (w > 0) g_log_bytes += w;
            fflush(g_log);
        }
    }
    if (to_page) ui_logbuf_add(line);
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_x(1, fmt, ap);
    va_end(ap);
}

static void out_file(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out_file(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_x(0, fmt, ap);
    va_end(ap);
}

/* avrcp.c logs through log_line (log.h). */
void log_line(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    out("%s", line);
}

static void hexline(const char *tag, const unsigned char *d, int n)
{
    char line[3 * 48 + 8];
    int i, pos = 0, show = n > 48 ? 48 : n;
    for (i = 0; i < show; i++)
        pos += snprintf(line + pos, sizeof line - (size_t)pos, "%02x%s",
                        d[i], i + 1 < show ? " " : "");
    line[pos] = 0;
    out_file("%s (%d): %s%s", tag, n, line, n > show ? " ..." : "");
}

/* A short line for the final summary notification (also logged). */
static void say(const char *fmt, ...)
{
    char line[256];
    size_t n = strlen(g_summary);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    out("%s", line);
    ui_log(line);
    if (strncmp(line, "FAIL", 4) == 0)
        ui_set_error(line[4] == ':' && line[5] == ' ' ? line + 6 : line);
    if (n + strlen(line) + 2 < sizeof g_summary)
        snprintf(g_summary + n, sizeof g_summary - n, "%s%s", n ? "\n" : "", line);
}

static int notify_via_sysutil(const char *text)
{
    static int tried, h = -1;
    static int32_t (*fn)(int32_t, const char *);
    if (!tried) {
        void *p = NULL;
        tried = 1;
        h = sceKernelLoadStartModule("/system/common/lib/libSceSysUtil.sprx",
                                     0, NULL, 0, NULL, NULL);
        if (h >= 0 && sceKernelDlsym(h, "sceSysUtilSendSystemNotificationWithText", &p) == 0 && p)
            memcpy(&fn, &p, sizeof p);
        out("notify: libSceSysUtil handle 0x%08x, function %s", (unsigned)h,
            fn ? "found" : "missing");
    }
    if (!fn) return -1;
    return fn(222, text);
}

/* On-screen notification. Tries sceKernelSendNotificationRequest (libkernel),
 * then a direct write to /dev/notification0 (what libkernel does inside),
 * then sceSysUtilSendSystemNotificationWithText. Each result is logged. */
static void notify_now(const char *fmt, ...)
{
    static struct notify_req r;
    char text[512];
    va_list ap;
    int rc, fd;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    memset(&r, 0, sizeof r);
    r.type = 0;
    r.target_id = -1;
    r.use_icon_uri = 1;
    snprintf(r.message, sizeof r.message, "%s", text);
    snprintf(r.icon_uri, sizeof r.icon_uri, "cxml://psnotification/tex_icon_system");

    rc = sceKernelSendNotificationRequest(0, &r, sizeof r, 0);
    out("notify \"%s\" -> 0x%08x", text, (unsigned)rc);
    if (rc >= 0) return;

    fd = open("/dev/notification0", O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        ssize_t w = write(fd, &r, sizeof r);
        close(fd);
        out("notify: /dev/notification0 write -> %ld", (long)w);
        if (w == (ssize_t)sizeof r) return;
    } else {
        out("notify: /dev/notification0 open failed");
    }
    rc = notify_via_sysutil(text);
    out("notify: sceSysUtilSendSystemNotificationWithText -> 0x%08x", (unsigned)rc);
}

/* ---- state file and rate-limited notifications -------------------------- */

static char g_state[32];
static char g_notif_last[160];     /* text of the last shown notification */
static char g_notif_pending[160];  /* newest text waiting for the rate limit */
static long g_notif_at;
static int  g_notif_any;
static int  g_quiet;               /* retries: only loud states notify */
static int  g_no_adapter;          /* sessions in a row that found no dongle (0: it was there) */

/* Show the pending notification once NOTIFY_GAP_MS has passed since the
 * previous one. Called from the USB pump and the reconnect wait. */
static void notify_tick(void)
{
    if (!g_notif_pending[0]) return;
    if (g_notif_any && now_ms() - g_notif_at < NOTIFY_GAP_MS) return;
    notify_now("%s", g_notif_pending);
    snprintf(g_notif_last, sizeof g_notif_last, "%s", g_notif_pending);
    g_notif_pending[0] = 0;
    g_notif_at = now_ms();
    g_notif_any = 1;
}

static void post_notification(const char *text)
{
    if (strcmp(text, g_notif_last) == 0) {
        /* Same as what is on screen already; drop anything older queued. */
        if (g_notif_pending[0]) out("notify: dropped queued \"%s\"", g_notif_pending);
        g_notif_pending[0] = 0;
        return;
    }
    if (g_notif_pending[0] && strcmp(g_notif_pending, text) != 0)
        out("notify: replacing queued \"%s\"", g_notif_pending);
    snprintf(g_notif_pending, sizeof g_notif_pending, "%s", text);
    notify_tick();
}

static int write_state_file(const char *path, const char *st)
{
    char tmp[96];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) return -1;
    ok = fprintf(f, "%s\n", st) > 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Record a state. The state file and log are updated on every change; the
 * notification text (if any) is shown only on a change, at most once per
 * NOTIFY_GAP_MS, and never twice in a row. loud = 0 texts are skipped while
 * retrying quietly. */
/* ---- single instance ------------------------------------------ */
static char g_token[48];
static int  g_quit;               /* a newer instance asked us to exit */
static long g_quit_checked;

static int read_small(const char *path, char *out, size_t outlen)
{
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f) return -1;
    n = fread(out, 1, outlen - 1, f);
    fclose(f);
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return 0;
}

/* Cheap: at most one small file read every 500 ms. */
static void quit_check(void)
{
    char t[64];
    long now = (long)(sceKernelGetProcessTime() / 1000u);
    if (g_quit || now - g_quit_checked < 500) return;
    g_quit_checked = now;
    if ((read_small(QUIT_PATH, t, sizeof t) == 0 || read_small(QUIT_PATH_ALT, t, sizeof t) == 0) &&
        t[0] && strcmp(t, g_token) != 0)
        g_quit = 1;
}

static void set_state(const char *st, const char *msg, int loud)
{
    if (strcmp(st, g_state) == 0) return;
    snprintf(g_state, sizeof g_state, "%s", st);
    out("state: %s", st);
    ui_set_state(st);
    if (write_state_file(STATE_PATH, st) != 0 && write_state_file(STATE_PATH_ALT, st) != 0)
        out("state: could not write %s", STATE_PATH);
    if (msg && (loud || !g_quiet)) post_notification(msg);
}

static const char *usb_err(int rc)
{
    switch (rc) {
    case 0:   return "ok";
    case -1:  return "I/O error";
    case -2:  return "invalid parameter";
    case -3:  return "access denied";
    case -4:  return "no device (unplugged)";
    case -5:  return "not found";
    case -6:  return "busy (held by another driver)";
    case -7:  return "timeout";
    case -8:  return "overflow";
    case -9:  return "pipe error (stall)";
    case -10: return "interrupted";
    case -11: return "out of memory";
    case -12: return "not supported";
    default:  return "error";
    }
}

static const char *hci_err(int s)
{
    switch (s) {
    case 0x00: return "success";
    case 0x02: return "unknown connection";
    case 0x04: return "page timeout (device not answering)";
    case 0x05: return "authentication failure";
    case 0x06: return "PIN or key missing";
    case 0x07: return "memory capacity exceeded";
    case 0x08: return "connection timeout";
    case 0x09: return "connection limit exceeded";
    case 0x0B: return "connection already exists";
    case 0x0C: return "command disallowed";
    case 0x0D: return "rejected, limited resources";
    case 0x0E: return "rejected, security reasons";
    case 0x0F: return "rejected, unacceptable BD_ADDR";
    case 0x10: return "accept timeout";
    case 0x11: return "unsupported feature or parameter";
    case 0x12: return "invalid parameters";
    case 0x13: return "remote user terminated";
    case 0x14: return "remote low resources";
    case 0x15: return "remote power off";
    case 0x16: return "terminated by local host";
    case 0x17: return "repeated attempts";
    case 0x18: return "pairing not allowed";
    case 0x1A: return "unsupported remote feature";
    case 0x1F: return "unspecified error";
    case 0x22: return "LMP response timeout";
    case 0x23: return "LMP transaction collision";
    case 0x25: return "encryption mode not acceptable";
    case 0x29: return "pairing with unit key not supported";
    case 0x2F: return "insufficient security";
    case 0x37: return "simple pairing not supported by host";
    default:   return "error";
    }
}

static const char *l2_result(unsigned r)
{
    switch (r) {
    case 0: return "success";
    case 1: return "pending";
    case 2: return "PSM not supported";
    case 3: return "security block";
    case 4: return "no resources";
    case 6: return "invalid source CID";
    case 7: return "source CID already allocated";
    default: return "refused";
    }
}

static unsigned le16(const unsigned char *p) { return (unsigned)(p[0] | (p[1] << 8)); }
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static void addr_str(const unsigned char a[6], char s[18])
{
    snprintf(s, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* ---- module loading ---------------------------------------------------- */

/* OrbisSysModule USBD = 0xB7; declared to avoid header path issues in plugin build. */
int32_t sceSysmoduleLoadModule(int moduleId);

static int load_usbd(void)
{
    static const char *paths[] = {
        /* Game sandbox cannot see /system; GoldHEN plugins dir is visible. */
        "/data/GoldHEN/plugins/libSceUsbd.sprx",
        "/data/hearbridge/libSceUsbd.sprx",
        "libSceUsbd.sprx",
        "/system/common/lib/libSceUsbd.sprx",
        "/system/priv/lib/libSceUsbd.sprx",
    };
    int32_t h = -1;
    unsigned i;

    /* Prefer sysmodule when available (works from launcher / ShellUI). */
    {
        int32_t sm = sceSysmoduleLoadModule(0x00B7); /* ORBIS_SYSMODULE_USBD */
        out("sceSysmoduleLoadModule(USBD) -> 0x%08x", (unsigned)sm);
    }
    for (i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, NULL);
        out("load %s -> 0x%08x", paths[i], (unsigned)h);
        if (h >= 0) break;
    }
    if (h < 0) {
        say("FAIL: could not load libSceUsbd.sprx (0x%08x)", (unsigned)h);
        return 0;
    }

#define SYM(field, name, required) do {                                     \
        void *p_ = NULL;                                                     \
        if (sceKernelDlsym(h, name, &p_) < 0 || !p_) {                       \
            out("symbol %s missing", name);                                  \
            if (required) { say("FAIL: libSceUsbd has no %s", name); return 0; } \
        }                                                                    \
        memcpy(&U.field, &p_, sizeof p_);                                    \
    } while (0)

    SYM(Init,               "sceUsbdInit",               1);
    SYM(Exit,               "sceUsbdExit",               0);
    SYM(GetDeviceList,      "sceUsbdGetDeviceList",      1);
    SYM(FreeDeviceList,     "sceUsbdFreeDeviceList",     0);
    SYM(GetDeviceDescriptor,"sceUsbdGetDeviceDescriptor",1);
    SYM(GetBusNumber,       "sceUsbdGetBusNumber",       0);
    SYM(GetDeviceAddress,   "sceUsbdGetDeviceAddress",   0);
    SYM(Open,               "sceUsbdOpen",               1);
    SYM(Close,              "sceUsbdClose",              1);
    SYM(GetDescriptor,      "sceUsbdGetDescriptor",      0);
    SYM(KernelDriverActive, "sceUsbdKernelDriverActive", 0);
    SYM(DetachKernelDriver, "sceUsbdDetachKernelDriver", 0);
    SYM(AttachKernelDriver, "sceUsbdAttachKernelDriver", 0);
    SYM(ClaimInterface,     "sceUsbdClaimInterface",     1);
    SYM(ReleaseInterface,   "sceUsbdReleaseInterface",   1);
    SYM(ControlTransfer,    "sceUsbdControlTransfer",    1);
    SYM(InterruptTransfer,  "sceUsbdInterruptTransfer",  1);
    SYM(BulkTransfer,       "sceUsbdBulkTransfer",       1);
#undef SYM
    return 1;
}

/* ---- descriptor parsing (after PS5 src/bt/usb_hci_desc.c) --------------- */

static struct {
    uint8_t ev, in, out;
    uint16_t ev_mps, in_mps, out_mps;
} EP;

/* Interface 0, alt 0: interrupt IN = events, bulk IN / bulk OUT = ACL.
 * Fallbacks are the usual CSR8510 addresses 0x81 / 0x82 / 0x02. */
static void find_endpoints(usb_hnd *h)
{
    unsigned char cfg[512];
    int n = -1, i, cur_if = -1, cur_alt = -1;

    if (U.GetDescriptor)
        n = U.GetDescriptor(h, 0x02 /* CONFIGURATION */, 0, cfg, sizeof cfg);
    if (n >= 9) {
        for (i = 0; i + 2 <= n && cfg[i] >= 2 && i + cfg[i] <= n; i += cfg[i]) {
            uint8_t type = cfg[i + 1];
            if (type == 0x04 && cfg[i] >= 9) {               /* INTERFACE */
                cur_if = cfg[i + 2];
                cur_alt = cfg[i + 3];
                out("interface %d alt %d class %02x/%02x/%02x", cur_if, cur_alt,
                    cfg[i + 5], cfg[i + 6], cfg[i + 7]);
            } else if (type == 0x05 && cfg[i] >= 7) {        /* ENDPOINT */
                uint8_t addr = cfg[i + 2], kind = cfg[i + 3] & 3;
                uint16_t mps = (uint16_t)((cfg[i + 4] | (cfg[i + 5] << 8)) & 0x7FF);
                out("  endpoint 0x%02x type %d mps %d", addr, kind, mps);
                if (cur_if != 0 || cur_alt != 0) continue;
                if (kind == 3 && (addr & 0x80) && !EP.ev)  { EP.ev = addr;  EP.ev_mps = mps; }
                if (kind == 2 && (addr & 0x80) && !EP.in)  { EP.in = addr;  EP.in_mps = mps; }
                if (kind == 2 && !(addr & 0x80) && !EP.out) { EP.out = addr; EP.out_mps = mps; }
            }
        }
    } else {
        out("config descriptor read failed (%d), using default endpoints", n);
    }
    if (!EP.ev)  { EP.ev = 0x81;  out("no interrupt IN found, using 0x81"); }
    if (!EP.in)  { EP.in = 0x82;  out("no bulk IN found, using 0x82"); }
    if (!EP.out) { EP.out = 0x02; out("no bulk OUT found, using 0x02"); }
    out("endpoints: events 0x%02x, ACL in 0x%02x (mps %d), ACL out 0x%02x (mps %d)",
        EP.ev, EP.in, EP.in_mps, EP.out, EP.out_mps);
}

/* ---- state ------------------------------------------------------------- */

struct dev {
    unsigned char addr[6];
    uint8_t  psrm;
    uint32_t cod;
    uint16_t clk;
    int      rssi;          /* last RSSI, 127 = not reported */
    int      rssi_sum;      /* for average (only real samples) */
    int      rssi_n;
    char     name[64];
    int      name_status;   /* -1 not asked, else HCI status */
    int      name_tried;    /* ui_scan asked (or skipped) the name */
};

static int dev_rssi_avg(const struct dev *d)
{
    if (d->rssi_n > 0) return d->rssi_sum / d->rssi_n;
    return d->rssi;
}

enum ch_state { CH_FREE, CH_CONNECTING, CH_CONFIG, CH_OPEN, CH_CLOSED };

struct chan {
    enum ch_state st;
    int      role;          /* ROLE_SDP / ROLE_AVSIG / ROLE_MEDIA */
    int      inbound, pending;
    unsigned psm, scid, dcid, result;  /* scid = our (unique) local CID */
    int      ours_ok, theirs_ok;
    unsigned peer_mtu;
    unsigned char cfg_id;
};

static usb_hnd *H;

static struct {
    /* command tracking */
    unsigned wait_op;
    int      cc_done, cs_done, cs_status;
    unsigned char cc[260];
    int      cc_len;
    int      transport_dead;
    int      poll_ms;
    /* event assembler */
    unsigned char ev[EV_BUF];
    int      ev_fill;
    int      ev_resync;         /* bytes dropped to find the next event header */
    /* controller */
    unsigned char local_addr[6];
    unsigned acl_mtu, acl_pkts;
    int      acl_inflight;      /* ACL packets not yet completed by controller */
    int      acl_stalls;
    long     acl_last_credit_ms;  /* last Number_Of_Completed_Packets for us */
    int      acl_media_drops;     /* media frames dropped for lack of credits */
    int      acl_leak_resync;
    int      media_tx;            /* l2_send is sending a media frame */
    int      user_abort;          /* a UI command ended this session */
    /* inquiry */
    struct dev dev[MAX_DEV];
    int      ndev;
    int      inq_done, inq_status;
    int      scan_ui;           /* inside ui_scan(): publish results live */
    int      scan_abort;        /* a UI command arrived: stop the scan cleanly */
    int      inq_active;        /* Inquiry accepted and not yet complete/cancelled */
    /* remote name */
    int      name_done;
    /* connection */
    unsigned char peer[6];
    int      conn_done, conn_status, connected;
    int      conn_pending;      /* Create_Connection sent, waiting for our CC */
    int      accept_in;         /* reconnect loop: accept a saved headset that calls us */
    int      inbound;           /* the current link was started by the headset */
    uint32_t in_cod;            /* class from its Connection Request */
    int      page_scan;         /* page scan is on in the controller */
    unsigned handle;
    int      disc_done, disc_reason;
    int      auth_done, auth_status;
    int      enc_done, enc_status, enc_on;
    int      ssp_done, ssp_status;
    int      have_key, key_type;
    unsigned char link_key[16];
    int      need_pairing;      /* auth refused (0x18/0x05) without a saved key */
    int      authed;
    /* ACL that arrived before its Connection Complete */
    struct { unsigned char d[516]; int n; unsigned handle; long t; } hold[ACL_HOLD_MAX];
    int      nhold, hold_replay;
    /* ACL reassembly */
    unsigned char acl[2048];
    int      acl_fill, acl_need;
    /* L2CAP */
    unsigned char sig_id;
    unsigned next_cid;          /* next local CID, never reused in a run */
    struct chan ch[MAX_CH];
    unsigned info_wait_id;
    int      info_got;
    int      disc_rsp_got;
    long     media_rx;
    int      stop_requested;
} G;

/* Kept across sessions (G is cleared per session). */
static int g_ignore_saved_key;  /* headset rejected our key: pair fresh next time */
static int g_end_reason = -1;   /* why the link ended: HCI reason from the headphones, 0x13 when they closed the stream, -1 unknown */
/* Case close / power button (0x13) or remote power-off (0x15): stay disconnected.
 * Out-of-range supervision timeout (0x08) is a drop that may rejoin. */
static int drop_is_away(int reason)
{
    return reason == 0x13 || reason == 0x15;
}
static int g_session_streamed;  /* last session reached streaming */

/* AVDTP state for our single local SEP. */
static struct {
    int  configured, cfg_by_peer;
    int  opened, open_by_peer;
    int  started, start_by_peer;
    int  closed_by_peer;
    int  remote_seid;           /* peer SEP in our stream */
    unsigned char sbc_ie[4];    /* configured SBC codec information */
    unsigned char tl;           /* our transaction label counter */
    unsigned char wait_label, wait_sig;
    int  rsp_done, rsp_msg, rsp_len;
    unsigned char rsp[256];
} AV;

/* AVRCP absolute volume (src/avrcp.c), reset per session. */
static avrcp_state AVR;
static hfp_state HFP;
static unsigned g_hfp_scid;
static int g_hs_vol = -1;       /* last headphone level (0..127) seen this run */
/* ---- transport --------------------------------------------------------- */

static void on_event(const unsigned char *ev, int n);
static void on_acl(const unsigned char *pkt, int n);
static void notify_tick(void);
static void on_l2cap(unsigned cid, const unsigned char *d, int len);

static int hci_send(unsigned op, const void *params, int plen)
{
    unsigned char cmd[260];
    int rc;
    cmd[0] = (unsigned char)(op & 0xFF);
    cmd[1] = (unsigned char)(op >> 8);
    cmd[2] = (unsigned char)plen;
    if (plen > 0) memcpy(cmd + 3, params, (size_t)plen);
    rc = U.ControlTransfer(H, 0x20 /* host-to-device, class, device */, 0x00,
                           0, 0, cmd, (uint16_t)(3 + plen), CTRL_TIMEOUT_MS);
    hexline("hci cmd", cmd, 3 + plen);
    if (rc < 0) {
        out("hci cmd 0x%04x send error %d (%s)", op, rc, usb_err(rc));
        if (rc == -4) G.transport_dead = 1;
        return 0;
    }
    return 1;
}

static void pump_once(void);
static int wait_flag(volatile int *flag, long timeout_ms);

static int acl_send_raw(const unsigned char *pkt, int n)
{
    int32_t done = 0;
    int rc;
    /* Host flow control: never exceed the controller's ACL buffer count.
     * Callers that must not block (media) check acl_credits_for() first. */
    if (G.acl_pkts && G.acl_inflight >= (int)G.acl_pkts) {
        long deadline = now_ms() + CREDIT_WAIT_SIG_MS;
        G.acl_stalls++;
        while (G.acl_inflight >= (int)G.acl_pkts && now_ms() < deadline &&
               !G.transport_dead && G.connected)
            pump_once();
        if (G.acl_inflight >= (int)G.acl_pkts) {
            if (now_ms() - G.acl_last_credit_ms >= CREDIT_LEAK_MS) {
                /* No completion for a long time: the count leaked. */
                out("acl: no completions for %ld ms (%d in flight), resyncing credits",
                    now_ms() - G.acl_last_credit_ms, G.acl_inflight);
                G.acl_inflight = 0;
                G.acl_leak_resync++;
                G.acl_last_credit_ms = now_ms();
            } else {
                out("acl: no buffer credits for %d ms (%d in flight), dropping packet",
                    CREDIT_WAIT_SIG_MS, G.acl_inflight);
                return 0;
            }
        }
    }
    rc = U.BulkTransfer(H, EP.out, (unsigned char *)pkt, n, &done, BULK_OUT_TIMEOUT);
    if (rc < 0 || done != n) {
        out("acl tx error %d (%s), done %d/%d", rc, usb_err(rc), (int)done, n);
        if (rc == -4) G.transport_dead = 1;
        return 0;
    }
    G.acl_inflight++;
    if (G.acl_inflight == 1) G.acl_last_credit_ms = now_ms(); /* outstanding period starts */
    return 1;
}

/* Dispatch every complete event in the assembler. */
static void ev_drain(void)
{
    while (G.ev_fill >= 2) {
        int need;
        if (!hci_evt_plausible(G.ev[0], G.ev[1])) {
            /* Lost framing (a USB packet went missing): drop a byte and look
             * again, the next real event header brings the stream back. */
            if (G.ev_resync++ < 5)
                out("hci: event framing lost (0x%02x len %u), resyncing", G.ev[0], G.ev[1]);
            memmove(G.ev, G.ev + 1, (size_t)(G.ev_fill - 1));
            G.ev_fill--;
            continue;
        }
        need = 2 + G.ev[1];
        if (G.ev_fill < need) break;
        on_event(G.ev, need);
        memmove(G.ev, G.ev + need, (size_t)(G.ev_fill - need));
        G.ev_fill -= need;
    }
}

/* Replay ACL packets that arrived before their Connection Complete. */
static void acl_replay_held(void)
{
    int i, n = G.nhold;
    G.hold_replay = 0;
    G.nhold = 0;
    for (i = 0; i < n; i++) {
        if (G.connected && G.hold[i].handle == (G.handle & 0x0FFF) &&
            now_ms() - G.hold[i].t <= ACL_HOLD_MS) {
            out("acl: replaying %d held bytes for handle 0x%03x", G.hold[i].n, G.hold[i].handle);
            on_acl(G.hold[i].d, G.hold[i].n);
        } else {
            out("acl: dropping held packet for handle 0x%03x", G.hold[i].handle);
        }
    }
}

/* ---- HCI event reader thread ----------------------------------
 * The interrupt endpoint is read by its own thread with a long timeout, so
 * events (notably Number_Of_Completed_Packets) are never lost to a short
 * transfer timing out, and are read even while the main thread is blocked
 * in a bulk OUT. Bytes go to an SPSC ring that pump_once() drains. */
#define EVQ_SIZE 65536u           /* power of two */
#define EVT_TIMEOUT_MS 200
static struct {
    unsigned char q[EVQ_SIZE];
    volatile uint32_t wr, rd;     /* wr: thread, rd: main */
    volatile int run, running, err, dead;
    volatile long full_waits;
    void *thr;
} EVT;

/* Raw HCI events for hci_cmd_sync() (Realtek prepare and shared BD_ADDR).
 * on_event() still runs; this is an extra copy while capture is on. */
#define HCI_CAP_MAX 8
static unsigned char g_cap_ev[HCI_CAP_MAX][HCI_PKT_MAX];
static int g_cap_len[HCI_CAP_MAX];
static int g_cap_n, g_cap_rd, g_cap_on;
static void cap_push(const unsigned char *ev, int n)
{
    int i;
    if (!g_cap_on || n < 2 || n > HCI_PKT_MAX) return;
    if (g_cap_n >= HCI_CAP_MAX) return;
    i = g_cap_n++;
    memcpy(g_cap_ev[i], ev, (size_t)n);
    g_cap_len[i] = n;
}

static int hci_wrap_cmd(void *self, unsigned op, const void *args, int nargs)
{
    (void)self;
    return hci_send(op, args, nargs);
}

static int hci_wrap_pump(void *self, int wait_ms)
{
    long deadline = now_ms() + (wait_ms > 0 ? wait_ms : 0);
    (void)self;
    if (wait_ms <= 0) {
        pump_once();
        return g_cap_rd < g_cap_n;
    }
    while (now_ms() < deadline && !G.transport_dead) {
        if (g_cap_rd < g_cap_n) return 1;
        pump_once();
    }
    if (g_cap_rd < g_cap_n) return 1;
    return G.transport_dead ? -1 : 0;
}

static int hci_wrap_next_event(void *self, unsigned char *dst, int cap)
{
    int n;
    (void)self;
    if (g_cap_rd >= g_cap_n) return 0;
    n = g_cap_len[g_cap_rd];
    if (n > cap) n = cap;
    if (n > 0) memcpy(dst, g_cap_ev[g_cap_rd], (size_t)n);
    g_cap_rd++;
    if (g_cap_rd >= g_cap_n) g_cap_rd = g_cap_n = 0;
    return n;
}

static int hci_wrap_next_acl(void *self, unsigned char *dst, int cap)
{
    (void)self; (void)dst; (void)cap;
    return 0;
}

static const hci_ops g_hci_ops = {
    .next_acl = hci_wrap_next_acl,
    .next_event = hci_wrap_next_event,
    .pump = hci_wrap_pump,
    .cmd = hci_wrap_cmd,
};

static hci_t hci_live(void)
{
    hci_t h;
    g_cap_rd = g_cap_n = 0;
    g_cap_on = 1;
    h.ctx = &H;
    h.ops = &g_hci_ops;
    return h;
}

static void hci_live_done(void)
{
    g_cap_on = 0;
    g_cap_rd = g_cap_n = 0;
}

static void *evt_main(void *arg)
{
    unsigned char buf[512];
    (void)arg;
    while (EVT.run) {
        int32_t got = 0;
        int rc = U.InterruptTransfer(H, EP.ev, buf, sizeof buf, &got, EVT_TIMEOUT_MS);
        /* A timeout can still carry the first packets of an event: keep them,
         * dropping them would cut the next event in half. */
        if (got > 0 && (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT || rc == SCE_USBD_ERROR_TIMEOUT)) {
            int32_t i = 0;
            while (i < got && EVT.run) {
                uint32_t wr = __atomic_load_n(&EVT.wr, __ATOMIC_RELAXED);
                uint32_t rd = __atomic_load_n(&EVT.rd, __ATOMIC_ACQUIRE);
                uint32_t room = EVQ_SIZE - (wr - rd);
                uint32_t n;
                if (room == 0) {          /* never drop events: wait for main */
                    EVT.full_waits++;
                    sceKernelUsleep(1000);
                    continue;
                }
                n = (uint32_t)(got - i);
                if (n > room) n = room;
                {
                    uint32_t k;
                    for (k = 0; k < n; k++)
                        EVT.q[(wr + k) & (EVQ_SIZE - 1)] = buf[i + (int32_t)k];
                }
                __atomic_store_n(&EVT.wr, wr + n, __ATOMIC_RELEASE);
                i += (int32_t)n;
            }
        } else if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT && rc != SCE_USBD_ERROR_TIMEOUT) {
            EVT.err = rc;
            if (rc == -4) { EVT.dead = 1; break; }
            sceKernelUsleep(2000);        /* avoid a hot loop on persistent errors */
        }
    }
    EVT.running = 0;
    return NULL;
}

static void evt_start(void)
{
    int rc;
    EVT.wr = EVT.rd = 0;
    EVT.err = EVT.dead = 0;
    EVT.full_waits = 0;
    EVT.run = 1;
    EVT.running = 1;
    rc = scePthreadCreate(&EVT.thr, NULL, evt_main, NULL, "hb_hci_evt");
    if (rc != 0) {
        EVT.run = EVT.running = 0;
        out("hci: event thread create failed 0x%08x, polling inline", (unsigned)rc);
    } else {
        out("hci: event reader thread started (timeout %d ms)", EVT_TIMEOUT_MS);
    }
}

static void evt_stop(void)
{
    if (!EVT.run && !EVT.running) return;
    EVT.run = 0;
    if (EVT.thr) scePthreadJoin(EVT.thr, NULL);
    EVT.thr = NULL;
    EVT.running = 0;
}

static void evt_feed(const unsigned char *chunk, int got)
{
    /* Copy in pieces and dispatch complete events after each piece. After
     * a drain at most one partial event (< 258 bytes) is left, so there is
     * always room and nothing is ever discarded. */
    int off = 0;
    while (off < got) {
        int room = (int)sizeof G.ev - G.ev_fill;
        int take = got - off;
        if (take > room) take = room;
        memcpy(G.ev + G.ev_fill, chunk + off, (size_t)take);
        G.ev_fill += take;
        off += take;
        ev_drain();
    }
}

/* ---- UI commands ---------------------------------------------- */
static struct ui_cmd g_pending;   /* taken from the UI, not yet applied */
static int g_have_pending;
static int g_want;                /* user picked a device to connect */
static unsigned char g_want_addr[6];
static int g_scan_req;            /* run a UI scan in the next session */
static int addr_allowed(const unsigned char addr[6], uint32_t cod);
static struct dev g_scan_devs[UI_SCAN_MAX];   /* last scan, audio devices only */
static int g_nscan;
static int g_idle;                /* user disconnected: no auto reconnect */
/* The user pressed Disconnect: nothing connects (not after a failed scan
 * session, not after a daemon restart) until the user picks a device again.
 * Kept in USER_DISC_PATH so it survives a restart. */
#define USER_DISC_PATH "/data/hearbridge/user_disconnected"
static int g_user_disc;

static void user_disc_set(int on)
{
    g_user_disc = on;
    if (on) {
        FILE *f = fopen(USER_DISC_PATH, "w");
        if (f) { fputs("1\n", f); fclose(f); }
    } else {
        unlink(USER_DISC_PATH);
    }
}

/* May the UI connect to this address? Only an audio sink from the last
 * scan or the saved list (cod.h). */
static int connect_target_ok(const unsigned char addr[6])
{
    uint32_t cod = 0;
    int i;
    for (i = 0; i < g_nscan; i++)
        if (memcmp(g_scan_devs[i].addr, addr, 6) == 0) { cod = g_scan_devs[i].cod; break; }
    return addr_allowed(addr, cod);
}
static int g_ui_stop;
static char g_cur_name[64];
static uint32_t g_cur_cod;

/* Does this command end the running session? */
static int cmd_aborts_session(const struct ui_cmd *c)
{
    switch (c->type) {
    case UI_CMD_SCAN:
    case UI_CMD_DISCONNECT:
    case UI_CMD_STOP:
    case UI_CMD_REOPEN:
        return 1;
    case UI_CMD_CONNECT:
        return !(G.connected && memcmp(c->addr, G.peer, 6) == 0);
    case UI_CMD_FORGET:
        return memcmp(c->addr, G.peer, 6) == 0;
    default:
        return 0;
    }
}

/* Poll interrupt IN (events) and bulk IN (ACL) once each. */
/* Forget a device: the saved entry and its link key. Only the daemon thread
 * writes devices.txt and linkkeys.bin (the web page just queues the command),
 * so the two files never get two writers. */
static void forget_device(const unsigned char addr[6])
{
    char a[18];
    addr_str(addr, a);
    devices_remove(addr);
    linkkeys_delete(addr);
    out("forget: %s removed from the saved list and its link key deleted", a);
}

static void pump_once(void)
{
    unsigned char chunk[512];
    int32_t got = 0;
    int rc;

    if (G.transport_dead) return;

    notify_tick();
    quit_check();
    if (g_quit && !G.stop_requested) {
        out("daemon: newer instance started, exiting");
        G.stop_requested = 1;
        G.transport_dead = 1;     /* unwind every wait loop now */
        return;
    }
    if (!g_have_pending && ui_take_cmd(&g_pending))
        g_have_pending = 1;
    if (g_have_pending && !G.user_abort) {
        if (g_pending.type == UI_CMD_CONNECT && !connect_target_ok(g_pending.addr)) {
            char a[18];
            addr_str(g_pending.addr, a);
            out("ui: connect to %s refused, it is not headphones or a speaker", a);
            g_have_pending = 0;
        } else if (G.scan_ui && g_pending.type == UI_CMD_SCAN) {
            out("ui: scan requested while scanning, keeping the running scan");
            g_have_pending = 0;
        } else if (G.scan_ui && cmd_aborts_session(&g_pending)) {
            /* Keep the transport alive so Inquiry_Cancel gets its Command
             * Complete; ui_scan() returns early and the command runs next. */
            out("ui: command %d during the scan, stopping the scan first", g_pending.type);
            G.user_abort = 1;
            G.scan_abort = 1;
        } else if (g_pending.type == UI_CMD_FORGET && !cmd_aborts_session(&g_pending)) {
            /* A device other than the current one: no reason to end the session. */
            forget_device(g_pending.addr);
        } else if (cmd_aborts_session(&g_pending)) {
            out("ui: command %d ends this session", g_pending.type);
            if (g_pending.type == UI_CMD_DISCONNECT) set_state("disconnecting", NULL, 0);
            G.user_abort = 1;
            G.transport_dead = 1; /* unwind; cleared again before teardown */
            return;
        }
        g_have_pending = 0;       /* nothing to do (e.g. connect to current) */
    }

    if (EVT.run) {
        static int last_err;
        uint32_t wr = __atomic_load_n(&EVT.wr, __ATOMIC_ACQUIRE);
        uint32_t rd = EVT.rd;
        while (rd != wr) {
            int n = 0;
            while (rd != wr && n < (int)sizeof chunk)
                chunk[n++] = EVT.q[(rd++) & (EVQ_SIZE - 1)];
            __atomic_store_n(&EVT.rd, rd, __ATOMIC_RELEASE);
            evt_feed(chunk, n);
        }
        if (EVT.err != last_err) {
            last_err = EVT.err;
            if (last_err) out("event read error %d (%s)", last_err, usb_err(last_err));
        }
        if (EVT.dead) G.transport_dead = 1;
        rc = 1;
    } else {
        got = 0;
        rc = U.InterruptTransfer(H, EP.ev, chunk, sizeof chunk, &got, (uint32_t)G.poll_ms);
    }
    if (rc == 0 && got > 0) {
        evt_feed(chunk, (int)got);
    } else if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT && rc != SCE_USBD_ERROR_TIMEOUT) {
        /* Real errors only; PS4 sceUsbd returns 0x80240007 (= no data). */
        out("event read error %d (%s)", rc, usb_err(rc));
        if (rc == -4) G.transport_dead = 1;
    }

    if (G.hold_replay) acl_replay_held();

    got = 0;
    rc = U.BulkTransfer(H, EP.in, chunk, sizeof chunk, &got, (uint32_t)G.poll_ms);
    if (rc == 0 && got > 0) {
        on_acl(chunk, (int)got);
    } else if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT && rc != SCE_USBD_ERROR_TIMEOUT
               && rc != LIBUSB_ERROR_OVERFLOW) {
        out("acl read error %d (%s)", rc, usb_err(rc));
        if (rc == -4) G.transport_dead = 1;
    }
}

static void pump_until(long deadline_ms)
{
    while (now_ms() < deadline_ms && !G.transport_dead)
        pump_once();
}

/* Wait until *flag is set or the deadline. */
static int wait_flag(volatile int *flag, long timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    while (!*flag && now_ms() < deadline && !G.transport_dead)
        pump_once();
    return *flag;
}

/* Send an HCI command and wait for Command Complete (0x0E).
 * Returns 1 if status byte is 0. Copies return parameters to ret. */
static int hci_cmd_cc(unsigned op, const void *params, int plen,
                      unsigned char *ret, int retcap, int *retlen)
{
    long deadline;
    G.wait_op = op;
    G.cc_done = 0;
    G.cs_done = 0;
    G.cs_status = 0;
    G.cc_len = 0;
    if (!hci_send(op, params, plen)) return 0;
    deadline = now_ms() + CMD_TIMEOUT_MS;
    while (now_ms() < deadline && !G.transport_dead) {
        pump_once();
        if (G.cs_done && G.cs_status != 0) {
            out("hci cmd 0x%04x rejected, status 0x%02x (%s)",
                op, G.cs_status, hci_err(G.cs_status));
            G.wait_op = 0;
            return 0;
        }
        if (G.cc_done) break;
    }
    G.wait_op = 0;
    if (!G.cc_done) {
        out("hci cmd 0x%04x timeout, no Command Complete", op);
        return 0;
    }
    if (ret && retcap > 0 && G.cc_len > 0) {
        int n = G.cc_len > retcap ? retcap : G.cc_len;
        memcpy(ret, G.cc, (size_t)n);
        if (retlen) *retlen = n;
    } else if (retlen) {
        *retlen = G.cc_len;
    }
    return G.cc_len > 0 && G.cc[0] == 0;
}

/* Send a command that answers with Command Status (0x0F), then an async event.
 * timeout_ms <= 0 uses CMD_TIMEOUT_MS. soft_timeout: missing CS is not an error
 * (Inquiry_Cancel after inquiry already finished). */
static int hci_cmd_status_ex(unsigned op, const void *params, int plen,
                             long timeout_ms, int soft_timeout)
{
    long deadline;
    G.wait_op = op;
    G.cs_done = 0;
    G.cs_status = 0xFF;
    G.cc_done = 0;
    if (!hci_send(op, params, plen)) return 0;
    if (timeout_ms <= 0) timeout_ms = CMD_TIMEOUT_MS;
    deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline && !G.transport_dead) {
        pump_once();
        if (G.cs_done) break;
        /* Some controllers reply with Command Complete for cancel-like ops. */
        if (G.cc_done) {
            G.wait_op = 0;
            return G.cc_len > 0 && G.cc[0] == 0;
        }
        /* Remote_Name_Request Complete can race ahead of Command Status. */
        if (op == OP_REMOTE_NAME_REQ && G.name_done) {
            G.cs_done = 1;
            G.cs_status = 0;
            break;
        }
    }
    G.wait_op = 0;
    if (!G.cs_done) {
        if (soft_timeout) {
            out("hci cmd 0x%04x: no Command Status (ignored)", op);
            return 1;
        }
        out("hci cmd 0x%04x timeout, no Command Status", op);
        return 0;
    }
    if (G.cs_status != 0) {
        out("hci cmd 0x%04x status 0x%02x (%s)", op, G.cs_status, hci_err(G.cs_status));
        return 0;
    }
    return 1;
}

static int hci_cmd_status(unsigned op, const void *params, int plen)
{
    return hci_cmd_status_ex(op, params, plen, CMD_TIMEOUT_MS, 0);
}

/* ---- L2CAP helpers ----------------------------------------------------- */

static unsigned char next_sig_id(void)
{
    if (++G.sig_id == 0) G.sig_id = 1;
    return G.sig_id;
}

/* Wait up to wait_ms for `need` free ACL buffers. 1 if available. */
static int acl_credits_for(int need, long wait_ms)
{
    long deadline;
    if (!G.acl_pkts) return 1;
    if (need > (int)G.acl_pkts) need = (int)G.acl_pkts;
    if ((int)G.acl_pkts - G.acl_inflight >= need) return 1;
    G.acl_stalls++;
    deadline = now_ms() + wait_ms;
    while ((int)G.acl_pkts - G.acl_inflight < need && now_ms() < deadline &&
           !G.transport_dead && G.connected)
        pump_once();
    if ((int)G.acl_pkts - G.acl_inflight >= need) return 1;
    if (G.acl_inflight > 0 && now_ms() - G.acl_last_credit_ms >= CREDIT_LEAK_MS) {
        out("acl: no completions for %ld ms (%d in flight), resyncing credits",
            now_ms() - G.acl_last_credit_ms, G.acl_inflight);
        G.acl_inflight = 0;
        G.acl_leak_resync++;
        G.acl_last_credit_ms = now_ms();
        return 1;
    }
    return 0;
}

/* Send one L2CAP frame to the peer's channel `cid` (the remote CID, or
 * CID_SIGNALING). Fragments into ACL packets of at most acl_mtu bytes. */
static int l2_send(unsigned cid, const unsigned char *d, int len)
{
    unsigned char frame[2048];
    unsigned char pkt[1100];
    unsigned mtu = G.acl_mtu ? G.acl_mtu : 1021;
    int total = 4 + len, off = 0;
    if (!G.connected) return 0;
    if (len < 0 || total > (int)sizeof frame) {
        out("l2cap: frame %d too large", len);
        return 0;
    }
    if (mtu > sizeof pkt - 4) mtu = sizeof pkt - 4;
    /* Media: all fragments must fit in free controller buffers right now
     * (short wait), else drop the whole frame instead of stalling. */
    if (G.media_tx && !acl_credits_for((total + (int)mtu - 1) / (int)mtu, CREDIT_WAIT_MEDIA_MS)) {
        G.acl_media_drops++;
        return 2;
    }
    put16(frame, (unsigned)len);
    put16(frame + 2, cid);
    if (len > 0) memcpy(frame + 4, d, (size_t)len);
    while (off < total) {
        int chunk = total - off;
        if (chunk > (int)mtu) chunk = (int)mtu;
        /* PB=0b10 first (flushable), 0b01 continuation; BC=00 */
        put16(pkt, (off ? 0x1000u : 0x2000u) | (G.handle & 0x0FFFu));
        put16(pkt + 2, (unsigned)chunk);
        memcpy(pkt + 4, frame + off, (size_t)chunk);
        if (!acl_send_raw(pkt, chunk + 4)) return 0;
        off += chunk;
    }
    return 1;
}

static int sig_send(unsigned char code, unsigned char id,
                    const unsigned char *d, int len)
{
    unsigned char cmd[64];
    if (len < 0 || len + 4 > (int)sizeof cmd) return 0;
    cmd[0] = code;
    cmd[1] = id;
    put16(cmd + 2, (unsigned)len);
    if (len > 0) memcpy(cmd + 4, d, (size_t)len);
    hexline("l2cap sig tx", cmd, len + 4);
    return l2_send(CID_SIGNALING, cmd, len + 4);
}

static const char *role_name(int role)
{
    switch (role) {
    case ROLE_SDP:   return "SDP";
    case ROLE_AVSIG: return "AVDTP-sig";
    case ROLE_MEDIA: return "AVDTP-media";
    case ROLE_AVCTP: return "AVRCP";
    case ROLE_RFCOMM:return "RFCOMM-HFP";
    default:         return "?";
    }
}

static int chan_live(const struct chan *c)
{
    return c->st == CH_CONNECTING || c->st == CH_CONFIG || c->st == CH_OPEN;
}

/* Look up by our local CID. */
static struct chan *chan_by_scid(unsigned scid)
{
    int i;
    for (i = 0; i < MAX_CH; i++)
        if (chan_live(&G.ch[i]) && G.ch[i].scid == scid) return &G.ch[i];
    return NULL;
}

static struct chan *chan_find_role(int role, int open_only)
{
    int i;
    for (i = 0; i < MAX_CH; i++) {
        struct chan *c = &G.ch[i];
        if (c->role != role) continue;
        if (open_only ? c->st == CH_OPEN : chan_live(c)) return c;
    }
    return NULL;
}

/* New channel with a unique local CID (0x0040, 0x0041, ...). */
static struct chan *chan_alloc(unsigned psm, int role, int inbound)
{
    int i;
    for (i = 0; i < MAX_CH; i++) {
        struct chan *c = &G.ch[i];
        if (c->st != CH_FREE) continue;
        memset(c, 0, sizeof *c);
        if (G.next_cid < 0x0040 || G.next_cid > 0xFFFF) G.next_cid = 0x0040;
        c->scid = G.next_cid++;
        c->psm = psm;
        c->role = role;
        c->inbound = inbound;
        c->peer_mtu = L2_MTU;   /* default until the peer configures */
        return c;
    }
    out("l2cap: no free channel slot");
    return NULL;
}

static int chan_send(struct chan *c, const unsigned char *d, int len)
{
    if (!c || (c->st != CH_OPEN && c->st != CH_CONFIG)) return 0;
    if (c->peer_mtu && (unsigned)len > c->peer_mtu)
        out("l2cap: %d bytes exceed peer MTU %u on cid 0x%04x", len, c->peer_mtu, c->scid);
    return l2_send(c->dcid, d, len);
}

static void chan_send_cfg(struct chan *c)
{
    unsigned char r[8];
    put16(r, c->dcid);
    put16(r + 2, 0);
    r[4] = 0x01; r[5] = 0x02;
    put16(r + 6, L2_MTU);       /* what we can receive */
    c->cfg_id = next_sig_id();
    sig_send(L2_CFG_REQ, c->cfg_id, r, 8);
}

static void chan_send_cfg_rsp(struct chan *c, unsigned char id,
                              const unsigned char *opt, int optlen)
{
    unsigned char r[10];
    unsigned mtu = 0;
    int n = 6;
    int i;
    put16(r, c->dcid);
    put16(r + 2, 0); /* flags */
    put16(r + 4, 0); /* result = success */
    for (i = 0; i + 2 <= optlen; ) {
        unsigned char t = opt[i], l = opt[i + 1];
        if (i + 2 + l > optlen) break;
        if ((t & 0x7F) == 0x01 && l == 2) mtu = le16(opt + i + 2);
        i += 2 + l;
    }
    if (mtu) {
        c->peer_mtu = mtu;
        r[6] = 0x01; r[7] = 0x02;
        put16(r + 8, mtu);
        n = 10;
    }
    sig_send(L2_CFG_RSP, id, r, n);
}

static void chan_check_open(struct chan *c)
{
    if (c->st == CH_CONFIG && c->ours_ok && c->theirs_ok) {
        c->st = CH_OPEN;
        out("l2cap: %s OPEN local 0x%04x remote 0x%04x psm 0x%04x mtu %u%s",
            role_name(c->role), c->scid, c->dcid, c->psm, c->peer_mtu,
            c->inbound ? " (peer opened)" : "");
        if (c->role == ROLE_AVCTP) {
            /* As controller: ask the headphones for their volume + changes. */
            unsigned char r[32];
            int n;
            AVR.now_ms = now_ms();
            n = avrcp_build_register_volume(&AVR, r, (int)sizeof r);
            if (n > 0) l2_send(c->dcid, r, n);
            out("avrcp: control channel up, registered for VOLUME_CHANGED");
        }
    }
}

static void chan_disconnect(struct chan *c);

/* Outbound L2CAP connection; waits until open / refused / timeout. */
static struct chan *l2_connect_t(unsigned psm, int role, long wait_ms);
static struct chan *l2_connect(unsigned psm, int role)
{
    return l2_connect_t(psm, role, L2_CONN_WAIT_MS);
}

/* Our own AVRCP open attempts during the stream */
#define AVR_MAX_TRIES 5
static struct chan *avr_c;
static long avr_next, avr_t;
static int avr_tries;

/* Send CONN_REQ and return at once; on_l2cap finishes the setup while the
 * stream runs (CH_OPEN) or marks it CH_CLOSED when refused. */
static struct chan *l2_connect_async(unsigned psm, int role)
{
    unsigned char r[4];
    struct chan *c = chan_alloc(psm, role, 0);
    if (!c) return NULL;
    c->st = CH_CONNECTING;
    put16(r, psm);
    put16(r + 2, c->scid);
    sig_send(L2_CONN_REQ, next_sig_id(), r, 4);
    out("l2cap: CONN_REQ psm 0x%04x local 0x%04x (%s, async)", psm, c->scid, role_name(role));
    return c;
}

static struct chan *l2_connect_t(unsigned psm, int role, long wait_ms)
{
    unsigned char r[4];
    struct chan *c = chan_alloc(psm, role, 0);
    long deadline;
    if (!c) return NULL;
    c->st = CH_CONNECTING;
    put16(r, psm);
    put16(r + 2, c->scid);
    sig_send(L2_CONN_REQ, next_sig_id(), r, 4);
    out("l2cap: CONN_REQ psm 0x%04x local 0x%04x (%s)", psm, c->scid, role_name(role));

    deadline = now_ms() + wait_ms;
    while (now_ms() < deadline && !G.transport_dead && !G.disc_done) {
        pump_once();
        if (c->st == CH_OPEN || c->st == CH_CLOSED) break;
        if (c->pending && c->st == CH_CONNECTING) {
            deadline = now_ms() + L2_PEND_WAIT_MS; /* one extension */
            c->pending = 0;
        }
        /* Peer opened the same kind of channel to us meanwhile: use it. */
        if (role == ROLE_AVCTP) {
            struct chan *o = chan_find_role(ROLE_AVCTP, 1);
            if (o && o != c) {
                out("l2cap: peer-opened AVRCP channel wins");
                chan_disconnect(c);
                return o;
            }
        }
        if (role == ROLE_AVSIG) {
            struct chan *o = chan_find_role(ROLE_AVSIG, 1);
            if (o && o != c) {
                out("l2cap: peer-opened AVDTP signaling channel wins");
                chan_disconnect(c);
                return o;
            }
        }
    }
    if (c->st == CH_OPEN) return c;
    if (c->st == CH_CLOSED)
        out("l2cap: psm 0x%04x refused (%s)", psm, l2_result(c->result));
    else
        out("l2cap: psm 0x%04x not open (state %d ours=%d theirs=%d)",
            psm, (int)c->st, c->ours_ok, c->theirs_ok);
    c->st = CH_FREE;
    return NULL;
}

static void chan_disconnect(struct chan *c)
{
    unsigned char p[4];
    if (!c || !chan_live(c)) return;
    if (G.connected && c->st != CH_CONNECTING) {
        put16(p, c->dcid);
        put16(p + 2, c->scid);
        G.disc_rsp_got = 0;
        sig_send(L2_DISC_REQ, next_sig_id(), p, 4);
        wait_flag(&G.disc_rsp_got, 2000);
    }
    out("l2cap: %s local 0x%04x closed", role_name(c->role), c->scid);
    c->st = CH_FREE;
}

/* ---- event / ACL handlers ---------------------------------------------- */

static int same_addr(const unsigned char *a, const unsigned char *b)
{
    return memcmp(a, b, 6) == 0;
}

static struct dev *dev_find(const unsigned char addr[6])
{
    int i;
    for (i = 0; i < G.ndev; i++)
        if (same_addr(G.dev[i].addr, addr)) return &G.dev[i];
    return NULL;
}

static void ui_scan_publish(void);

/* Is this address a device we may list, pair or connect? cod is the class
 * seen now (0 = unknown). Saved devices keep working when a later event
 * carries no class (see cod.h). */
static int addr_allowed(const unsigned char addr[6], uint32_t cod)
{
    uint32_t saved_cod = 0;
    int saved;
    if (cod && cod_is_audio_sink(cod)) return 1;    /* common case, no file read */
    saved = devices_find(addr, &saved_cod);
    return cod_device_allowed(cod, saved, saved_cod);
}

/* A device that calls us on its own must be saved and an audio sink. */
static int incoming_ok(const unsigned char addr[6], uint32_t cod)
{
    uint32_t saved_cod = 0;
    if (!devices_find(addr, &saved_cod)) return 0;
    return cod_device_allowed(cod, 1, saved_cod);
}

/* Addresses already logged as "not an audio device" in this scan. */
static unsigned char g_ign[24][6];
static int g_ign_n;

static void note_ignored(const unsigned char addr[6], uint32_t cod)
{
    char astr[18];
    int i;
    for (i = 0; i < g_ign_n; i++)
        if (memcmp(g_ign[i], addr, 6) == 0) return;
    if (g_ign_n < 24) memcpy(g_ign[g_ign_n++], addr, 6);
    addr_str(addr, astr);
    out("filter: %s cod 0x%06x (major %d minor %d) is not headphones or a speaker, hidden",
        astr, cod, COD_MAJOR(cod), COD_MINOR(cod));
}

static struct dev *dev_add(const unsigned char addr[6], uint32_t cod,
                           uint8_t psrm, uint16_t clk, int rssi)
{
    struct dev *d = dev_find(addr);
    if (!d && !addr_allowed(addr, cod)) {
        note_ignored(addr, cod);
        return NULL;
    }
    if (!d) {
        if (G.ndev >= MAX_DEV) return NULL;
        d = &G.dev[G.ndev++];
        memset(d, 0, sizeof *d);
        memcpy(d->addr, addr, 6);
        d->rssi = 127;
        d->name_status = -1;
    }
    d->cod = cod;
    if (psrm) d->psrm = psrm;
    d->clk = clk;
    if (rssi != 127) {
        d->rssi = rssi;
        d->rssi_sum += rssi;
        d->rssi_n++;
    }
    if (G.scan_ui) ui_scan_publish();     /* the page lists it right away */
    return d;
}

static void prepare_peer(const unsigned char addr[6]);
static int addr_allowed(const unsigned char addr[6], uint32_t cod);
static int incoming_ok(const unsigned char addr[6], uint32_t cod);

static void handle_auth_events(const unsigned char *ev, int n)
{
    char astr[18];

    /* PIN Code Request (0x16) */
    if (ev[0] == 0x16 && n >= 8 && same_addr(ev + 2, G.peer)) {
        unsigned char pin[23];
        memset(pin, 0, sizeof pin);
        memcpy(pin, ev + 2, 6);
        pin[6] = 4;
        pin[7] = pin[8] = pin[9] = pin[10] = '0';
        hci_send(OP_PIN_CODE_REPLY, pin, (int)sizeof pin);
        out("auth: PIN Code Reply 0000");
        return;
    }

    /* Link Key Request (0x17) */
    if (ev[0] == 0x17 && n >= 8 && same_addr(ev + 2, G.peer)) {
        if (!G.have_key) {
            hci_send(OP_LINK_KEY_NEG_REPLY, ev + 2, 6);
            out("auth: Link Key Neg Reply (no stored key)");
        } else {
            unsigned char kr[22];
            memcpy(kr, ev + 2, 6);
            memcpy(kr + 6, G.link_key, 16);
            hci_send(OP_LINK_KEY_REPLY, kr, 22);
            out("auth: Link Key Reply with saved key (type %u)", (unsigned)G.key_type);
        }
        return;
    }

    /* Link Key Notification (0x18): persist right away (fsync inside). */
    if (ev[0] == 0x18 && n >= 25 && same_addr(ev + 2, G.peer)) {
        memcpy(G.link_key, ev + 8, 16);
        G.key_type = ev[24];
        G.have_key = 1;
        g_ignore_saved_key = 0;
        addr_str(ev + 2, astr);
        if (linkkeys_store(G.peer, G.link_key, G.key_type) == 0)
            out("auth: link key for %s (type %u) saved to %s (fsync ok)",
                astr, G.key_type, linkkeys_saved_path());
        else
            out("auth: link key for %s (type %u) could NOT be saved to %s",
                astr, G.key_type, LINKKEYS_PATH);
        return;
    }

    /* Authentication Complete (0x06) */
    if (ev[0] == 0x06 && n >= 5) {
        unsigned h = le16(ev + 3) & 0x0FFF;
        if (h == (G.handle & 0x0FFF) || !G.handle) {
            G.auth_status = ev[2];
            G.auth_done = 1;
            out("auth: Authentication Complete status 0x%02x (%s)",
                ev[2], hci_err(ev[2]));
        }
        return;
    }

    /* Encryption Change (0x08) */
    if (ev[0] == 0x08 && n >= 6) {
        unsigned h = le16(ev + 3) & 0x0FFF;
        if (h == (G.handle & 0x0FFF)) {
            G.enc_status = ev[2];
            G.enc_on = (ev[2] == 0 && ev[5] != 0);
            G.enc_done = 1;
            out("auth: Encryption Change status 0x%02x enabled=%d", ev[2], G.enc_on);
        }
        return;
    }

    /* IO Capability Request (0x31) - reply NoInputNoOutput, no OOB, General Bonding */
    if (ev[0] == 0x31 && n >= 8 && same_addr(ev + 2, G.peer)) {
        unsigned char io[9];
        memcpy(io, ev + 2, 6);
        io[6] = 0x03; /* NoInputNoOutput */
        io[7] = 0x00; /* OOB not present */
        io[8] = 0x04; /* General Bonding, no MITM -> Just Works */
        hci_send(OP_IO_CAP_REPLY, io, 9);
        addr_str(ev + 2, astr);
        out("auth: IO Cap Reply NoInputNoOutput for %s", astr);
        return;
    }

    /* User Confirmation Request (0x33) - auto-accept for Just Works */
    if (ev[0] == 0x33 && n >= 8 && same_addr(ev + 2, G.peer)) {
        hci_send(OP_USER_CONFIRM_REPLY, ev + 2, 6);
        out("auth: User Confirmation Reply");
        return;
    }

    /* User Passkey Request (0x34) - we have no input, reject */
    if (ev[0] == 0x34 && n >= 8 && same_addr(ev + 2, G.peer)) {
        hci_send(OP_PASSKEY_NEG_REPLY, ev + 2, 6);
        out("auth: Passkey Neg Reply (no input)");
        return;
    }

    /* Simple Pairing Complete (0x36) */
    if (ev[0] == 0x36 && n >= 8 && same_addr(ev + 3, G.peer)) {
        G.ssp_status = ev[2];
        G.ssp_done = 1;
        out("auth: Simple Pairing Complete status 0x%02x (%s)",
            ev[2], hci_err(ev[2]));
        return;
    }
}

static void on_event(const unsigned char *ev, int n)
{
    unsigned op;
    char astr[18];

    if (n < 2) return;
    if (g_cap_on) cap_push(ev, n);
    if (ev[0] != 0x13) hexline("hci evt", ev, n);

    /* Command Complete (0x0E) */
    if (ev[0] == 0x0E && n >= 5) {
        op = le16(ev + 3);
        if (G.wait_op && op == G.wait_op) {
            int nret = n - 5;
            if (nret < 0) nret = 0;
            if (nret > (int)sizeof G.cc) nret = (int)sizeof G.cc;
            memcpy(G.cc, ev + 5, (size_t)nret);
            G.cc_len = nret;
            G.cc_done = 1;
        } else {
            out("ignored Command Complete for opcode 0x%04x", op);
        }
        return;
    }

    /* Command Status (0x0F) */
    if (ev[0] == 0x0F && n >= 6) {
        op = le16(ev + 4);
        if (G.wait_op && op == G.wait_op) {
            G.cs_status = ev[2];
            G.cs_done = 1;
            out("Command Status opcode 0x%04x status 0x%02x", op, ev[2]);
        } else if (ev[2] != 0) {
            out("Command Status opcode 0x%04x status 0x%02x (%s) (not waiting)",
                op, ev[2], hci_err(ev[2]));
        } else {
            out("Command Status opcode 0x%04x ok (not waiting for it)", op);
        }
        return;
    }

    /* Inquiry Result (0x02) */
    if (ev[0] == 0x02 && n >= 3) {
        int i, num = ev[2];
        const unsigned char *p = ev + 3;
        for (i = 0; i < num && p + 14 <= ev + n; i++, p += 14) {
            uint32_t cod = (uint32_t)p[9] | ((uint32_t)p[10] << 8) | ((uint32_t)p[11] << 16);
            uint16_t clk = (uint16_t)le16(p + 12);
            struct dev *d = dev_add(p, cod, p[6], clk, 127);
            if (d) {
                addr_str(p, astr);
                out("inquiry: %s cod 0x%06x psrm %u clk 0x%04x",
                    astr, cod, p[6], clk);
            }
        }
        return;
    }

    /* Inquiry Result with RSSI (0x22): addr(6) psrm(1) reserved(1) cod(3) clk(2) rssi(1) = 14 */
    if (ev[0] == 0x22 && n >= 3) {
        int i, num = ev[2];
        const unsigned char *p = ev + 3;
        for (i = 0; i < num && p + 14 <= ev + n; i++, p += 14) {
            uint32_t cod = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16);
            uint16_t clk = (uint16_t)le16(p + 11);
            int rssi = (int8_t)p[13];
            struct dev *d = dev_add(p, cod, p[6], clk, rssi);
            if (d) {
                addr_str(p, astr);
                out("inquiry: %s cod 0x%06x rssi %d dBm psrm %u clk 0x%04x",
                    astr, cod, rssi, p[6], clk);
            }
        }
        return;
    }

    /* Inquiry Complete (0x01) */
    if (ev[0] == 0x01 && n >= 3) {
        G.inq_status = ev[2];
        G.inq_done = 1;
        G.inq_active = 0;
        out("inquiry complete status 0x%02x, %d device(s)", ev[2], G.ndev);
        return;
    }

    /* Remote Name Request Complete (0x07) */
    if (ev[0] == 0x07 && n >= 9) {
        struct dev *d = dev_find(ev + 3);
        int k, m = n - 9;
        G.name_done = 1;
        if (G.wait_op == OP_REMOTE_NAME_REQ && !G.cs_done) {
            G.cs_status = 0;
            G.cs_done = 1;
        }
        if (!d) {
            addr_str(ev + 3, astr);
            out("remote name for unknown %s status 0x%02x", astr, ev[2]);
            return;
        }
        d->name_status = ev[2];
        d->name[0] = 0;
        if (ev[2] == 0) {
            if (m > (int)sizeof d->name - 1) m = (int)sizeof d->name - 1;
            for (k = 0; k < m && ev[9 + k]; k++) d->name[k] = (char)ev[9 + k];
            d->name[k] = 0;
        }
        addr_str(d->addr, astr);
        out("remote name %s: status 0x%02x \"%s\"", astr, ev[2], d->name);
        return;
    }

    /* Connection Request (0x04): addr(6) class(3) link_type(1). Only a saved
     * headphone / speaker is accepted, and only while the reconnect loop
     * listens (or when it is the very device we are paging). */
    if (hci_conn_request_ok(ev, n)) {
        uint32_t cod = (uint32_t)ev[8] | ((uint32_t)ev[9] << 8) | ((uint32_t)ev[10] << 16);
        int mine = G.conn_pending && same_addr(ev + 2, G.peer);
        unsigned char rp[7];
        const char *why = NULL;
        addr_str(ev + 2, astr);
        /* SCO/eSCO (call audio): refuse. HFP is battery-only. Match PS5:
         * Reject Synchronous Connection Request when we already have ACL. */
        if (ev[11] != 0x01) {
            memcpy(rp, ev + 2, 6);
            rp[6] = 0x0D;       /* limited resources */
            if (G.connected && same_addr(ev + 2, G.peer)) {
                hci_send(OP_REJECT_SYNC_CONN, rp, 7);
                out("hfp: headset asked for call audio, refused");
            } else {
                hci_send(OP_REJECT_CONN, rp, 7);
                out("incoming: %s cod 0x%06x rejected (not an ACL link)", astr, cod);
            }
            return;
        }
        if (G.connected) why = "already connected";
        else if (!mine && !G.accept_in) why = "not listening";
        else if (!mine && G.conn_pending) why = "busy paging another device";
        else if (mine ? !addr_allowed(ev + 2, cod) : !incoming_ok(ev + 2, cod))
            why = "not a saved headphone or speaker";
        memcpy(rp, ev + 2, 6);
        if (why) {
            rp[6] = 0x0F;       /* unacceptable BD_ADDR */
            hci_send(OP_REJECT_CONN, rp, 7);
            out("incoming: %s cod 0x%06x rejected (%s)", astr, cod, why);
            return;
        }
        if (!mine) {
            prepare_peer(ev + 2);
            G.conn_pending = 1;
        }
        G.inbound = 1;
        G.in_cod = cod;
        rp[6] = 0x00;           /* become master */
        hci_send(OP_ACCEPT_CONN, rp, 7);
        out("incoming: %s cod 0x%06x accepted", astr, cod);
        return;
    }

    /* Connection Complete (0x03) */
    if (ev[0] == 0x03 && n >= 13) {
        unsigned h = le16(ev + 3) & 0x0FFF;
        addr_str(ev + 5, astr);
        if (!G.conn_pending || !same_addr(ev + 5, G.peer)) {
            out("Connection Complete for %s handle 0x%03x status 0x%02x is not ours, ignoring",
                astr, h, ev[2]);
            if (ev[2] == 0 && !(G.connected && h == (G.handle & 0x0FFF))) {
                unsigned char d[3];
                put16(d, h);
                d[2] = 0x13;
                hci_send(OP_DISCONNECT, d, 3);
                out("disconnecting unexpected handle 0x%03x", h);
            }
            return;
        }
        G.conn_pending = 0;
        G.conn_status = ev[2];
        G.conn_done = 1;
        if (ev[2] == 0) {
            G.handle = h;
            G.connected = 1;
            out("ACL connected: handle 0x%03x addr %s link_type %u enc %u",
                G.handle, astr, ev[11], ev[12]);
            if (G.nhold) G.hold_replay = 1;
        } else {
            out("ACL connect failed: status 0x%02x (%s) addr %s",
                ev[2], hci_err(ev[2]), astr);
            G.nhold = 0;
        }
        return;
    }

    /* Disconnection Complete (0x05) */
    if (ev[0] == 0x05 && n >= 6) {
        unsigned h = le16(ev + 3) & 0x0FFF;
        if (G.connected && h == (G.handle & 0x0FFF)) {
            G.disc_reason = ev[5];
            G.disc_done = 1;
            if (ev[5] != 0x16) g_end_reason = ev[5];     /* 0x16 is our own hang-up */
            G.connected = 0;
            out("ACL disconnected: status 0x%02x reason 0x%02x (%s)",
                ev[2], ev[5], hci_err(ev[5]));
        }
        return;
    }

    /* Number Of Completed Packets (0x13): return ACL buffer credits. */
    if (ev[0] == 0x13 && n >= 3) {
        int i, num = ev[2];
        for (i = 0; i < num && 3 + i * 4 + 4 <= n; i++) {
            unsigned h = le16(ev + 3 + i * 4) & 0x0FFF;
            int done = (int)le16(ev + 5 + i * 4);
            if (h == (G.handle & 0x0FFF)) {
                if (done > 0) G.acl_last_credit_ms = now_ms();
                G.acl_inflight -= done;
                if (G.acl_inflight < 0) G.acl_inflight = 0;
            }
        }
        return;
    }

    handle_auth_events(ev, n);
}

static void on_signaling(const unsigned char *d, int len)
{
    const unsigned char *end = d + len;

    while (end - d >= 4) {
        unsigned char op = d[0], id = d[1];
        int blen = (int)le16(d + 2);
        const unsigned char *pl = d + 4;
        unsigned char outb[16];
        struct chan *c;

        if (blen > end - pl) break;
        hexline("l2cap sig rx", d, 4 + blen);

        switch (op) {
        case L2_CONN_REQ:
            if (blen >= 4) {
                unsigned psm = le16(pl);
                unsigned their_cid = le16(pl + 2);
                int role = 0;
                if (psm == PSM_SDP) {
                    role = ROLE_SDP;
                } else if (psm == PSM_AVCTP) {
                    role = chan_find_role(ROLE_AVCTP, 0) ? 0 : ROLE_AVCTP;
                } else if (psm == PSM_AVDTP) {
                    /* Second AVDTP channel after OPEN = media transport. */
                    role = (AV.opened && chan_find_role(ROLE_AVSIG, 1) &&
                            !chan_find_role(ROLE_MEDIA, 0)) ? ROLE_MEDIA : ROLE_AVSIG;
                } else if (psm == PSM_RFCOMM) {
                    role = chan_find_role(ROLE_RFCOMM, 0) ? 0 : ROLE_RFCOMM;
                }
                c = role ? chan_alloc(psm, role, 1) : NULL;
                if (c) {
                    c->dcid = their_cid;
                    c->st = CH_CONFIG;
                    put16(outb, c->scid);
                    put16(outb + 2, their_cid);
                    put16(outb + 4, 0);
                    put16(outb + 6, 0);
                    sig_send(L2_CONN_RSP, id, outb, 8);
                    out("l2cap: accepted inbound psm 0x%04x as %s, local 0x%04x remote 0x%04x",
                        psm, role_name(role), c->scid, c->dcid);
                    chan_send_cfg(c);
                } else {
                    put16(outb, 0);
                    put16(outb + 2, their_cid);
                    put16(outb + 4, role ? 0x0004 : 0x0002);
                    put16(outb + 6, 0);
                    sig_send(L2_CONN_RSP, id, outb, 8);
                    out("l2cap: rejected inbound psm 0x%04x", psm);
                }
            }
            break;

        case L2_CONN_RSP:
            if (blen >= 8) {
                unsigned dcid = le16(pl);
                unsigned scid = le16(pl + 2);
                unsigned result = le16(pl + 4);
                c = chan_by_scid(scid);
                if (c && c->st == CH_CONNECTING) {
                    c->result = result;
                    out("l2cap: CONN_RSP local 0x%04x remote 0x%04x result %u (%s)",
                        scid, dcid, result, l2_result(result));
                    if (result == 1) {
                        c->pending = 1;
                        if (dcid) c->dcid = dcid;
                    } else if (result != 0) {
                        c->st = CH_CLOSED;
                    } else {
                        c->dcid = dcid;
                        c->st = CH_CONFIG;
                        chan_send_cfg(c);
                    }
                }
            }
            break;

        case L2_CFG_REQ:
            if (blen >= 4) {
                unsigned dcid = le16(pl);
                c = chan_by_scid(dcid);
                if (c) {
                    c->theirs_ok = 1;
                    chan_send_cfg_rsp(c, id, pl + 4, blen - 4);
                    chan_check_open(c);
                } else {
                    out("l2cap: CFG_REQ for unknown local cid 0x%04x", dcid);
                }
            }
            break;

        case L2_CFG_RSP:
            if (blen >= 6) {
                unsigned scid = le16(pl);   /* = our local CID */
                unsigned result = le16(pl + 4);
                c = chan_by_scid(scid);
                if (c) {
                    if (result == 0) {
                        c->ours_ok = 1;
                        chan_check_open(c);
                    } else {
                        out("l2cap: CFG_RSP local 0x%04x result %u", scid, result);
                    }
                }
            }
            break;

        case L2_DISC_REQ:
            if (blen >= 4) {
                unsigned dcid = le16(pl), scid = le16(pl + 2);  /* dcid = ours */
                put16(outb, dcid);
                put16(outb + 2, scid);
                sig_send(L2_DISC_RSP, id, outb, 4);
                c = chan_by_scid(dcid);
                out("l2cap: peer closed %s local 0x%04x", c ? role_name(c->role) : "?", dcid);
                if (c) c->st = CH_FREE;
            }
            break;

        case L2_DISC_RSP:
            G.disc_rsp_got = 1;
            if (blen >= 4) {
                c = chan_by_scid(le16(pl + 2));
                if (c) c->st = CH_FREE;
            }
            break;

        case L2_ECHO_REQ:
            sig_send(L2_ECHO_RSP, id, pl, blen);
            break;

        case L2_INFO_REQ:
            if (blen >= 2) {
                unsigned type = le16(pl);
                if (type == 1) { /* connectionless MTU */
                    put16(outb, type);
                    put16(outb + 2, 0);
                    put16(outb + 4, 0x0040);
                    sig_send(L2_INFO_RSP, id, outb, 6);
                } else if (type == 2) { /* extended features */
                    put16(outb, type);
                    put16(outb + 2, 0);
                    outb[4] = 0x00; outb[5] = 0x00; outb[6] = 0x00; outb[7] = 0x00;
                    sig_send(L2_INFO_RSP, id, outb, 8);
                } else if (type == 3) { /* fixed channels */
                    put16(outb, type);
                    put16(outb + 2, 0);
                    memset(outb + 4, 0, 8);
                    outb[4] = 0x06; /* signaling + connectionless */
                    sig_send(L2_INFO_RSP, id, outb, 12);
                } else {
                    put16(outb, type);
                    put16(outb + 2, 1); /* not supported */
                    sig_send(L2_INFO_RSP, id, outb, 4);
                }
            }
            break;

        case L2_INFO_RSP:
            if (G.info_wait_id && id == G.info_wait_id) G.info_got = 1;
            break;

        case L2_CMD_REJ:
            out("l2cap: Command Reject id=%u", id);
            break;

        default:
            out("l2cap: unhandled signal 0x%02x id=%u len=%d", op, id, blen);
            break;
        }
        d = pl + blen;
    }
}

/* ---- SDP server (PSM 1, inbound) --------------------------------------- */

static void on_sdp(struct chan *c, const unsigned char *d, int len)
{
    unsigned char rsp[700];
    int max = c->peer_mtu ? (int)c->peer_mtu : L2_MTU, n;
    if (max > (int)sizeof rsp) max = (int)sizeof rsp;
    hexline("sdp rx", d, len);
    n = sdp_server_handle(d, len, rsp, max);
    if (n > 0) {
        hexline("sdp tx", rsp, n);
        chan_send(c, rsp, n);
    }
}

/* ---- AVDTP ------------------------------------------------------------- */

static const char *av_sig_name(unsigned sig)
{
    static const char *n[] = { "?", "DISCOVER", "GET_CAPABILITIES", "SET_CONFIGURATION",
        "GET_CONFIGURATION", "RECONFIGURE", "OPEN", "START", "CLOSE", "SUSPEND",
        "ABORT", "SECURITY_CONTROL", "GET_ALL_CAPABILITIES", "DELAYREPORT" };
    return sig < sizeof n / sizeof n[0] ? n[sig] : "?";
}

/* Our SEP's capabilities: Media Transport + Media Codec SBC. */
static int local_caps(unsigned char *o)
{
    int n = 0;
    o[n++] = 0x01; o[n++] = 0x00;
    o[n++] = 0x07; o[n++] = 0x06;
    o[n++] = 0x00; o[n++] = 0x00;           /* audio, SBC */
    o[n++] = SBC_IE0; o[n++] = SBC_IE1;
    o[n++] = SBC_MIN_BP; o[n++] = SBC_MAX_BP;
    return n;
}

/* Find the SBC codec information (4 bytes) in a capability list. */
static int find_sbc_ie(const unsigned char *caps, int len, unsigned char ie[4])
{
    int i = 0;
    while (i + 2 <= len) {
        unsigned char cat = caps[i], clen = caps[i + 1];
        if (i + 2 + clen > len) break;
        if (cat == 0x07 && clen >= 6 && (caps[i + 2] >> 4) == 0 && caps[i + 3] == 0x00) {
            memcpy(ie, caps + i + 4, 4);
            return 1;
        }
        i += 2 + clen;
    }
    return 0;
}

static void av_reply(struct chan *c, unsigned char label, unsigned char sig, int msg,
                     const unsigned char *body, int blen)
{
    unsigned char r[64];
    r[0] = AV_HDR(label, msg);
    r[1] = sig;
    if (blen > 0) memcpy(r + 2, body, (size_t)blen);
    hexline("avdtp tx", r, 2 + blen);
    chan_send(c, r, 2 + blen);
}

static void av_handle_command(struct chan *c, unsigned char label, unsigned char sig,
                              const unsigned char *b, int blen)
{
    unsigned char r[32];
    int acp = blen >= 1 ? (b[0] >> 2) & 0x3F : 0;

    out("avdtp: peer %s (label %u)", av_sig_name(sig), label);
    switch (sig) {
    case AV_DISCOVER:
        r[0] = (unsigned char)((LOCAL_SEID << 2) | (AV.configured ? 0x02 : 0));
        r[1] = 0x00;                     /* media audio, TSEP = SRC */
        av_reply(c, label, sig, AV_MSG_ACCEPT, r, 2);
        return;

    case AV_GET_CAPABILITIES:
    case AV_GET_ALL_CAPS:
        if (acp != LOCAL_SEID) {
            r[0] = AV_ERR_BAD_ACP_SEID;
            av_reply(c, label, sig, AV_MSG_REJECT, r, 1);
            return;
        }
        av_reply(c, label, sig, AV_MSG_ACCEPT, r, local_caps(r));
        return;

    case AV_SET_CONFIG: {
        unsigned char ie[4];
        if (acp != LOCAL_SEID || blen < 2) {
            r[0] = 0x00; r[1] = AV_ERR_BAD_ACP_SEID;
            av_reply(c, label, sig, AV_MSG_REJECT, r, 2);
            return;
        }
        if (AV.configured) {
            r[0] = 0x00; r[1] = AV_ERR_SEP_IN_USE;
            av_reply(c, label, sig, AV_MSG_REJECT, r, 2);
            return;
        }
        if (!find_sbc_ie(b + 2, blen - 2, ie)) {
            r[0] = 0x07; r[1] = 0x29;    /* media codec: unsupported codec */
            av_reply(c, label, sig, AV_MSG_REJECT, r, 2);
            return;
        }
        memcpy(AV.sbc_ie, ie, 4);
        AV.remote_seid = (b[1] >> 2) & 0x3F;
        AV.configured = AV.cfg_by_peer = 1;
        out("avdtp: peer configured us: remote seid %d SBC %02x %02x bitpool %u-%u",
            AV.remote_seid, ie[0], ie[1], ie[2], ie[3]);
        av_reply(c, label, sig, AV_MSG_ACCEPT, NULL, 0);
        return;
    }

    case AV_GET_CONFIG:
        if (!AV.configured) {
            r[0] = AV_ERR_BAD_STATE;
            av_reply(c, label, sig, AV_MSG_REJECT, r, 1);
            return;
        }
        r[0] = 0x01; r[1] = 0x00; r[2] = 0x07; r[3] = 0x06; r[4] = 0x00; r[5] = 0x00;
        memcpy(r + 6, AV.sbc_ie, 4);
        av_reply(c, label, sig, AV_MSG_ACCEPT, r, 10);
        return;

    case AV_OPEN:
        if (!AV.configured || AV.opened) {
            r[0] = AV_ERR_BAD_STATE;
            av_reply(c, label, sig, AV_MSG_REJECT, r, 1);
            return;
        }
        AV.opened = AV.open_by_peer = 1;
        av_reply(c, label, sig, AV_MSG_ACCEPT, NULL, 0);
        return;

    case AV_START:
        if (!AV.opened) {
            r[0] = blen >= 1 ? b[0] : 0; r[1] = AV_ERR_BAD_STATE;
            av_reply(c, label, sig, AV_MSG_REJECT, r, 2);
            return;
        }
        AV.started = AV.start_by_peer = 1;
        av_reply(c, label, sig, AV_MSG_ACCEPT, NULL, 0);
        return;

    case AV_SUSPEND:
        AV.started = 0;
        av_reply(c, label, sig, AV_MSG_ACCEPT, NULL, 0);
        return;

    case AV_CLOSE:
    case AV_ABORT:
        AV.started = AV.opened = AV.configured = 0;
        AV.closed_by_peer = 1;
        av_reply(c, label, sig, AV_MSG_ACCEPT, NULL, 0);
        return;

    case AV_DELAY_REPORT:
        if (blen >= 3) out("avdtp: peer delay report %u.%u ms",
                           (unsigned)((b[1] << 8) | b[2]) / 10, (unsigned)((b[1] << 8) | b[2]) % 10);
        av_reply(c, label, sig, AV_MSG_ACCEPT, NULL, 0);
        return;

    case AV_RECONFIGURE:
        r[0] = 0x00; r[1] = AV_ERR_NOT_SUPPORTED;
        av_reply(c, label, sig, AV_MSG_REJECT, r, 2);
        return;

    default:
        av_reply(c, label, sig, AV_MSG_GEN_REJECT, NULL, 0);
        return;
    }
}

static void on_avdtp(struct chan *c, const unsigned char *d, int len)
{
    unsigned char label, msg, sig, ptype;
    hexline("avdtp rx", d, len);
    if (len < 2) return;
    label = (unsigned char)((d[0] >> 4) & 0x0F);
    ptype = (unsigned char)((d[0] >> 2) & 0x03);
    msg = (unsigned char)(d[0] & 0x03);
    sig = (unsigned char)(d[1] & 0x3F);
    if (ptype != 0) {
        out("avdtp: fragmented packet type %u ignored", ptype);
        return;
    }
    if (msg == AV_MSG_COMMAND) {
        av_handle_command(c, label, sig, d + 2, len - 2);
        return;
    }
    /* A response: match label and signal of our outstanding command. */
    if (AV.wait_label && label == AV.wait_label && sig == AV.wait_sig) {
        int body = len - 2;
        if (body > (int)sizeof AV.rsp) body = (int)sizeof AV.rsp;
        memcpy(AV.rsp, d + 2, (size_t)body);
        AV.rsp_len = body;
        AV.rsp_msg = msg;
        AV.rsp_done = 1;
    } else {
        out("avdtp: unmatched response label %u sig 0x%02x", label, sig);
    }
}

/* HFP AG over inbound RFCOMM: headset battery percent (see hfp.c). */
static void hfp_tx(void *ud, const unsigned char *p, int n)
{
    struct chan *c = (struct chan *)ud;
    (void)ud;
    if (!c) c = chan_find_role(ROLE_RFCOMM, 1);
    if (c && n > 0) l2_send(c->dcid, p, n);
}
static void hfp_say(const char *m) { out("%s", m); }
static void on_rfcomm(struct chan *c, const unsigned char *d, int len)
{
    if (g_hfp_scid != c->scid) {
        int keep_b = HFP.battery, keep_bs = HFP.battery_seq, keep_src = HFP.battery_src;
        hfp_init(&HFP, hfp_tx, c, hfp_say);
        /* Keep last % across RFCOMM reopen (codec reconnect). New peer
         * already cleared the UI battery in ui_set_device. */
        HFP.battery = keep_b;
        HFP.battery_seq = keep_bs;
        HFP.battery_src = keep_src;
        g_hfp_scid = c->scid;
        out("hfp: RFCOMM open (battery)");
    }
    HFP.ud = c;
    hfp_input(&HFP, d, len);
    if (HFP.battery >= 0)
        ui_set_battery(-2, HFP.battery); /* -2 = HFP percent */
}

/* AVRCP control channel: absolute volume both ways (see avrcp.c). */
static void avctp_send(const unsigned char *p, int n)
{
    struct chan *c = chan_find_role(ROLE_AVCTP, 1);
    if (c && n > 0) l2_send(c->dcid, p, n);
}

static void on_avctp(struct chan *c, const unsigned char *d, int len)
{
    unsigned char r[128];
    int n;
    AVR.now_ms = now_ms();
    n = avrcp_input(&AVR, d, len, r, (int)sizeof r);
    if (n > 0) l2_send(c->dcid, r, n);
    if (AVR.need_register) {
        AVR.need_register = 0;
        avctp_send(r, avrcp_build_register_volume(&AVR, r, (int)sizeof r));
    }
    if (AVR.need_batt) {
        int bn = avrcp_build_register_battery(&AVR, r, (int)sizeof r);
        if (bn > 0) avctp_send(r, bn);   /* clears need_batt, sets batt_tried */
    }
}

/* 1 when the headphones take remote volume (the page shows the control). */
static int avrcp_avail(void)
{
    return chan_find_role(ROLE_AVCTP, 1) && avrcp_reported(&AVR);
}

/* Headphone volume (0..127) toward the headphones, both roles. */
static void avrcp_set_volume(int vol)
{
    unsigned char r[32];
    if (!chan_find_role(ROLE_AVCTP, 1)) {
        AVR.volume = vol;
        return;
    }
    AVR.now_ms = now_ms();
    avctp_send(r, avrcp_build_set_volume(&AVR, vol, r, (int)sizeof r));
    avctp_send(r, avrcp_build_volume_changed(&AVR, r, (int)sizeof r));
    out("avrcp: SetAbsoluteVolume %d/127 sent", vol);
}

static void on_l2cap(unsigned cid, const unsigned char *d, int len)
{
    struct chan *c;
    if (cid == CID_SIGNALING) {
        on_signaling(d, len);
        return;
    }
    c = chan_by_scid(cid);
    if (!c) {
        out("l2cap: %d bytes on unknown local cid 0x%04x", len, cid);
        return;
    }
    switch (c->role) {
    case ROLE_SDP:   on_sdp(c, d, len); break;
    case ROLE_AVSIG: on_avdtp(c, d, len); break;
    case ROLE_MEDIA: G.media_rx++; break;
    case ROLE_AVCTP: on_avctp(c, d, len); break;
    case ROLE_RFCOMM:on_rfcomm(c, d, len); break;
    default:         break;
    }
}

static unsigned char next_av_label(void)
{
    AV.tl = (unsigned char)((AV.tl + 1) & 0x0F);
    if (!AV.tl) AV.tl = 1;
    return AV.tl;
}

/* Send one of our commands and wait for its response, answering peer
 * commands meanwhile. Returns the message type (2 accept, 3 reject,
 * 1 general reject) or -1 on timeout / no channel. */
static int avdtp_cmd(unsigned char sig, const unsigned char *body, int blen,
                     unsigned char *rsp, int rsp_max, int *rsp_len, long timeout_ms)
{
    unsigned char pkt[64];
    unsigned char label = next_av_label();
    struct chan *c = chan_find_role(ROLE_AVSIG, 1);
    long deadline;
    int n = 2;

    if (rsp_len) *rsp_len = 0;
    if (!c) {
        out("avdtp: no signaling channel");
        return -1;
    }
    if (blen < 0 || blen + 2 > (int)sizeof pkt) return -1;
    pkt[0] = AV_HDR(label, AV_MSG_COMMAND);
    pkt[1] = sig;
    if (blen > 0) {
        memcpy(pkt + 2, body, (size_t)blen);
        n += blen;
    }
    AV.wait_label = label;
    AV.wait_sig = sig;
    AV.rsp_done = 0;
    AV.rsp_msg = -1;
    AV.rsp_len = 0;
    out("avdtp: -> %s (label %u)", av_sig_name(sig), label);
    hexline("avdtp tx", pkt, n);
    if (!chan_send(c, pkt, n)) {
        AV.wait_label = 0;
        return -1;
    }
    deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : AVDTP_RSP_MS);
    while (now_ms() < deadline && !G.transport_dead && !G.disc_done && c->st == CH_OPEN) {
        pump_once();
        if (AV.rsp_done) break;
    }
    AV.wait_label = 0;
    if (!AV.rsp_done) {
        out("avdtp: no response to %s", av_sig_name(sig));
        return -1;
    }
    if (AV.rsp_msg != AV_MSG_ACCEPT)
        out("avdtp: %s %s, error 0x%02x", av_sig_name(sig),
            AV.rsp_msg == AV_MSG_REJECT ? "rejected" : "general reject",
            AV.rsp_len > 0 ? AV.rsp[AV.rsp_len - 1] : 0);
    if (rsp && rsp_max > 0) {
        int k = AV.rsp_len > rsp_max ? rsp_max : AV.rsp_len;
        memcpy(rsp, AV.rsp, (size_t)k);
        if (rsp_len) *rsp_len = k;
    }
    return AV.rsp_msg;
}

static void on_acl(const unsigned char *pkt, int n)
{
    unsigned handle, flags, dlen;
    const unsigned char *payload;
    int plen;

    if (n < 4) return;
    handle = le16(pkt) & 0x0FFF;
    flags = (le16(pkt) >> 12) & 0x3;
    dlen = le16(pkt + 2);
    if ((int)dlen + 4 > n) {
        out("acl: short packet (claim %u, have %d)", dlen, n - 4);
        dlen = (unsigned)(n - 4);
    }
    if (!G.connected || handle != (G.handle & 0x0FFF)) {
        if (!G.connected && G.conn_pending) {
            /* Data can beat the Connection Complete event through the two USB
             * pipes. Hold it briefly and replay once the handle is known. */
            int k;
            if (G.nhold >= ACL_HOLD_MAX) {
                out("acl: hold queue full, dropping oldest");
                memmove(&G.hold[0], &G.hold[1], sizeof G.hold[0] * (ACL_HOLD_MAX - 1));
                G.nhold = ACL_HOLD_MAX - 1;
            }
            k = G.nhold++;
            if (n > (int)sizeof G.hold[k].d) n = (int)sizeof G.hold[k].d;
            memcpy(G.hold[k].d, pkt, (size_t)n);
            G.hold[k].n = n;
            G.hold[k].handle = handle;
            G.hold[k].t = now_ms();
            out("acl: holding %d bytes for handle 0x%03x until Connection Complete", n, handle);
            return;
        }
        out("acl: packet for handle 0x%03x (ours 0x%03x%s), ignoring", handle, G.handle,
            G.connected ? "" : ", not connected");
        return;
    }
    payload = pkt + 4;
    plen = (int)dlen;

    /* PB 0b10 = start, 0b01 = continue. Reassemble one L2CAP PDU. */
    if (flags == 0x2) {
        if (plen < 4) return;
        G.acl_need = (int)le16(payload) + 4;
        G.acl_fill = 0;
        if (G.acl_need > (int)sizeof G.acl) {
            out("acl: L2CAP PDU %d too large", G.acl_need);
            G.acl_need = 0;
            return;
        }
        if (plen > G.acl_need) plen = G.acl_need;
        memcpy(G.acl, payload, (size_t)plen);
        G.acl_fill = plen;
    } else if (flags == 0x1) {
        if (!G.acl_need) return;
        if (G.acl_fill + plen > G.acl_need) plen = G.acl_need - G.acl_fill;
        memcpy(G.acl + G.acl_fill, payload, (size_t)plen);
        G.acl_fill += plen;
    } else {
        out("acl: unexpected PB flags %u", flags);
        return;
    }

    if (G.acl_need && G.acl_fill >= G.acl_need) {
        unsigned l2len = le16(G.acl);
        unsigned cid = le16(G.acl + 2);
        if ((int)l2len + 4 <= G.acl_fill)
            on_l2cap(cid, G.acl + 4, (int)l2len);
        G.acl_need = G.acl_fill = 0;
    }
}

/* ---- high-level steps -------------------------------------------------- */

static const char *hci_ver_name(uint8_t v)
{
    static const char *names[] = {
        "1.0b", "1.1", "1.2", "2.0", "2.1", "3.0",
        "4.0", "4.1", "4.2", "5.0", "5.1", "5.2", "5.3", "5.4", "6.0"
    };
    return v < sizeof names / sizeof names[0] ? names[v] : "unknown";
}

#define cod_major COD_MAJOR
#define cod_minor COD_MINOR


static int parse_bdaddr(const char *s, unsigned char addr_out[6])
{
    unsigned int b[6];
    int i;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6 &&
        sscanf(s, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return 0;
    for (i = 0; i < 6; i++) addr_out[5 - i] = (unsigned char)b[i]; /* HCI little-endian */
    return 1;
}

static int load_target_override(unsigned char addr_out[6])
{
    FILE *f;
    char line[64];
    f = fopen(TARGET_PATH, "r");
    if (!f) f = fopen(TARGET_PATH_ALT, "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    fclose(f);
    if (!parse_bdaddr(line, addr_out)) {
        out("target override file present but BD_ADDR parse failed");
        return 0;
    }
    return 1;
}

static int usb_vid_known(unsigned vid, unsigned pid)
{
    if (vid == CSR_VID && pid == CSR_PID) return 1; /* CSR8510 */
    if (vid == 0x0bda) return 1;                    /* Realtek, incl. 8771 / B00C */
    if (vid == 0x37ad || vid == 0x2357) return 1;   /* TP-Link (often RTL8761BU) */
    if (vid == 0x0b05) return 1;                    /* ASUS (BT600 and similar) */
    return 0;
}

static int usb_is_bt_hci_class(unsigned cls, unsigned sub, unsigned proto)
{
    (void)proto;
    return cls == 0xE0 && sub == 0x01;
}

static int usb_stick_ok(const struct usb_dev_desc *d)
{
    if (!d) return 0;
    if (usb_vid_known(d->idVendor, d->idProduct)) return 1;
    return usb_is_bt_hci_class(d->bDeviceClass, d->bDeviceSubClass, d->bDeviceProtocol);
}

/* Proven CSR8510 mask from the original PS4 bring-up (no Extended Inquiry Result). */
static const unsigned char g_evmask_csr[8] = {
    0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D
};
/* Wider mask so mode-2 (EIR) result events arrive on Realtek and other sticks. */
static const unsigned char g_evmask_wide[8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF, 0x7D
};

static int setup_controller(void)
{
    unsigned char r[64], p[248];
    int n = 0, family = HB_BT_FAMILY_OTHER;
    char why[192], astr[18];
    const unsigned char *mask;
    int is_csr, is_rtl;
    hci_t hci;

    if (!hci_cmd_cc(OP_RESET, NULL, 0, r, sizeof r, &n) || r[0]) {
        say("FAIL: HCI Reset status 0x%02x", n > 0 ? r[0] : 0xFF);
        return 0;
    }
    say("HCI Reset: ok");
    /* Give a ROM-mode Realtek time to be ready. Harmless on CSR. */
    sceKernelUsleep(250 * 1000);

    hci = hci_live();
    if (!hb_rtl_prepare(hci, &family, why, (int)sizeof why)) {
        hci_live_done();
        say("FAIL: Realtek firmware: %s", why[0] ? why : "upload failed");
        return 0;
    }
    is_csr = family == HB_BT_FAMILY_CSR;
    is_rtl = family == HB_BT_FAMILY_REALTEK;
    mask = is_csr ? g_evmask_csr : g_evmask_wide;
    hci_live_done();

    /* Event mask before inquiry mode. CSR keeps the proven PS4 mask. */
    if (!hci_cmd_cc(OP_SET_EVENT_MASK, mask, 8, r, sizeof r, &n) || r[0])
        out("Set_Event_Mask failed (continuing)");
    else
        out("Set_Event_Mask: %s", is_csr ? "CSR/PS4 0x3DBFF807FFFBFFFF" : "EIR 0x7DBFFFFFFFFFFFFF");

    if (!is_csr) {
        unsigned char filt = 0;
        if (!hci_cmd_cc(OP_SET_EVENT_FILTER, &filt, 1, r, sizeof r, &n) || r[0])
            out("Set_Event_Filter clear failed (continuing)");
    }

    /* Shared host address so a headset paired on one stick reconnects on the other.
     * CSR PSKEY is saved without a warm reset (that dropped the USB pipe on PS5);
     * Realtek gets the address in the firmware config. */
    hci = hci_live();
    hb_host_bdaddr_sync_usb(hci, is_csr, is_rtl);
    hci_live_done();

    if (!hci_cmd_cc(OP_READ_BD_ADDR, NULL, 0, r, sizeof r, &n) || n < 7 || r[0]) {
        say("FAIL: adapter USB went away after BD_ADDR sync");
        return 0;
    }
    memcpy(G.local_addr, r + 1, 6);
    addr_str(G.local_addr, astr);
    say("BD_ADDR: %s", astr);

    /* Realtek firmware Reset clears the event mask; put it back. */
    if (!hci_cmd_cc(OP_SET_EVENT_MASK, mask, 8, r, sizeof r, &n) || r[0])
        out("Set_Event_Mask after BD_ADDR sync failed (continuing)");

    if (!hci_cmd_cc(OP_READ_LOCAL_VER, NULL, 0, r, sizeof r, &n) || n < 9 || r[0]) {
        say("FAIL: Read Local Version");
        return 0;
    }
    {
        unsigned hci_rev = (unsigned)(r[2] | (r[3] << 8));
        unsigned manuf = (unsigned)(r[5] | (r[6] << 8));
        const char *fam = btchip_family_name(family);
        say("HCI %s rev 0x%04x, LMP %s, manuf 0x%04x%s%s%s",
            hci_ver_name(r[1]), hci_rev, hci_ver_name(r[4]), manuf,
            manuf == 0x000A ? " (CSR)" : "",
            fam && fam[0] ? " family " : "",
            fam && fam[0] ? fam : "");
    }

    /* A Low Energy only stick cannot do A2DP. Features byte 4 bit 5:
     * BR/EDR not supported. hci_cmd_cc return starts at the status byte. */
    if (hci_cmd_cc(OP_READ_LOCAL_FEATURES, NULL, 0, r, sizeof r, &n) && n >= 9 && r[0] == 0) {
        if (r[5] & 0x20) {
            say("FAIL: this adapter has no Bluetooth Classic (BR/EDR); audio needs it");
            return 0;
        }
    }

    /* Return params: status, ACL_Data_Packet_Length(2), SCO_Data_Packet_Length(1),
     * Total_Num_ACL_Data_Packets(2) at r[4..5], Total_Num_SCO_Data_Packets(2). */
    if (hci_cmd_cc(OP_READ_BUFFER_SIZE, NULL, 0, r, sizeof r, &n) && n >= 8 && r[0] == 0) {
        G.acl_mtu = (unsigned)(r[1] | (r[2] << 8));
        G.acl_pkts = (unsigned)(r[4] | (r[5] << 8));
        if (G.acl_pkts == 0) G.acl_pkts = 1;
        out("ACL buffers: MTU %u, packets %u (SCO packets %u)", G.acl_mtu, G.acl_pkts,
            (unsigned)(r[6] | (r[7] << 8)));
    } else {
        G.acl_mtu = 1021;
        G.acl_pkts = 7;
        out("Read_Buffer_Size failed, assuming MTU 1021 / 7 packets");
    }

    p[0] = 1; /* SSP enabled */
    if (!hci_cmd_cc(OP_WRITE_SSP_MODE, p, 1, r, sizeof r, &n) || r[0])
        out("Write_Simple_Pairing_Mode failed (continuing)");
    else
        out("Write_Simple_Pairing_Mode: enabled");

    /* CSR: inquiry mode 1 (RSSI results). Realtek and others: mode 2 (EIR).
     * Mode 2 is accepted by some CSR clones that then emit no results. */
    p[0] = is_csr ? 1 : 2;
    if (!hci_cmd_cc(OP_WRITE_INQUIRY_MODE, p, 1, r, sizeof r, &n) || r[0])
        out("Write_Inquiry_Mode failed (continuing with standard results)");
    else
        out("Write_Inquiry_Mode: %s", is_csr ? "RSSI (CSR/PS4)" : "EIR");

    put16(p, PAGE_TIMEOUT);
    if (!hci_cmd_cc(OP_WRITE_PAGE_TIMEOUT, p, 2, r, sizeof r, &n) || r[0])
        out("Write_Page_Timeout failed (continuing)");
    else
        out("Write_Page_Timeout: 0x%04x (%.2f s)", PAGE_TIMEOUT, PAGE_TIMEOUT * 0.000625);

    /* Same COD as PS5 HearBridge: Audio/Video major, unclassified minor.
     * Sony earbuds often only open HFP (battery) when the remote looks like
     * an A/V gateway rather than a plain computer dongle. */
    p[0] = 0x00; p[1] = 0x04; p[2] = 0x00;
    if (!hci_cmd_cc(OP_WRITE_CLASS_OF_DEV, p, 3, r, sizeof r, &n) || r[0])
        out("Write_Class_Of_Device failed (continuing)");
    else
        out("Write_Class_Of_Device: 0x000400 (Audio/Video)");

    memset(p, 0, 248);
    snprintf((char *)p, 248, "HearBridge PS4");
    if (!hci_cmd_cc(OP_WRITE_LOCAL_NAME, p, 248, r, sizeof r, &n) || r[0])
        out("Write_Local_Name failed (continuing)");
    else
        out("Write_Local_Name: HearBridge PS4");

    return 1;
}

/* Make sure the controller is not inquiring before we page. Inquiry_Cancel
 * answers with Command Complete; status 0x0C means no inquiry was running.
 * If no Command Complete arrives, wait for Inquiry Complete instead. */
static int inquiry_ensure_stopped(void)
{
    unsigned char r[8];
    int n = 0;
    long deadline;

    if (!G.inq_active) return 1;
    out("inquiry still active, sending Inquiry_Cancel before paging");
    if (hci_cmd_cc(OP_INQUIRY_CANCEL, NULL, 0, r, sizeof r, &n) || G.cc_done) {
        out("Inquiry_Cancel Command Complete status 0x%02x", n > 0 ? r[0] : 0);
        G.inq_active = 0;
        /* Let a late Inquiry Complete drain before the next command. */
        pump_until(now_ms() + 200);
        return 1;
    }
    /* No Command Complete: Inquiry Complete (0x01) also means stopped. */
    out("Inquiry_Cancel: no Command Complete yet, waiting for Inquiry Complete");
    deadline = now_ms() + INQ_STOP_WAIT_MS;
    while (G.inq_active && !G.inq_done && now_ms() < deadline && !G.transport_dead)
        pump_once();
    if (G.inq_done) G.inq_active = 0;
    if (G.inq_active && !G.transport_dead) {
        /* One more cancel: the first may have crossed an inquiry restart. */
        if (hci_cmd_cc(OP_INQUIRY_CANCEL, NULL, 0, r, sizeof r, &n) || G.cc_done || G.inq_done) {
            out("Inquiry_Cancel (2nd) done");
            G.inq_active = 0;
        }
    }
    if (G.inq_active && !G.transport_dead && !G.connected) {
        /* The controller does not answer Cancel at all (seen once on the
         * first scan after start). A reset ends any inquiry; configure it
         * again and carry on with what was found. */
        out("inquiry: no answer to Cancel, resetting the controller");
        G.ev_fill = 0;
        if (setup_controller()) {
            G.inq_active = 0;
            G.inq_done = 1;
            G.page_scan = 0;
            say("Controller reset, inquiry stopped");
        }
    }
    if (G.inq_active) {
        say("FAIL: inquiry did not stop, not paging");
        return 0;
    }
    return 1;
}

static int run_inquiry(void)
{
    unsigned char p[5];
    int i;

    /* GIAC, length, max responses (0 = unlimited) */
    p[0] = 0x33; p[1] = 0x8B; p[2] = 0x9E;
    p[3] = INQUIRY_LEN;
    p[4] = 0;

    G.inq_done = 0;
    G.inq_status = 0;
    G.ndev = 0;
    g_ign_n = 0;
    set_state("scanning", "HearBridge: scanning for headphones", 0);
    out("starting inquiry (%d x 1.28 s)", INQUIRY_LEN);

    G.inq_active = 1;   /* set first: Inquiry Complete may follow quickly */
    if (!hci_cmd_status(OP_INQUIRY, p, 5)) {
        G.inq_active = 0;
        say("FAIL: Inquiry rejected");
        return 0;
    }
    {
        long deadline = now_ms() + INQUIRY_WAIT_MS;
        while (!G.inq_done && !G.scan_abort && !G.transport_dead && now_ms() < deadline)
            pump_once();
        if (G.scan_abort && !G.inq_done)
            out("inquiry: stopping early for a UI command");
        else if (!G.inq_done)
            out("inquiry wait timed out");
    }
    if (!inquiry_ensure_stopped()) return 0;

    say("Found %d device(s)", G.ndev);
    for (i = 0; i < G.ndev; i++) {
        char astr[18];
        struct dev *d = &G.dev[i];
        addr_str(d->addr, astr);
        out("  [%d] %s cod 0x%06x major %d minor %d avg_rssi %d n=%d psrm %u \"%s\"",
            i, astr, d->cod, cod_major(d->cod), cod_minor(d->cod),
            dev_rssi_avg(d), d->rssi_n, d->psrm, d->name);
    }
    return G.ndev > 0;
}

static int ask_remote_name(struct dev *d, long wait_ms)
{
    unsigned char np[10];
    char astr[18];
    if (!d) return 0;
    if (!inquiry_ensure_stopped()) return 0;
    addr_str(d->addr, astr);
    memcpy(np, d->addr, 6);
    np[6] = d->psrm ? d->psrm : 0x01;
    np[7] = 0;
    put16(np + 8, (unsigned)(d->clk | 0x8000));
    G.name_done = 0;
    out("asking name of %s", astr);
    /* Short CS wait; name complete can race ahead and satisfies the wait. */
    if (!hci_cmd_status_ex(OP_REMOTE_NAME_REQ, np, 10, CMD_STATUS_SHORT_MS, 0)) {
        /* Still wait briefly in case CS was missed but complete arrives. */
        if (!wait_flag(&G.name_done, 500)) {
            out("remote name command failed for %s (device kept)", astr);
            return 0;
        }
    } else if (!G.name_done) {
        if (!wait_flag(&G.name_done, wait_ms))
            out("remote name timeout for %s (device kept)", astr);
    }
    return d->name[0] != 0;
}

/* Per-peer state before a connection (outgoing or incoming): the saved
 * link key and cleared connection / auth flags. */
static void prepare_peer(const unsigned char addr[6])
{
    int kt = 0;
    memcpy(G.peer, addr, 6);
    G.conn_done = 0;
    G.conn_status = 0xFF;
    G.connected = 0;
    G.handle = 0;
    G.nhold = 0;
    G.hold_replay = 0;
    G.auth_done = G.enc_done = G.ssp_done = 0;
    G.have_key = linkkeys_load(addr, G.link_key, &kt);
    G.key_type = kt;
    if (G.have_key && g_ignore_saved_key) {
        out("auth: saved link key was rejected last time, pairing fresh");
        G.have_key = 0;
    } else if (G.have_key) {
        out("auth: loaded persisted link key type=%u", (unsigned)G.key_type);
    } else {
        out("auth: no persisted link key for this device");
    }
}

static int g_soft_fail;   /* last connect failed only because nobody answered */
static int g_busy_fails;  /* Create_Connection refused as busy, in a row */
/* Paging is limited (src/pager.h). Outgoing pages only at daemon start (one
 * page, about 5 s) and on Connect / Reconnect / Try now or a pairing try (4
 * pages, about 25 s). Never after a stream ended or the link dropped, never
 * after a scan, never while waiting. Then the daemon only listens (page scan
 * on, USB session open). A Connection Request from a saved headphone or
 * speaker is accepted in every phase. */
#define PAGE_TRIES     HB_PAGE_TRIES
static struct hb_pager g_pager = { HB_PAGE_TRIES_START };   /* pages left before the quiet phase */
#define BUSY_FAILS_MAX 6  /* then give up the session */
#define BUSY_WAIT_MS   4000

/* After the ACL link is up: drop the page scan (no second caller) and use a
 * shorter supervision timeout so a headset that vanished (powered off, out
 * of range) is noticed in about 10 s instead of 20 s. */
static void link_up_setup(void)
{
    unsigned char p[4], r[8];
    int n = 0;
    if (G.page_scan) {
        p[0] = 0x00;
        if (hci_cmd_cc(OP_WRITE_SCAN_ENABLE, p, 1, r, sizeof r, &n)) G.page_scan = 0;
    }
    G.accept_in = 0;
    put16(p, G.handle);
    put16(p + 2, SUPERVISION_UNITS);
    if (!hci_cmd_cc(OP_WRITE_LINK_SUPV, p, 4, r, sizeof r, &n))
        out("Write_Link_Supervision_Timeout failed (continuing)");
    else
        out("link supervision timeout: %d s", SUPERVISION_UNITS * 625 / 1000000);
}


static int connect_device(struct dev *d)
{
    unsigned char p[13];
    char astr[18];
    const char *label;

    addr_str(d->addr, astr);
    label = d->name[0] ? d->name : astr;
    snprintf(g_cur_name, sizeof g_cur_name, "%s", d->name);
    g_cur_cod = d->cod;
    ui_set_device(d->addr, d->name[0] ? d->name : NULL);
    set_state("connecting", "HearBridge: connecting to headphones", 0);
    say("Connecting to %s (%s) cod 0x%06x", label, astr, d->cod);

    /* Never page while an inquiry is still running (a page during an inquiry fails). */
    if (!inquiry_ensure_stopped()) return 0;

    prepare_peer(d->addr);
    g_soft_fail = 0;
    if (!G.conn_pending && !G.connected) G.inbound = 0;   /* a call we accepted is still on its way */

    memcpy(p, d->addr, 6);
    put16(p + 6, 0xCC18); /* DM1, DH1, DM3, DH3, DM5, DH5 */
    p[8] = d->psrm ? d->psrm : 0x01;
    p[9] = 0;
    put16(p + 10, d->clk ? (unsigned)(d->clk | 0x8000) : 0);
    p[12] = 1; /* allow role switch */

    /* A saved device with a key is paged for a shorter time (the reconnect
     * loop tries again soon); a new pairing keeps the long page. */
    {
        unsigned char pt[2], r[8];
        int n = 0;
        put16(pt, G.have_key ? PAGE_TIMEOUT_SAVED : PAGE_TIMEOUT);
        hci_cmd_cc(OP_WRITE_PAGE_TIMEOUT, pt, 2, r, sizeof r, &n);
    }

    G.conn_pending = 1;
    if (!hci_cmd_status(OP_CREATE_CONN, p, 13)) {
        if (hci_create_conn_busy(G.cs_status) && ++g_busy_fails < BUSY_FAILS_MAX) {
            /* The headset is calling us right now (or the link is already
             * there). Keep waiting for its Connection Complete instead of
             * restarting the USB session in the middle of the connect. */
            out("Create_Connection refused (0x%02x), the headset is probably calling: waiting for it",
                G.cs_status);
            if (!G.connected) wait_flag(&G.conn_done, BUSY_WAIT_MS);
            if (!G.connected) {
                G.conn_pending = 0;
                G.nhold = 0;
                g_soft_fail = !G.user_abort;       /* listen, then page again, same session */
                say("FAIL: no call from the headset after the refused page");
                return 0;
            }
            G.inbound = 1;
            goto linked;
        }
        G.conn_pending = 0;
        G.nhold = 0;
        say("FAIL: Create_Connection rejected");
        return 0;
    }
    g_busy_fails = 0;
    if (!wait_flag(&G.conn_done, CONN_WAIT_MS)) {
        out("Create_Connection timeout, cancelling");
        /* A UI command (Disconnect) cut the wait short: the transport is
         * still fine, so cancel properly and watch for a late connection. */
        if (G.user_abort && !EVT.dead) G.transport_dead = 0;
        /* Create_Connection_Cancel answers with Command Complete. */
        hci_cmd_cc(OP_CREATE_CONN_CANCEL, d->addr, 6, NULL, 0, NULL);
        pump_until(now_ms() + 1000);
        if (!G.connected) {
            G.conn_pending = 0;
            G.nhold = 0;
            say("FAIL: page timeout / no Connection Complete");
            g_soft_fail = !G.user_abort;
            return 0;
        }
    }
    if (!G.connected) {
        say("FAIL: connect status 0x%02x (%s)", G.conn_status, hci_err(G.conn_status));
        /* Page timeout, connection timeout, failed to establish: nobody
         * answered, so listening and paging again is the right answer. */
        g_soft_fail = G.conn_status == 0x04 || G.conn_status == 0x08 || G.conn_status == 0x3E;
        return 0;
    }
linked:
    g_busy_fails = 0;
    say("ACL handle 0x%03x", G.handle);
    link_up_setup();
    return 1;
}

/* Decide what an authentication failure means for the next attempt. */
static void note_auth_failure(int st)
{
    if (st < 0) return;
    if (!G.have_key && (st == 0x18 || st == 0x05)) {
        G.need_pairing = 1;
        out("auth: headset refused pairing (0x%02x) and no saved key: needs pairing mode", st);
    } else if (G.have_key && (st == 0x05 || st == 0x06)) {
        g_ignore_saved_key = 1;
        out("auth: saved link key rejected (0x%02x), next attempt pairs fresh", st);
    }
}

static int run_auth(void)
{
    unsigned char p[3];
    long deadline;

    /* Give the remote a moment to start SSP itself, then request auth. */
    pump_until(now_ms() + 500);

    set_state("authenticating", NULL, 0);
    put16(p, G.handle);
    G.auth_done = 0;
    G.auth_status = 0xFF;
    if (!hci_cmd_status(OP_AUTH_REQUESTED, p, 2)) {
        say("FAIL: Authentication_Requested rejected");
        return 0;
    }
    deadline = now_ms() + AUTH_WAIT_MS;
    while (now_ms() < deadline && !G.transport_dead) {
        pump_once();
        if (G.auth_done) break;
        if (G.disc_done) {
            say("FAIL: disconnected during auth (reason 0x%02x)", G.disc_reason);
            if (G.ssp_done && G.ssp_status) note_auth_failure(G.ssp_status);
            else note_auth_failure(G.disc_reason);
            return 0;
        }
    }
    if (!G.auth_done) {
        say("FAIL: authentication timeout");
        if (G.ssp_done && G.ssp_status) note_auth_failure(G.ssp_status);
        return 0;
    }
    if (G.auth_status != 0) {
        say("FAIL: auth status 0x%02x (%s)", G.auth_status, hci_err(G.auth_status));
        note_auth_failure(G.auth_status);
        return 0;
    }
    G.authed = 1;
    out("authentication ok%s", G.have_key ? ", link key available" : "");

    put16(p, G.handle);
    p[2] = 1;
    G.enc_done = 0;
    G.enc_on = 0;
    if (!hci_cmd_status(OP_SET_ENCRYPTION, p, 3)) {
        say("FAIL: Set_Connection_Encryption rejected");
        return 0;
    }
    if (!wait_flag(&G.enc_done, ENC_WAIT_MS) || !G.enc_on) {
        say("FAIL: encryption not enabled (status 0x%02x)", G.enc_status);
        return 0;
    }
    say("Encryption: on");
    return 1;
}

/* Info exchange, then the AVDTP signaling channel (ours, or the peer's if it
 * opened one first). */
static int open_avdtp(void)
{
    unsigned char r[4];
    struct chan *c;
    int i;

    for (i = 0; i < 2; i++) {
        unsigned type = (i == 0) ? 2u : 3u;
        put16(r, type);
        G.info_wait_id = next_sig_id();
        G.info_got = 0;
        sig_send(L2_INFO_REQ, (unsigned char)G.info_wait_id, r, 2);
        wait_flag(&G.info_got, 1500);
        G.info_wait_id = 0;
    }

    set_state("opening_audio", NULL, 0);
    c = chan_find_role(ROLE_AVSIG, 1);
    if (!c) c = l2_connect(PSM_AVDTP, ROLE_AVSIG);
    if (!c) {
        say("FAIL: AVDTP signaling channel not opened");
        return 0;
    }
    say("AVDTP signaling open (local 0x%04x remote 0x%04x)", c->scid, c->dcid);
    return 1;
}

static int sbc_state_from_ie(hb_sbc_state *st, const unsigned char ie[4])
{
    memset(st, 0, sizeof *st);
    st->fs_code = (ie[0] & 0x10) ? HB_SBC_FS_48K : (ie[0] & 0x20) ? HB_SBC_FS_44K1 :
                  (ie[0] & 0x40) ? HB_SBC_FS_32K : HB_SBC_FS_16K;
    st->mode = (ie[0] & 0x01) ? HB_SBC_JOINT : (ie[0] & 0x02) ? HB_SBC_STEREO :
               (ie[0] & 0x04) ? HB_SBC_DUAL : HB_SBC_MONO;
    st->nblocks = (ie[1] & 0x10) ? 16 : (ie[1] & 0x20) ? 12 : (ie[1] & 0x40) ? 8 : 4;
    st->nsub = (ie[1] & 0x04) ? 8 : 4;
    st->alloc = (ie[1] & 0x01) ? HB_SBC_LOUDNESS : HB_SBC_SNR;
    st->bitpool = ie[3];
    return hb_sbc_setup(st);
}

/* Pick one value from each field of the peer's SBC capabilities that we
 * also support: 48 kHz (else 44.1), joint stereo (else stereo), 16 blocks,
 * 8 subbands, loudness (else SNR), bitpool up to 53. */
static int choose_sbc_config(const unsigned char cap[4], unsigned char cfg[4], int pref)
{
    unsigned char a = cap[0], b = cap[1];
    unsigned lo = cap[2], hi = cap[3];
    unsigned ceil;
    /* pref: 0 Auto, 1 SBC, 2 SBC HQ, 3 SBC-XQ. HQ is joint/stereo with a
     * higher bitpool; XQ is dual channel. Both only if the sink advertised
     * them (checked by the caller via codec_avail); fall back to plain SBC. */
    int xq = pref == 3 && (a & 0x04) && cap[3] >= 35;
    int hq = pref == 2 && cap[3] > 53 && (a & 0x03);
    if (xq) {
        cfg[0] = (a & 0x10) ? 0x10 : (a & 0x20) ? 0x20 : 0;
        if (!cfg[0]) return 0;
        cfg[0] |= 0x04;                               /* dual channel */
    } else {
        cfg[0] = (a & 0x10) ? 0x10 : (a & 0x20) ? 0x20 : 0;
        if (!cfg[0]) return 0;
        cfg[0] |= (a & 0x01) ? 0x01 : (a & 0x02) ? 0x02 : 0;
        if (!(cfg[0] & 0x0F)) return 0;
    }
    cfg[1] = (b & 0x10) ? 0x10 : (b & 0x20) ? 0x20 : (b & 0x40) ? 0x40 : 0x80;
    cfg[1] |= (b & 0x04) ? 0x04 : 0x08;
    cfg[1] |= (b & 0x01) ? 0x01 : 0x02;
    if (lo < SBC_MIN_BP) lo = SBC_MIN_BP;
    /* Ceiling by the chosen codec. Plain SBC stays at the CSR8510 + 2.4 GHz
     * coexistence cap; HQ and XQ climb higher when the headset allows it. */
    ceil = xq ? 38u : hq ? 53u : (unsigned)SBC_MAX_BP;
    if (hi > ceil) hi = ceil;
    if (hi < lo) return 0;
    cfg[2] = (unsigned char)lo;
    cfg[3] = (unsigned char)hi;
    return 1;
}

static uint64_t now_us(void) { return sceKernelGetProcessTime(); }

/* Stream PCM from UDP (or silence) as RTP + SBC on the media channel.
 * Returns 1 on clean stop command, 0 on link/stream loss. */
static int stream_pcm(struct chan *m)
{
    static const int fs_hz[4] = { 16000, 32000, 44100, 48000 };
    hb_sbc_state st;
    unsigned char pkt[1100];
    int16_t pcm[16 * 8 * 2 * 16]; /* up to 15 SBC frames of stereo */
    int fs, nch, spf, flen, fpp, mtu, i;
    long frames_sent = 0, pkts = 0, silent_frames = 0;
    unsigned seq = 1;
    uint32_t ts = 0;
    uint64_t t0, next_log;
    int stop = 0;
    int bp_max, drops_seen = 0, resyncs = 0;
    long catchups = 0;
    unsigned long last_rx = 0, last_tap_file = 0;
    long no_pcm_s = 0;
    int warned_no_pcm = 0;
    uint64_t bp_clean_since, next_ui = 0;

    if (sbc_state_from_ie(&st, AV.sbc_ie) != 0) {
        say("FAIL: SBC config %02x %02x bitpool %u not usable",
            AV.sbc_ie[0], AV.sbc_ie[1], AV.sbc_ie[3]);
        return 0;
    }
    fs = fs_hz[st.fs_code];
    nch = hb_sbc_channels(&st);
    spf = st.nblocks * st.nsub;
    flen = (int)hb_sbc_frame_len(&st);
    mtu = m->peer_mtu ? (int)m->peer_mtu : L2_MTU;
    if (mtu > (int)sizeof pkt) mtu = (int)sizeof pkt;
    fpp = (mtu - 13) / flen;
    if (fpp > 15) fpp = 15;
    {
        int max_by_buf = (int)(sizeof pcm / (sizeof(int16_t) * 2)) / (spf > 0 ? spf : 1);
        if (fpp > max_by_buf) fpp = max_by_buf;
    }
    if (fpp < 1) { say("FAIL: media MTU %d too small for %d-byte SBC frames", mtu, flen); return 0; }

    out("stream: SBC %d Hz %s blocks %d subbands %d bitpool %d, frame %d B, %d frames/packet, MTU %d",
        fs, st.mode == HB_SBC_JOINT ? "joint" : st.mode == HB_SBC_STEREO ? "stereo" : "mono/dual",
        st.nblocks, st.nsub, st.bitpool, flen, fpp, mtu);
    if (fs != 48000)
        out("stream: negotiated %d Hz, the 48 kHz game audio is resampled to it", fs);

    /* No SO_REUSEADDR, so a port still held by an old instance fails
     * here instead of silently receiving nothing. Ask the holder to stop
     * and retry for ~10 s, then fall back to PCM_UDP_PORT_ALT (the tap
     * reads the port from PCM_PORT_FILE). */
    {
        int port = PCM_UDP_PORT, rc = -1, tries = 0;
        long t_end = now_ms() + 10000;
        for (;;) {
            rc = pcm_udp_open_port(port);
            tries++;
            if (rc == 0) break;
            out("stream: UDP bind 127.0.0.1:%d failed errno %d (try %d)", port, rc, tries);
            if (tries == 1) pcm_udp_send_stop_port(port);
            if (port == PCM_UDP_PORT && now_ms() >= t_end) {
                port = PCM_UDP_PORT_ALT;
                t_end = now_ms() + 1500;
                continue;
            }
            if (port == PCM_UDP_PORT_ALT && now_ms() >= t_end) break;
            pump_until(now_ms() + 500);
            if (G.transport_dead || G.disc_done) break;
        }
        if (rc != 0) {
            say("FAIL: UDP bind 127.0.0.1:%d/%d (errno %d)", PCM_UDP_PORT, PCM_UDP_PORT_ALT, rc);
            set_state("error_udp_bind", "HearBridge: audio input port busy", 0);
            return 0;
        }
        {
            FILE *pf = fopen(PCM_PORT_FILE, "w");
            if (pf) { fprintf(pf, "%d\n", port); fclose(pf); }
        }
        out("stream: UDP bind 127.0.0.1:%d ok (%d tries)%s", port, tries,
            port != PCM_UDP_PORT ? " - fallback port, the tap follows " PCM_PORT_FILE : "");
    }
    pcm_udp_set_out_rate(fs);
    /* Jitter-buffer target from the UI latency control. Auto keeps the
     * adaptive default; a manual value really changes the ring target. */
    {
        int la = ui_get_lat_auto();
        if (!la) pcm_udp_set_target_ms(ui_get_lat_ms());
        else     pcm_udp_set_target_ms(70);
        ui_set_lat_auto_ms(la ? pcm_udp_target_ms() : 0);
        out("stream: jitter target %d ms (%s)", pcm_udp_target_ms(), la ? "auto" : "manual");
    }
    out("stream: listening for PCM on UDP 127.0.0.1:%d (send \"stop\" to exit)", pcm_udp_port());
    g_session_streamed = 1;
    set_state("streaming", "HearBridge: streaming to headphones", 1);
    if (devices_touch(G.peer, g_cur_name, g_cur_cod) != 0)
        out("devices: could not save %s", DEVICES_PATH);
    g_want = 0;                   /* now it is simply the last used device */
    ui_set_streaming(1);

    /* AVRCP: most headphones open the control channel themselves right after
     * the stream starts. If not, we open it from the stream loop without
     * blocking audio, and retry: some headphones (some earbuds) answer
     * "no resources" while they are still busy setting up the link. */
    avr_next = now_ms() + 1500;
    avr_tries = 0;
    avr_c = NULL;

    G.poll_ms = STREAM_POLL_MS;
    G.acl_stalls = 0;
    G.acl_media_drops = 0;
    G.acl_last_credit_ms = now_ms();
    bp_max = st.bitpool;
    t0 = now_us();
    bp_clean_since = t0;
    next_log = t0 + 5000000ull;

    while (!stop && !G.stop_requested) {
        uint64_t due = t0 + (uint64_t)frames_sent * (uint64_t)spf * 1000000u / (uint64_t)fs;
        int nf, len = 13, got, need;

        if (G.transport_dead || G.disc_done || m->st != CH_OPEN) {
            out("stream: link or media channel lost after %ld frames", frames_sent);
            break;
        }
        if (!AV.started) {
            out("stream: peer suspended/closed the stream after %ld frames", frames_sent);
            g_end_reason = 0x13;        /* the headphones ended it */
            break;
        }
        if (pcm_udp_poll()) {
            stop = 1;
            G.stop_requested = 1;
            out("stream: stop command received");
            break;
        }
        if (due > now_us() + STREAM_AHEAD_US) {
            pump_once();
            continue;
        }
        if (due + STREAM_LATE_US < now_us()) {
            /* Long stall: skip ahead to real time, never burst. */
            uint64_t nowv = now_us();
            t0 = nowv - (uint64_t)frames_sent * (uint64_t)spf * 1000000u / (uint64_t)fs;
            resyncs++;
        } else if (due + 5000 < now_us()) {
            /* A little behind: catch up, but only into free ACL credits so
             * the controller queue never floods. */
            if (G.acl_pkts && G.acl_inflight >= (int)G.acl_pkts - 1) {
                pump_once();
                continue;
            }
            catchups++;
        }
        /* Congestion adaptation: drop bitpool on media drops, raise slowly. */
        if (G.acl_media_drops != drops_seen) {
            drops_seen = G.acl_media_drops;
            bp_clean_since = now_us();
            if (st.bitpool > SBC_ADAPT_MIN_BP) {
                int nb = st.bitpool - 4;
                if (nb < SBC_ADAPT_MIN_BP) nb = SBC_ADAPT_MIN_BP;
                if (nb < (int)AV.sbc_ie[2]) nb = (int)AV.sbc_ie[2];
                if (nb != st.bitpool) {
                    st.bitpool = nb;
                    flen = (int)hb_sbc_frame_len(&st);
                    fpp = (mtu - 13) / flen;
                    if (fpp > 15) fpp = 15;
                    {
                        int mb = (int)(sizeof pcm / (sizeof(int16_t) * 2)) / (spf > 0 ? spf : 1);
                        if (fpp > mb) fpp = mb;
                    }
                    out("stream: congestion, bitpool -> %d (frame %d B, %d/pkt)", nb, flen, fpp);
                }
            }
        } else if (st.bitpool < bp_max && now_us() - bp_clean_since > 10000000ull) {
            int nb = st.bitpool + 2;
            if (nb > bp_max) nb = bp_max;
            st.bitpool = nb;
            flen = (int)hb_sbc_frame_len(&st);
            fpp = (mtu - 13) / flen;
            if (fpp > 15) fpp = 15;
            {
                int mb = (int)(sizeof pcm / (sizeof(int16_t) * 2)) / (spf > 0 ? spf : 1);
                if (fpp > mb) fpp = mb;
            }
            bp_clean_since = now_us();
            out("stream: link clean 10 s, bitpool -> %d (frame %d B, %d/pkt)", nb, flen, fpp);
        }

        nf = fpp;
        need = nf * spf;
        memset(pcm, 0, sizeof pcm);
        /* Pull spf*nf stereo frames at the negotiated rate (resampled from
         * the 48 kHz game audio inside pcm_udp_read_s16). */
        got = pcm_udp_read_s16(pcm, need);
        if (got < need) {
#if SILENCE_KEEPALIVE
            /* remainder already zeroed */
            silent_frames += (need - got);
#else
            pump_once();
            continue;
#endif
        }
        /* AVRCP control channel: open / retry in the background. */
        {
            static const long backoff[AVR_MAX_TRIES] = { 0, 3000, 7000, 15000, 30000 };
            long nowm = now_ms();
            if (avr_c && avr_c->role == ROLE_AVCTP && !avr_c->inbound) {
                struct chan *o = NULL;
                int i;
                for (i = 0; i < MAX_CH; i++)
                    if (&G.ch[i] != avr_c && G.ch[i].role == ROLE_AVCTP && G.ch[i].st == CH_OPEN)
                        o = &G.ch[i];
                if (o) {
                    out("avrcp: headphones opened AVRCP themselves, dropping our attempt");
                    if (avr_c->st == CH_OPEN || avr_c->st == CH_CONFIG) chan_disconnect(avr_c);
                    else avr_c->st = CH_FREE;
                    avr_c = NULL;
                } else if (avr_c->st == CH_OPEN) {
                    out("avrcp: control channel open on attempt %d", avr_tries);
                    avr_c = NULL;
                } else if (avr_c->st == CH_CLOSED || (avr_c->st == CH_CONNECTING && nowm - avr_t > 8000)) {
                    out("avrcp: attempt %d %s (%s)", avr_tries,
                        avr_c->st == CH_CLOSED ? "refused" : "timed out",
                        avr_c->st == CH_CLOSED ? l2_result(avr_c->result) : "no answer");
                    avr_c->st = CH_FREE;
                    avr_c = NULL;
                    if (avr_tries < AVR_MAX_TRIES)
                        avr_next = nowm + backoff[avr_tries];
                    else
                        out("avrcp: no control channel after %d attempts (PS4-side volume only)", avr_tries);
                }
            } else if (avr_c) {
                avr_c = NULL;
            }
            if (!avr_c && avr_next && nowm >= avr_next) {
                avr_next = 0;
                if (chan_find_role(ROLE_AVCTP, 0)) {
                    out("avrcp: control channel already there");
                } else if (avr_tries < AVR_MAX_TRIES) {
                    avr_tries++;
                    out("avrcp: headphones did not open AVRCP, opening it (attempt %d of %d)",
                        avr_tries, AVR_MAX_TRIES);
                    avr_c = l2_connect_async(PSM_AVCTP, ROLE_AVCTP);
                    avr_t = nowm;
                }
            }
        }
        /* Headphone volume (AVRCP): page requests, keys, headphone reports. */
        {
            int req = ui_take_hs_volume();
            AVR.now_ms = now_ms();
            if (req >= 0) {
                avrcp_set_volume(req);
                g_hs_vol = req;
            } else if (avrcp_key_due(&AVR)) {
                avrcp_set_volume(AVR.volume);
            }
            if (AVR.changed) {
                AVR.changed = 0;
                g_hs_vol = AVR.volume;
            }
        }
        /* PS4-side level: the page's Boost (0..500 %, 100 = unchanged) with
         * a soft limiter above unity so loud passages are bent, not clipped.
         * Headphones that send their volume to us but do not apply it
         * themselves get it as extra attenuation. */
        {
            int vol = ui_volume();
            if (!AVR.sink_renders && AVR.remote_abs)
                vol = vol * AVR.volume / 127;
            if (got > 0)
                ui_eq_apply(pcm, got, hb_gain_q8(vol));
        }
        if (now_us() >= next_ui) {
            char cn[32];
            /* Name the live codec so the page's codec row can highlight it:
             * dual channel is SBC-XQ, a ceiling above plain SBC is SBC HQ. */
            const char *cname = st.mode == HB_SBC_DUAL ? "SBC-XQ" :
                                bp_max > SBC_MAX_BP ? "SBC HQ" : "SBC";
            snprintf(cn, sizeof cn, "%s", cname);
            ui_set_stream(cn, fs, st.bitpool, bp_max, pcm_udp_buffered() * 1000 / 48000,
                          G.acl_media_drops, resyncs);
            {
                float pk = 0.f, opk = 0.f;
                int i, n = got > 0 ? got * (nch > 0 ? nch : 2) : 0;
                for (i = 0; i < n; i++) {
                    float a = (float)pcm[i];
                    if (a < 0) a = -a;
                    a /= 32768.f;
                    if (a > pk) pk = a;
                }
                opk = pk;
                ui_set_levels(pkts, frames_sent, pk, opk,
                              pcm_udp_buffered(), fpp);
                ui_set_avrcp_flags(avrcp_avail() ? 1 : 0,
                                   AVR.remote_abs ? 1 : 0,
                                   (AVR.notify_label >= 0 || AVR.ct_registered) ? 1 : 0,
                                   AVR.sink_renders ? 1 : 0);
            }
            ui_set_hs_volume(avrcp_avail(), avrcp_avail() ? AVR.volume : -1);
            /* Only push a fresh reading. Never clear here: a brief HFP/AVRCP
             * gap (codec reconnect, device switch in progress) used to flash
             * "Not shown". ui_set_device clears when the peer address changes. */
            if (HFP.battery >= 0)
                ui_set_battery(-2, HFP.battery); /* -2 = HFP percent (no AVRCP level) */
            else if (avrcp_avail() && AVR.battery >= 0)
                ui_set_battery(AVR.battery, avrcp_battery_level(AVR.battery));
            next_ui = now_us() + 1000000ull;
        }
        /* If SBC is mono, downmix; if stereo/joint, pcm is already interleaved. */
        if (nch == 1) {
            for (i = 0; i < need; i++) {
                int l = pcm[i * 2], r = pcm[i * 2 + 1];
                pcm[i] = (int16_t)((l + r) / 2);
            }
        }

        for (i = 0; i < nf; i++) {
            const int16_t *frame_pcm = pcm + i * spf * nch;
            if (hb_sbc_encode_frame(&st, frame_pcm, pkt + len, sizeof pkt - (size_t)len) != flen) {
                say("FAIL: SBC encode error");
                G.poll_ms = POLL_MS;
                pcm_udp_close();
                ui_set_streaming(0);
                ui_set_hs_volume(0, -1);
                return 0;
            }
            len += flen;
        }
        pkt[0] = 0x80; pkt[1] = 0x60;
        pkt[2] = (unsigned char)(seq >> 8); pkt[3] = (unsigned char)seq;
        pkt[4] = (unsigned char)(ts >> 24); pkt[5] = (unsigned char)(ts >> 16);
        pkt[6] = (unsigned char)(ts >> 8);  pkt[7] = (unsigned char)ts;
        pkt[8] = 0; pkt[9] = 0; pkt[10] = 0; pkt[11] = 1;
        pkt[12] = (unsigned char)(nf & 0x0F);
        G.media_tx = 1;
        i = chan_send(m, pkt, len);
        G.media_tx = 0;
        if (!i) {
            out("stream: send failed after %ld frames", frames_sent);
            break;
        }
        seq = (seq + 1) & 0xFFFF;
        ts += (uint32_t)(nf * spf);
        frames_sent += nf;
        pkts++;

        if (now_us() >= next_log) {
            out("stream: %ld SBC frames, %ld pkts, buf=%d, silent=%ld, stalls=%d, "
                "media_drops=%d, resyncs=%d, credit_resync=%d, bitpool=%d, inflight=%d/%u, "
                "pcm_over=%d pcm_under=%d, drift=%+dppm (base %+d), fill=%d ms, catchup=%ld, "
                "udp_rx=%lu pkts (+%lu) port %d, tap=%s hello=%lu",
                frames_sent, pkts, pcm_udp_buffered(), silent_frames, G.acl_stalls,
                G.acl_media_drops, resyncs, G.acl_leak_resync, st.bitpool, G.acl_inflight,
                G.acl_pkts, pcm_udp_overflows(), pcm_udp_underruns(), pcm_udp_drift_ppm(),
                pcm_udp_drift_base_ppm(), pcm_udp_fill_avg() * 1000 / 48000, catchups,
                pcm_udp_rx_pkts(), pcm_udp_rx_pkts() - last_rx, pcm_udp_port(),
                pcm_udp_tap_title()[0] ? pcm_udp_tap_title() : "-", pcm_udp_hellos());
            /* The tap writes "<TITLE> <pkts>" to TAP_STATUS_FILE: if it keeps
             * sending while nothing arrives here, someone else holds the port. */
            {
                char line[64], title[16] = "";
                unsigned long tp = 0;
                FILE *tf = fopen(TAP_STATUS_FILE, "r");
                if (tf) {
                    if (fgets(line, sizeof line, tf)) sscanf(line, "%15s %lu", title, &tp);
                    fclose(tf);
                }
                if (pcm_udp_rx_pkts() == last_rx) no_pcm_s += 5; else no_pcm_s = 0;
                if (tp > last_tap_file + 50 && last_tap_file && no_pcm_s >= 10) {
                    if (!warned_no_pcm)
                        say("WARNING: game tap %s is sending (%lu pkts) but no PCM arrives on UDP %d "
                            "(another process holds the port?)", title, tp, pcm_udp_port());
                    warned_no_pcm = 1;
                } else if (pcm_udp_rx_pkts() != last_rx) {
                    warned_no_pcm = 0;
                }
                last_tap_file = tp;
                ui_set_pcm(pcm_udp_rx_pkts(), no_pcm_s, pcm_udp_tap_title()[0] ? pcm_udp_tap_title() : title,
                           warned_no_pcm);
            }
            last_rx = pcm_udp_rx_pkts();
            next_log = now_us() + 5000000ull;
        }
        pump_once();
    }

    G.poll_ms = POLL_MS;
    pcm_udp_close();
    ui_set_streaming(0);
    ui_set_hs_volume(0, -1);
    if (drop_is_away(g_end_reason) || drop_is_away(G.disc_reason)) {
        /* Drop the "still linked" UI right away (case / power-off). */
        ui_set_device(NULL, NULL);
        ui_set_battery(-1, -1);
    }
    say("Stream: %ld SBC frames in %ld packets, %.1f s, silent=%ld, stalls=%d%s",
        frames_sent, pkts, (double)(now_us() - t0) / 1e6, silent_frames, G.acl_stalls,
        stop ? " (stop)" : "");
    return stop ? 1 : 0;
}

/* DISCOVER, configure the headset's SBC sink, OPEN, media channel, START,
 * stream PCM, then SUSPEND/CLOSE. Peer-driven configuration is accepted too.
 * Returns 1 on clean stop, 0 on failure/link drop. */
static int run_stream(void)
{
    unsigned char rsp[256], body[16], seps[64], cfg[4];
    int len = 0, msg = -1, nsep, i, snk = 0, try;
    struct chan *m = NULL;
    long deadline;

    for (try = 0; try < 2 && msg < 0; try++) {
        if (try) out("avdtp: retrying DISCOVER");
        msg = avdtp_cmd(AV_DISCOVER, NULL, 0, rsp, (int)sizeof rsp, &len, AV_DISCOVER_MS);
        if (G.disc_done) break;
    }
    if (msg != AV_MSG_ACCEPT || len < 2) {
        say("FAIL: AVDTP discover (msg %d)", msg);
        out("AVDTP discover failed");
        if (!AV.configured) return 0;
        nsep = 0;
    } else {
        if (len > (int)sizeof seps) len = (int)sizeof seps;
        memcpy(seps, rsp, (size_t)len);
        nsep = len / 2;
        for (i = 0; i < nsep; i++) {
            int seid = (seps[i * 2] >> 2) & 0x3F, in_use = (seps[i * 2] >> 1) & 1;
            int media = (seps[i * 2 + 1] >> 4) & 0x0F, tsep = (seps[i * 2 + 1] >> 3) & 1;
            out("avdtp: SEP seid %d %s %s%s", seid, tsep ? "SNK" : "SRC",
                media == 0 ? "audio" : "other", in_use ? " (in use)" : "");
            if (!snk && tsep == 1 && media == 0 && !in_use) snk = seid;
        }
        say("AVDTP: %d endpoint(s), audio sink seid %d", nsep, snk);
    }

    /* Configure, unless the peer already configured our SEP. */
    if (!AV.configured) {
        unsigned char cap[4];
        if (!snk) { say("FAIL: no free audio sink endpoint"); return 0; }
        body[0] = (unsigned char)(snk << 2);
        msg = avdtp_cmd(AV_GET_CAPABILITIES, body, 1, rsp, (int)sizeof rsp, &len, 0);
        if (msg != AV_MSG_ACCEPT || !find_sbc_ie(rsp, len, cap)) {
            if (msg == AV_MSG_ACCEPT) hexline("avdtp: sink caps (no SBC)", rsp, len);
            if (!AV.configured) { say("FAIL: sink seid %d SBC capabilities", snk); return 0; }
        } else if (!AV.configured) {
            int avail;
            out("avdtp: sink SBC caps %02x %02x bitpool %u-%u", cap[0], cap[1], cap[2], cap[3]);
            /* UI codec chips: bit1=SBC, bit2=HQ, bit3=XQ (same as PS5). */
            avail = 1 << 1;
            if (cap[3] > 53 && (cap[0] & 0x03)) avail |= 1 << 2;
            if ((cap[0] & 0x04) && cap[3] >= 35) avail |= 1 << 3;
            ui_set_codec_caps(avail);
            {
                int pref = ui_get_codec_pref();
                if (!choose_sbc_config(cap, cfg, pref)) { say("FAIL: no common SBC configuration"); return 0; }
                body[0] = (unsigned char)(snk << 2);
                body[1] = (unsigned char)(LOCAL_SEID << 2);
                body[2] = 0x01; body[3] = 0x00;
                body[4] = 0x07; body[5] = 0x06; body[6] = 0x00; body[7] = 0x00;
                memcpy(body + 8, cfg, 4);
                msg = avdtp_cmd(AV_SET_CONFIG, body, 12, rsp, (int)sizeof rsp, &len, 0);
                /* Headset advertised HQ/XQ but refused the config: fall back to plain SBC. */
                if (msg != AV_MSG_ACCEPT && !AV.configured && pref >= 2) {
                    out("avdtp: pref %d refused, retrying plain SBC", pref);
                    if (!choose_sbc_config(cap, cfg, 1)) { say("FAIL: no common SBC configuration"); return 0; }
                    memcpy(body + 8, cfg, 4);
                    msg = avdtp_cmd(AV_SET_CONFIG, body, 12, rsp, (int)sizeof rsp, &len, 0);
                }
                if (msg == AV_MSG_ACCEPT && !AV.configured) {
                    AV.configured = 1;
                    AV.remote_seid = snk;
                    memcpy(AV.sbc_ie, cfg, 4);
                } else if (!AV.configured) {
                    say("FAIL: SET_CONFIGURATION refused");
                    return 0;
                }
            }
        }
    }
    say("Stream configured%s: remote seid %d, SBC %02x %02x bitpool %u",
        AV.cfg_by_peer ? " by headset" : "", AV.remote_seid,
        AV.sbc_ie[0], AV.sbc_ie[1], AV.sbc_ie[3]);

    if (!AV.opened) {
        body[0] = (unsigned char)(AV.remote_seid << 2);
        msg = avdtp_cmd(AV_OPEN, body, 1, rsp, (int)sizeof rsp, &len, 0);
        if (msg == AV_MSG_ACCEPT) AV.opened = 1;
        else if (!AV.opened) { say("FAIL: OPEN refused"); return 0; }
    }

    /* Media transport: the OPEN initiator connects it. */
    if (AV.open_by_peer) {
        deadline = now_ms() + 3000;
        while (now_ms() < deadline && !G.disc_done && !(m = chan_find_role(ROLE_MEDIA, 1)))
            pump_once();
    }
    if (!m) m = chan_find_role(ROLE_MEDIA, 1);
    if (!m) m = l2_connect(PSM_AVDTP, ROLE_MEDIA);
    if (!m) { say("FAIL: media channel not opened"); return 0; }
    say("Media channel open (local 0x%04x remote 0x%04x mtu %u)", m->scid, m->dcid, m->peer_mtu);

    if (!AV.started) {
        body[0] = (unsigned char)(AV.remote_seid << 2);
        msg = avdtp_cmd(AV_START, body, 1, rsp, (int)sizeof rsp, &len, 0);
        if (msg == AV_MSG_ACCEPT) AV.started = 1;
        else if (!AV.started) { say("FAIL: START refused"); return 0; }
    }
    say("Streaming started%s", AV.start_by_peer ? " (by headset)" : "");

    i = stream_pcm(m);

    /* Disconnect pressed: the UI command marked the transport dead to unwind
     * the loops. The link itself is fine, so close the stream properly
     * (SUSPEND, CLOSE, short waits) before the ACL link is dropped. */
    if (G.user_abort && !EVT.dead) G.transport_dead = 0;
    if (AV.started && !G.disc_done && !G.transport_dead) {
        body[0] = (unsigned char)(AV.remote_seid << 2);
        if (avdtp_cmd(AV_SUSPEND, body, 1, rsp, (int)sizeof rsp, &len,
                      G.user_abort ? 250 : 0) == AV_MSG_ACCEPT)
            AV.started = 0;
    }
    if (AV.opened && !G.disc_done && !G.transport_dead) {
        body[0] = (unsigned char)(AV.remote_seid << 2);
        if (avdtp_cmd(AV_CLOSE, body, 1, rsp, (int)sizeof rsp, &len,
                      G.user_abort ? 250 : 0) == AV_MSG_ACCEPT)
            AV.opened = AV.configured = 0;
    }
    if (chan_live(m) && !G.user_abort) chan_disconnect(m);
    return i;
}

static void teardown(void)
{
    unsigned char p[4];
    static const int order[4] = { ROLE_AVCTP, ROLE_MEDIA, ROLE_AVSIG, ROLE_SDP };
    int k, i;

    /* A normal end closes every L2CAP channel and waits for the answers.
     * When the user pressed Disconnect the stream was already closed
     * (AVDTP), so go straight to the HCI Disconnect: it ends the channels
     * with the link and the headphones drop in well under a second. */
    for (k = 0; k < 4 && G.connected && !G.user_abort; k++)
        for (i = 0; i < MAX_CH; i++)
            if (G.ch[i].role == order[k] && chan_live(&G.ch[i]))
                chan_disconnect(&G.ch[i]);

    if (G.connected) {
        put16(p, G.handle);
        p[2] = 0x13; /* Remote User Terminated */
        G.disc_done = 0;
        hci_cmd_status(OP_DISCONNECT, p, 3);
        wait_flag(&G.disc_done, G.user_abort ? 1000 : 3000);
        if (!G.disc_done) out("teardown: no Disconnection Complete, the controller reset ends the link");
    }

    {
        unsigned char r[8];
        int n = 0;
        hci_cmd_cc(OP_RESET, NULL, 0, r, sizeof r, &n);   /* also drops any link left */
        G.page_scan = 0;
    }
}

/* ---- main / daemon ------------------------------------------------------- */

static void hb_crumb(const char *s)
{
    int fd = open("/data/hearbridge/crumb.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) fd = open("/user/data/hearbridge/crumb.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) fd = open("/data/crumb.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) { write(fd, s, strlen(s)); write(fd, "\n", 1); fsync(fd); close(fd); }
}

static void ensure_log_dir(void)
{
    mkdir("/data/hearbridge", 0777);
    mkdir("/user/data/hearbridge", 0777);
    mkdir("/data/GoldHEN/hearbridge", 0777); /* per-title game tap logs */
}


static struct dev *dev_slot(const unsigned char a[6], const char *name, uint32_t cod)
{
    struct dev *d = dev_find(a);
    int i;
    if (!d) {
        if (G.ndev >= MAX_DEV) return NULL;
        d = &G.dev[G.ndev++];
        memset(d, 0, sizeof *d);
        memcpy(d->addr, a, 6);
        d->psrm = 0x01;
        d->rssi = 127;
        d->name_status = -1;
        d->cod = cod ? cod : 0x240404;
        /* Reuse page parameters from the last scan, if we saw it there. */
        for (i = 0; i < g_nscan; i++)
            if (memcmp(g_scan_devs[i].addr, a, 6) == 0) {
                d->psrm = g_scan_devs[i].psrm ? g_scan_devs[i].psrm : 0x01;
                d->clk = g_scan_devs[i].clk;
                if (!d->name[0]) snprintf(d->name, sizeof d->name, "%s", g_scan_devs[i].name);
                if (!cod) d->cod = g_scan_devs[i].cod;
            }
    }
    if (name && name[0] && !d->name[0]) snprintf(d->name, sizeof d->name, "%s", name);
    return d;
}

/* Target to page: the UI pick, else the override file, else the last used
 * saved device. NULL when there is none (then the UI decides). */
static struct dev *direct_target(void)
{
    unsigned char a[6], k[16];
    int kt = 0;
    char astr[18];
    struct dev *d = NULL;
    struct saved_dev sd;
    const char *why = "";

    if (g_want) {
        struct saved_dev all[DEVICES_MAX];
        int n = devices_load(all), i;
        const char *nm = NULL;
        uint32_t cod = 0;
        for (i = 0; i < n; i++)
            if (memcmp(all[i].addr, g_want_addr, 6) == 0) { nm = all[i].name; cod = all[i].cod; }
        d = dev_slot(g_want_addr, nm, cod);
        why = "picked in the UI";
    } else if (load_target_override(a)) {
        d = dev_slot(a, NULL, 0);
        why = "override file";
    } else if (devices_last_audio(&sd)) {
        d = dev_slot(sd.addr, sd.name, sd.cod);
        why = "last used";
    }
    if (!d) return NULL;
    addr_str(d->addr, astr);
    out("daemon: paging %s %s directly (%s)", why, astr,
        linkkeys_load(d->addr, k, &kt) ? "saved key" : "no saved key");
    if (!d->name[0]) ask_remote_name(d, 2500);
    return d;
}

/* Headphones first, then speakers, other audio, TV-like last; strongest
 * signal first within a group, unknown signal last. */
static int scan_before(const struct dev *a, const struct dev *b)
{
    int ra = ui_cod_rank(a->cod), rb = ui_cod_rank(b->cod);
    int sa = dev_rssi_avg(a), sb = dev_rssi_avg(b);
    if (ra != rb) return ra < rb;
    if (sa == 127) sa = -200;
    if (sb == 127) sb = -200;
    return sa > sb;
}

/* Sorted candidate list into g_scan_devs and the UI. Cheap: called on
 * every inquiry result and after every name. */
static void ui_scan_publish(void)
{
    struct ui_scan_dev out_[UI_SCAN_MAX];
    int i, j, n = 0;
    g_nscan = 0;
    for (i = 0; i < G.ndev && g_nscan < UI_SCAN_MAX; i++)
        if (addr_allowed(G.dev[i].addr, G.dev[i].cod)) g_scan_devs[g_nscan++] = G.dev[i];
    for (i = 0; i < g_nscan; i++)
        for (j = i + 1; j < g_nscan; j++)
            if (scan_before(&g_scan_devs[j], &g_scan_devs[i])) {
                struct dev t = g_scan_devs[i]; g_scan_devs[i] = g_scan_devs[j]; g_scan_devs[j] = t;
            }
    for (i = 0; i < g_nscan; i++) {
        memcpy(out_[n].addr, g_scan_devs[i].addr, 6);
        out_[n].cod = g_scan_devs[i].cod;
        out_[n].rssi = dev_rssi_avg(&g_scan_devs[i]);
        snprintf(out_[n].name, sizeof out_[n].name, "%s", g_scan_devs[i].name);
        n++;
    }
    ui_set_scan_results(out_, n);
}

/* Inquiry for the UI: every audio sink candidate, shown live while found,
 * names asked best candidates first (TVs / set-top boxes are not asked). */
static void ui_scan(void)
{
    int i, k, n;

    G.scan_ui = 1;
    G.scan_abort = 0;
    ui_set_scanning(1);
    ui_set_scan_results(NULL, 0);
    run_inquiry();
    ui_scan_publish();
    ui_set_scanning(2);
    n = g_nscan;
    for (k = 0; k < n && !G.scan_abort && !G.transport_dead; k++) {
        struct dev *d = NULL;
        ui_scan_publish();                         /* order may change with names */
        for (i = 0; i < g_nscan; i++) {
            struct dev *x = dev_find(g_scan_devs[i].addr);
            if (x && !x->name[0] && !x->name_tried && ui_cod_rank(x->cod) < 3) { d = x; break; }
        }
        if (!d) break;
        d->name_tried = 1;
        ask_remote_name(d, 2000);
        ui_scan_publish();
    }
    ui_scan_publish();
    ui_set_scanning(0);
    G.scan_ui = 0;
    if (G.scan_abort) say("Scan stopped: %d device(s) so far", g_nscan);
    else say("Scan finished: %d device(s) nearby", g_nscan);
}

/* Make the controller connectable so a saved headset can call us back. */
static void enable_page_scan(void)
{
    unsigned char p[1] = { 0x02 /* page scan only, not discoverable */ };
    unsigned char r[8];
    int n = 0;
    if (G.page_scan) return;
    if (hci_cmd_cc(OP_WRITE_SCAN_ENABLE, p, 1, r, sizeof r, &n) && n >= 1 && r[0] == 0) {
        G.page_scan = 1;
        out("page scan on: a saved headset can reconnect by itself");
    } else {
        out("Write_Scan_Enable failed (paging only)");
    }
}

/* The headset started the link: take over its identity for the session. */
static void adopt_inbound(void)
{
    struct saved_dev all[DEVICES_MAX];
    int n = devices_load(all), i;
    const char *nm = NULL;
    uint32_t cod = G.in_cod;
    struct dev *d;
    char astr[18];
    for (i = 0; i < n; i++)
        if (memcmp(all[i].addr, G.peer, 6) == 0) { nm = all[i].name; if (!cod) cod = all[i].cod; }
    d = dev_slot(G.peer, nm, cod);
    snprintf(g_cur_name, sizeof g_cur_name, "%s", d ? d->name : "");
    g_cur_cod = cod ? cod : 0x240404;
    ui_set_device(G.peer, g_cur_name[0] ? g_cur_name : NULL);
    addr_str(G.peer, astr);
    say("%s connected by itself (%s)", g_cur_name[0] ? g_cur_name : astr, astr);
}

/* Quiet phase: nothing is paged. Page scan stays on and the USB session stays
 * open, so a saved headset that wakes up (case opened, power on) connects by
 * itself. Ends with a link, a command from the page, a stop or a dead USB
 * transport. */
static void quiet_listen(void)
{
    enable_page_scan();
    G.accept_in = 1;
    /* Same as PS5 idle: not "listening" (that read as still linked). Page scan
     * stays on so a saved headset can call when it leaves the case. */
    set_state("disconnected", NULL, 0);
    out("daemon: page window used up, quiet now: disconnected, page scan on, no more paging "
        "until Reconnect, a scan or a new device is picked");
    while (!G.connected && !G.transport_dead && !G.user_abort && !G.stop_requested && !g_have_pending)
        pump_once();
    if (!G.connected && G.conn_pending && !G.transport_dead)
        wait_flag(&G.conn_done, 3000);          /* an accepted call is still completing */
}

/* Connect to a device with a limited number of pages (g_pager): page
 * it, listen briefly for its call, page again, and when the budget is used
 * up only listen (quiet_listen). The USB link stays open the whole time.
 * Returns 1 when linked (by us or by the headset). */
static int connect_loop(struct dev *pick)
{
    int attempt = 0;

    for (;;) {
        long wait, end;
        if (!(g_scan_req || G.user_abort)) enable_page_scan();
        G.accept_in = 1;
        if (hb_pager_take(&g_pager)) {
            out("daemon: page, %d more after this one", g_pager.budget);
            if (connect_device(pick)) {
                G.accept_in = 0;
                if (G.inbound) adopt_inbound();
                return 1;
            }
            if (!g_soft_fail || G.user_abort || G.transport_dead || G.stop_requested ||
                g_have_pending || G.need_pairing)
                return 0;
            if (g_pager.budget > 0) {
                wait = hb_pager_gap_ms(attempt);
                attempt++;
                set_state("reconnect_wait", NULL, 0);
                out("daemon: nobody answered, listening %ld ms for a call before paging again", wait);
                end = now_ms() + wait;
                while (now_ms() < end && !G.connected && !G.transport_dead && !G.user_abort &&
                       !G.stop_requested && !g_have_pending)
                    pump_once();
                if (!G.connected && G.conn_pending)
                    wait_flag(&G.conn_done, 3000);
            }
        } else {
            quiet_listen();
        }
        if (G.connected) {
            G.accept_in = 0;
            link_up_setup();
            adopt_inbound();
            return 1;
        }
        if (G.transport_dead || G.user_abort || G.stop_requested || g_have_pending) return 0;
    }
}

/* One connect + stream session. Returns SES_STOP, SES_NEED_PAIRING or
 * SES_RETRY. */
static int session(void)
{
    usb_dev **list = NULL, *target = NULL, *first_bt = NULL;
    int count, i, rc, detached = 0, claimed = 0, stopped = 0, idle = 0, torn = 0, repeat;
    unsigned tvid = 0, tpid = 0, fvid = 0, fpid = 0;
    char seen[96];
    char usb_n[96];
    char want_id[16];
    struct ui_adapter ads[HB_AD_MAX];
    int n_ad = 0, sel;
    struct dev *pick;
    struct saved_dev last_after_scan;

    seen[0] = 0;
    usb_n[0] = 0;
    want_id[0] = 0;
    sel = hb_adapter_selection(want_id, (int)sizeof want_id);
    memset(&G, 0, sizeof G);
    memset(&AV, 0, sizeof AV);
    avrcp_init(&AVR, g_hs_vol >= 0 ? g_hs_vol : 127);
    hfp_init(&HFP, hfp_tx, NULL, hfp_say);
    g_hfp_scid = 0;
    AVR.seek_vol = 0;           /* next / previous stay track keys */
    ui_set_hs_volume(0, -1);
    G.next_cid = 0x0040;
    G.poll_ms = POLL_MS;
    g_session_streamed = 0;
    g_end_reason = -1;

    if (!load_usbd()) goto finish;

    repeat = g_no_adapter > 0;      /* waiting for a dongle: log the first try only */
    rc = U.Init();
    if (!repeat) out("sceUsbdInit -> %d", rc);
    if (rc < 0) { say("FAIL: sceUsbdInit error 0x%08x", (unsigned)rc); goto finish; }

    count = U.GetDeviceList(&list);
    if (count < 0 || !list) {
        say("FAIL: USB device list error 0x%08x", (unsigned)count);
        goto finish;
    }
    if (!repeat) out("USB devices: %d", count);
    for (i = 0; i < count; i++) {
        struct usb_dev_desc d;
        memset(&d, 0, sizeof d);
        rc = U.GetDeviceDescriptor(list[i], &d);
        if (rc >= 0) {
            if (!usb_n[0])
                snprintf(usb_n, sizeof usb_n, "%04X:%04X", d.idVendor, d.idProduct);
            else if (strlen(usb_n) < sizeof usb_n - 12) {
                char one[12];
                snprintf(one, sizeof one, ", %04X:%04X", d.idVendor, d.idProduct);
                strncat(usb_n, one, sizeof usb_n - strlen(usb_n) - 1);
            }
        }
        if (!repeat)
            out("  [%d] bus %d addr %d  %04X:%04X class %02x/%02x/%02x  (desc rc %d)%s", i,
                U.GetBusNumber ? U.GetBusNumber(list[i]) : -1,
                U.GetDeviceAddress ? U.GetDeviceAddress(list[i]) : -1,
                d.idVendor, d.idProduct, d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol, rc,
                (rc >= 0 && usb_stick_ok(&d)) ? "  [bt]" : "");
        if (rc >= 0 && usb_stick_ok(&d)) {
            char id[12], lab[40];
            hb_adapter_id_fmt(d.idVendor, d.idProduct, id, (int)sizeof id);
            btchip_describe((int)d.idVendor, (int)d.idProduct, lab, (int)sizeof lab);
            if (n_ad < HB_AD_MAX) {
                snprintf(ads[n_ad].id, sizeof ads[n_ad].id, "%s", id);
                snprintf(ads[n_ad].label, sizeof ads[n_ad].label, "%s", lab[0] ? lab : id);
                ads[n_ad].in_use = 0;
                n_ad++;
            }
            if (!seen[0])
                snprintf(seen, sizeof seen, "%04X:%04X", d.idVendor, d.idProduct);
            else if (strlen(seen) < sizeof seen - 12) {
                char one[12];
                snprintf(one, sizeof one, ", %04X:%04X", d.idVendor, d.idProduct);
                strncat(seen, one, sizeof seen - strlen(seen) - 1);
            }
            /* Preference: exact VID:PID, else first stick (auto/usb). */
            if (sel == HB_SEL_ID && want_id[0] && !strcmp(id, want_id)) {
                target = list[i];
                tvid = d.idVendor;
                tpid = d.idProduct;
            } else if (sel != HB_SEL_ID && !target) {
                target = list[i];
                tvid = d.idVendor;
                tpid = d.idProduct;
            }
            if (!first_bt) {
                first_bt = list[i];
                fvid = d.idVendor;
                fpid = d.idProduct;
            }
        }
    }
    /* Exact pref missing: fall back to first Bluetooth stick. */
    if (!target && first_bt) {
        target = first_bt;
        tvid = fvid;
        tpid = fpid;
        if (sel == HB_SEL_ID)
            out("daemon: preferred adapter %s not present, using %04X:%04X",
                want_id, tvid, tpid);
    }
    if (n_ad) {
        char cur[12];
        int k;
        hb_adapter_id_fmt(tvid, tpid, cur, (int)sizeof cur);
        for (k = 0; k < n_ad; k++)
            ads[k].in_use = !strcmp(ads[k].id, cur);
        ui_set_adapters(ads, n_ad, target ? cur : "");
    }

    if (!target) {
        if (!repeat) {
            if (usb_n[0])
                say("FAIL: no CSR8510 (0A12:0001) or Realtek RTL8761BU dongle (%d USB devices seen: %s)",
                    count, usb_n);
            else
                say("FAIL: no CSR8510 (0A12:0001) or Realtek RTL8761BU dongle (%d USB devices seen)",
                    count);
        }
        set_state("no_adapter",
                  "HearBridge: plug in a CSR8510 or Realtek RTL8761BU USB dongle", 1);
        g_no_adapter++;
        goto finish;
    }
    if (repeat) out("daemon: Bluetooth dongle %04X:%04X is there again", tvid, tpid);
    g_no_adapter = 0;
    {
        char label[64];
        btchip_describe((int)tvid, (int)tpid, label, (int)sizeof label);
        say("Found adapter %s", label[0] ? label : "USB HCI");
        if (seen[0] && strchr(seen, ','))
            out("daemon: other Bluetooth USB candidates: %s (using %04X:%04X)", seen, tvid, tpid);
    }
    set_state("init", NULL, 0);

    rc = U.Open(target, &H);
    if (rc < 0 || !H) {
        say("FAIL: open failed, error %d (%s)", rc, usb_err(rc));
        H = NULL;
        goto finish;
    }
    out("opened");

    if (U.KernelDriverActive)
        out("kernel driver active on interface 0: %d", U.KernelDriverActive(H, 0));

    rc = U.ClaimInterface(H, 0);
    if (rc < 0 && U.DetachKernelDriver) {
        out("claim interface 0 failed: %d (%s), trying to detach system driver",
            rc, usb_err(rc));
        if (U.DetachKernelDriver(H, 0) == 0) {
            detached = 1;
            rc = U.ClaimInterface(H, 0);
        }
    }
    if (rc < 0) {
        say("FAIL: claim interface 0 error %d (%s)", rc, usb_err(rc));
        if (rc == LIBUSB_ERROR_BUSY)
            say("The system Bluetooth service may already hold the adapter");
        goto finish;
    }
    claimed = 1;
    out("interface 0 claimed%s", detached ? " (after detaching system driver)" : "");

    find_endpoints(H);
    evt_start();

    if (!setup_controller()) goto release;

    /* UI scan, or page a known target directly (never during an inquiry).
     * With no target at all, scan once and wait for the user to pick. */
    if (g_scan_req) {
        g_scan_req = 0;
        ui_scan();
        /* A scan pages nothing afterwards (the user is probably pairing something
         * else): the daemon only listens for a saved headset, and the found list
         * stays on the page. Try now pages the last used headphones again. */
        if (g_user_disc || g_have_pending || G.user_abort || !devices_last_audio(&last_after_scan)) {
            set_state("choose_device", NULL, 0);
            idle = 1;
            goto release;
        }
        hb_pager_quiet(&g_pager);
        out("daemon: scan finished, no paging (listening only)");
    }
    pick = direct_target();
    if (!pick) {
        static int first_scan_done;
        if (!first_scan_done) {
            first_scan_done = 1;
            ui_scan();
        }
        set_state("choose_device", "HearBridge: open the HearBridge page to pick headphones", 1);
        idle = 1;
        goto release;
    }
    if (!connect_loop(pick)) goto release;

    if (!run_auth()) goto release;
    if (open_avdtp()) {
        stopped = run_stream();
    }

    if (G.user_abort && !EVT.dead) G.transport_dead = 0;
    pump_until(now_ms() + (G.user_abort ? 100 : 500));
    teardown();
    torn = 1;

release:
    if (G.user_abort && !EVT.dead) G.transport_dead = 0;
    if (!torn && (G.connected || G.conn_pending) && !G.transport_dead) teardown();
    if (!torn && G.page_scan && !G.transport_dead) {
        unsigned char off = 0, r[8];
        int n = 0;
        hci_cmd_cc(OP_WRITE_SCAN_ENABLE, &off, 1, r, sizeof r, &n);   /* nobody listens any more */
        G.page_scan = 0;
    }
    evt_stop();
    if (claimed) {
        U.ReleaseInterface(H, 0);
        out("interface 0 released");
    }
    if (detached && U.AttachKernelDriver) {
        rc = U.AttachKernelDriver(H, 0);
        out("reattach system driver: %d", rc);
    }

finish:
    if (H) { U.Close(H); H = NULL; }
    if (list && U.FreeDeviceList) U.FreeDeviceList(list);
    if (U.Exit) U.Exit();
    if (stopped || G.stop_requested) return SES_STOP;
    if (idle) return SES_IDLE;
    if (G.need_pairing) return SES_NEED_PAIRING;
    return SES_RETRY;
}

/* Apply a UI command between sessions. */
static void apply_cmd(const struct ui_cmd *c)
{
    char a[18];
    switch (c->type) {
    case UI_CMD_SCAN:
        g_scan_req = 1;
        g_idle = 0;
        say("Scanning for headphones and speakers");
        break;
    case UI_CMD_CONNECT:
        addr_str(c->addr, a);
        if (!connect_target_ok(c->addr)) {
            say("Not connecting to %s: it is not headphones or a speaker", a);
            break;
        }
        memcpy(g_want_addr, c->addr, 6);
        g_want = 1;
        g_idle = 0;
        hb_pager_trigger(&g_pager);     /* a fresh short window for the device picked */
        user_disc_set(0);
        say("Connecting to %s (picked in the UI)", a);
        break;
    case UI_CMD_DISCONNECT:
        g_idle = 1;
        g_want = 0;
        g_scan_req = 0;
        user_disc_set(1);
        ui_set_device(NULL, NULL);
        say("Disconnected. Pick a device or press Reconnect.");
        break;
    case UI_CMD_FORGET:
        forget_device(c->addr);
        if (g_want && memcmp(g_want_addr, c->addr, 6) == 0) g_want = 0;
        addr_str(c->addr, a);
        say("Forgot %s", a);
        if (memcmp(c->addr, G.peer, 6) == 0) {
            g_idle = 1;
            ui_set_device(NULL, NULL);
        }
        break;
    case UI_CMD_STOP:
        g_ui_stop = 1;
        say("Stop requested from the UI");
        break;
    case UI_CMD_REOPEN: {
        char pref[24];
        hb_adapter_pref_word(pref, (int)sizeof pref);
        g_idle = 0;
        g_want = 0;
        g_scan_req = 0;
        say("Switching Bluetooth adapter (%s)", pref);
        break;
    }
    default:
        break;
    }
}

/* Sleep between sessions while still flushing queued notifications.
 * ms < 0 waits until a UI command (or a quit request) arrives. */
static void daemon_wait(long ms)
{
    long end = now_ms() + ms;
    while ((ms < 0 || now_ms() < end) && !g_quit) {
        notify_tick();
        quit_check();
        if (!g_have_pending && ui_take_cmd(&g_pending)) g_have_pending = 1;
        if (g_have_pending) break;
        sceKernelUsleep(100000);
    }
}

/* GET path from 127.0.0.1:port; the reply (body included) goes to buf.
 * 1 if something answered. */
static int http_local(int port, const char *path, char *buf, int max)
{
    struct sockaddr_in a;
    struct timeval tv = { 2, 0 };
    char req[96];
    int fd, got = 0, r;
    if (max > 0) buf[0] = 0;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return 0;
    }
    r = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", path);
    if (write(fd, req, (size_t)r) != r) {
        close(fd);
        return 0;
    }
    while (got < max - 1 && (r = (int)read(fd, buf + got, (size_t)(max - 1 - got))) > 0) got += r;
    close(fd);
    if (max > 0) buf[got < 0 ? 0 : got] = 0;
    return got > 0;
}

/* Web UI: 8090, waiting up to ~10 s for an old instance to let go of it;
 * 8091 if it stays taken. */
static void start_ui(void)
{
    static const int ports[2] = { UI_PORT, UI_PORT_ALT };
    char ip[32];
    int k, tries, rc = 0;
    for (k = 0; k < 2; k++) {
        for (tries = 0; tries < (k ? 6 : 20); tries++) {
            rc = ui_start(PROBE_VERSION, ports[k]);
            if (rc == 0) break;
            sceKernelUsleep(500000);
        }
        out("ui: bind port %d: %s (errno %d, %d tries)", ports[k], rc == 0 ? "ok" : "failed", rc, tries + (rc == 0));
        { char cb[64]; snprintf(cb, sizeof cb, "UI:bind port %d rc=%d", ports[k], rc); hb_crumb(cb); }
        if (rc == 0) break;
        if (k == 0) {
            char st[256];
            if (http_local(UI_PORT, "/api/status", st, sizeof st))
                out("ui: port %d is held by: %.120s", UI_PORT, strstr(st, "{") ? strstr(st, "{") : st);
        }
    }
    if (rc != 0) {
        out("ui: could not open a web UI port");
        return;
    }
    ui_local_ip(ip, sizeof ip);
    { char cb[96]; snprintf(cb, sizeof cb, "UI:up http://%s:%d", ip, ui_port()); hb_crumb(cb); }
    out("ui: http://%s:%d", ip, ui_port());
    notify_now("HearBridge: open http://%s:%d", ip, ui_port());
}

/* Daemon entry used by the BinLoader ELF and by the GoldHEN plugin. */
int daemon_main(void)
{
    static const int backoff_ms[] = { 300, 500, 1000, 2000, 3000 };   /* no throttling: the dongle is ours */
    const int nbackoff = (int)(sizeof backoff_ms / sizeof backoff_ms[0]);
    int attempt = 0, fail_idx = 0, rc, pair_notified = 0;
    long pair_since = -1, wait_ms;

    hb_crumb("A:main-entry");
    ensure_log_dir();
    hb_crumb("B:logdir");
    g_log = fopen(LOG_PATH, "w");
    g_log_path = LOG_PATH;
    if (!g_log) {
        g_log = fopen(LOG_PATH_ALT, "w");
        g_log_path = LOG_PATH_ALT;
    }
    hb_crumb("C:log-open");
    out("HearBridge PS4 daemon %s", PROBE_VERSION);
    if (!g_log) out("warning: could not open %s for writing", LOG_PATH);
    hb_rtl_ensure_firmware();
    hb_crumb("D:rtl-fw");

    notify_now("HearBridge PS4 %s", PROBE_VERSION);
    snprintf(g_notif_last, sizeof g_notif_last, "HearBridge PS4 %s", PROBE_VERSION);
    g_notif_at = now_ms();
    g_notif_any = 1;
    /* Single instance: ask any running daemon to exit, wait for it. */
    {
        FILE *f;
        char st[32];
        long t_end, next_stop = 0;
        int seen_stopped = 0;
        snprintf(g_token, sizeof g_token, "hb-%llu-%d",
                 (unsigned long long)sceKernelGetProcessTime(), (int)getpid());
        f = fopen(QUIT_PATH, "w");
        if (!f) f = fopen(QUIT_PATH_ALT, "w");
        if (f) { fprintf(f, "%s\n", g_token); fclose(f); }
        {
            /* Also ask over HTTP (closes its web server too). */
            char rsp[256];
            int pk;
            for (pk = UI_PORT; pk <= UI_PORT_ALT; pk++)
                if (http_local(pk, "/api/quit", rsp, sizeof rsp))
                    out("daemon: asked instance on port %d to quit: %.80s", pk,
                        strstr(rsp, "{") ? strstr(rsp, "{") : "(no JSON)");
        }
        t_end = now_ms() + 6000;
        while (now_ms() < t_end) {
            if (now_ms() >= next_stop) {
                pcm_udp_send_stop();      /* a streaming instance exits on this */
                next_stop = now_ms() + 1000;
            }
            if ((read_small(STATE_PATH, st, sizeof st) == 0 ||
                 read_small(STATE_PATH_ALT, st, sizeof st) == 0) && strcmp(st, "stopped") == 0) {
                seen_stopped = 1;
                break;
            }
            sceKernelUsleep(200000);
        }
        out("daemon: takeover %s", seen_stopped ? "done (previous instance stopped)"
                                                : "wait ended (no running instance answered)");
        if (seen_stopped) sceKernelUsleep(500000);  /* let it release the dongle */
    }
    /* Saved devices: import the paired devices' link keys (oldest first so the
     * newest key ends up as "last used"). */
    {
        struct saved_dev tmp[DEVICES_MAX];
        if (devices_load(tmp) == 0) {
            unsigned char ka[LINKKEYS_MAX][6];
            int nk = linkkeys_list(ka, LINKKEYS_MAX), k;
            for (k = 0; k < nk; k++) devices_touch(ka[k], NULL, 0);
            if (nk) out("devices: imported %d paired device(s) from link keys", nk);
        }
    }
    hb_crumb("E:pre-start_ui");
    start_ui();
    hb_crumb("F:post-start_ui");
    set_state("starting", NULL, 0);
    if (access(USER_DISC_PATH, F_OK) == 0) {
        g_user_disc = 1;
        out("daemon: Disconnect was pressed earlier, not connecting until a device is picked");
    }

    for (;;) {
        if (g_have_pending) {
            g_no_adapter = 0;             /* a command: look again now and log it */
            apply_cmd(&g_pending);
            g_have_pending = 0;
            if (g_ui_stop) break;
            g_quiet = 0;
            fail_idx = 0;
        }
        if (g_idle || (g_user_disc && !g_want && !g_scan_req)) {
            set_state("idle", NULL, 0);
            daemon_wait(-1);
            if (g_quit) break;
            continue;
        }
        attempt++;
        if (g_no_adapter < 2) out("daemon: session attempt %d", attempt);
        g_summary[0] = '\0';
        rc = session();
        if (rc == SES_STOP || G.stop_requested || g_quit || g_ui_stop) break;
        if (g_have_pending) continue;     /* a UI command ended the session */
        if (rc == SES_IDLE) {
            daemon_wait(-1);              /* wait for the user to pick */
            if (g_quit) break;
            continue;
        }

        /* Every later session is a retry: intermediate states stay silent. */
        g_quiet = 1;
        if (G.authed) {
            /* Pairing worked (or the key was accepted): leave pairing wait. */
            pair_since = -1;
            pair_notified = 0;
        }
        if (g_session_streamed) {
            fail_idx = 0;
            hb_pager_after_stream(&g_pager, g_end_reason);
            if (drop_is_away(g_end_reason)) {
                /* Case / power-off: leave the UI disconnected (not "listening"). */
                ui_set_device(NULL, NULL);
                ui_set_battery(-1, -1);
                out("daemon: stream ended away (0x%02x): disconnected until the headphones call "
                    "or Connect / Try now", g_end_reason & 0xff);
            } else {
                out("daemon: stream ended (reason %d): no paging, waiting until the headphones call "
                    "or Connect / Try now", g_end_reason);
            }
        }

        if (rc == SES_NEED_PAIRING) {
            if (pair_since < 0) pair_since = now_ms();
            if (now_ms() - pair_since < PAIR_WINDOW_MS) {
                set_state("waiting_pairing",
                          pair_notified ? NULL : "HearBridge: put headphones in pairing mode", 1);
                pair_notified = 1;
                wait_ms = PAIR_RETRY_MS;
                hb_pager_trigger(&g_pager);   /* each pairing try pages the device being paired */
                out("daemon: waiting %ld ms for pairing mode (%ld s into window)",
                    wait_ms, (now_ms() - pair_since) / 1000);
            } else {
                /* The window is over: stop paging until the user tries again. */
                set_state("pairing_timeout", NULL, 0);
                out("daemon: pairing window over, waiting for the user");
                pair_since = -1;
                pair_notified = 0;
                daemon_wait(-1);
                if (g_quit) break;
                continue;
            }
        } else {
            wait_ms = (long)backoff_ms[fail_idx < nbackoff ? fail_idx : nbackoff - 1];
            if (fail_idx < nbackoff) fail_idx++;
            if (g_no_adapter) {
                /* No dongle: look again every few seconds at first, then every
                 * 30 s. A command from the page ends the wait at once. */
                wait_ms = g_no_adapter < 4 ? 3000 : g_no_adapter < 8 ? 10000 : 30000;
            } else if (g_session_streamed) {
                if (drop_is_away(g_end_reason))
                    set_state("disconnected", "HearBridge: headphones turned off", 1);
                else
                    set_state("disconnected", "HearBridge: headphones disconnected", 1);
            } else {
                set_state("reconnect_wait", NULL, 1);
            }
            if (g_no_adapter < 2) out("daemon: waiting %ld ms before reconnect", wait_ms);
        }
        daemon_wait(wait_ms);
    }

    set_state("stopped", NULL, 0);
    /* Close every socket so nothing of this instance stays behind. */
    pcm_udp_close();
    ui_stop();
    out("ui: web server closed");
    /* Shown right away; the stop is the last thing this payload does. */
    notify_now("HearBridge PS4 %s stopped", PROBE_VERSION);
    out("done");
    if (g_log) fclose(g_log);
    return 0;
}

#if !defined(HEARBRIDGE_PLUGIN)
int main(void)
{
    return daemon_main();
}
#endif
