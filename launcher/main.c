/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HearBridge PS4 launcher (home screen tile).
 *
 * The audio work is done by the daemon payload; this app only makes sure it
 * runs and shows its web UI:
 *   1. asks /api/status on 127.0.0.1:6767 and :6768 whether a daemon with
 *      the web UI is running (state not "stopped"), and which version;
 *   2. if none answers, or it is older than the daemon bundled in this
 *      package, sends /app0/hearbridge_ps4_payload.elf to the GoldHEN
 *      BinLoader on 127.0.0.1:9090 (the new daemon takes over any running
 *      instance by itself) and waits for the UI to answer;
 *   3. shows a message with the page address (http://<console>:6767 for
 *      a phone or PC) and finishes, no browser is opened. Only when
 *      /data/hearbridge/open_browser exists it opens the PS4 Browser.
 * Before that it installs the game audio plugin (install.c): copies
 * /app0/hearbridge_game_tap.prx to /data/GoldHEN/plugins/, adds it under
 * [default] in plugins.ini and turns on PluginLoader_Enabled in config.ini
 * (originals kept as .bak once).
 * Steps are logged to /data/hearbridge/launcher.log. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Pad.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

#include "addr.h"
#include "install.h"
#include "../src/daemon_main.h"

static void lg(const char *fmt, ...);

/* In-process daemon when BinLoader accepts the ELF but never runs it (e.g. 14.02). */
static OrbisPthread g_daemon_thr;
static volatile int g_daemon_inplace;

static void *daemon_thr(void *arg)
{
    (void)arg;
    lg("inplace: daemon_main start");
    (void)daemon_main();
    lg("inplace: daemon_main returned");
    g_daemon_inplace = 0;
    return NULL;
}

static int start_daemon_inplace(void)
{
    int rc;
    if (g_daemon_inplace)
        return 0;
    g_daemon_inplace = 1;
    rc = scePthreadCreate(&g_daemon_thr, 0, daemon_thr, 0, "hb_daemon");
    if (rc != 0) {
        g_daemon_inplace = 0;
        lg("inplace: scePthreadCreate failed 0x%08x", (unsigned)rc);
        return -1;
    }
    lg("inplace: daemon thread created");
    return 0;
}

/* GoldHEN syscall 500: load a PRX into SceShellUI so the daemon survives
 * when this launcher tile is closed (BinLoader is broken on some firmwares).
 * SceShellUI is jailed: /data is not visible there, so we also copy the PRX
 * into its sandbox and load via the in-jail path (/system_tmp/... etc.). */
#define GOLDHEN_SDK_CMD_PROCESS_PRX_LOAD 6
#define DAEMON_PRX_NAME "hearbridge_daemon.prx"
#define DAEMON_PRX_APP0 "/app0/hearbridge_daemon.prx"
#define DAEMON_PRX_GOLDHEN "/data/GoldHEN/plugins/hearbridge_daemon.prx"
#define SHELLUI_NAME "SceShellUI"
#define SHELLUI_TITLE "NPXS20001"
#define SANDBOX_ROOT "/mnt/sandbox"
#define LOCK_PATH "/data/hearbridge/daemon.lock"
#define LOCK_PATH_ALT "/user/data/hearbridge/daemon.lock"

struct proc_prx_load {
    char process_name[32];
    char prx_path[100];
    uint64_t res;
} __attribute__((packed));

static uint64_t goldhen_raw(uint64_t cmd, void *data, int *carry)
{
    register uint64_t rax __asm__("rax") = 500;
    register uint64_t rdi __asm__("rdi") = cmd;
    register uint64_t rsi __asm__("rsi") = (uint64_t)(uintptr_t)data;
    uint8_t cf;
    __asm__ volatile("syscall\n\tsetc %3"
                     : "+r"(rax), "+r"(rdi), "+r"(rsi), "=r"(cf)
                     :
                     : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    if (carry) *carry = cf;
    return rax;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[8192];
    size_t n;
    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out);
            return -1;
        }
    }
    fclose(in);
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    return 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int prx_path_unreachable(int64_t res)
{
    uint32_t r = (uint32_t)res;
    return r == 0x80020002u || r == 0x80020001u ||
           r == 0x8002000du || r == 0x80020014u;
}

