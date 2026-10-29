/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
#ifndef ZZP_DIAG_H
#define ZZP_DIAG_H
#include <exec/types.h>
int  zzp_diag_print(int all);                   /* ZZPlug DIAG: the firmware's diagnostic page */
void zzp_portsc_words(ULONG portsc, char *out); /* EHCI PORTSC in words (out >= 200 bytes) */
#endif
