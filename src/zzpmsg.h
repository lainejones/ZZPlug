/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * zzpmsg.h - how ZZPlugPrefs talks to the running ZZPlug: a message to the
 * public port "ZZPlug", answered with ReplyMsg. Text answers go into the
 * sender's buffer, one item per line.
 */
#ifndef ZZP_MSG_H
#define ZZP_MSG_H
#include <exec/ports.h>

#define ZZP_MSG_LIST   1    /* buf <- "index<TAB>depth<TAB>name<TAB>what ZZPlug does with it" per device;
                             * a line "-1<TAB>0<TAB>note<TAB>" when the bus has a remark */
#define ZZP_MSG_DETAIL 2    /* arg = device index; buf <- its details, one per line */
#define ZZP_MSG_PAD    3    /* the first game pad: out[0] = HID buttons as the pad numbers them (bit 0 =
                             * button 1), out[1] = what lowlevel.library reads, out[2] = number of
                             * buttons; rc = -1 when no pad is plugged in */
#define ZZP_MSG_RESCAN 4    /* settings changed: read ENV: again and enumerate everything again */
#define ZZP_MSG_STOP   5    /* end ZZPlug; rc = 1 and the reason in buf when it has to stay */

struct zzp_msg {
    struct Message m;
    UWORD cmd;
    WORD  rc;
    LONG  arg;
    ULONG out[3];
    char *buf;
    ULONG buflen;
};

#endif