static int find_shellui_sandbox(char *out, size_t out_sz)
{
    DIR *d;
    struct dirent *e;
    char best[64] = "";
    static const char *const known[] = {
        SANDBOX_ROOT "/" SHELLUI_TITLE "_000",
        SANDBOX_ROOT "/" SHELLUI_TITLE "_001",
        NULL
    };
    int i;

    /* HearBridge's own jail often cannot opendir(/mnt/sandbox); still try
     * the well-known ShellUI roots (FTP can see them from outside). */
    for (i = 0; known[i]; i++) {
        if (is_dir(known[i])) {
            lg("inject: sandbox known %s", known[i]);
            snprintf(out, out_sz, "%s", known[i]);
            return 0;
        }
        lg("inject: known missing %s", known[i]);
    }

    d = opendir(SANDBOX_ROOT);
    if (!d) {
        lg("inject: cannot open %s errno=%d", SANDBOX_ROOT, errno);
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, SHELLUI_TITLE "_", sizeof(SHELLUI_TITLE)) != 0)
            continue;
        lg("inject: sandbox found %s/%s", SANDBOX_ROOT, e->d_name);
        if (best[0] == '\0' || strcmp(e->d_name, SHELLUI_TITLE "_000") == 0)
            snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    if (best[0] == '\0')
        return -1;
    snprintf(out, out_sz, "%s/%s", SANDBOX_ROOT, best);
    return 0;
}

static int64_t load_into_shellui(const char *path)
{
    struct proc_prx_load args;
    int cf = 0;
    uint64_t rax;

    memset(&args, 0, sizeof args);
    strncpy(args.process_name, SHELLUI_NAME, sizeof args.process_name - 1);
    strncpy(args.prx_path, path, sizeof args.prx_path - 1);
    rax = goldhen_raw(GOLDHEN_SDK_CMD_PROCESS_PRX_LOAD, &args, &cf);
    lg("inject: PROCESS_PRX_LOAD path=%s rax=0x%llx carry=%d res=0x%llx",
       path, (unsigned long long)rax, cf, (unsigned long long)args.res);
    return (int64_t)args.res;
}

/* 0 = loaded into ShellUI, -1 = failed. Tries GoldHEN plugins dir, then
 * paths inside the ShellUI jail (same strategy as inject_common.h). */
static int inject_daemon_shellui(void)
{
    static const char *const sbx_sub[] = { "/system_tmp", "/tmp", "" };
    char sbx[96];
    char real[192], view[192];
    int64_t res;
    unsigned i;

    /* Stale lock from a killed in-app daemon would make the plugin skip. */
    unlink(LOCK_PATH);
    unlink(LOCK_PATH_ALT);

    /* Prefer fopen over access(): on PS4 access(R_OK) can fail for /app0
     * files that fopen can still read (seen with hearbridge_daemon.prx). */
    {
        FILE *probe = fopen(DAEMON_PRX_APP0, "rb");
        if (!probe) {
            lg("inject: no %s (fopen failed errno=%d)", DAEMON_PRX_APP0, errno);
            return -1;
        }
        fclose(probe);
        lg("inject: found %s", DAEMON_PRX_APP0);
    }

    mkdir("/data/GoldHEN", 0777);
    mkdir("/data/GoldHEN/plugins", 0777);
    if (copy_file(DAEMON_PRX_APP0, DAEMON_PRX_GOLDHEN) == 0) {
        lg("inject: try GoldHEN path %s", DAEMON_PRX_GOLDHEN);
        res = load_into_shellui(DAEMON_PRX_GOLDHEN);
        if (res >= 0)
            return 0;
        if (!prx_path_unreachable(res)) {
            lg("inject: GoldHEN path load failed hard");
            return -1;
        }
    } else {
        lg("inject: cannot write %s", DAEMON_PRX_GOLDHEN);
        res = (int64_t)(int32_t)0x80020002u;
    }

    /* Even if we cannot write into ShellUI's jail from this app, try loading
     * well-known in-jail paths (file may have been planted via FTP/install). */
    {
        static const char *const views[] = {
            "/system_tmp/" DAEMON_PRX_NAME,
            "/tmp/" DAEMON_PRX_NAME,
            "/" DAEMON_PRX_NAME,
            NULL
        };
        int vi;
        for (vi = 0; views[vi]; vi++) {
            lg("inject: try in-jail path %s", views[vi]);
            res = load_into_shellui(views[vi]);
            if (res >= 0)
                return 0;
            if (!prx_path_unreachable(res))
                return -1;
        }
    }

    if (find_shellui_sandbox(sbx, sizeof sbx) != 0) {
        lg("inject: no ShellUI sandbox under %s", SANDBOX_ROOT);
        return -1;
    }
    for (i = 0; i < sizeof sbx_sub / sizeof sbx_sub[0]; i++) {
        char dir[160];
        snprintf(dir, sizeof dir, "%s%s", sbx, sbx_sub[i]);
        if (!is_dir(dir)) {
            lg("inject: skip missing %s", dir);
            continue;
        }
        snprintf(real, sizeof real, "%s/%s", dir, DAEMON_PRX_NAME);
        if (sbx_sub[i][0] == '\0')
            snprintf(view, sizeof view, "/%s", DAEMON_PRX_NAME);
        else
            snprintf(view, sizeof view, "%s/%s", sbx_sub[i], DAEMON_PRX_NAME);
        if (copy_file(DAEMON_PRX_APP0, real) != 0) {
            lg("inject: write %s failed errno=%d", real, errno);
            continue;
        }
        lg("inject: try sandbox %s (ShellUI sees %s)", real, view);
        res = load_into_shellui(view);
        if (res >= 0)
            return 0;
        unlink(real);
        if (!prx_path_unreachable(res))
            return -1;
    }
    lg("inject: all ShellUI paths failed");
    return -1;
}


