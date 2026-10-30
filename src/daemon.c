/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * daemon.c - ZZPlug START: the background half. It scans the bus, puts
 * every boot-protocol mouse and keyboard to work through input.device and
 * watches the root port and the hubs for plugging. The firmware polls the
 * interrupt endpoints on schedule; here they are reaped every few
 * milliseconds and re-armed. Anything plugged or unplugged means a rescan.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <devices/input.h>
#include <devices/timer.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "core.h"
#include "hid.h"
#include "report.h"
#include "usbdefs.h"
#include "msd.h"
#include "device.h"
#include "hidrep.h"
#include "joy.h"
#include "daemon.h"
#include "zzpmsg.h"
#include <stdarg.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>

#define DRV_MOUSE 1
#define DRV_KBD   2
#define DRV_HUB   3
#define DRV_PAD   4
#define DRV_RECORD 5            /* ZZPlug DUMP: an interface nothing drives, read for the record */
#define MAXDRV    12
#define TICK_MS   20
#define ROOT_CHECK_MS 500

#define HID_SET_REPORT   0x09
#define HID_SET_IDLE     0x0a
#define HID_SET_PROTOCOL 0x0b

struct drv {
    uint8_t kind;
    int dev;
    uint8_t iface, ep;
    uint16_t maxpkt, interval;
    uint8_t leds;
    uint8_t fails;
    uint32_t reports, naks, errors;
    int last;                   /* the last status other than NAK */
    struct zzp_mouse m;
    struct zzp_kbd k;
    /* a game pad */
    struct zzp_padmap pm;
    struct zzp_padmap pmraw;    /* buttons as the pad numbers them (ZZPlugPrefs' Learn) */
    int slot;
    uint32_t state;
    uint8_t lastrep[16];
    int lastlen;
    uint8_t recrep[64];         /* ZZPlug DUMP: the last report recorded, to keep only changes */
    int reclen;
};
static int npads;

static struct zzp_mailbox *MB;
static struct zzp_transport T;
static struct zzp_bus B;
static struct drv D[MAXDRV];
static int ND;
static uint16_t generation;
static struct zzp_hid_shared shared;
static struct MsgPort *inport;
static struct IOStdReq *inreq;
static int input_open;
static int rescan_wanted;
static int rec_on, rec_ended;          /* ZZPlug DUMP is recording; the recording ended */

/* ---- USB sticks: one device unit each, kept across rescans by serial ---- */
struct stick {
    int used;                   /* this unit belongs to a stick (seen at least once) */
    int dev;                    /* bus index now, -1 = not plugged in */
    uint16_t vid, pid;
    char serial[ZZP_STRLEN];
    struct zzp_msd msd;
    int status;                 /* MSD_* of the last init */
    int nparts;
    struct zzp_part parts[MAXPART];
    char name[MAXPART][12];     /* DOS device name mounted for each part, "" = none */
    uint32_t mstart[MAXPART], mcount[MAXPART];   /* what that name was mounted with */
};
static struct stick ST[ZZD_UNITS];
static int device_ok;
static char mountmsg[160];

static ULONG dostype_for(uint8_t type)
{
    char v[16];
    /* ENV:ZZPlug.dostype overrides (e.g. 0x4D534800) */
    if (GetVar((STRPTR)"ZZPlug.dostype", (STRPTR)v, sizeof(v), 0) > 0) {
        ULONG d = 0; const char *p = v;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        while (*p) { char c = *p++; d = d * 16 + (ULONG)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }
        return d;
    }
    (void)type;
    return 0x4D534400UL;                        /* "MSD\0": OS 3.2's CrossDOS reads FAT12/16/32 with it (verified
                                                 * on a FAT32 stick); "FAT2" and "MSH\0" give "Not a DOS disk" */
}

static int dos_name_exists(const char *name)
{
    struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
    int found = FindDosEntry(dl, (STRPTR)name, LDF_DEVICES) != NULL;
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return found;
}

/* mount (or remount, when the name was used for different geometry) in the background:
 * Mount waits for the filesystem, and the filesystem waits for this task */
static void mount_part(int unit, int j)
{
    struct stick *s = &ST[unit];
    struct zzp_part *p = &s->parts[j];
    char name[12], file[48], cmd[200];
    FILE *f;
    BPTR in, out;
    int exists;

    if (j == 0) sprintf(name, "USB%d", unit); else sprintf(name, "USB%d_%d", unit, j);
    if (s->name[j][0] && s->mstart[j] == p->start && s->mcount[j] == p->count && dos_name_exists(name))
        return;                                 /* the same layout as mounted: a disk change is enough */
    sprintf(file, "T:ZZPlug-%s", name);
    f = fopen(file, "w");
    if (!f) return;
    fprintf(f, "/* written by ZZPlug: %s on zzusbdisk.device unit %d, %s, blocks %lu-%lu */\n",
            name, unit, zzp_fat_name(p->type), (unsigned long)p->start, (unsigned long)(p->start + p->count - 1));
    fprintf(f, "%s:\n", name);                  /* MountList format: the name, the keywords, then # */
    fprintf(f, "Device = %s\nUnit = %d\nFlags = 0\nFileSystem = %s\nDosType = 0x%08lX\n",
            ZZD_NAME, unit, dostype_for(p->type) == 0x46415400UL ? "L:fat95" : "L:CrossDOSFileSystem",
            (unsigned long)dostype_for(p->type));
    fprintf(f, "Surfaces = 1\nBlocksPerTrack = 1\nSectorSize = %lu\nReserved = 0\nLowCyl = %lu\nHighCyl = %lu\n",
            (unsigned long)s->msd.blksize, (unsigned long)p->start, (unsigned long)(p->start + p->count - 1));
    fprintf(f, "Buffers = 50\nBufMemType = 1\nMaxTransfer = 0x10000\nMask = 0x7FFFFFFE\nStackSize = 4096\nPriority = 5\nGlobVec = -1\nActivate = 1\n#\n");
    fclose(f);
    exists = dos_name_exists(name);
    if (exists)
        sprintf(cmd, "Assign %s: DISMOUNT\nMount %s: FROM %s", name, name, file);
    else
        sprintf(cmd, "Mount %s: FROM %s", name, file);
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (SystemTags((STRPTR)cmd, SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE, NP_StackSize, 8192, TAG_DONE) == -1) {
        Close(in); Close(out);
        sprintf(mountmsg, "could not run Mount for %s:", name);
        return;
    }
    strcpy(s->name[j], name);
    s->mstart[j] = p->start;
    s->mcount[j] = p->count;
    sprintf(mountmsg, "%s: mounted (%s, %lu MB)", name, zzp_fat_name(p->type),
            (unsigned long)((uint64_t)p->count * s->msd.blksize / 1048576UL));
}

