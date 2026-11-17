/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synchronous HCI command waits. From HearBridge-PS5 src/bt/hci_cmd.c,
 * trimmed to Command Complete, one-shot Complete, and vendor events. */
#include "hci_cmd.h"
#include "log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HEARBRIDGE_PAYLOAD
extern uint64_t sceKernelGetProcessTime(void);
long now_ms(void)
{
    return (long)(sceKernelGetProcessTime() / 1000u);
}
#else
#include <time.h>
long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}
#endif

void hci_addr_str(const unsigned char addr[6], char buf[18])
{
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static int cmd_sync(hci_t hci, unsigned op, const void *params, int plen,
                    unsigned char *out, int *out_len, int out_max,
                    int attempts, int first_ms, int last_ms)
{
    int attempt;

    if (!hci.ops || !hci.ctx) return 0;

    for (attempt = 1; attempt <= attempts; attempt++) {
        long deadline = now_ms() + (attempt == attempts ? last_ms : first_ms);
        unsigned char ev[HCI_PKT_MAX];

        if (!hci.ops->cmd(hci.ctx, op, params, plen)) {
            log_line("hci_cmd: transport error sending %#06x", op);
            return 0;
        }

        while (now_ms() < deadline) {
            int n, pr;

            pr = hci.ops->pump(hci.ctx, 20);
            if (pr < 0) {
                log_line("hci_cmd: transport lost while waiting for %#06x", op);
                return 0;
            }
            while ((n = hci.ops->next_event(hci.ctx, ev, (int)sizeof ev)) > 0) {
                if (ev[0] == 0x0E && n >= 6) {
                    unsigned rop = (unsigned)ev[3] | ((unsigned)ev[4] << 8);
                    if (rop == op) {
                        if (out && out_len && out_max > 0) {
                            int copy = n < out_max ? n : out_max;
                            memcpy(out, ev, (size_t)copy);
                            *out_len = copy;
                        }
                        if (ev[5] != 0) {
                            log_line("hci_cmd: %#06x status %#04x", op, ev[5]);
                            return 0;
                        }
                        return 1;
                    }
                }
            }
            if (hci.ops->next_acl) {
                while (hci.ops->next_acl(hci.ctx, ev, (int)sizeof ev) > 0) { }
            }
        }
        if (attempts == 1) log_line("hci_cmd: %#06x no reply", op);
        else log_line("hci_cmd: %#06x no reply (try %d)", op, attempt);
    }
    if (hci.ops->diag) hci.ops->diag(hci.ctx);
    return 0;
}

int hci_cmd_sync(hci_t hci, unsigned op, const void *params, int plen,
                 unsigned char *out, int *out_len, int out_max)
{
    return cmd_sync(hci, op, params, plen, out, out_len, out_max, 3, 1000, 3000);
}

int hci_cmd_sync_once(hci_t hci, unsigned op, const void *params, int plen,
                      unsigned char *out, int *out_len, int out_max)
{
    return cmd_sync(hci, op, params, plen, out, out_len, out_max, 1, 3000, 3000);
}

int hci_cmd_vendor(hci_t hci, unsigned op, const void *params, int plen,
                   unsigned char *out, int *out_len, int out_max)
{
    long deadline;
    unsigned char ev[HCI_PKT_MAX];
    int saw_cc = 0;

    if (!hci.ops || !hci.ctx) return 0;
    if (!hci.ops->cmd(hci.ctx, op, params, plen)) {
        log_line("hci_cmd: transport error sending vendor %#06x", op);
        return 0;
    }
    deadline = now_ms() + 2000;
    while (now_ms() < deadline) {
        int n, pr;
        pr = hci.ops->pump(hci.ctx, 20);
        if (pr < 0) {
            log_line("hci_cmd: transport lost waiting vendor %#06x", op);
            return 0;
        }
        while ((n = hci.ops->next_event(hci.ctx, ev, (int)sizeof ev)) > 0) {
            if (ev[0] == 0xFF && n >= 2) {
                int payload = n - 2;
                if (out && out_len && out_max > 0) {
                    int copy = payload < out_max ? payload : out_max;
                    if (copy > 0) memcpy(out, ev + 2, (size_t)copy);
                    *out_len = copy;
                }
                return 1;
            }
            if (ev[0] == 0x0E && n >= 6) {
                unsigned rop = (unsigned)ev[3] | ((unsigned)ev[4] << 8);
                if (rop == op) {
                    if (ev[5] != 0) {
                        log_line("hci_cmd: vendor %#06x status %#04x", op, ev[5]);
                        return 0;
                    }
                    saw_cc = 1;
                }
            }
        }
        if (hci.ops->next_acl) {
            while (hci.ops->next_acl(hci.ctx, ev, (int)sizeof ev) > 0) { }
        }
    }
    if (saw_cc) {
        log_line("hci_cmd: vendor %#06x Command Complete only (no EVT_VENDOR)", op);
        if (out_len) *out_len = 0;
        return 1;
    }
    log_line("hci_cmd: vendor %#06x no reply", op);
    return 0;
}