#ifndef LAUNCHER_VERSION
#define LAUNCHER_VERSION "1.1.0"
#endif
#ifndef DAEMON_VERSION
#define DAEMON_VERSION "0.0.0"
#endif
/* Build id of the bundled daemon (hash of its sources). A running daemon with
 * the same version but another id, or none, is replaced as well. */
#ifndef DAEMON_BUILD
#define DAEMON_BUILD "dev"
#endif

#define BUNDLED_ELF  "/app0/hearbridge_ps4_payload.elf"
#define DATA_DIR     "/data/hearbridge"
#define LOG_PATH     DATA_DIR "/launcher.log"
#define UI_PORT      8090
#define UI_PORT_ALT  8091   /* when 8090 stays taken */
#define LOADER_PORT  9090
#define START_WAIT_MS 8000    /* inject/inplace UI wait */
#define BINLOADER_WAIT_MS 3000 /* 14.02 accepts ELF but does not run it */

/* SceWebBrowserDialogParam (328 bytes); OpenOrbis has no types for it. */
typedef struct {
    OrbisCommonDialogBaseParam base;   /* 0 */
    uint64_t size;                     /* 48 */
    int32_t mode;                      /* 56: 1 default layout, 2 custom rect */
    int32_t user_id;                   /* 60 */
    const char *url;                   /* 64 */
    void *callback_init;               /* 72 */
    uint16_t width, height, pos_x, pos_y; /* 80 */
    uint32_t parts;                    /* 88 */
    uint16_t header_width, header_x, header_y, pad0; /* 92 */
    uint32_t control;                  /* 100 */
    void *ime_param;                   /* 104 */
    void *webview_param;               /* 112 */
    uint32_t animation;                /* 120 */
    uint8_t reserved[202];             /* 124 */
    uint16_t pad1;
} hb_web_dialog_param;
_Static_assert(sizeof(hb_web_dialog_param) == 328, "web dialog param size");

int sceWebBrowserDialogInitialize(void);
int sceWebBrowserDialogOpen(hb_web_dialog_param *param);
int sceWebBrowserDialogUpdateStatus(void);
int sceWebBrowserDialogClose(void);
int sceWebBrowserDialogTerminate(void);
/* OpenOrbis declares this one without parameters. */
int hb_launch_browser(const char *uri, void *param) __asm__("sceSystemServiceLaunchWebBrowser");