/* after a scan: match the storage devices on the bus to units, start them, mount what is new */
static void sticks_after_scan(void)
{
    static uint8_t sector[4096];
    int i, k, j;
    for (k = 0; k < ZZD_UNITS; k++) ST[k].dev = -1;
    for (i = 0; i < B.ndev; i++) {
        struct zzp_dev *u = &B.dev[i];
        struct stick *s = NULL;
        if (!u->used || !u->ok || zzp_msd_find(u) < 0) continue;
        for (k = 0; k < ZZD_UNITS && !s; k++)   /* the same stick as before keeps its unit */
            if (ST[k].used && ST[k].dev < 0 && ST[k].vid == u->vid && ST[k].pid == u->pid && !strcmp(ST[k].serial, u->serial))
                s = &ST[k];
        for (k = 0; k < ZZD_UNITS && !s; k++)   /* else a unit nobody has mounted, else any free one */
            if (!ST[k].used) s = &ST[k];
        for (k = 0; k < ZZD_UNITS && !s; k++)
            if (ST[k].dev < 0) s = &ST[k];
        if (!s) continue;                       /* more sticks than units */
        k = (int)(s - ST);
        if (!s->used || s->vid != u->vid || s->pid != u->pid || strcmp(s->serial, u->serial))
            s->nparts = 0;                      /* a different stick on this unit */
        s->used = 1;
        s->dev = i;
        s->vid = u->vid; s->pid = u->pid;
        strcpy(s->serial, u->serial);
        s->status = zzp_msd_init(&s->msd, &B, i);
        if (s->status != MSD_OK) continue;
        if (zzp_msd_read(&s->msd, 0, 1, sector) == MSD_OK)
            s->nparts = zzp_parse_parts(sector, s->msd.blocks, s->parts);
    }
    for (k = 0; k < ZZD_UNITS; k++) {
        struct stick *s = &ST[k];
        if (!s->used) continue;
        if (s->dev < 0 || s->status != MSD_OK) { zzd_set_media(k, NULL); continue; }
        zzd_set_media(k, &s->msd);
        for (j = 0; j < s->nparts; j++)
            mount_part(k, j);
        if (!s->nparts) sprintf(mountmsg, "the stick on unit %d has no FAT partition ZZPlug can mount", k);
    }
}

static void write_sticks(FILE *f)
{
    int k, j;
    for (k = 0; k < ZZD_UNITS; k++) {
        struct stick *s = &ST[k];
        if (!s->used) continue;
        fprintf(f, "  %s unit %d: ", ZZD_NAME, k);
        if (s->dev < 0) { fprintf(f, "unplugged\n"); continue; }
        if (s->status != MSD_OK) {
            fprintf(f, "not ready (%s, sense %d/%02X/%02X)\n", s->status == MSD_NOMEDIA ? "no medium" : zzp_status_name(s->status),
                    s->msd.sense_key, s->msd.asc, s->msd.ascq);
            continue;
        }
        fprintf(f, "%s %s, %lu MB%s; %lu reads, %lu writes, %lu errors\n", s->msd.vendor, s->msd.product,
                (unsigned long)((uint64_t)s->msd.blocks * s->msd.blksize / 1048576UL), s->msd.write_protected ? ", write-protected" : "",
                (unsigned long)zzd_units[k].reads, (unsigned long)zzd_units[k].writes, (unsigned long)zzd_units[k].errors);
        for (j = 0; j < s->nparts; j++)
            fprintf(f, "    partition %d: %s, %lu MB from block %lu%s%s%s\n", j + 1, zzp_fat_name(s->parts[j].type),
                    (unsigned long)((uint64_t)s->parts[j].count * s->msd.blksize / 1048576UL), (unsigned long)s->parts[j].start,
                    s->name[j][0] ? " -> " : "", s->name[j], s->name[j][0] ? ":" : "");
    }
    if (mountmsg[0]) fprintf(f, "  %s\n", mountmsg);
}

static void inject(const struct zzp_event *ev, int n)
{
    struct InputEvent ie;
    int i;
    if (!input_open) return;
    for (i = 0; i < n; i++) {
        memset(&ie, 0, sizeof(ie));
        ie.ie_Class = ev[i].cls == ZE_RAWKEY ? IECLASS_RAWKEY : IECLASS_RAWMOUSE;
        ie.ie_Code = ev[i].code;
        ie.ie_Qualifier = ev[i].qual;
        ie.ie_X = ev[i].dx;
        ie.ie_Y = ev[i].dy;
        inreq->io_Command = IND_WRITEEVENT;
        inreq->io_Data = &ie;
        inreq->io_Length = sizeof(ie);
        DoIO((struct IORequest *)inreq);
    }
}

/* a transfer description for a driver's interrupt endpoint */
static void ep_xfer(struct drv *d, struct zzp_xfer *x, uint8_t *buf, uint32_t len)
{
    struct zzp_dev *u = &B.dev[d->dev];
    memset(x, 0, sizeof(*x));
    x->type = XT_INTERRUPT;
    x->addr = u->addr;
    x->ep = (uint8_t)(d->ep & 0x0f);
    x->dir_in = 1;
    x->speed = u->speed;
    x->maxpkt = d->maxpkt;
    x->tt_hub = u->tt_hub; x->tt_port = u->tt_port; x->tt_multi = u->tt_multi; x->tt_think = u->tt_think;
    x->data = buf;
    x->len = len;
}

static int arm(struct drv *d)
{
    struct zzp_xfer x;
    ep_xfer(d, &x, NULL, d->maxpkt);
    return T.periodic(T.ctx, PO_ARM, &x, d->interval, generation);
}

static void release_all(void)
{
    struct zzp_event ev[ZZP_MAXEVENTS];
    int i;
    for (i = 0; i < ND; i++) {
        if (D[i].kind == DRV_MOUSE) inject(ev, zzp_mouse_release(&D[i].m, ev));
        if (D[i].kind == DRV_KBD) inject(ev, zzp_kbd_release(&D[i].k, ev));
    }
}

