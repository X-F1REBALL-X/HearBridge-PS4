/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hearbridge_daemon - GoldHEN plugin that runs the HearBridge daemon.
 *
 * BinLoader on firmware 14.02 receives ELF payloads but does not execute them.
 * GoldHEN plugins still load and run, so the daemon is packaged as a .prx and
 * started from plugin_load on a background thread.
 *
 * Load rules (plugin_load):
 *  - Only retail game titles (CUSA/CUSB/CUSS/CUSJ/PPSA/PLJS/PLJM/PLAS) or the
 *    HearBridge launcher itself (HRBG). Skip NPXS / other system and homebrew.
 *  - Single instance: if /data/hearbridge/daemon.pid lock is held / UI already
 *    answers, return without starting another copy.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "hb_plugin_common.h"
#include "../src/daemon_main.h"
#include "../src/pcm_udp.h"

attr_public const char *g_pluginName = "hearbridge_daemon";
attr_public const char *g_pluginDesc = "HearBridge PS4 daemon (plugin)";
attr_public const char *g_pluginAuth = "HearBridge";
attr_public uint32_t g_pluginVersion = 0x00010200; /* 1.2.0 */

#define LOCK_PATH "/data/hearbridge/daemon.lock"
#define LOCK_PATH_ALT "/user/data/hearbridge/daemon.lock"
#define UI_PORT 8090
#define UI_PORT_ALT 8091

static OrbisPthread g_thr;
static volatile int g_started;

static int title_ok(const char *t)
{
    /* ShellUI inject (NPXS20001) is the preferred persistent host on 14.02.
     * Also allow retail games and the HearBridge tile itself. */
    static const char *ok[] = {
        "NPXS20001", "CUSA", "CUSB", "CUSS", "CUSJ", "PPSA", "PLJS", "PLJM", "PLAS", "HRBG", NULL
    };
    int i;
    if (!t || !t[0]) return 0;
    for (i = 0; ok[i]; i++) {
        size_t n = strlen(ok[i]);
        if (strncmp(t, ok[i], n) == 0) return 1;
    }
    return 0;
}

/* 1 if something already answers on the daemon UI ports. */
static int ui_already_up(void)
{
    /* Lightweight TCP connect to loopback; avoids pulling full HTTP stack. */
    int fd, k, ports[2] = { UI_PORT, UI_PORT_ALT };
    struct sockaddr_in a;
    for (k = 0; k < 2; k++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        memset(&a, 0, sizeof a);
        a.sin_len = sizeof a;
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)ports[k]);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            close(fd);
            return 1;
        }
        close(fd);
    }
    return 0;
}

/* Best-effort lock. If the file already exists, another copy may be alive;
 * ui_already_up() is the reliable guard. Stale locks are cleared when the
 * daemon thread exits (or by the launcher). */
#define K_O_WRONLY 0x0001
#define K_O_CREAT  0x0200
#define K_O_EXCL   0x0800
static int take_lock(void)
{
    int32_t fd;
    mkdir("/data/hearbridge", 0777);
    mkdir("/user/data/hearbridge", 0777);
    fd = sceKernelOpen(LOCK_PATH, K_O_WRONLY | K_O_CREAT | K_O_EXCL, 0666);
    if (fd < 0)
        fd = sceKernelOpen(LOCK_PATH_ALT, K_O_WRONLY | K_O_CREAT | K_O_EXCL, 0666);
    if (fd < 0) return 0;
    sceKernelClose(fd);
    return 1;
}

static void *daemon_thread(void *arg)
{
    int i;
    (void)arg;
    /* Give the host process time to finish starting before USB/net work. */
    for (i = 0; i < 50; i++) /* ~5 s */
        sceKernelUsleep(100000);
    final_printf("[GoldHEN] hearbridge_daemon thread start\n");
    (void)daemon_main();
    final_printf("[GoldHEN] hearbridge_daemon thread exit\n");
    /* Allow a later process to start a new copy after this one dies. */
    unlink(LOCK_PATH);
    unlink(LOCK_PATH_ALT);
    g_started = 0;
    return NULL;
}


/* Same-process PCM bridge for hearbridge_game_tap (UDP loopback is unreliable
 * between sceNet sockets inside one game process). */
int32_t attr_public hb_pcm_feed(const void *data, size_t nbytes)
{
    if (!data || nbytes == 0 || nbytes > 8192) return -1;
    pcm_udp_feed(data, (int)nbytes);
    return 0;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    char title[16] = {0};
    int rc;
    (void)argc;
    (void)argv;

    final_printf("[GoldHEN] %s Plugin Started.\n", g_pluginName);
    rc = hb_title_id(title, sizeof title);
    final_printf("[GoldHEN] %s title=%s rc=%d\n", g_pluginName, title, rc);
    {
        static const char *marks[] = {
            "/data/hearbridge/daemon_plugin.loaded",
            "/user/data/hearbridge/daemon_plugin.loaded",
            "/system_tmp/hearbridge_daemon.loaded",
            "/tmp/hearbridge_daemon.loaded",
            NULL
        };
        int mi;
        char line[128];
        int n = snprintf(line, sizeof line, "plugin_load title=%s rc=%d\n", title, rc);
        mkdir("/data/hearbridge", 0777);
        mkdir("/user/data/hearbridge", 0777);
        for (mi = 0; marks[mi]; mi++) {
            int32_t fd = sceKernelOpen(marks[mi], K_O_WRONLY | K_O_CREAT | K_O_EXCL, 0666);
            if (fd < 0)
                fd = sceKernelOpen(marks[mi], K_O_WRONLY | K_O_CREAT, 0666);
            if (fd >= 0) {
                sceKernelWrite(fd, line, (size_t)n);
                sceKernelClose(fd);
                break;
            }
        }
    }
    if (!title_ok(title)) {
        final_printf("[GoldHEN] %s skip title %s\n", g_pluginName, title);
        return 0;
    }
    if (g_started) return 0;
    if (ui_already_up()) {
        final_printf("[GoldHEN] %s UI already up, skip\n", g_pluginName);
        return 0;
    }
    if (!take_lock()) {
        /* Stale lock after a crash: if UI is down, clear and retry once. */
        if (!ui_already_up()) {
            unlink(LOCK_PATH);
            unlink(LOCK_PATH_ALT);
            if (!take_lock()) {
                final_printf("[GoldHEN] %s lock held, skip\n", g_pluginName);
                return 0;
            }
        } else {
            final_printf("[GoldHEN] %s lock held, skip\n", g_pluginName);
            return 0;
        }
    }
    g_started = 1;
    rc = scePthreadCreate(&g_thr, NULL, daemon_thread, NULL, "hb_daemon");
    if (rc != 0) {
        final_printf("[GoldHEN] %s pthread failed 0x%08x\n", g_pluginName, (unsigned)rc);
        unlink(LOCK_PATH);
        unlink(LOCK_PATH_ALT);
        g_started = 0;
        hb_notify("HearBridge daemon thread failed");
        return 0;
    }
    hb_notify("HearBridge daemon starting");
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    /* Daemon watches g_quit via /api/quit; do not join here (would hang unload). */
    return 0;
}

int32_t attr_module_hidden module_start(int64_t argc, const void *args)
{
    /* sceKernelLoadStartModule (boot plugin) only hits module_start, not
     * GoldHEN's plugin_load entry — start the same way. */
    (void)argc;
    (void)args;
    return plugin_load(0, NULL);
}

int32_t attr_module_hidden module_stop(int64_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}