static void lg(const char *fmt, ...)
{
    FILE *f = fopen(LOG_PATH, "a");
    char ts[32];
    time_t now = time(NULL);
    va_list ap;
    if (!f) return;
    if (fseek(f, 0, SEEK_END) == 0 && ftell(f) > 256 * 1024) {   /* keep the log small: start over */
        fclose(f);
        if (!(f = fopen(LOG_PATH, "w"))) return;
    }
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static int user_id(void)
{
    int id = -1;
    if (sceUserServiceGetForegroundUser(&id) == 0 && id != -1 && id != 0xff) return id;
    if (sceUserServiceGetInitialUser(&id) == 0) return id;
    return -1;
}

static int tcp_open(int port, int timeout_s)
{
    struct sockaddr_in sa;
    struct timeval tv;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 1 if a daemon's web UI answers on port and is not stopped; its version
 * goes to ver ("" if none answers). A stopped instance whose web server
 * stayed open answers with state "stopped": not running. */
static char g_console_ip[24];     /* from the daemon's status reply ("ip") */

static int daemon_status_port(int port, char *ver, size_t n, char *build, size_t bn)
{
    static const char req[] = "GET /api/status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
    char buf[1024], *v;
    int fd, got = 0, r;
    ver[0] = 0;
    build[0] = 0;
    if ((fd = tcp_open(port, 3)) < 0) return 0;
    if (write(fd, req, sizeof req - 1) < 0) {
        close(fd);
        return 0;
    }
    while (got < (int)sizeof buf - 1 && (r = (int)read(fd, buf + got, sizeof buf - 1 - got)) > 0)
        got += r;
    close(fd);
    buf[got] = 0;
    if (got <= 0) return 0;
    if ((v = strstr(buf, "\"version\":\""))) {
        v += 11;
        snprintf(ver, n, "%.*s", (int)strcspn(v, "\""), v);
    }
    hb_json_str(buf, "build", build, bn);
    {
        char ip[24];
        if (hb_json_str(buf, "ip", ip, sizeof ip) && hb_ip_ok(ip)) snprintf(g_console_ip, sizeof g_console_ip, "%s", ip);
    }
    return strstr(buf, "\"state\":\"stopped\"") == NULL;
}

static int version_cmp(const char *a, const char *b);

/* Best running instance over both ports: 1 if one runs; its version and
 * port are returned. */
static int daemon_status(char *ver, size_t n, char *build, size_t bn, int *port)
{
    static const int ports[2] = { UI_PORT, UI_PORT_ALT };
    char v[32], b[24];
    int k, up = 0;
    ver[0] = 0;
    build[0] = 0;
    *port = 0;
    for (k = 0; k < 2; k++) {
        int r = daemon_status_port(ports[k], v, sizeof v, b, sizeof b);
        lg("port %d: %s %s %s", ports[k], r ? "running" : v[0] ? "stopped" : "no answer", v, b);
        /* the newest version wins; at the same version the one with the bundled build id */
        if (r && (!up || version_cmp(v, ver) > 0 ||
                  (version_cmp(v, ver) == 0 && strcmp(b, DAEMON_BUILD) == 0 && strcmp(build, DAEMON_BUILD) != 0))) {
            up = 1;
            snprintf(ver, n, "%s", v);
            snprintf(build, bn, "%s", b);
            *port = ports[k];
        }
    }
    return up;
}

/* 1 when the running daemon is the one bundled in this package. */
static int daemon_current(const char *ver, const char *build)
{
    return version_cmp(ver, DAEMON_VERSION) > 0 ||
           (version_cmp(ver, DAEMON_VERSION) == 0 && strcmp(build, DAEMON_BUILD) == 0);
}

/* >0 if version a is newer than b ("x.y.z"). */
static int version_cmp(const char *a, const char *b)
{
    int va[3] = {0, 0, 0}, vb[3] = {0, 0, 0}, i;
    sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (i = 0; i < 3; i++)
        if (va[i] != vb[i]) return va[i] - vb[i];
    return 0;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL, *nb;
    size_t cap = 0, n = 0, r;
    if (!f) return NULL;
    do {
        if (n == cap) {
            cap = cap ? cap * 2 : (size_t)1 << 20;
            if (!(nb = realloc(buf, cap))) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        r = fread(buf + n, 1, cap - n, f);
        n += r;
    } while (r > 0);
    fclose(f);
    *len = n;
    return buf;
}

/* 0 sent, -1 bundled ELF unreadable, -2 BinLoader not listening, -3 short write. */
static int send_daemon(void)
{
    size_t len = 0, off = 0;
    unsigned char *elf = slurp(BUNDLED_ELF, &len);
    int fd, rc = 0;
    ssize_t w;
    if (!elf || len < 64) {
        free(elf);
        lg("cannot read %s", BUNDLED_ELF);
        return -1;
    }
    if ((fd = tcp_open(LOADER_PORT, 5)) < 0) {
        free(elf);
        lg("BinLoader 127.0.0.1:%d not reachable", LOADER_PORT);
        return -2;
    }
    while (off < len) {
        w = write(fd, elf + off, len - off);
        if (w <= 0) {
            rc = -3;
            break;
        }
        off += (size_t)w;
    }
    close(fd);
    free(elf);
    lg("sent daemon %s (%zu of %zu bytes) to BinLoader", DAEMON_VERSION, off, len);
    return rc;
}

static void msg_param_init(OrbisMsgDialogParam *p)
{
    memset(p, 0, sizeof *p);
    p->baseParam.size = sizeof p->baseParam;
    p->baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)(uintptr_t)&p->baseParam);
    p->size = sizeof *p;
    p->userId = user_id();
}

static void message(const char *text)
{
    static OrbisMsgDialogParam p;
    static OrbisMsgDialogUserMessageParam u;
    msg_param_init(&p);
    memset(&u, 0, sizeof u);
    p.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    p.userMsgParam = &u;
    u.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    u.msg = text;
    lg("message: %s", text);
    sceMsgDialogInitialize();
    if (sceMsgDialogOpen(&p) < 0) return;
    while (sceMsgDialogUpdateStatus() == ORBIS_COMMON_DIALOG_STATUS_RUNNING) usleep(16000);
    sceMsgDialogTerminate();
}

/* The choice is read from the controller, not from the dialog:
 * sceMsgDialogGetResult gives no usable answer on the console, so Yes could
 * not be told from No. The message has only an OK button and the pad is
 * polled with scePadReadState: the enter button (X, or Circle when the console
 * is set the Asian way) opens the browser, the other one closes.
 * The pad reads nothing while the system message is up but works right after
 * it. So the message says 'First press OK', and the choice is read after it
 * for 45 s with the notification repeated every 8 s. */
static int g_pad = -1;

static void pad_start(void)
{
    scePadInit();
    g_pad = scePadOpen(user_id(), ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    lg("pad open (user %d): handle %d", user_id(), g_pad);
}

static uint32_t pad_buttons(void)
{
    static OrbisPadData d;
    if (g_pad < 0) return 0;
    memset(&d, 0, sizeof d);
    if (scePadReadState(g_pad, &d) != 0) return 0;
    return d.buttons;
}

/* Enter button of the console: X (cross) in the west, Circle in Asia. */
static uint32_t enter_mask(void)
{
    int32_t v = 1;
    if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN, &v) != 0) v = 1;
    lg("enter button assign: %d (%s)", (int)v, v == 0 ? "circle" : "cross");
    return v == 0 ? ORBIS_PAD_BUTTON_CIRCLE : ORBIS_PAD_BUTTON_CROSS;
}

