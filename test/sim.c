/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * sim.c - a simulated USB bus behind a zzp_transport: the ZZ9000 root port,
 * a 4-port single-TT USB 2.0 hub, a low-speed mouse, a high-speed flash
 * drive. Each device keeps the state real ones do (address, configuration,
 * port power/reset/enable) and checks that it is reached the way the
 * firmware requires: high speed without a split route, low and full speed
 * only through the right hub port's transaction translator.
 */
#include <string.h>
#include "sim.h"
#include "../src/usbdefs.h"

static const uint8_t hub_dev[18] = { 18, 1, 0x00,0x02, 9,0,1, 64, 0x40,0x1a, 0x01,0x01, 0x11,0x01, 0,2,0, 1 };
static const uint8_t hub_cfg[25] = { 9,2,25,0, 1,1,0, 0xe0, 50,
                                     9,4,0,0,1, 9,0,0, 0,
                                     7,5,0x81,3, 1,0, 12 };
static const uint8_t hub_hd[9]   = { 9, 0x29, 4, 0x00,0x00, 50, 100, 0x00, 0xff };

static const uint8_t mouse_dev[18] = { 18,1, 0x00,0x02, 0,0,0, 8, 0x6d,0x04, 0x5a,0xc0, 0x00,0x63, 1,2,0, 1 };
static const uint8_t mouse_cfg[34] = { 9,2,34,0, 1,1,0, 0xa0, 50,
                                       9,4,0,0,1, 3,1,2, 0,
                                       9,0x21,0x11,0x01,0,1,0x22,52,0,
                                       7,5,0x81,3, 4,0, 10 };

static const uint8_t flash_dev[18] = { 18,1, 0x10,0x02, 0,0,0, 64, 0x51,0x09, 0x66,0x16, 0x00,0x01, 1,2,3, 1 };
static const uint8_t flash_cfg[32] = { 9,2,32,0, 1,1,0, 0x80, 50,
                                       9,4,0,0,2, 8,6,0x50, 0,
                                       7,5,0x81,2, 0x00,0x02, 0,
                                       7,5,0x02,2, 0x00,0x02, 0 };

static const char *const strings[3][3] = {
    { "", "USB 2.0 Hub", "" },
    { "Logitech", "USB Optical Mouse", "" },
    { "Kingston", "DataTraveler 3.0", "E0D55E6C09F01A11" },
};

static void sim_dev(struct sim_dev *d, int kind, int speed)
{
    memset(d, 0, sizeof(*d));
    d->kind = kind;
    d->speed = speed;
}

void sim_init(struct sim *s, int root_kind)
{
    memset(s, 0, sizeof(*s));
    if (root_kind == SIM_HUB) {
        sim_dev(&s->dev[0], SIM_HUB, SPEED_HIGH);
        s->ndev = 1;
    } else if (root_kind == SIM_MOUSE) {
        sim_dev(&s->dev[0], SIM_MOUSE, SPEED_LOW);
        s->ndev = 1;
    } else if (root_kind == SIM_FLASH) {
        sim_dev(&s->dev[0], SIM_FLASH, SPEED_HIGH);
        s->ndev = 1;
    }
    s->root = s->ndev ? 0 : -1;
}

int sim_plug(struct sim *s, int port, int kind)
{
    struct sim_dev *d = &s->dev[s->ndev];
    sim_dev(d, kind, kind == SIM_MOUSE ? SPEED_LOW : SPEED_HIGH);
    d->on_hub = 1;
    d->hub_port = port;
    s->dev[0].port_dev[port] = s->ndev;
    s->dev[0].port_status[port] = 0;
    return s->ndev++;
}

static int put(struct zzp_xfer *x, const void *src, uint32_t n)
{
    if (n > x->len) n = x->len;
    memcpy(x->data, src, n);
    x->actual = n;
    return XS_OK;
}

static int string_desc(struct zzp_xfer *x, const char *str)
{
    uint8_t b[128];
    int n = (int)strlen(str), i;
    if (!n) return XS_STALL;
    b[0] = (uint8_t)(2 + 2 * n); b[1] = 3;
    for (i = 0; i < n; i++) { b[2 + 2 * i] = (uint8_t)str[i]; b[3 + 2 * i] = 0; }
    return put(x, b, b[0]);
}

