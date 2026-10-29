/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * msd.c - bulk-only transport (USB Mass Storage Class, BBB):
 *   a 31-byte command block wrapper on bulk OUT, the data on bulk IN or OUT,
 *   a 13-byte command status wrapper on bulk IN.
 * A stalled endpoint is cleared and the status read again; a phase error
 * or a garbled status means a reset recovery (class reset, clear both halts).
 */
#include <string.h>
#include "msd.h"
#include "usbdefs.h"

#define CBW_SIG 0x43425355UL
#define CSW_SIG 0x53425355UL
#define REQ_MSD_RESET   0xff
#define REQ_GET_MAX_LUN 0xfe
#define FEATURE_ENDPOINT_HALT 0
#define DATA_TIMEOUT 5000

static void put32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t get32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

int zzp_msd_find(const struct zzp_dev *d)
{
    int i;
    for (i = 0; i < d->nif; i++) {
        const struct zzp_if *f = &d->ifs[i];
        if (f->cls == CLASS_STORAGE && f->proto == 0x50 && (f->subclass == 6 || f->subclass == 2 || f->subclass == 5))
            return i;
    }
    return -1;
}

static struct zzp_dev *D(struct zzp_msd *m) { return &m->b->dev[m->dev]; }

static int bulk(struct zzp_msd *m, int in, void *buf, uint32_t len, uint32_t *actual, uint16_t timeout)
{
    struct zzp_dev *d = D(m);
    struct zzp_xfer x;
    int r;
    memset(&x, 0, sizeof(x));
    x.type = XT_BULK;
    x.addr = d->addr;
    x.ep = (uint8_t)((in ? m->ep_in : m->ep_out) & 0x0f);
    x.dir_in = (uint8_t)(in ? 1 : 0);
    x.speed = d->speed;
    x.maxpkt = in ? m->mps_in : m->mps_out;
    x.tt_hub = d->tt_hub; x.tt_port = d->tt_port; x.tt_multi = d->tt_multi; x.tt_think = d->tt_think;
    x.timeout_ms = timeout;
    x.data = (uint8_t *)buf;
    x.len = len;
    m->b->xfers++;
    r = m->b->t->xfer(m->b->t->ctx, &x);
    if (r != XS_OK) m->b->errors++;
    if (actual) *actual = x.actual;
    return r;
}

/* a halted endpoint: CLEAR_FEATURE(ENDPOINT_HALT), and the transport resets its data toggle */
static void clear_halt(struct zzp_msd *m, int in)
{
    struct zzp_dev *d = D(m);
    uint8_t ep = in ? m->ep_in : m->ep_out;
    zzp_control(m->b, d, RT_OUT | RT_STANDARD | 0x02, REQ_CLEAR_FEATURE, FEATURE_ENDPOINT_HALT, ep, NULL, 0, NULL);
    if (m->b->t->clear_toggle)
        m->b->t->clear_toggle(m->b->t->ctx, d->addr, (uint8_t)(ep & 0x0f), (uint8_t)(in ? 1 : 0));
}

static void reset_recovery(struct zzp_msd *m)
{
    m->resets++;
    zzp_control(m->b, D(m), RT_OUT | RT_CLASS | RT_INTERFACE, REQ_MSD_RESET, 0, m->iface, NULL, 0, NULL);
    clear_halt(m, 1);
    clear_halt(m, 0);
}