static void notify(const char *text)
{
    static OrbisNotificationRequest q;
    memset(&q, 0, sizeof q);
    q.type = NotificationRequest;
    q.reqId = NotificationRequest;
    q.targetId = -1;
    snprintf(q.message, sizeof q.message, "%s", text);
    lg("notification: %s (rc 0x%08x)", text, (unsigned)sceKernelSendNotificationRequest(0, &q, sizeof q, 0));
}

/* 1 = open the browser, 0 = close. open_m / close_m are pad button masks. */
static int choose_open(const char *text, uint32_t open_m, uint32_t close_m, const char *open_l, const char *close_l)
{
    static OrbisMsgDialogParam p;
    static OrbisMsgDialogUserMessageParam u;
    int answer = -1, st, last = -1, n = 0, armed = 0;
    long t0 = now_ms(), t_end, until;
    uint32_t b, prev = 0;
    char note[160];

    msg_param_init(&p);
    memset(&u, 0, sizeof u);
    p.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    p.userMsgParam = &u;
    u.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    u.msg = text;
    lg("message (pad choice): %s", text);
    sceMsgDialogInitialize();
    if (sceMsgDialogOpen(&p) < 0) return 0;
    t_end = t0 + 600000;
    for (;;) {                        /* phase 1: the dialog is up, the pad may or may not be readable */
        st = (int)sceMsgDialogUpdateStatus();
        if (st != last) { lg("dialog status %d (0 none, 1 initialized, 2 running, 3 finished)", st); last = st; }
        if (st == ORBIS_COMMON_DIALOG_STATUS_FINISHED) break;
        if (st == ORBIS_COMMON_DIALOG_STATUS_NONE && ++n > 125) break;
        if (st != ORBIS_COMMON_DIALOG_STATUS_NONE) n = 0;
        if (now_ms() > t_end) break;
        b = pad_buttons();
        if (b == 0) armed = 1;        /* the press that started the app must not count */
        if (armed && (b & ~prev) && (b & (open_m | close_m))) {
            answer = (b & open_m) ? 1 : 0;
            lg("pad press 0x%08x during the message -> %s", (unsigned)b, answer ? "open browser" : "close");
            break;
        }
        prev = b;
        usleep(16000);
    }
    if (answer >= 0 && st != ORBIS_COMMON_DIALOG_STATUS_FINISHED) {
        sceMsgDialogClose();
        for (n = 0; n < 125 && (int)sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED; n++) usleep(16000);
    }
    sceMsgDialogTerminate();
    if (answer >= 0) return answer;

    /* phase 2: the message was closed with its own OK button and the pad was not
     * seen working behind it (or the press was missed): ask once more, with a
     * notification, and read the pad with nothing else on screen. */
    if (g_pad < 0) { lg("pad not available, closing"); return 0; }
    snprintf(note, sizeof note, "HearBridge: press %s to open the page in the PS4 web browser, %s to close.", open_l, close_l);
    until = now_ms() + 1000;          /* let go of the button that closed the message */
    while (now_ms() < until && pad_buttons() != 0) usleep(16000);
    prev = pad_buttons();
    t_end = now_ms() + 45000;
    until = 0;
    while (now_ms() < t_end) {
        if (now_ms() >= until) { notify(note); until = now_ms() + 8000; }   /* the notification fades, repeat it */
        b = pad_buttons();
        if ((b & ~prev) && (b & (open_m | close_m))) {
            answer = (b & open_m) ? 1 : 0;
            lg("pad press 0x%08x after the message -> %s", (unsigned)b, answer ? "open browser" : "close");
            return answer;
        }
        prev = b;
        usleep(16000);
    }
    lg("no button pressed, closing");
    return 0;
}

