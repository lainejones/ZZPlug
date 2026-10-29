/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * mailbox.c - one request at a time through the ZZ9000's USB proxy
 * aperture (docs/usb-proxy-protocol.md in the zz9000-firmware repository):
 * write the 48-byte header, the 16-byte v2 extension and any OUT data at
 * base+0xA000, ring the doorbell at base+0xDE, poll the status word.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <libraries/configvars.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include "mailbox.h"
#include "usbdefs.h"

#define MNT_MANUFACTURER 0x6d6e
#define ZZ9000_Z3        4
#define ZZ9000_Z2        3
#define APERTURE_OFFSET  0xa000
#define REG_PROXY_CMD    0xde

#define CMD_CONTROL_XFER 0x01
#define CMD_BULK_XFER    0x02
#define CMD_INT_XFER     0x03
#define CMD_RESET_PORT   0x05
#define CMD_CHECK_PORT   0x0c
#define CMD_QUERY_CAPS   0x0d
#define CMD_PERIODIC_ARM  0x11
#define CMD_PERIODIC_REAP 0x12
#define CMD_PERIODIC_STOP 0x13
#define DOORBELL_V2      0x8000

#define ST_OK      0x00
#define ST_PENDING 0x01

#define CAP_BASE   0x0000000fUL   /* v2, request IDs, epochs, validation */

#define FLAG_SPLIT     0x0001
#define FLAG_MULTI_TT  0x0004
#define FLAG_THINK_SHIFT 4

#define HDR   48
#define EXT   16
#define DATA  64
#define DATA_MAX 16384

struct ExpansionBase *ExpansionBase;

/* ---- the header, field by field at its documented offsets (big-endian = native) ---- */
static void w16(volatile UBYTE *p, UWORD v) { *(volatile UWORD *)p = v; }
static void w32(volatile UBYTE *p, ULONG v) { *(volatile ULONG *)p = v; }
static UWORD r16(volatile UBYTE *p) { return *(volatile UWORD *)p; }
static ULONG r32(volatile UBYTE *p) { return *(volatile ULONG *)p; }

static void wait_us(struct zzp_mailbox *m, ULONG us)
{
    if (!m->timer_open) { Delay(1); return; }
    m->treq->tr_node.io_Command = TR_ADDREQUEST;
    m->treq->tr_time.tv_secs = us / 1000000UL;
    m->treq->tr_time.tv_micro = us % 1000000UL;
    DoIO((struct IORequest *)m->treq);
}

/* one request through the primary mailbox; data_out/in may be NULL */
static int request(struct zzp_mailbox *m, UWORD cmd, volatile UBYTE *hdr_fill, ULONG timeout_ms,
                   const UBYTE *out, ULONG out_len, UBYTE *in, ULONG in_cap, ULONG *actual, int is_query)
{
    volatile UBYTE *ap = m->ap;
    ULONG id, waited, limit, got, i;
    UWORD status;

    (void)hdr_fill;
    if (out_len > DATA_MAX || in_cap > DATA_MAX) return XS_BADPARAM;
    if (r16(ap + 2) == ST_PENDING) return XS_HOSTERROR;      /* someone else's request is in flight */

    id = ++m->request_id;
    w16(ap + 0, cmd);
    w16(ap + 2, ST_PENDING);
    w32(ap + 24, timeout_ms);
    /* the v2 extension */
    w16(ap + HDR + 0, 2);
    w16(ap + HDR + 2, 64);
    w32(ap + HDR + 4, id);
    w32(ap + HDR + 8, is_query ? 0 : m->epoch);
    w32(ap + HDR + 12, m->caps);
    for (i = 0; i < out_len; i++)
        ap[DATA + i] = out[i];
    CacheClearU();
    w16(m->base + REG_PROXY_CMD, (UWORD)(cmd | DOORBELL_V2));

    limit = (timeout_ms ? timeout_ms : 1000) * 1000UL + 150000UL;
    for (waited = 0; ; waited += 1000) {
        CacheClearE((APTR)ap, HDR + EXT, CACRF_ClearD);
        status = r16(ap + 2);
        if (status != ST_PENDING) break;
        if (waited >= limit) return XS_TIMEOUT;
        wait_us(m, 1000);
    }
    if (r32(ap + HDR + 4) != id) return XS_STALE;            /* not our completion */
    if (is_query || cmd == CMD_RESET_PORT || status == 0xf4)
        m->epoch = r32(ap + HDR + 8);
    else if (r32(ap + HDR + 8) != m->epoch)
        return XS_STALE;
    m->caps = r32(ap + HDR + 12);

    got = r32(ap + 20);
    if (got > in_cap) got = in_cap;
    if (in && got) {
        CacheClearE((APTR)(ap + DATA), got, CACRF_ClearD);
        for (i = 0; i < got; i++)
            in[i] = ap[DATA + i];
    }
    if (actual) *actual = got;
    return status;
}