static void stop_all(void)
{
    struct zzp_xfer x;
    int i;
    for (i = 0; i < ND; i++) {
        ep_xfer(&D[i], &x, NULL, D[i].maxpkt);
        T.periodic(T.ctx, PO_STOP, &x, D[i].interval, generation);
    }
}

static uint16_t interval_for(const struct zzp_dev *u, uint8_t binterval)
{
    if (u->speed == SPEED_HIGH) {               /* 2^(bInterval-1) microframes */
        int e = binterval ? binterval - 1 : 0;
        if (e > 15) e = 15;
        return (uint16_t)(1u << e);
    }
    return binterval ? binterval : 10;          /* frames (ms) */
}

static void add_drv(int di, int kind, const struct zzp_if *f)
{
    struct zzp_dev *u = &B.dev[di];
    struct drv *d;
    int j;
    if (ND >= MAXDRV) return;
    for (j = 0; j < f->nep; j++)
        if ((f->ep[j].attr & 3) == 3 && (f->ep[j].addr & 0x80)) break;
    if (j == f->nep) return;                    /* no interrupt IN endpoint */
    d = &D[ND];
    memset(d, 0, sizeof(*d));
    d->kind = (uint8_t)kind;
    d->dev = di;
    d->iface = f->number;
    d->ep = f->ep[j].addr;
    d->maxpkt = f->ep[j].maxpkt ? f->ep[j].maxpkt : 8;
    d->interval = interval_for(u, f->ep[j].interval);
    if (kind == DRV_MOUSE) zzp_mouse_init(&d->m, &shared);
    if (kind == DRV_KBD) zzp_kbd_init(&d->k, &shared);
    if (kind == DRV_PAD) {
        /* the report descriptor says where the stick and the buttons are */
        static uint8_t desc[512];
        uint32_t got = 0;
        if (npads >= JOY_SLOTS) return;
        if (zzp_control(&B, u, RT_IN | RT_STANDARD | RT_INTERFACE, REQ_GET_DESCRIPTOR, 0x2200, f->number,
                        desc, sizeof(desc), &got) != XS_OK || !zzp_padmap_parse(&d->pm, desc, (int)got))
            return;
        d->pmraw = d->pm;
        {   /* the button layout: ENV:ZZPlug.padbuttons.VVVV-PPPP for this kind of pad, else
             * ENV:ZZPlug.padbuttons ("3 2 1 4 ..." = HID buttons for red, blue, green, yellow,
             * reverse, forward, ...), else a known pad's, else as the pad numbers them */
            uint8_t order[PAD_MAXBUTTONS];
            char v[64], name[40];
            int n = 0;
            sprintf(name, "ZZPlug.padbuttons.%04X-%04X", (unsigned)u->vid, (unsigned)u->pid);
            if (GetVar((STRPTR)name, (STRPTR)v, sizeof(v), 0) > 0
                || GetVar((STRPTR)"ZZPlug.padbuttons", (STRPTR)v, sizeof(v), 0) > 0) {
                char *p = v;
                while (*p && n < PAD_MAXBUTTONS) {
                    while (*p == ' ' || *p == ',') p++;
                    if (*p == '-') { order[n++] = PAD_NONE; p++; continue; }   /* no button in this place */
                    if (*p < '0' || *p > '9') break;
                    order[n] = 0;
                    while (*p >= '0' && *p <= '9') order[n] = (uint8_t)(order[n] * 10 + (*p++ - '0'));
                    n++;
                }
            } else
                n = zzp_padmap_order_for(u->vid, u->pid, order);
            if (n) zzp_padmap_reorder(&d->pm, order, n);
        }
        d->slot = npads;
        zzp_control(&B, u, RT_OUT | RT_CLASS | RT_INTERFACE, HID_SET_IDLE, 0, f->number, NULL, 0, NULL);
    }
    if (kind == DRV_MOUSE) {
        /* the report descriptor: a wheel and side buttons if it has them (report protocol);
         * a descriptor that does not read as a mouse leaves it in boot protocol */
        static uint8_t desc[512];
        uint32_t got = 0;
        if (zzp_control(&B, u, RT_IN | RT_STANDARD | RT_INTERFACE, REQ_GET_DESCRIPTOR, 0x2200, f->number,
                        desc, sizeof(desc), &got) == XS_OK && zzp_mousemap_parse(&d->m.map, desc, (int)got)
            && zzp_control(&B, u, RT_OUT | RT_CLASS | RT_INTERFACE, HID_SET_PROTOCOL, 1, f->number,
                           NULL, 0, NULL) == XS_OK)
            d->m.full = 1;
    }
    if (kind == DRV_MOUSE || kind == DRV_KBD) {
        /* boot protocol (unless the mouse reads in full), and reports only on change */
        if (!(kind == DRV_MOUSE && d->m.full))
            zzp_control(&B, u, RT_OUT | RT_CLASS | RT_INTERFACE, HID_SET_PROTOCOL, 0, f->number, NULL, 0, NULL);
        zzp_control(&B, u, RT_OUT | RT_CLASS | RT_INTERFACE, HID_SET_IDLE, 0, f->number, NULL, 0, NULL);
    }
    if (kind == DRV_KBD) {                      /* the lights from the start: num lock on, caps as it is */
        d->leds = zzp_kbd_leds(&d->k);
        zzp_control(&B, u, RT_OUT | RT_CLASS | RT_INTERFACE, HID_SET_REPORT, 0x0200, f->number, &d->leds, 1, NULL);
    }
    if (arm(d) == XS_OK) {
        ND++;
        if (kind == DRV_PAD) { joy_set(d->slot, 1, 0); npads++; }
    }
}

