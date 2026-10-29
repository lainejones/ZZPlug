/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * device.h - zzusbdisk.device, built in memory by the running ZZPlug: one
 * unit per USB stick. Callers' BeginIO puts the request on ZZPlug's port;
 * ZZPlug's task does the USB work, so it stays the only one using the card.
 */
#ifndef ZZP_DEVICE_H
#define ZZP_DEVICE_H
#include <exec/types.h>
#include <exec/ports.h>
#include "msd.h"

#define ZZD_NAME  "zzusbdisk.device"
#define ZZD_UNITS 4

struct zzd_unit {
    int present;                /* a ready stick is behind this unit */
    struct zzp_msd *msd;        /* how to reach it */
    ULONG changenum;            /* bumped on every insert and remove */
    struct List ints;           /* TD_ADDCHANGEINT requests */
    uint32_t blocks, blksize;
    int wp;
    ULONG reads, writes, errors;
};

extern struct zzd_unit zzd_units[ZZD_UNITS];

int  zzd_open(void);                    /* create the device and its port: 1 ok */
void zzd_close(void);                   /* only when nobody has it open */
int  zzd_in_use(void);                  /* opened by someone (a mounted filesystem) */
int  zzd_mounted(char *name, int max);  /* a DOS device mounted on zzusbdisk.device: 1 and its name */
ULONG zzd_sigmask(void);
void zzd_service(void);                 /* do every queued request */
void zzd_set_media(int unit, struct zzp_msd *msd);   /* NULL = removed; tells the filesystems */

#endif
