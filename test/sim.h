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
    /* a flash drive: bulk-only transport over a RAM disk */
    uint8_t *disk;
    uint32_t blocks;
    int phase;                  /* 0 = waiting for a command, 1 = data, 2 = status */
    uint8_t cbw[31];
    uint32_t data_len, done;
    uint8_t status;             /* for the status wrapper */
    uint8_t sense[3];
    int attention;              /* the next command fails with a unit attention */
    int stall_data_once;        /* the next data stage stalls */
    int halted_in, halted_out;
    int resets;
    uint8_t mbr_type;
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
void sim_disk(struct sim_dev *d, uint32_t blocks, int with_mbr, uint8_t fat_type);   /* give a flash drive a disk */

#endif