static void write_env(void)
{
    FILE *f = fopen(ZZP_ENVFILE, "w");
    if (!f) return;
    int i;
    static const char *const kind[] = { "", "mouse", "keyboard", "hub", "game pad", "recording" };
    fprintf(f, "ZZPlug is running (ZZ9000 USB, Zorro %d, protocol v2)\n", MB->zorro);
    zzp_print_tree(f, &B, 1);
    for (i = 0; i < ND; i++) {
        struct drv *d = &D[i];
        int k;
        fprintf(f, "  driver %d: %s on address %d, endpoint %d, every %d %s: %lu reports, %lu empty, %lu errors (last %s)\n",
                i, kind[d->kind], B.dev[d->dev].addr, d->ep & 15, d->interval,
                B.dev[d->dev].speed == SPEED_HIGH ? "microframes" : "ms",
                (unsigned long)d->reports, (unsigned long)d->naks, (unsigned long)d->errors, zzp_status_name(d->last));
        if (d->kind == DRV_MOUSE)
            fprintf(f, "    %s\n", d->m.full ? (d->m.map.wheel.present ? "report protocol: wheel and up to 5 buttons"
                                                                      : "report protocol: up to 5 buttons")
                                         : d->m.map.x.present ? "boot reports (it did not change to report protocol)"
                                                              : "boot protocol: 3 buttons");
        if (d->kind == DRV_MOUSE && d->m.full) {
            const struct zzp_padmap *p = &d->m.map;
            fprintf(f, "    map: ids %d, x %d/%d (%ld..%ld), y %d/%d, wheel %d/%d, pan %d/%d, buttons %d at %d, last",
                    p->uses_ids, p->x.bit, p->x.size, (long)p->x.lmin, (long)p->x.lmax, p->y.bit, p->y.size,
                    p->wheel.present ? p->wheel.bit : -1, p->wheel.size, p->pan.present ? p->pan.bit : -1, p->pan.size,
                    p->nbuttons, p->button[0].bit);
            for (k = 0; k < d->lastlen; k++) fprintf(f, " %02X", d->lastrep[k]);
            fputc(10, f);
        }
        if (d->kind == DRV_PAD) {
            fprintf(f, "    joystick port %d (lowlevel port %d): %d buttons, X %s, Y %s, hat %s; state %08lX; last report",
                    joy_port_of(d->slot) + 1, joy_port_of(d->slot), d->pm.nbuttons,
                    d->pm.x.present ? "yes" : "no", d->pm.y.present ? "yes" : "no", d->pm.hat.present ? "yes" : "no",
                    (unsigned long)d->state);
            for (k = 0; k < d->lastlen; k++) fprintf(f, " %02X", d->lastrep[k]);
            fputc('\n', f);
        }
    }
    fprintf(f, "  USB interrupts: %lu\n", (unsigned long)mb_irq_count(MB));
    write_sticks(f);
    if (B.kicked) fprintf(f, "  (the port looked empty at start; a port reset found the device)\n");
    fclose(f);
}

static void scan(void)
{
    int i, k;
    if (rec_on) { rec_on = 0; rec_ended = 1; }  /* the drivers are rebuilt: the recording ends */
    release_all();
    stop_all();
    ND = 0;
    npads = 0;
    for (k = 0; k < JOY_SLOTS; k++) joy_set(k, 0, 0);
    generation++;
    zzp_scan(&B);
    for (i = 0; i < B.ndev; i++) {
        struct zzp_dev *u = &B.dev[i];
        if (!u->used || !u->ok) continue;
        for (k = 0; k < u->nif; k++) {
            const struct zzp_if *f = &u->ifs[k];
            if (f->cls == CLASS_HUB) add_drv(i, DRV_HUB, f);
            else if (f->cls == CLASS_HID && f->subclass == 1 && f->proto == 2) add_drv(i, DRV_MOUSE, f);
            else if (f->cls == CLASS_HID && f->subclass == 1 && f->proto == 1) add_drv(i, DRV_KBD, f);
            else if (f->cls == CLASS_HID) add_drv(i, DRV_PAD, f);     /* joined only if the descriptor is a pad's */
        }
    }
    if (device_ok) sticks_after_scan();
    write_env();
}

static void set_leds(struct drv *d)
{
    uint8_t leds = zzp_kbd_leds(&d->k);
    if (leds == d->leds) return;
    d->leds = leds;
    zzp_control(&B, &B.dev[d->dev], RT_OUT | RT_CLASS | RT_INTERFACE, HID_SET_REPORT, 0x0200, d->iface, &leds, 1, NULL);
}

/* collect what each endpoint has: reports become input events */
/* ---- ZZPlug DUMP: every report as it changes, for devices ZZPlug does not know yet ---- */
#define REC_MAX 256
struct rec { ULONG ms; uint8_t drv, len; uint8_t data[64]; };
static struct rec REC[REC_MAX];
static int rec_head, rec_tail, rec_nd0;
static ULONG rec_ms, rec_lost;

static void rec_add(int i, const uint8_t *buf, int len)
{
    struct drv *d = &D[i];
    struct rec *e;
    if (len > 64) len = 64;
    if (len == d->reclen) {                     /* only changes, and not an idle stick's jitter */
        int k, jitter = 1;
        for (k = 0; k < len && jitter; k++) {
            int was = d->recrep[k], now = buf[k], diff = now - was;
            if (diff == 0) continue;
            /* a centred 8-bit axis wobbling by 1 or 2; buttons change by whole bits, far from 0x80 */
            if (!(was >= 0x70 && was <= 0x90 && diff >= -2 && diff <= 2)) jitter = 0;
        }
        if (jitter) return;
    }
    d->reclen = len;
    memcpy(d->recrep, buf, (size_t)len);
    if ((rec_head + 1) % REC_MAX == rec_tail) { rec_lost++; return; }
    e = &REC[rec_head];
    e->ms = rec_ms; e->drv = (uint8_t)i; e->len = (uint8_t)len;
    memcpy(e->data, buf, (size_t)len);
    rec_head = (rec_head + 1) % REC_MAX;
}