static void clear_header(struct zzp_mailbox *m)
{
    int i;
    for (i = 4; i < HDR; i += 2)
        w16(m->ap + i, 0);
}

/* the parts of the header every transfer has */
static void fill_header(struct zzp_mailbox *m, struct zzp_xfer *x)
{
    volatile UBYTE *ap = m->ap;
    UWORD flags = 0;
    int i;
    clear_header(m);
    w32(ap + 4, x->addr);
    w16(ap + 8, x->ep);
    w16(ap + 10, x->dir_in ? 0x80 : 0);
    w16(ap + 12, x->type);
    w16(ap + 14, x->maxpkt);
    w32(ap + 16, x->len);
    w16(ap + 28, x->speed);
    for (i = 0; i < 8; i++)
        ap[32 + i] = x->setup[i];
    if (x->tt_hub && x->speed != SPEED_HIGH) {
        flags = FLAG_SPLIT | (x->tt_multi ? FLAG_MULTI_TT : 0) | (UWORD)((x->tt_think & 3) << FLAG_THINK_SHIFT);
        w16(ap + 40, x->tt_hub);
        w16(ap + 42, x->tt_port);
    }
    w16(ap + 44, flags);
}

static int mb_periodic(void *ctx, int op, struct zzp_xfer *x, uint16_t interval, uint16_t generation)
{
    struct zzp_mailbox *m = (struct zzp_mailbox *)ctx;
    UWORD cmd = op == PO_ARM ? CMD_PERIODIC_ARM : op == PO_REAP ? CMD_PERIODIC_REAP : CMD_PERIODIC_STOP;
    ULONG actual = 0;
    int r;

    x->type = XT_INTERRUPT;
    fill_header(m, x);
    w16(m->ap + 30, interval);
    w16(m->ap + 46, generation);
    r = request(m, cmd, NULL, 100, NULL, 0,
                op == PO_REAP ? x->data : NULL, op == PO_REAP ? x->len : 0, &actual, 0);
    x->actual = actual;
    return r;
}

static int mb_requery(void *ctx)
{
    struct zzp_mailbox *m = (struct zzp_mailbox *)ctx;
    clear_header(m);
    return request(m, CMD_QUERY_CAPS, NULL, 100, NULL, 0, NULL, 0, NULL, 1);
}

static int mb_xfer(void *ctx, struct zzp_xfer *x)
{
    struct zzp_mailbox *m = (struct zzp_mailbox *)ctx;
    UWORD cmd;
    ULONG actual = 0;
    int r;

    if (x->len > DATA_MAX) return XS_BADPARAM;
    cmd = x->type == XT_CONTROL ? CMD_CONTROL_XFER : x->type == XT_BULK ? CMD_BULK_XFER : CMD_INT_XFER;
    fill_header(m, x);
    r = request(m, cmd, NULL, x->timeout_ms,
                x->dir_in ? NULL : x->data, x->dir_in ? 0 : x->len,
                x->dir_in ? x->data : NULL, x->dir_in ? x->len : 0, &actual, 0);
    x->actual = x->dir_in ? actual : (r == XS_OK ? x->len : 0);
    return r;
}

static int mb_root_check(void *ctx, int *speed)
{
    struct zzp_mailbox *m = (struct zzp_mailbox *)ctx;
    int r;
    clear_header(m);
    r = request(m, CMD_CHECK_PORT, NULL, 100, NULL, 0, NULL, 0, NULL, 0);
    if (r == ST_OK) *speed = r16(m->ap + 28);
    return r;
}

static int mb_root_reset(void *ctx, int *speed)
{
    struct zzp_mailbox *m = (struct zzp_mailbox *)ctx;
    int r;
    clear_header(m);
    r = request(m, CMD_RESET_PORT, NULL, 500, NULL, 0, NULL, 0, NULL, 0);
    if (r != ST_OK) return r;
    wait_us(m, 20000);
    return mb_root_check(ctx, speed);
}

static void mb_delay(void *ctx, int ms)
{
    wait_us((struct zzp_mailbox *)ctx, (ULONG)ms * 1000UL);
}

/* is another USB driver holding the port? zzusbhw.device open = Poseidon */
static int poseidon_has_it(void)
{
    struct Node *n;
    int open = 0;
    Forbid();
    n = FindName(&SysBase->DeviceList, (STRPTR)"zzusbhw.device");
    if (n && ((struct Library *)n)->lib_OpenCnt > 0) open = 1;
    Permit();
    return open;
}