/* Progress dialog while the daemon starts. */
static int progress_open(const char *text)
{
    static OrbisMsgDialogParam p;
    static OrbisMsgDialogProgressBarParam b;
    msg_param_init(&p);
    memset(&b, 0, sizeof b);
    p.mode = ORBIS_MSG_DIALOG_MODE_PROGRESS_BAR;
    p.progBarParam = &b;
    b.barType = ORBIS_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
    b.msg = text;
    sceMsgDialogInitialize();
    return sceMsgDialogOpen(&p) >= 0;
}

static void progress_close(int open)
{
    long t_end = now_ms() + 2000;
    if (!open) return;
    sceMsgDialogClose();
    while (sceMsgDialogUpdateStatus() == ORBIS_COMMON_DIALOG_STATUS_RUNNING && now_ms() < t_end)
        usleep(16000);
    sceMsgDialogTerminate();
}

/* The dialog's magic encodes this block's address: keep it static. */
static hb_web_dialog_param g_web __attribute__((aligned(16)));
static char g_url[48];

static int web_dialog_open(int mode)
{
    memset(&g_web, 0, sizeof g_web);
    g_web.base.size = sizeof g_web.base;
    g_web.base.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)(uintptr_t)&g_web.base);
    g_web.size = sizeof g_web;
    g_web.mode = mode;
    g_web.user_id = user_id();
    g_web.url = g_url;
    if (mode == 2) {
        g_web.width = 1920;
        g_web.height = 1080;
        g_web.header_width = 1920;
    }
    return sceWebBrowserDialogOpen(&g_web);
}

/* The PS4 Browser app only. The in-app web browser dialog is used just when
 * /data/hearbridge/use_dialog exists (testing): its buttons do nothing on
 * the console, and opening it a second time can crash the console. */
#define USE_DIALOG_FLAG "/data/hearbridge/use_dialog"
#define OPEN_BROWSER_FLAG "/data/hearbridge/open_browser"   /* only then the tile opens the PS4 Browser */
static int show_ui_dialog(void);