/* the device the transfer reaches: by address, among devices that are reset and enabled */
static int find(struct sim *s, struct zzp_xfer *x)
{
    int i;
    for (i = 0; i < s->ndev; i++) {
        struct sim_dev *d = &s->dev[i];
        if (!d->enabled || d->addr != x->addr) continue;
        return i;
    }
    return -1;
}

static int route_ok(struct sim *s, struct sim_dev *d, struct zzp_xfer *x)
{
    if (x->speed != d->speed) { s->bad_speed++; return 0; }
    if (d->speed == SPEED_HIGH) {
        if (x->tt_hub || x->tt_port) { s->bad_split++; return 0; }
        return 1;
    }
    if (!d->on_hub) return 1;                  /* low speed straight on the root port: the caller should never get here */
    if (x->tt_hub != s->dev[0].addr || x->tt_port != d->hub_port) { s->bad_split++; return 0; }
    return 1;
}

/* ---- the flash drive's disk and SCSI --------------------------------------- */

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rle32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void sim_disk(struct sim_dev *d, uint32_t blocks, int with_mbr, uint8_t fat_type)
{
    static uint8_t store[2][4096 * 512];
    static int used;
    uint8_t *b;
    d->disk = store[used++ & 1];
    d->blocks = blocks;
    memset(d->disk, 0, (size_t)blocks * 512);
    d->attention = 1;
    d->mbr_type = fat_type;
    b = d->disk;
    if (with_mbr) {                         /* one partition from block 63 to the end */
        uint8_t *e = b + 446;
        e[4] = fat_type;
        le32(e + 8, 63);
        le32(e + 12, blocks - 63);
        b[510] = 0x55; b[511] = 0xaa;
        b = d->disk + 63 * 512;
    }
    b[0] = 0xeb; b[1] = 0x3c; b[2] = 0x90;  /* a FAT boot sector */
    memcpy(b + 3, "MSDOS5.0", 8);
    b[11] = 0x00; b[12] = 0x02; b[13] = 8;
    b[510] = 0x55; b[511] = 0xaa;
}

static int scsi_cmd(struct sim_dev *d)
{
    const uint8_t *cdb = d->cbw + 15;
    uint32_t lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) | cdb[5];
    uint32_t n = (uint32_t)(cdb[7] << 8) | cdb[8];
    d->status = 0;
    if (d->attention && cdb[0] != 0x03 && cdb[0] != 0x12) {
        d->attention = 0;
        d->sense[0] = 6; d->sense[1] = 0x28; d->sense[2] = 0;   /* unit attention: medium may have changed */
        d->status = 1;
        return 0;
    }
    if ((cdb[0] == 0x28 || cdb[0] == 0x2a) && (lba >= d->blocks || n > d->blocks - lba)) {
        d->sense[0] = 5; d->sense[1] = 0x21; d->sense[2] = 0;   /* LBA out of range */
        d->status = 1;
        return 0;
    }
    return 1;
}

