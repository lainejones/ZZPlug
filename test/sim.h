/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
#ifndef ZZP_SIM_H
#define ZZP_SIM_H
#include <stdint.h>
#include "../src/transport.h"

#define SIM_NONE  0
#define SIM_HUB   1
#define SIM_MOUSE 2
#define SIM_FLASH 3

struct sim_dev {
    int kind, speed;
    int on_hub, hub_port;
    int enabled;
    uint8_t addr, configured, refuse_address;
    /* a hub's ports */
    int port_dev[5];
    uint16_t port_status[5], port_change[5];
};

struct sim {
    struct sim_dev dev[8];
    int ndev, root;
    /* what went wrong, counted */
    int bad_split, bad_speed, bad_setup, bad_maxpkt;
    int xfers, resets, waited_ms;
};

void sim_init(struct sim *s, int root_kind);
int  sim_plug(struct sim *s, int port, int kind);   /* on the root hub's port 1..4 */
void sim_transport(struct sim *s, struct zzp_transport *t);

#endif