static void poll_endpoints(void)
{
    uint8_t buf[64];
    struct zzp_event ev[ZZP_MAXEVENTS];
    struct zzp_xfer x;
    int i, r;

    for (i = 0; i < ND; i++) {
        struct drv *d = &D[i];
        ep_xfer(d, &x, buf, d->maxpkt > sizeof(buf) ? sizeof(buf) : d->maxpkt);
        r = T.periodic(T.ctx, PO_REAP, &x, d->interval, generation);
        if (r == XS_NAK) { d->naks++; continue; }
        d->last = r;
        if (r == XS_STALE) { rescan_wanted = 1; return; }
        if (r != XS_OK) d->errors++;
        if (r == XS_OK) {
            d->reports++;
            d->fails = 0;
            if (rec_on && d->kind != DRV_HUB) rec_add(i, buf, (int)x.actual);
            if (d->kind == DRV_MOUSE) {
                d->lastlen = x.actual < sizeof(d->lastrep) ? (int)x.actual : (int)sizeof(d->lastrep);
                memcpy(d->lastrep, buf, (size_t)d->lastlen);
                inject(ev, zzp_mouse_report(&d->m, buf, (int)x.actual, ev));
            }
            else if (d->kind == DRV_KBD) { inject(ev, zzp_kbd_report(&d->k, buf, (int)x.actual, ev)); set_leds(d); }
            else if (d->kind == DRV_HUB && x.actual) rescan_wanted = 1;     /* a port changed */
            else if (d->kind == DRV_RECORD) ;                               /* recorded above, nothing else */
            else if (d->kind == DRV_PAD) {
                uint32_t s = zzp_pad_state(&d->pm, buf, (int)x.actual, NULL);
                d->lastlen = x.actual < sizeof(d->lastrep) ? (int)x.actual : (int)sizeof(d->lastrep);
                memcpy(d->lastrep, buf, (size_t)d->lastlen);
                if (s != 0xffffffffUL) { d->state = s; joy_set(d->slot, 1, s); }
            }
        } else if (++d->fails > 20) {
            rescan_wanted = 1;                  /* the device keeps failing: it is probably gone */
        }
        if (arm(d) == XS_STALE) rescan_wanted = 1;
    }
}

/* ---- ZZPlugPrefs' questions (zzpmsg.h) ---- */
struct sbuf { char *p; ULONG len, used; };

static void bput(struct sbuf *b, const char *fmt, ...)
{
    char tmp[400];
    ULONG n;
    va_list ap;
    va_start(ap, fmt);
    vsprintf(tmp, fmt, ap);
    va_end(ap);
    n = strlen(tmp);
    if (b->used + n + 1 > b->len) n = b->len > b->used + 1 ? b->len - b->used - 1 : 0;
    memcpy(b->p + b->used, tmp, n);
    b->used += n;
    b->p[b->used] = 0;
}

static void kind_of(const struct zzp_dev *u, char *out)
{
    int i;
    out[0] = 0;
    if (u->cls && u->cls != CLASS_VENDOR) { strcpy(out, zzp_class_name(u->cls, u->subclass, u->proto)); return; }
    for (i = 0; i < u->nif; i++) {
        const char *n = zzp_class_name(u->ifs[i].cls, u->ifs[i].subclass, u->ifs[i].proto);
        if (strstr(out, n)) continue;
        if (out[0]) strcat(out, ", ");
        strcat(out, n);
    }
    if (!out[0]) strcpy(out, "unknown kind");
}

/* in a few words: what ZZPlug does with device i */
static void dev_status(int i, char *s)
{
    const struct zzp_dev *u = &B.dev[i];
    char kind[80];
    int k, j;
    s[0] = 0;
    if (!u->ok) { sprintf(s, "problem: %s", u->note[0] ? u->note : "it could not be read"); return; }
    for (k = 0; k < ND; k++) {
        const struct drv *d = &D[k];
        if (d->dev != i) continue;
        if (s[0]) strcat(s, ", ");
        if (d->kind == DRV_HUB) sprintf(s + strlen(s), "hub, %d ports", u->hub_ports);
        else if (d->kind == DRV_MOUSE && d->m.full)
            sprintf(s + strlen(s), "mouse, %d buttons%s, working", d->m.map.nbuttons > 5 ? 5 : d->m.map.nbuttons,
                    d->m.map.wheel.present ? (d->m.map.pan.present ? ", wheel, tilt" : ", wheel") : "");
        else if (d->kind == DRV_MOUSE) strcat(s, "mouse, working");
        else if (d->kind == DRV_KBD) strcat(s, "keyboard, working");
        else if (d->kind == DRV_PAD) sprintf(s + strlen(s), "game pad, joystick port %d", joy_port_of(d->slot) + 1);
    }
    for (k = 0; k < ZZD_UNITS; k++) {
        const struct stick *t = &ST[k];
        if (!t->used || t->dev != i) continue;
        if (s[0]) strcat(s, ", ");
        if (t->status != MSD_OK) { strcat(s, t->status == MSD_NOMEDIA ? "storage, no medium" : "storage, not ready"); continue; }
        if (!t->nparts) { strcat(s, "storage, no FAT partition"); continue; }
        strcat(s, "drive");
        for (j = 0; j < t->nparts; j++)
            if (t->name[j][0]) sprintf(s + strlen(s), " %s:", t->name[j]);
    }
    if (!s[0] && u->is_hub) sprintf(s, "hub, %d ports", u->hub_ports);
    if (!s[0]) { kind_of(u, kind); sprintf(s, "%s - ZZPlug has no driver for it yet", kind); }
}

static void list_dev(struct sbuf *b, int i)
{
    const struct zzp_dev *u = &B.dev[i];
    char st[200];
    int k;
    dev_status(i, st);
    bput(b, "%d\t%d\t%s\t%s\n", i, u->depth, u->product[0] ? u->product : (u->vid ? "(no name)" : "a device"), st);
    for (k = 0; k < B.ndev; k++)
        if (B.dev[k].used && B.dev[k].parent == i) list_dev(b, k);
}

