/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synchronous HCI command helpers used by the Realtek firmware loader and
 * the shared host BD_ADDR path. Ported from HearBridge-PS5 (opcodes and
 * Command Complete / vendor waits), without the PS5 MediaTek scan-pause
 * machinery. */
#ifndef HEARBRIDGE_HCI_CMD_H
#define HEARBRIDGE_HCI_CMD_H

#include "hci.h"

#define HB_OP_RESET               0x0C03
#define HB_OP_SET_EVENT_MASK      0x0C01
#define HB_OP_SET_EVENT_FILTER    0x0C05
#define HB_OP_WRITE_LOCAL_NAME    0x0C13
#define HB_OP_WRITE_PAGE_TIMEOUT  0x0C18
#define HB_OP_WRITE_SCAN_ENABLE   0x0C1A
#define HB_OP_WRITE_CLASS_OF_DEV  0x0C24
#define HB_OP_WRITE_INQUIRY_MODE  0x0C45
#define HB_OP_WRITE_SSP_MODE      0x0C56
#define HB_OP_READ_LOCAL_VERSION  0x1001
#define HB_OP_READ_LOCAL_FEATURES 0x1003
#define HB_OP_READ_BUFFER_SIZE    0x1005
#define HB_OP_READ_BD_ADDR        0x1009

/* Wait for Command Complete for op (retries up to 3 times). Copies the
 * event (code..params) into out[0..*out_len). 1 if status is 0. */
int hci_cmd_sync(hci_t hci, unsigned op, const void *params, int plen,
                 unsigned char *out, int *out_len, int out_max);

/* Like hci_cmd_sync() but the command is sent once (Realtek patch
 * fragments must not be repeated). */
int hci_cmd_sync_once(hci_t hci, unsigned op, const void *params, int plen,
                      unsigned char *out, int *out_len, int out_max);

/* Wait for Vendor-Specific event 0xFF (CSR BCCMD). Copies the event
 * payload (after the 0xFF length byte) into out. */
int hci_cmd_vendor(hci_t hci, unsigned op, const void *params, int plen,
                   unsigned char *out, int *out_len, int out_max);

/* Formats a Bluetooth address (little-endian as on the wire) into buf. */
void hci_addr_str(const unsigned char addr[6], char buf[18]);

#endif
