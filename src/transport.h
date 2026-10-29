/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * transport.h - what the USB logic needs from the hardware: one transfer at
 * a time, a root-port reset and check, and a delay. The ZZ9000 mailbox
 * (mailbox.c) and the host test's simulated bus (test/sim.c) both provide it.
 */
#ifndef ZZP_TRANSPORT_H
#define ZZP_TRANSPORT_H

#include <stdint.h>

#define XT_CONTROL   0
#define XT_BULK      1
#define XT_INTERRUPT 2

/* results (the proxy's own status values) */
#define XS_OK          0x00
#define XS_ERROR       0xff
#define XS_TIMEOUT     0xfe
#define XS_STALL       0xfd
#define XS_NAK         0xfc
#define XS_OFFLINE     0xf7
#define XS_BADPARAM    0xf6
#define XS_UNSUPPORTED 0xf5
#define XS_STALE       0xf4
#define XS_HOSTERROR   0xf2
#define XS_NOBOARD     0x100    /* ours: no ZZ9000, or another driver has it */

struct zzp_xfer {
    uint8_t  type;              /* XT_* */
    uint8_t  addr, ep;          /* device address 0..127, endpoint 0..15 */
    uint8_t  dir_in;            /* 1 = IN */
    uint8_t  speed;             /* SPEED_* */
    uint16_t maxpkt;
    uint8_t  tt_hub, tt_port;   /* split transaction: the nearest high-speed hub, 0 = none */
    uint8_t  tt_multi, tt_think;
    uint16_t timeout_ms;
    uint8_t  setup[8];          /* control: the setup packet exactly as on the wire */
    uint8_t *data;
    uint32_t len;               /* requested */
    uint32_t actual;            /* transferred */
};

struct zzp_transport {
    void *ctx;
    int  (*xfer)(void *ctx, struct zzp_xfer *x);               /* XS_* */
    int  (*root_check)(void *ctx, int *speed);                 /* XS_OK (connected, speed set) or XS_OFFLINE */
    int  (*root_reset)(void *ctx, int *speed);                 /* reset; then as root_check */
    void (*delay_ms)(void *ctx, int ms);
};

#endif