static void detail_dev(struct sbuf *b, int i)
{
    const struct zzp_dev *u;
    char kind[80];
    int k, j;
    static const char *const dname[] = { "", "mouse", "keyboard", "hub", "game pad", "recording" };
    if (i < 0 || i >= B.ndev || !B.dev[i].used) { bput(b, "That device is gone.\n"); return; }
    u = &B.dev[i];
    bput(b, "%s\n", u->product[0] ? u->product : "(no name)");
    if (u->manufacturer[0]) bput(b, "Made by %s\n", u->manufacturer);
    if (u->parent < 0) bput(b, "Plugged into the ZZ9000\n");
    else bput(b, "Port %d of %s\n", u->port, B.dev[(int)u->parent].product[0] ? B.dev[(int)u->parent].product : "a hub");
    if (!u->ok) bput(b, "Problem: %s\n", u->note[0] ? u->note : "it could not be read");
    if (!u->vid) return;
    kind_of(u, kind);
    bput(b, "Kind: %s\n", kind);
    bput(b, "ID %04X:%04X, USB %x.%02x\n", u->vid, u->pid, u->bcd_usb >> 8, u->bcd_usb & 0xff);
    bput(b, "Runs at %s\n", zzp_speed_name(u->speed));
    bput(b, "%s, up to %d mA\n", u->self_powered ? "Self-powered" : "Bus-powered", u->max_power_ma);
    if (u->tt_hub) bput(b, "Address %d, split via hub %d port %d\n", u->addr, u->tt_hub, u->tt_port);
    else bput(b, "Address %d\n", u->addr);
    if (u->serial[0]) bput(b, "Serial %s\n", u->serial);
    for (k = 0; k < ND; k++) {
        const struct drv *d = &D[k];
        if (d->dev != i) continue;
        bput(b, "Driver: %s, %lu reports, %lu errors\n", dname[d->kind], (unsigned long)d->reports, (unsigned long)d->errors);
        if (d->kind == DRV_PAD)
            bput(b, "Joystick port %d, %d buttons%s\n", joy_port_of(d->slot) + 1, d->pm.nbuttons, d->pm.hat.present ? ", hat" : "");
    }
    for (k = 0; k < ZZD_UNITS; k++) {
        const struct stick *t = &ST[k];
        if (!t->used || t->dev != i) continue;
        if (t->status != MSD_OK) { bput(b, "Storage: %s\n", t->status == MSD_NOMEDIA ? "no medium" : "not ready"); continue; }
        bput(b, "%s %s\n", t->msd.vendor, t->msd.product);
        bput(b, "%lu MB%s\n", (unsigned long)((uint64_t)t->msd.blocks * t->msd.blksize / 1048576UL),
             t->msd.write_protected ? ", write-protected" : "");
        bput(b, "%lu reads, %lu writes, %lu errors\n", (unsigned long)zzd_units[k].reads,
             (unsigned long)zzd_units[k].writes, (unsigned long)zzd_units[k].errors);
        for (j = 0; j < t->nparts; j++)
            bput(b, "Partition %d: %s, %lu MB%s%s%s\n", j + 1, zzp_fat_name(t->parts[j].type),
                 (unsigned long)((uint64_t)t->parts[j].count * t->msd.blksize / 1048576UL),
                 t->name[j][0] ? ", as " : "", t->name[j], t->name[j][0] ? ":" : "");
    }
}

static int stop_refused(char *why)
{
    char nm[32];
    if (zzd_mounted(nm, sizeof(nm))) {          /* stopping would pull the device from under a filesystem */
        sprintf(why, "%s: is mounted on a USB stick, and ZZPlug serves it", nm);
        return 1;
    }
    if (!joy_can_close()) {                     /* someone patched ReadJoyPort after us and calls our code */
        strcpy(why, "another program patched ReadJoyPort after ZZPlug");
        return 1;
    }
    return 0;
}

static void bhex(struct sbuf *b, const uint8_t *p, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++)
        bput(b, "%s%02X", k % 16 == 0 ? (k ? "\n      " : "      ") : " ", p[k]);
    bput(b, "\n");
}

/* every device's descriptors, as text; then record from HID interfaces nothing drives */
static void dump_start(struct sbuf *b)
{
    static uint8_t buf[1024];
    uint16_t replen[16];
    uint32_t got;
    int i, k, j;

    for (i = 0; i < B.ndev; i++) {
        struct zzp_dev *u = &B.dev[i];
        char st[160];
        if (!u->used) continue;
        dev_status(i, st);
        bput(b, "== device %d: %s [%04X:%04X]%s%s, %s speed, address %d, ", i,
             u->product[0] ? u->product : "(no name)", u->vid, u->pid,
             u->manufacturer[0] ? " by " : "", u->manufacturer,
             u->speed == SPEED_HIGH ? "high" : u->speed == SPEED_FULL ? "full" : "low", u->addr);
        if (u->parent < 0) bput(b, "on the ZZ9000's port\n");
        else bput(b, "port %d of the hub at address %d\n", u->port, B.dev[(int)u->parent].addr);
        bput(b, "   ZZPlug: %s\n", st[0] ? st : "-");
        if (u->note[0]) bput(b, "   note: %s\n", u->note);
        if (!u->ok || !u->addr) continue;
        got = 0;
        if (zzp_control(&B, u, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_DEVICE << 8, 0, buf, 18, &got) == XS_OK) {
            bput(b, "   device descriptor (%lu bytes):\n", (unsigned long)got);
            bhex(b, buf, got);
        }
        got = 0;
        if (zzp_control(&B, u, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_CONFIG << 8, 0, buf, 9, &got) == XS_OK && got >= 9) {
            uint16_t total = (uint16_t)(buf[2] | (buf[3] << 8));
            if (total > sizeof(buf)) total = sizeof(buf);
            got = 0;
            if (zzp_control(&B, u, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_CONFIG << 8, 0, buf, total, &got) == XS_OK) {
                bput(b, "   configuration descriptor (%lu bytes):\n", (unsigned long)got);
                bhex(b, buf, got);
            }
        }
        {   /* each HID interface's report descriptor length, from its HID descriptor (0x21) */
            uint8_t *c = buf;
            uint32_t clen = got, p = 0;
            int ifnum = -1;
            memset(replen, 0, sizeof(replen));
            while (p + 2 <= clen && c[p] >= 2) {
                if (c[p + 1] == 0x04 && p + 2 < clen) ifnum = c[p + 2];
                else if (c[p + 1] == 0x21 && p + 8 < clen && ifnum >= 0 && ifnum < 16)
                    replen[ifnum] = (uint16_t)(c[p + 7] | (c[p + 8] << 8));
                p += c[p];
            }
        }
        for (k = 0; k < u->nif; k++) {
            const struct zzp_if *f = &u->ifs[k];
            uint32_t want;
            if (f->cls != CLASS_HID) continue;
            want = f->number < 16 && replen[f->number] ? replen[f->number] : sizeof(buf);
            if (want > sizeof(buf)) want = sizeof(buf);
            got = 0;
            if (zzp_control(&B, u, RT_IN | RT_STANDARD | RT_INTERFACE, REQ_GET_DESCRIPTOR, 0x2200, f->number,
                            buf, want, &got) == XS_OK) {
                if (got > want) got = want;
                bput(b, "   interface %d: HID report descriptor (%lu bytes):\n", f->number, (unsigned long)got);
                bhex(b, buf, got);
            }
        }
    }

    /* record: the drivers' reports, and HID interfaces nothing drives get a reader of their own */
    rec_nd0 = ND;
    for (i = 0; i < B.ndev; i++) {
        struct zzp_dev *u = &B.dev[i];
        if (!u->used || !u->ok) continue;
        for (k = 0; k < u->nif; k++) {
            const struct zzp_if *f = &u->ifs[k];
            int taken = 0;
            if (f->cls != CLASS_HID) continue;
            for (j = 0; j < ND; j++) if (D[j].dev == i && D[j].iface == f->number) taken = 1;
            if (taken || ND >= MAXDRV) continue;
            for (j = 0; j < f->nep; j++)
                if ((f->ep[j].attr & 3) == 3 && (f->ep[j].addr & 0x80)) break;
            if (j == f->nep) continue;
            memset(&D[ND], 0, sizeof(D[ND]));
            D[ND].kind = DRV_RECORD;
            D[ND].dev = i;
            D[ND].iface = f->number;
            D[ND].ep = f->ep[j].addr;
            D[ND].maxpkt = f->ep[j].maxpkt ? f->ep[j].maxpkt : 8;
            D[ND].interval = interval_for(u, f->ep[j].interval);
            if (arm(&D[ND]) == XS_OK) ND++;
        }
    }
    for (j = 0; j < ND; j++) D[j].reclen = -1;
    rec_head = rec_tail = 0; rec_lost = 0; rec_ms = 0; rec_ended = 0;
    rec_on = 1;
    bput(b, "== recording from:");
    for (j = 0; j < ND; j++)
        if (D[j].kind != DRV_HUB)
            bput(b, " [address %d interface %d%s]", B.dev[D[j].dev].addr, D[j].iface,
                 D[j].kind == DRV_RECORD ? ", not driven" : "");
    bput(b, "\n");
}