static void show_ui(void)
{
    int rc;
    FILE *f = fopen(USE_DIALOG_FLAG, "rb");
    if (f) {
        fclose(f);
        lg("%s present: web dialog", USE_DIALOG_FLAG);
        if (show_ui_dialog() == 0) return;
    }
    rc = hb_launch_browser(g_url, NULL);
    lg("Browser app %s: 0x%08x", g_url, (unsigned)rc);
    if (rc == 0) {
        usleep(3000000);   /* let the shell start the browser before this app exits */
        lg("browser launch handed over");
        return;
    }
    {
        char text[256];
        snprintf(text, sizeof text, "HearBridge is running.\n\nOpen %s in the web browser, "
                 "or the address shown in the HearBridge notification on a phone or PC.", g_url);
        message(text);
    }
}

/* 0 when the dialog was shown (and closed again). */
static int show_ui_dialog(void)
{
    int rc, st;
    rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_WEB_BROWSER_DIALOG);
    lg("web dialog module: 0x%08x", (unsigned)rc);
    rc = sceWebBrowserDialogInitialize();
    lg("web dialog init: 0x%08x", (unsigned)rc);
    if ((rc = web_dialog_open(2)) != 0) {
        lg("web dialog full screen: 0x%08x, trying default layout", (unsigned)rc);
        rc = web_dialog_open(1);
    }
    if (rc == 0) {
        lg("web dialog open: %s", g_url);
        while ((st = sceWebBrowserDialogUpdateStatus()) == ORBIS_COMMON_DIALOG_STATUS_RUNNING ||
               st == ORBIS_COMMON_DIALOG_STATUS_INITIALIZED)
            usleep(16000);
        lg("web dialog closed (%d)", st);
        sceWebBrowserDialogClose();
        sceWebBrowserDialogTerminate();
        return 0;
    }
    lg("web dialog: 0x%08x", (unsigned)rc);
    return -1;
}

