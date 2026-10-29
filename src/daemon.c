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
#include "daemon.h"

#define DRV_MOUSE 1
#define DRV_KBD   2
#define DRV_HUB   3
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
};

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
    if (kind != DRV_HUB) {
        /* boot protocol, and reports only on change */
        zzp_control(&B, u, RT_OUT | RT_CLASS | RT_INTERFACE, HID_SET_PROTOCOL, 0, f->number, NULL, 0, NULL);
        zzp_control(&B, u, RT_OUT | RT_CLASS | RT_INTERFACE, HID_SET_IDLE, 0, f->number, NULL, 0, NULL);
    }
    if (arm(d) == XS_OK) ND++;
}

static void write_env(void)
{
    FILE *f = fopen(ZZP_ENVFILE, "w");
    if (!f) return;
    int i;
    static const char *const kind[] = { "", "mouse", "keyboard", "hub" };
    fprintf(f, "ZZPlug is running (ZZ9000 USB, Zorro %d, protocol v2)\n", MB->zorro);
    zzp_print_tree(f, &B, 1);
    for (i = 0; i < ND; i++)
        fprintf(f, "  driver %d: %s on address %d, endpoint %d, every %d %s: %lu reports, %lu empty, %lu errors (last %s)\n",
                i, kind[D[i].kind], B.dev[D[i].dev].addr, D[i].ep & 15, D[i].interval,
                B.dev[D[i].dev].speed == SPEED_HIGH ? "microframes" : "ms",
                (unsigned long)D[i].reports, (unsigned long)D[i].naks, (unsigned long)D[i].errors, zzp_status_name(D[i].last));
    fprintf(f, "  USB interrupts: %lu\n", (unsigned long)mb_irq_count(MB));
    if (B.kicked) fprintf(f, "  (the port looked empty at start; a port reset found the device)\n");
    fclose(f);
}

static void scan(void)
{
    int i, k;
    release_all();
    stop_all();
    ND = 0;
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
        }
    }
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
            if (d->kind == DRV_MOUSE) inject(ev, zzp_mouse_report(&d->m, buf, (int)x.actual, ev));
            else if (d->kind == DRV_KBD) { inject(ev, zzp_kbd_report(&d->k, buf, (int)x.actual, ev)); set_leds(d); }
            else if (d->kind == DRV_HUB && x.actual) rescan_wanted = 1;     /* a port changed */
        } else if (++d->fails > 20) {
            rescan_wanted = 1;                  /* the device keeps failing: it is probably gone */
        }
        if (arm(d) == XS_STALE) rescan_wanted = 1;
    }
}

int zzp_daemon(struct zzp_mailbox *mb, int quiet)
{
    struct MsgPort *pub, *tport = NULL;
    struct timerequest *treq = NULL;
    struct zzp_event ev[ZZP_MAXEVENTS];
    int speed, root_on, r, since_root = 0, i, timer_open = 0, usbsig, env_ticks = 0;
    ULONG sig, usbmask, tmask;

    Forbid();
    pub = FindPort((STRPTR)ZZP_PORTNAME);
    Permit();
    if (pub) { printf("ZZPlug: already running\n"); return RETURN_WARN; }
    if (!(pub = CreateMsgPort())) return RETURN_FAIL;
    pub->mp_Node.ln_Name = (char *)ZZP_PORTNAME;
    pub->mp_Node.ln_Pri = 0;
    AddPort(pub);

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
        sig = Wait(usbmask | tmask | SIGBREAKF_CTRL_C);
        if (sig & SIGBREAKF_CTRL_C) break;
        poll_endpoints();                       /* on the interrupt, and on every tick as a safety net */
        if (GetMsg(tport)) {
            for (i = 0; i < ND; i++)
                if (D[i].kind == DRV_KBD) inject(ev, zzp_kbd_tick(&D[i].k, TICK_MS, ev));
            since_root += TICK_MS;
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
    if (timer_open) CloseDevice((struct IORequest *)treq);
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (usbsig >= 0) FreeSignal(usbsig);
    DeleteFile((STRPTR)ZZP_ENVFILE);
    if (input_open) CloseDevice((struct IORequest *)inreq);
    if (inreq) DeleteIORequest((struct IORequest *)inreq);
    if (inport) DeleteMsgPort(inport);
    RemPort(pub);
    DeleteMsgPort(pub);
    if (!quiet) printf("ZZPlug stopped\n");
    return RETURN_OK;
}
