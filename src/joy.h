/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * joy.h - USB game pads as lowlevel.library joystick ports: ReadJoyPort()
 * is patched so port 1 (joystick port 2) answers with the first pad while
 * one is plugged in, port 2 with the second. Programs that read the
 * joystick hardware directly do not go through it.
 */
#ifndef ZZP_JOY_H
#define ZZP_JOY_H
#include <exec/types.h>

#define JOY_SLOTS 2
int  joy_open(void);                    /* patch ReadJoyPort: 1 ok */
int  joy_can_close(void);               /* 0 = someone patched ReadJoyPort after us: ZZPlug must stay */
void joy_close(void);                   /* put it back */
void joy_set(int slot, int present, ULONG state);   /* PAD_* bits */
int  joy_port_of(int slot);             /* the lowlevel port number a slot answers on */

#endif