int main(void)
{
    char ver[32], build[24];
    int up, need_send, rc, prog, port = 0;
    struct install_result ir;
    char note[512];
    long t0, t_end;

    mkdir(DATA_DIR, 0777);
    lg("HearBridge launcher %s (bundled daemon %s)", LAUNCHER_VERSION, DAEMON_VERSION);
    sceUserServiceInitialize(NULL);
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
    sceCommonDialogInitialize();
    sceSystemServiceHideSplashScreen();

    install_all(lg, &ir);
    note[0] = 0;
    if (ir.plugin < 0 || ir.plugins_ini < 0)
        snprintf(note + strlen(note), sizeof note - strlen(note),
                 "HearBridge plugins could not be installed (details in "
                 "/data/hearbridge/launcher.log). Headphone audio from the system still works.\n\n");
    else if (ir.plugin > 0 || ir.plugins_ini > 0)
        snprintf(note + strlen(note), sizeof note - strlen(note),
                 "Plugins installed (tap+boot+daemon). Open a game for headphones audio. "
                 "you start from now on.\n\n");
    if (ir.loader > 0)
        snprintf(note + strlen(note), sizeof note - strlen(note),
                 "Plugin Loader was turned on in GoldHEN settings. Restart the console once "
                 "so GoldHEN loads plugins.\n\n");
    else if (ir.loader < 0)
        snprintf(note + strlen(note), sizeof note - strlen(note),
                 "Turn on Plugin Loader in GoldHEN Settings (Plugins), then restart the console.\n\n");

    up = daemon_status(ver, sizeof ver, build, sizeof build, &port);
    need_send = !up || !daemon_current(ver[0] ? ver : "0.0.0", build);
    lg("daemon UI: %s %s build %s (port %d), bundled %s build %s", up ? "running" : "not running", ver, build,
       port, DAEMON_VERSION, DAEMON_BUILD);

    if (need_send) {
        prog = progress_open(up ? "Updating HearBridge..." : "Starting HearBridge...");
        rc = send_daemon();
        if (rc == 0) {
            /* Short wait: on 14.02 BinLoader never runs the ELF. */
            t0 = now_ms();
            t_end = t0 + BINLOADER_WAIT_MS;
            up = 0;
            while (now_ms() < t_end) {
                if (prog)
                    sceMsgDialogProgressBarSetValue(
                        0, (uint32_t)((now_ms() - t0) * 100 / BINLOADER_WAIT_MS));
                if (daemon_status(ver, sizeof ver, build, sizeof build, &port) && daemon_current(ver, build)) {
                    up = 1;
                    break;
                }
                usleep(200000);
            }
            lg("daemon UI after start: %s %s port %d (%ld ms)", up ? "up" : "no answer", ver, port,
               now_ms() - t0);
        }
        progress_close(prog);
        if (rc == -1)
            message("HearBridge could not read its daemon from the package. Reinstall the HearBridge package.");
        else if (!up) {
            /* BinLoader did not run the ELF (e.g. firmware 14.02). Prefer
             * injecting the daemon PRX into SceShellUI so it survives games;
             * fall back to an in-process thread in this tile. */
            int injected = 0;
            lg("fallback: BinLoader path did not bring UI up (rc=%d)", rc);
            prog = progress_open("Starting HearBridge (ShellUI)...");
            if (inject_daemon_shellui() == 0) {
                t0 = now_ms();
                t_end = t0 + START_WAIT_MS;
                while (now_ms() < t_end) {
                    if (prog)
                        sceMsgDialogProgressBarSetValue(0, (uint32_t)((now_ms() - t0) * 100 / START_WAIT_MS));
                    /* Accept any live daemon UI (build may be "dev" in early builds). */
                    if (daemon_status(ver, sizeof ver, build, sizeof build, &port)) {
                        up = 1;
                        injected = 1;
                        break;
                    }
                    usleep(200000);
                }
                lg("inject: daemon UI %s %s port %d (%ld ms)", up ? "up" : "no answer", ver, port,
                   now_ms() - t0);
            }
            progress_close(prog);
            if (!up) {
                prog = progress_open("Starting HearBridge (in-app)...");
                if (start_daemon_inplace() == 0) {
                    t0 = now_ms();
                    t_end = t0 + START_WAIT_MS;
                    while (now_ms() < t_end) {
                        if (prog)
                            sceMsgDialogProgressBarSetValue(0, (uint32_t)((now_ms() - t0) * 100 / START_WAIT_MS));
                        if (daemon_status(ver, sizeof ver, build, sizeof build, &port)) {
                            up = 1;
                            break;
                        }
                        usleep(200000);
                    }
                    lg("inplace: daemon UI %s %s port %d (%ld ms)", up ? "up" : "no answer", ver, port,
                       now_ms() - t0);
                }
                progress_close(prog);
            }
            if (injected)
                g_daemon_inplace = 0; /* ShellUI owns it; this tile can exit */
            if (!up)
                message("HearBridge could not start its daemon.\n\n"
                        "Details: /data/hearbridge/launcher.log");
        }
    }
    while (note[0] && note[strlen(note) - 1] == '\n') note[strlen(note) - 1] = 0;
    if (up) {
        snprintf(g_url, sizeof g_url, "http://127.0.0.1:%d/", port);
        if (access(OPEN_BROWSER_FLAG, F_OK) == 0) {
            /* Only when asked for with the flag file. */
            if (note[0]) message(note);
            show_ui();
        } else {
            /* The page is for other devices on the network (and the PS4
             * browser on request): tell the address and finish. */
            char text[1024];
            uint32_t open_m;
            pad_start();
            open_m = enter_mask();
            uint32_t close_m = open_m == ORBIS_PAD_BUTTON_CROSS ? ORBIS_PAD_BUTTON_CIRCLE : ORBIS_PAD_BUTTON_CROSS;
            const char *open_l = open_m == ORBIS_PAD_BUTTON_CROSS ? "X" : "Circle";
            const char *close_l = close_m == ORBIS_PAD_BUTTON_CROSS ? "X" : "Circle";
            hb_ui_text(text, sizeof text, note, g_console_ip, port, LAUNCHER_VERSION, open_l, close_l);
            if (choose_open(text, open_m, close_m, open_l, close_l)) show_ui();
        }
    } else if (note[0]) {
        message(note);
    }

    if (g_daemon_inplace) {
        /* Keep this process alive so the in-app daemon keeps running. Leaving
         * the tile (or Circle on the address dialog) still ends us; until then
         * headphones and the web UI stay up. */
        lg("inplace: staying resident (daemon in this process)");
        for (;;) {
            if (!g_daemon_inplace) break;
            usleep(1000000);
            if (!daemon_status(ver, sizeof ver, build, sizeof build, &port)) {
                lg("inplace: UI gone, exiting");
                break;
            }
        }
    }
    lg("launcher done");
    sceSystemServiceLoadExec("exit", NULL);
    return 0;
}
