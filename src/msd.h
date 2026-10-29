/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * msd.h - USB mass storage, bulk-only transport with SCSI commands: what a
 * USB stick needs to be read and written block by block. Portable C over
 * the core's transport; the host tests run it against a simulated stick.
 */
#ifndef ZZP_MSD_H
#define ZZP_MSD_H
#include <stdint.h>
#include "core.h"

#define MSD_CHUNK 16384         /* one command moves at most this much (the mailbox's staging area) */

struct zzp_msd {
    struct zzp_bus *b;
    int dev;                    /* index in the bus */
    uint8_t iface, ep_in, ep_out, lun, max_lun;
    uint16_t mps_in, mps_out;
    uint32_t tag;
    uint32_t blocks;            /* number of blocks */
    uint32_t blksize;
    uint8_t removable, write_protected, ready;
    uint8_t sense_key, asc, ascq;   /* from the last failed command */
    char vendor[9], product[17];
    uint32_t resets;            /* reset recoveries done */
};

/* errors (besides the transport's XS_*) */
#define MSD_OK        0
#define MSD_FAILED    0x200     /* the device reported the command failed; see sense */
#define MSD_NOMEDIA   0x201
#define MSD_PROTOCOL  0x202     /* a bad status wrapper or phase error */
#define MSD_RANGE     0x203

int  zzp_msd_find(const struct zzp_dev *d);                      /* bulk-only SCSI interface index, -1 none */
int  zzp_msd_init(struct zzp_msd *m, struct zzp_bus *b, int dev);    /* MSD_OK when ready with a capacity */
int  zzp_msd_read(struct zzp_msd *m, uint32_t lba, uint32_t count, void *buf);
int  zzp_msd_write(struct zzp_msd *m, uint32_t lba, uint32_t count, const void *buf);
int  zzp_msd_sync(struct zzp_msd *m);                            /* SYNCHRONIZE CACHE, tolerated if unsupported */
int  zzp_msd_scsi(struct zzp_msd *m, const uint8_t *cdb, int cdblen, void *data, uint32_t len, int in, uint32_t *actual);

/* partitions */
#define MAXPART 4
struct zzp_part {
    uint32_t start, count;
    uint8_t type;               /* MBR type, or 0 for a disk with no partition table (one FAT volume) */
};
int  zzp_parse_parts(const uint8_t *sector0, uint32_t disk_blocks, struct zzp_part *out);   /* FAT volumes found */
const char *zzp_fat_name(uint8_t type);

#endif