int mb_open(struct zzp_mailbox *m)
{
    struct ConfigDev *cd;
    int r;

    memset(m, 0, sizeof(*m));
    if (poseidon_has_it()) {
        strcpy(m->why, "Poseidon is using the ZZ9000's USB port (zzusbhw.device is open)");
        return 0;
    }
    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library", 37))) {
        strcpy(m->why, "no expansion.library");
        return 0;
    }
    cd = FindConfigDev(NULL, MNT_MANUFACTURER, ZZ9000_Z3);
    m->zorro = 3;
    if (!cd) { cd = FindConfigDev(NULL, MNT_MANUFACTURER, ZZ9000_Z2); m->zorro = 2; }
    CloseLibrary((struct Library *)ExpansionBase);
    ExpansionBase = NULL;
    if (!cd) { strcpy(m->why, "no ZZ9000 found"); return 0; }
    m->base = (volatile UBYTE *)cd->cd_BoardAddr;
    m->ap = m->base + APERTURE_OFFSET;

    if ((m->tport = CreateMsgPort()) &&
        (m->treq = (struct timerequest *)CreateIORequest(m->tport, sizeof(struct timerequest))) &&
        OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)m->treq, 0) == 0)
        m->timer_open = 1;

    /* protocol v2: QUERY_CAPS with request ID 1 and epoch 0 */
    clear_header(m);
    r = request(m, CMD_QUERY_CAPS, NULL, 100, NULL, 0, NULL, 0, NULL, 1);
    if (r != ST_OK || m->epoch == 0 || (m->caps & CAP_BASE) != CAP_BASE) {
        strcpy(m->why, "the ZZ9000 firmware does not offer USB protocol v2 (firmware 2.8 or newer is needed)");
        mb_close(m);
        return 0;
    }
    return 1;
}

void mb_close(struct zzp_mailbox *m)
{
    if (m->timer_open) CloseDevice((struct IORequest *)m->treq);
    if (m->treq) DeleteIORequest((struct IORequest *)m->treq);
    if (m->tport) DeleteMsgPort(m->tport);
    m->timer_open = 0; m->treq = NULL; m->tport = NULL;
}

/* ---- the USB event interrupt ----------------------------------------------- */
#define REG_STATUS        0x04
#define REG_CONFIG_KEY    0xe8
#define REG_CONFIG_PRESENT 0xea
#define CFG_KEY_INT2      5
#define EVENT_PENDING     0x0010
#define EVENT_ACK         0x0108

struct irqdata {
    volatile UBYTE *base;
    struct Task *task;
    ULONG sigmask;
    volatile ULONG count;
};

static struct Interrupt irq;
static struct irqdata irqd;
static int irq_installed, irq_int2;

/* runs in the interrupt: acknowledge the card, wake the task. a1 = is_Data */
static ULONG usb_isr(struct irqdata *d __asm("a1"))
{
    volatile UWORD *st = (volatile UWORD *)(d->base + REG_STATUS);
    if (!(*st & EVENT_PENDING))
        return 0;                               /* not ours: the next server on the chain */
    *st = EVENT_ACK;
    d->count++;
    Signal(d->task, d->sigmask);
    return 1;
}

int mb_irq_install(struct zzp_mailbox *m, struct Task *task, ULONG sigmask)
{
    UWORD present;
    if (irq_installed) return 1;
    /* which level the card raises: ZZ9000.CFG int2 = on moves it to INT2 */
    *(volatile UWORD *)(m->base + REG_CONFIG_KEY) = CFG_KEY_INT2;
    irq_int2 = *(volatile UWORD *)(m->base + REG_CONFIG_KEY) != 0;
    present = *(volatile UWORD *)(m->base + REG_CONFIG_PRESENT);
    if (!present) irq_int2 = 0;
    irqd.base = m->base;
    irqd.task = task;
    irqd.sigmask = sigmask;
    irqd.count = 0;
    memset(&irq, 0, sizeof(irq));
    irq.is_Node.ln_Type = NT_INTERRUPT;
    irq.is_Node.ln_Pri = 0;
    irq.is_Node.ln_Name = (char *)"ZZPlug USB";
    irq.is_Data = &irqd;
    irq.is_Code = (VOID (*)())usb_isr;
    AddIntServer(irq_int2 ? INTB_PORTS : INTB_EXTER, &irq);
    irq_installed = 1;
    return 1;
}

void mb_irq_remove(struct zzp_mailbox *m)
{
    (void)m;
    if (!irq_installed) return;
    RemIntServer(irq_int2 ? INTB_PORTS : INTB_EXTER, &irq);
    irq_installed = 0;
}

ULONG mb_irq_count(struct zzp_mailbox *m)
{
    (void)m;
    return irqd.count;
}

void mb_transport(struct zzp_mailbox *m, struct zzp_transport *t)
{
    t->ctx = m;
    t->xfer = mb_xfer;
    t->root_check = mb_root_check;
    t->root_reset = mb_root_reset;
    t->delay_ms = mb_delay;
    t->periodic = mb_periodic;
    t->requery = mb_requery;
}
