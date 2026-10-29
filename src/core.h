/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * core.h - ZZPlug's USB host logic: find what is plugged in, give each
 * device an address, read what it is, look behind hubs. Portable C, no
 * Amiga headers: the hardware is reached only through a zzp_transport.
 */
#ifndef ZZP_CORE_H
#define ZZP_CORE_H

#include <stdint.h>
#include "transport.h"

#define ZZP_MAXDEV   32
#define ZZP_MAXIF    8
#define ZZP_MAXEP    4          /* per interface, the ones we keep */
#define ZZP_STRLEN   40
#define ZZP_NOTELEN  160

struct zzp_ep {
    uint8_t  addr;              /* bEndpointAddress (bit 7 = IN) */
    uint8_t  attr;              /* bmAttributes (bits 0-1 = type) */
    uint16_t maxpkt;
    uint8_t  interval;
};

struct zzp_if {
    uint8_t number, cls, subclass, proto, nep;
    struct zzp_ep ep[ZZP_MAXEP];
};

struct zzp_dev {
    uint8_t  used;
    uint8_t  addr;              /* 1..127 */
    uint8_t  speed;             /* SPEED_* */
    int8_t   parent;            /* index of the hub it hangs on, -1 = the ZZ9000 port */
    uint8_t  port;              /* port on that hub (1..), 0 = root */
    uint8_t  depth;
    uint8_t  tt_hub, tt_port, tt_multi, tt_think;   /* split route, for low/full speed behind a high-speed hub */
    uint8_t  maxpkt0;
    uint16_t vid, pid, bcd_usb, bcd_dev;
    uint8_t  cls, subclass, proto;
    uint8_t  config;            /* bConfigurationValue we selected, 0 = none */
    uint16_t max_power_ma;
    uint8_t  self_powered;
    uint8_t  nif;
    struct zzp_if ifs[ZZP_MAXIF];
    char     manufacturer[ZZP_STRLEN], product[ZZP_STRLEN], serial[ZZP_STRLEN];
    /* hubs */
    uint8_t  is_hub, hub_ports, hub_multi_tt, hub_think, hub_pwr_ms;
    uint8_t  ok;                /* fully enumerated */
    char     note[ZZP_NOTELEN]; /* what went wrong, in plain words */
};

struct zzp_bus {
    struct zzp_transport *t;
    struct zzp_dev dev[ZZP_MAXDEV];
    int  ndev;
    uint8_t next_addr;
    int  root_status;           /* XS_OK, XS_OFFLINE, ... from the last scan */
    int  kick_offline;          /* reset the root port once even when it looks empty (cold-boot hubs) */
    int  kicked;                /* that reset found something */
    char note[ZZP_NOTELEN];     /* a bus-level remark (nothing plugged in, ...) */
    uint32_t xfers, errors;     /* counters */
};

void zzp_bus_init(struct zzp_bus *b, struct zzp_transport *t);
int  zzp_scan(struct zzp_bus *b);            /* reset the root port and enumerate everything: device count */
int  zzp_control(struct zzp_bus *b, const struct zzp_dev *d, uint8_t rt, uint8_t req,
                 uint16_t value, uint16_t index, void *data, uint16_t len, uint32_t *actual);

const char *zzp_class_name(uint8_t cls, uint8_t subclass, uint8_t proto);
const char *zzp_speed_name(uint8_t speed);
const char *zzp_status_name(int status);
const struct zzp_if *zzp_find_if(const struct zzp_dev *d, uint8_t cls, int subclass, int proto);   /* -1 = any */

#endif
