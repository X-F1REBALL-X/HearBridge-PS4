/* SPDX-License-Identifier: GPL-3.0-or-later */
/* hearbridge_boot - tiny GoldHEN game plugin: after the game settles,
 * load hearbridge_daemon.prx into THIS process via sceKernelLoadStartModule
 * if the UI ports are down. Keeps the heavy PRX out of plugins.ini [default]
 * so only games that reach this boot path map it (and only once). */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "hb_plugin_common.h"

attr_public const char *g_pluginName = "hearbridge_boot";
attr_public const char *g_pluginDesc = "HearBridge deferred daemon boot";
attr_public const char *g_pluginAuth = "HearBridge";
attr_public uint32_t g_pluginVersion = 0x00010000;

#define DAEMON_PRX "/data/GoldHEN/plugins/hearbridge_daemon.prx"
#define BOOT_LOG "/data/GoldHEN/hearbridge_boot.log"
#define UI_PORT 8090
#define UI_PORT_ALT 8091

static OrbisPthread g_thr;
static volatile int g_started;

static int title_game(const char *t)
{
    static const char *ok[] = {
        "CUSA", "CUSB", "CUSS", "CUSJ", "PPSA", "PLJS", "PLJM", "PLAS",
        "PCAS", "PCJS", "PCKS", "PLKS", NULL
    };
    int i;
    if (!t || !t[0]) return 0;
    for (i = 0; ok[i]; i++) {
        size_t n = strlen(ok[i]);
        if (strncmp(t, ok[i], n) == 0) return 1;
    }
    return 0;
}

static int ui_up(void)
{
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

#define K_O_WRONLY 0x0001
#define K_O_CREAT  0x0200
#define K_O_APPEND 0x0008
static void blog(const char *line)
{
    int32_t fd = sceKernelOpen(BOOT_LOG, K_O_WRONLY | K_O_CREAT | K_O_APPEND, 0666);
    if (fd < 0) return;
    sceKernelWrite(fd, line, strlen(line));
    sceKernelClose(fd);
}

static void *boot_thr(void *arg)
{
    int i, rc;
    char msg[160];
    (void)arg;
    /* ~8 s: let the game finish early init (full daemon under [default] crashed early). */
    for (i = 0; i < 80; i++)
        sceKernelUsleep(100000);
    if (ui_up()) {
        blog("boot: UI already up, skip\n");
        hb_notify("HearBridge already running");
        g_started = 0;
        return NULL;
    }
    blog("boot: loading daemon prx\n");
    hb_notify("HearBridge daemon loading");
    /* sceKernelLoadStartModule path, args, flags, opt, res */
    rc = sceKernelLoadStartModule(DAEMON_PRX, 0, NULL, 0, NULL, NULL);
    snprintf(msg, sizeof msg, "boot: LoadStartModule rc=0x%08x\n", (unsigned)rc);
    blog(msg);
    if (rc < 0)
        hb_notify("HearBridge daemon load failed");
    else
        hb_notify("HearBridge daemon loaded");
    g_started = 0;
    return NULL;
}

int32_t attr_public plugin_load(int32_t argc, const char *argv[])
{
    char title[16] = {0};
    int rc;
    char line[96];
    (void)argc;
    (void)argv;
    rc = hb_title_id(title, sizeof title);
    snprintf(line, sizeof line, "boot plugin_load title=%s rc=%d\n", title[0] ? title : "?", rc);
    blog(line);
    if (!title_game(title))
        return 0;
    if (g_started)
        return 0;
    g_started = 1;
    if (scePthreadCreate(&g_thr, NULL, boot_thr, NULL, "hb_boot") != 0) {
        g_started = 0;
        blog("boot: pthread failed\n");
        return 0;
    }
    return 0;
}

int32_t attr_public plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    return 0;
}

int32_t attr_module_hidden module_start(int64_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}

int32_t attr_module_hidden module_stop(int64_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}