int zzp_msd_scsi(struct zzp_msd *m, const uint8_t *cdb, int cdblen, void *data, uint32_t len, int in, uint32_t *actual)
{
    uint8_t cbw[31], csw[13];
    uint32_t got = 0, tag = ++m->tag, residue;
    int r, tries;

    if (actual) *actual = 0;
    memset(cbw, 0, sizeof(cbw));
    put32le(cbw, CBW_SIG);
    put32le(cbw + 4, tag);
    put32le(cbw + 8, len);
    cbw[12] = (uint8_t)(in ? 0x80 : 0x00);
    cbw[13] = m->lun;
    cbw[14] = (uint8_t)cdblen;
    memcpy(cbw + 15, cdb, (size_t)cdblen);

    r = bulk(m, 0, cbw, 31, NULL, 1000);
    if (r == XS_STALL) { reset_recovery(m); return MSD_PROTOCOL; }
    if (r != XS_OK) return r;

    if (len) {
        r = bulk(m, in, data, len, &got, DATA_TIMEOUT);
        if (r == XS_STALL) clear_halt(m, in);   /* the device ends the data early: the status tells why */
        else if (r != XS_OK) { reset_recovery(m); return r; }
    }

    for (tries = 0; tries < 2; tries++) {
        r = bulk(m, 1, csw, 13, &got, 1000);
        if (r == XS_STALL) { clear_halt(m, 1); continue; }
        break;
    }
    if (r != XS_OK || get32le(csw) != CSW_SIG || get32le(csw + 4) != tag) {
        reset_recovery(m);
        return r != XS_OK ? r : MSD_PROTOCOL;
    }
    residue = get32le(csw + 8);
    if (actual) *actual = residue <= len ? len - residue : 0;
    if (csw[12] == 2) { reset_recovery(m); return MSD_PROTOCOL; }
    return csw[12] == 0 ? MSD_OK : MSD_FAILED;
}

static void request_sense(struct zzp_msd *m)
{
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 }, s[18];
    uint32_t got = 0;
    memset(s, 0, sizeof(s));
    if (zzp_msd_scsi(m, cdb, 6, s, 18, 1, &got) == MSD_OK && got >= 14) {
        m->sense_key = (uint8_t)(s[2] & 0x0f);
        m->asc = s[12];
        m->ascq = s[13];
    }
}

/* a command that failed: read why */
static int checked(struct zzp_msd *m, int r)
{
    if (r == MSD_FAILED) request_sense(m);
    return r;
}

static void trimcopy(char *dst, const uint8_t *src, int n)
{
    int i;
    for (i = 0; i < n; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : ' ';
    dst[n] = 0;
    while (n > 0 && dst[n - 1] == ' ') dst[--n] = 0;
}

int zzp_msd_init(struct zzp_msd *m, struct zzp_bus *b, int dev)
{
    const struct zzp_dev *d = &b->dev[dev];
    const struct zzp_if *f;
    uint8_t buf[36], lun = 0;
    uint32_t got = 0;
    int i, r, fi = zzp_msd_find(d);

    memset(m, 0, sizeof(*m));
    m->b = b;
    m->dev = dev;
    m->blksize = 512;
    if (fi < 0) return MSD_PROTOCOL;
    f = &d->ifs[fi];
    m->iface = f->number;
    for (i = 0; i < f->nep; i++) {
        if ((f->ep[i].attr & 3) != 2) continue;
        if (f->ep[i].addr & 0x80) { m->ep_in = f->ep[i].addr; m->mps_in = f->ep[i].maxpkt; }
        else { m->ep_out = f->ep[i].addr; m->mps_out = f->ep[i].maxpkt; }
    }
    if (!m->ep_in || !m->ep_out) return MSD_PROTOCOL;

    /* how many logical units: many sticks stall this, which means one */
    if (zzp_control(b, d, RT_IN | RT_CLASS | RT_INTERFACE, REQ_GET_MAX_LUN, 0, m->iface, &lun, 1, &got) == XS_OK && got == 1)
        m->max_lun = lun;

    {   /* INQUIRY */
        uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
        memset(buf, 0, sizeof(buf));
        r = checked(m, zzp_msd_scsi(m, cdb, 6, buf, 36, 1, &got));
        if (r != MSD_OK) return r;
        m->removable = (buf[1] & 0x80) ? 1 : 0;
        trimcopy(m->vendor, buf + 8, 8);
        trimcopy(m->product, buf + 16, 16);
    }
    /* TEST UNIT READY until the medium is there (the first one usually reports a unit attention) */
    for (i = 0; i < 20; i++) {
        uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
        r = checked(m, zzp_msd_scsi(m, cdb, 6, NULL, 0, 0, NULL));
        if (r == MSD_OK) break;
        if (r == MSD_FAILED && m->sense_key == 2 && m->asc == 0x3a) return MSD_NOMEDIA;   /* medium not present */
        b->t->delay_ms(b->t->ctx, 100);
    }
    if (r != MSD_OK) return r;
    {   /* READ CAPACITY(10) */
        uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        r = checked(m, zzp_msd_scsi(m, cdb, 10, buf, 8, 1, &got));
        if (r != MSD_OK) return r;
        m->blocks = get32be(buf) + 1;
        m->blksize = get32be(buf + 4);
        if (m->blksize != 512 && m->blksize != 1024 && m->blksize != 2048 && m->blksize != 4096) return MSD_PROTOCOL;
    }
    {   /* MODE SENSE(6) for the write-protect switch; ignored if refused */
        uint8_t cdb[6] = { 0x1a, 0, 0x3f, 0, 4, 0 };
        if (zzp_msd_scsi(m, cdb, 6, buf, 4, 1, &got) == MSD_OK && got >= 4)
            m->write_protected = (buf[2] & 0x80) ? 1 : 0;
    }
    m->ready = 1;
    return MSD_OK;
}

static int rw(struct zzp_msd *m, uint32_t lba, uint32_t count, void *buf, int write)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t per = MSD_CHUNK / m->blksize, got = 0;
    int r;
    if (!m->ready) return MSD_NOMEDIA;
    if (lba > m->blocks || count > m->blocks - lba) return MSD_RANGE;
    while (count) {
        uint32_t n = count < per ? count : per;
        uint8_t cdb[10];
        cdb[0] = (uint8_t)(write ? 0x2a : 0x28); cdb[1] = 0;
        cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16); cdb[4] = (uint8_t)(lba >> 8); cdb[5] = (uint8_t)lba;
        cdb[6] = 0; cdb[7] = (uint8_t)(n >> 8); cdb[8] = (uint8_t)n; cdb[9] = 0;
        r = checked(m, zzp_msd_scsi(m, cdb, 10, p, n * m->blksize, !write, &got));
        if (r != MSD_OK) return r;
        if (got != n * m->blksize) return MSD_PROTOCOL;
        p += n * m->blksize; lba += n; count -= n;
    }
    return MSD_OK;
}