static void dump_stop(void)
{
    struct zzp_xfer x;
    int i;
    if (!rec_on) return;
    for (i = rec_nd0; i < ND; i++) {            /* the readers dump_start added */
        ep_xfer(&D[i], &x, NULL, D[i].maxpkt);
        T.periodic(T.ctx, PO_STOP, &x, D[i].interval, generation);
    }
    ND = rec_nd0;
    rec_on = 0;
}

/* the reports recorded since the last call, as many as fit */
static int dump_drain(struct sbuf *b, ULONG *out)
{
    ULONG n = 0;
    static const char *const kn[] = { "", "mouse", "keyboard", "hub", "game pad", "not driven" };
    while (rec_tail != rec_head && b->used + 280 < b->len) {
        struct rec *e = &REC[rec_tail];
        struct drv *d = &D[e->drv];
        int k;
        bput(b, "%4lu.%02lu s  address %d interface %d (%s):", (unsigned long)(e->ms / 1000),
             (unsigned long)(e->ms % 1000 / 10), B.dev[d->dev].addr, d->iface, kn[d->kind]);
        for (k = 0; k < e->len; k++) bput(b, " %02X", e->data[k]);
        bput(b, "\n");
        rec_tail = (rec_tail + 1) % REC_MAX;
        n++;
    }
    out[0] = n;
    out[1] = rec_lost;
    return rec_ended && rec_tail == rec_head ? 1 : 0;
}

/* answer one message; 1 = ZZPlug should stop */
static int answer(struct zzp_msg *m)
{
    struct sbuf b;
    int k, quit = 0;
    char why[120];
    b.p = m->buf; b.len = m->buflen; b.used = 0;
    if (b.p && b.len) b.p[0] = 0;
    else b.p = NULL;
    m->rc = 0;
    switch (m->cmd) {
    case ZZP_MSG_LIST:
        if (!b.p) break;
        for (k = 0; k < B.ndev; k++)
            if (B.dev[k].used && B.dev[k].parent < 0) list_dev(&b, k);
        if (B.note[0]) bput(&b, "-1\t0\t%s\t\n", B.note);
        break;
    case ZZP_MSG_DETAIL:
        if (b.p) detail_dev(&b, (int)m->arg);
        break;
    case ZZP_MSG_PAD:
        m->rc = -1;
        for (k = 0; k < ND; k++)
            if (D[k].kind == DRV_PAD && D[k].slot == m->arg) {
                struct zzp_dev *u = &B.dev[D[k].dev];
                uint16_t raw = 0;
                if (D[k].lastlen) zzp_pad_state(&D[k].pmraw, D[k].lastrep, D[k].lastlen, &raw);
                m->out[0] = raw;
                m->out[1] = D[k].state;
                m->out[2] = (ULONG)D[k].pmraw.nbuttons;
                if (m->m.mn_Length >= sizeof(struct zzp_msg))   /* an older ZZPlugPrefs has no out[3] */
                    m->out[3] = ((ULONG)u->vid << 16) | u->pid;
                if (b.p) {
                    struct zzp_padmap bi = D[k].pmraw;
                    uint8_t pos[PAD_MAXBUTTONS], order[PAD_MAXBUTTONS];
                    int i, n = zzp_padmap_order_for(u->vid, u->pid, order);
                    bput(&b, "%s\nport %d\n", u->product[0] ? u->product : "Game pad", joy_port_of(D[k].slot) + 1);
                    zzp_padmap_layout(&D[k].pm, &D[k].pmraw, pos);
                    for (i = 0; i < PAD_MAXBUTTONS; i++) bput(&b, i ? " %d" : "%d", pos[i]);
                    if (n) zzp_padmap_reorder(&bi, order, n);
                    zzp_padmap_layout(&bi, &D[k].pmraw, pos);
                    bput(&b, "\n");
                    for (i = 0; i < PAD_MAXBUTTONS; i++) bput(&b, i ? " %d" : "%d", pos[i]);
                    bput(&b, "\n");
                }
                m->rc = 0;
                break;
            }
        break;
    case ZZP_MSG_RESCAN:
        rescan_wanted = 1;
        break;
    case ZZP_MSG_DUMP:
        if (m->arg == 0 && b.p) { dump_stop(); dump_start(&b); }
        else if (m->arg == 1 && b.p) m->rc = (WORD)dump_drain(&b, m->out);
        else dump_stop();
        break;
    case ZZP_MSG_STOP:
        if (stop_refused(why)) {
            m->rc = 1;
            if (b.p) bput(&b, "%s", why);
            sprintf(mountmsg, "ZZPlug STOP refused: %s", why);
        } else
            quit = 1;
        break;
    default:
        m->rc = -1;
    }
    ReplyMsg(&m->m);
    return quit;
}