static int sim_bulk(struct sim *s, struct sim_dev *d, struct zzp_xfer *x)
{
    uint8_t *cdb = d->cbw + 15;
    if (x->dir_in ? d->halted_in : d->halted_out) return XS_STALL;
    if (!x->dir_in && x->ep == 2 && d->phase == 0) {          /* the command block wrapper */
        if (x->len != 31 || rle32(x->data) != 0x43425355UL) { s->bad_setup++; d->halted_in = d->halted_out = 1; return XS_STALL; }
        memcpy(d->cbw, x->data, 31);
        d->data_len = rle32(d->cbw + 8);
        d->done = 0;
        x->actual = 31;
        if (!scsi_cmd(d) || d->data_len == 0) { d->phase = 2; return XS_OK; }
        d->phase = 1;
        return XS_OK;
    }
    if (d->phase == 1) {                                       /* data */
        uint32_t lba = ((uint32_t)cdb[2] << 24) | ((uint32_t)cdb[3] << 16) | ((uint32_t)cdb[4] << 8) | cdb[5];
        uint8_t tmp[36];
        if (d->stall_data_once) { d->stall_data_once = 0; d->status = 1; d->sense[0] = 3; d->phase = 2;
                                  if (x->dir_in) d->halted_in = 1; else d->halted_out = 1; return XS_STALL; }
        if (x->len > d->data_len - d->done) { s->bad_setup++; return XS_BADPARAM; }
        switch (cdb[0]) {
        case 0x12:
            memset(tmp, 0, 36); tmp[0] = 0; tmp[1] = 0x80; memcpy(tmp + 8, "Kingston", 8); memcpy(tmp + 16, "DataTraveler 3.0", 16);
            memcpy(x->data, tmp, x->len < 36 ? x->len : 36); break;
        case 0x03:
            memset(x->data, 0, x->len); x->data[0] = 0x70; x->data[2] = d->sense[0]; x->data[7] = 10;
            x->data[12] = d->sense[1]; x->data[13] = d->sense[2]; break;
        case 0x25: {
            uint32_t last = d->blocks - 1;
            x->data[0] = (uint8_t)(last >> 24); x->data[1] = (uint8_t)(last >> 16); x->data[2] = (uint8_t)(last >> 8); x->data[3] = (uint8_t)last;
            x->data[4] = 0; x->data[5] = 0; x->data[6] = 2; x->data[7] = 0; break;
        }
        case 0x1a: memset(x->data, 0, x->len); break;
        case 0x28: memcpy(x->data, d->disk + (size_t)lba * 512 + d->done, x->len); break;
        case 0x2a: memcpy(d->disk + (size_t)lba * 512 + d->done, x->data, x->len); break;
        default: break;
        }
        x->actual = x->len;
        d->done += x->len;
        if (d->done >= d->data_len) d->phase = 2;
        return XS_OK;
    }
    if (x->dir_in && d->phase == 2) {                          /* the status wrapper */
        uint8_t *c = x->data;
        if (x->len < 13) { s->bad_setup++; return XS_BADPARAM; }
        le32(c, 0x53425355UL);
        memcpy(c + 4, d->cbw + 4, 4);
        le32(c + 8, d->data_len - d->done);
        c[12] = d->status;
        x->actual = 13;
        d->phase = 0;
        return XS_OK;
    }
    s->bad_setup++;                                            /* out of step */
    return XS_STALL;
}

