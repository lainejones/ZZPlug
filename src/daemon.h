/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
#ifndef ZZP_DAEMON_H
#define ZZP_DAEMON_H
#include "mailbox.h"

#define ZZP_PORTNAME "ZZPlug"
#define ZZP_ENVFILE  "ENV:ZZPlug.devices"

/* run until Ctrl-C or ZZPlug STOP: USB mice and keyboards drive the Amiga,
 * hubs are watched for plugging. Returns a DOS return code. */
int zzp_daemon(struct zzp_mailbox *mb, int quiet);

#endif