int zzp_daemon(struct zzp_mailbox *mb, int quiet)
{
    struct MsgPort *pub, *tport = NULL;
    struct timerequest *treq = NULL;
    struct zzp_event ev[ZZP_MAXEVENTS];
    int speed, root_on, r, since_root = 0, i, timer_open = 0, usbsig, env_ticks = 0;
    ULONG sig, usbmask, tmask, pubmask;
    struct zzp_msg *zm;
    int quit = 0;

    Forbid();
    pub = FindPort((STRPTR)ZZP_PORTNAME);
    Permit();
    if (pub) { printf("ZZPlug: already running\n"); return RETURN_WARN; }
    if (!(pub = CreateMsgPort())) return RETURN_FAIL;
    pub->mp_Node.ln_Name = (char *)ZZP_PORTNAME;
    pub->mp_Node.ln_Pri = 0;
    AddPort(pub);
    pubmask = 1UL << pub->mp_SigBit;

    MB = mb;
    mb_transport(mb, &T);
    zzp_bus_init(&B, &T);
    memset(&shared, 0, sizeof(shared));
    if ((inport = CreateMsgPort()) && (inreq = (struct IOStdReq *)CreateIORequest(inport, sizeof(*inreq))) &&
        OpenDevice((STRPTR)"input.device", 0, (struct IORequest *)inreq, 0) == 0)
        input_open = 1;
    else
        printf("ZZPlug: cannot open input.device - mice and keyboards will not work\n");
    SetTaskPri(FindTask(NULL), 10);

    /* the card's "a report is waiting" interrupt wakes us; a timer ticks for
     * key repeat and for watching the root port */
    usbsig = AllocSignal(-1);
    if (usbsig < 0 || !(tport = CreateMsgPort()) ||
        !(treq = (struct timerequest *)CreateIORequest(tport, sizeof(*treq))) ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)treq, 0) != 0) {
        printf("ZZPlug: no signal or timer\n");
        goto out;
    }
    timer_open = 1;
    usbmask = 1UL << usbsig;
    tmask = 1UL << tport->mp_SigBit;
    mb_irq_install(mb, FindTask(NULL), usbmask);
    device_ok = zzd_open();
    if (!device_ok) printf("ZZPlug: could not create %s - USB sticks will not mount\n", ZZD_NAME);
    if (!joy_open()) printf("ZZPlug: no lowlevel.library - game pads will not work\n");

    B.kick_offline = 1;
    scan();
    root_on = B.root_status == XS_OK;
    if (!quiet) {
        printf("ZZPlug is running; ZZPlug STOP or Ctrl-C ends it.\n");
        zzp_print_tree(stdout, &B, 0);
        fflush(stdout);
    }

    treq->tr_node.io_Command = TR_ADDREQUEST;
    treq->tr_time.tv_secs = 0;
    treq->tr_time.tv_micro = TICK_MS * 1000UL;
    SendIO((struct IORequest *)treq);
    for (;;) {
        sig = Wait(usbmask | tmask | pubmask | zzd_sigmask() | SIGBREAKF_CTRL_C);
        if (sig & SIGBREAKF_CTRL_C) {
            char why[120];
            if (!stop_refused(why)) break;
            sprintf(mountmsg, "ZZPlug STOP refused: %s", why);
            write_env();
        }
        while ((zm = (struct zzp_msg *)GetMsg(pub)))
            quit |= answer(zm);
        if (quit) break;
        zzd_service();                          /* reads and writes for the sticks */
        poll_endpoints();                       /* on the interrupt, and on every tick as a safety net */
        if (GetMsg(tport)) {
            for (i = 0; i < ND; i++)
                if (D[i].kind == DRV_KBD) inject(ev, zzp_kbd_tick(&D[i].k, TICK_MS, ev));
            since_root += TICK_MS;
            if (rec_on) rec_ms += TICK_MS;
            if (since_root >= ROOT_CHECK_MS) {
                since_root = 0;
                if (++env_ticks >= 4) { env_ticks = 0; write_env(); }   /* the counters, every 2 s */
                speed = 0;
                r = T.root_check(T.ctx, &speed);
                if (r == XS_STALE) { T.requery(T.ctx); rescan_wanted = 1; }
                else if ((r == XS_OK) != root_on) { root_on = r == XS_OK; rescan_wanted = 1; }
            }
            if (rescan_wanted) {
                rescan_wanted = 0;
                T.requery(T.ctx);
                T.delay_ms(T.ctx, 150);         /* let a freshly plugged device settle */
                scan();
                root_on = B.root_status == XS_OK;
            }
            treq->tr_node.io_Command = TR_ADDREQUEST;
            treq->tr_time.tv_secs = 0;
            treq->tr_time.tv_micro = TICK_MS * 1000UL;
            SendIO((struct IORequest *)treq);
        }
    }
    AbortIO((struct IORequest *)treq);
    WaitIO((struct IORequest *)treq);

out:
    release_all();
    stop_all();                                 /* no completions left: the card drops the interrupt */
    mb_irq_remove(mb);
    joy_close();
    if (device_ok) zzd_close();
    if (timer_open) CloseDevice((struct IORequest *)treq);
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (usbsig >= 0) FreeSignal(usbsig);
    DeleteFile((STRPTR)ZZP_ENVFILE);
    if (input_open) CloseDevice((struct IORequest *)inreq);
    if (inreq) DeleteIORequest((struct IORequest *)inreq);
    if (inport) DeleteMsgPort(inport);
    Forbid();                                   /* nobody finds the port from here on */
    RemPort(pub);
    Permit();
    while ((zm = (struct zzp_msg *)GetMsg(pub))) { zm->rc = -1; ReplyMsg(&zm->m); }
    DeleteMsgPort(pub);
    if (!quiet) printf("ZZPlug stopped\n");
    return RETURN_OK;
}