int zzp_msd_read(struct zzp_msd *m, uint32_t lba, uint32_t count, void *buf) { return rw(m, lba, count, buf, 0); }
int zzp_msd_write(struct zzp_msd *m, uint32_t lba, uint32_t count, const void *buf)
{
    if (m->write_protected) return MSD_FAILED;
    return rw(m, lba, count, (void *)buf, 1);
}

int zzp_msd_sync(struct zzp_msd *m)
{
    uint8_t cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int r = zzp_msd_scsi(m, cdb, 10, NULL, 0, 0, NULL);
    return r == MSD_FAILED ? MSD_OK : r;       /* plenty of sticks have no cache to sync */
}

/* ---- partitions ------------------------------------------------------------ */

static int fat_type(uint8_t t)
{
    return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0b || t == 0x0c || t == 0x0e;
}

/* a FAT boot sector: a jump, 512..4096 bytes per sector, a power-of-two cluster size */
static int is_fat_boot(const uint8_t *s)
{
    unsigned bps = (unsigned)(s[11] | (s[12] << 8)), spc = s[13];
    if (s[510] != 0x55 || s[511] != 0xaa) return 0;
    if (s[0] != 0xeb && s[0] != 0xe9) return 0;
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return 0;
    return spc && !(spc & (spc - 1));
}

int zzp_parse_parts(const uint8_t *s, uint32_t disk_blocks, struct zzp_part *out)
{
    int i, n = 0;
    if (s[510] != 0x55 || s[511] != 0xaa) return 0;
    if (is_fat_boot(s)) {                       /* no partition table: the whole disk is one FAT volume */
        out[0].start = 0; out[0].count = disk_blocks; out[0].type = 0;
        return 1;
    }
    for (i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + 16 * i;
        uint32_t start = get32le(e + 8), count = get32le(e + 12);
        if (!fat_type(e[4]) || !count || start >= disk_blocks) continue;
        if (count > disk_blocks - start) count = disk_blocks - start;
        out[n].start = start; out[n].count = count; out[n].type = e[4];
        n++;
    }
    return n;
}

const char *zzp_fat_name(uint8_t t)
{
    switch (t) {
    case 0: return "FAT (no partition table)";
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0e: return "FAT16";
    case 0x0b: case 0x0c: return "FAT32";
    default: return "?";
    }
}