static int sim_xfer(void *ctx, struct zzp_xfer *x)
{
    struct sim *s = (struct sim *)ctx;
    int i = find(s, x), kind;
    struct sim_dev *d;
    uint8_t rt = x->setup[0], req = x->setup[1];
    uint16_t value = (uint16_t)(x->setup[2] | (x->setup[3] << 8));
    uint16_t index = (uint16_t)(x->setup[4] | (x->setup[5] << 8));
    uint16_t len = (uint16_t)(x->setup[6] | (x->setup[7] << 8));

    x->actual = 0;
    s->xfers++;
    if (i < 0) return XS_TIMEOUT;
    d = &s->dev[i];
    if (!route_ok(s, d, x)) return XS_ERROR;
    if (x->type == XT_BULK && d->kind == SIM_FLASH && d->disk) return sim_bulk(s, d, x);
    if (x->type != XT_CONTROL || x->ep != 0) return XS_STALL;
    if (d->kind == SIM_FLASH && (rt & 0x60) == RT_CLASS) {
        if (req == 0xfe) return XS_STALL;                      /* GET_MAX_LUN: like most sticks */
        if (req == 0xff) { d->resets++; d->phase = 0; return XS_OK; }
    }
    if (d->kind == SIM_FLASH && (rt & 0x1f) == 0x02 && req == REQ_CLEAR_FEATURE) {
        if ((index & 0x80)) d->halted_in = 0; else d->halted_out = 0;
        return XS_OK;
    }
    if (len != x->len || ((rt & 0x80) != 0) != (x->dir_in != 0)) { s->bad_setup++; return XS_BADPARAM; }
    if (x->maxpkt > 8 && d->addr == 0 && d->kind == SIM_MOUSE) { s->bad_maxpkt++; return XS_ERROR; }
    kind = d->kind;

    if ((rt & 0x60) == RT_STANDARD) {
        switch (req) {
        case REQ_SET_ADDRESS:
            if (d->refuse_address) return XS_STALL;
            d->addr = (uint8_t)value;
            return XS_OK;
        case REQ_SET_CONFIGURATION:
            d->configured = (uint8_t)value;
            return XS_OK;
        case REQ_GET_DESCRIPTOR:
            switch (value >> 8) {
            case DT_DEVICE:
                return put(x, kind == SIM_HUB ? hub_dev : kind == SIM_MOUSE ? mouse_dev : flash_dev, 18);
            case DT_CONFIG:
                if (kind == SIM_HUB) return put(x, hub_cfg, sizeof(hub_cfg));
                if (kind == SIM_MOUSE) return put(x, mouse_cfg, sizeof(mouse_cfg));
                return put(x, flash_cfg, sizeof(flash_cfg));
            case DT_STRING:
                if ((value & 0xff) == 0) { static const uint8_t l[4] = { 4, 3, 0x09, 0x04 }; return put(x, l, 4); }
                if ((value & 0xff) > 3) return XS_STALL;
                return string_desc(x, strings[kind == SIM_MOUSE ? 1 : kind == SIM_FLASH ? 2 : 0][(value & 0xff) - 1]);
            }
            return XS_STALL;
        }
        return XS_STALL;
    }
    /* hub class requests */
    if (kind != SIM_HUB) return XS_STALL;
    if (req == REQ_GET_DESCRIPTOR && (value >> 8) == DT_HUB) return put(x, hub_hd, sizeof(hub_hd));
    if ((rt & 0x1f) == RT_OTHER && index >= 1 && index <= 4) {
        uint16_t *ps = &d->port_status[index], *pc = &d->port_change[index];
        int child = d->port_dev[index];
        switch (req) {
        case REQ_SET_FEATURE:
            if (value == PORT_POWER) {
                *ps |= PS_POWER;
                if (child) { *ps |= PS_CONNECTION; *pc |= PC_CONNECTION; }
                return XS_OK;
            }
            if (value == PORT_RESET) {
                if (!(*ps & PS_CONNECTION)) return XS_OK;
                *ps |= PS_ENABLE; *pc |= PC_RESET;
                if (s->dev[child].speed == SPEED_LOW) *ps |= PS_LOW_SPEED;
                if (s->dev[child].speed == SPEED_HIGH) *ps |= PS_HIGH_SPEED;
                s->dev[child].enabled = 1;
                s->dev[child].addr = 0;
                return XS_OK;
            }
            return XS_STALL;
        case REQ_CLEAR_FEATURE:
            if (value == C_PORT_CONNECTION) { *pc &= (uint16_t)~PC_CONNECTION; return XS_OK; }
            if (value == C_PORT_RESET) { *pc &= (uint16_t)~PC_RESET; return XS_OK; }
            return XS_OK;
        case REQ_GET_STATUS: {
            uint8_t b[4] = { (uint8_t)*ps, (uint8_t)(*ps >> 8), (uint8_t)*pc, (uint8_t)(*pc >> 8) };
            return put(x, b, 4);
        }
        }
    }
    return XS_STALL;
}

static int sim_root_check(void *ctx, int *speed)
{
    struct sim *s = (struct sim *)ctx;
    if (s->root < 0) return XS_OFFLINE;
    *speed = s->dev[s->root].speed;
    return XS_OK;
}

static int sim_root_reset(void *ctx, int *speed)
{
    struct sim *s = (struct sim *)ctx;
    int i;
    if (s->root < 0) return XS_OFFLINE;
    for (i = 0; i < s->ndev; i++) {            /* a reset of the root takes everything below with it */
        s->dev[i].enabled = 0;
        s->dev[i].addr = 0;
        s->dev[i].configured = 0;
    }
    s->dev[s->root].enabled = 1;
    s->resets++;
    *speed = s->dev[s->root].speed;
    return XS_OK;
}

static void sim_delay(void *ctx, int ms)
{
    ((struct sim *)ctx)->waited_ms += ms;
}

void sim_transport(struct sim *s, struct zzp_transport *t)
{
    t->ctx = s;
    t->xfer = sim_xfer;
    t->root_check = sim_root_check;
    t->root_reset = sim_root_reset;
    t->delay_ms = sim_delay;
}
