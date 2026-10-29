/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/* mailbox.h - the ZZ9000 USB proxy as a zzp_transport (AmigaOS only). */
#ifndef ZZP_MAILBOX_H
#define ZZP_MAILBOX_H
#include <exec/types.h>
#include "transport.h"

struct zzp_mailbox {
    volatile UBYTE *base;       /* the card's register base */
    volatile UBYTE *ap;         /* base + 0xA000: the USB aperture */
    ULONG request_id, epoch, caps;
    UWORD zorro;                /* 2 or 3 */
    struct MsgPort *tport;
    struct timerequest *treq;
    int timer_open;
    char why[96];               /* why mb_open failed */
};

int  mb_open(struct zzp_mailbox *m);           /* 1 = ready; else m->why says what is wrong */
void mb_close(struct zzp_mailbox *m);
void mb_transport(struct zzp_mailbox *m, struct zzp_transport *t);

/* The firmware raises an Amiga interrupt (INT6, or INT2 when ZZ9000.CFG asks
 * for it) while a periodic completion is waiting; it must be acknowledged
 * or the line stays asserted and the machine locks up. Install before
 * arming any endpoint, remove after stopping them all. */
int  mb_irq_install(struct zzp_mailbox *m, struct Task *task, ULONG sigmask);
void mb_irq_remove(struct zzp_mailbox *m);
ULONG mb_irq_count(struct zzp_mailbox *m);

#endif
