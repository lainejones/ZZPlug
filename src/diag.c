/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * diag.c - ZZPlug DIAG: print the ZZ9000 firmware's USB diagnostic page
 * (the last 4 KiB of the USB aperture, docs/usb-proxy-protocol.md in
 * zz9000-firmware) without sending anything. Port-reset events carry the
 * EHCI PORTSC register in their detail field when the reset found no
 * device, and that is decoded: port power, over-current, connect, line state.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include "diag.h"

#define APERTURE_OFFSET 0xA000
#define DIAG_OFFSET     (24576 - 4096)
#define DIAG_MAGIC      0x5a554447UL

static ULONG be32(const volatile UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }
static UWORD be16(const volatile UBYTE *p) { return (UWORD)((p[0] << 8) | p[1]); }

static const char *type_name(UWORD t)
{
    static const char *const n[] = { "?", "request", "done", "timeout", "late", "cancelled", "port reset",
                                     "host error", "recovery", "stale", "high water", "periodic", "iso" };
    return t < sizeof(n) / sizeof(n[0]) ? n[t] : "?";
}

static const char *cmd_name(ULONG c)
{
    switch (c) {
    case 1: return "CONTROL"; case 2: return "BULK"; case 3: return "INT"; case 5: return "RESET_PORT";
    case 0x0b: return "CLEAR_STALL"; case 0x0c: return "CHECK_PORT"; case 0x0d: return "QUERY_CAPS";
    case 0x0e: return "RETIRE_EP"; case 0x10: return "DIAG_SNAPSHOT"; case 0x11: return "PERIODIC_ARM";
    case 0x12: return "PERIODIC_REAP"; case 0x13: return "PERIODIC_STOP";
    }
    return "?";
}

void zzp_portsc_words(ULONG r, char *o)
{
    static const char *const ls[] = { "SE0", "K", "J", "?" };
    static const char *const spd[] = { "full", "low", "high", "?" };
    sprintf(o, "PORTSC %08lX: %s, %s, power %s, over-current %s%s, lines %s, speed %s%s%s%s",
            (unsigned long)r, (r & 1) ? "connected" : "NOT connected", (r & 4) ? "enabled" : "disabled",
            (r & (1UL << 12)) ? "on" : "OFF", (r & 0x10) ? "ACTIVE" : "no", (r & 0x20) ? " (changed)" : "",
            ls[(r >> 10) & 3], spd[(r >> 26) & 3], (r & 0x100) ? ", in reset" : "", (r & 2) ? ", connect changed" : "",
            (r & 0x80) ? ", suspended" : "");
}

int zzp_diag_print(int all)
{
    struct Library *ExpansionBase;
    struct ConfigDev *cd;
    volatile UBYTE *pg;
    static UBYTE copy[4096];
    ULONG gen1, gen2, next, count, i, first;
    int tries;

    if (!(ExpansionBase = OpenLibrary((STRPTR)"expansion.library", 37))) return 20;
    cd = FindConfigDev(NULL, 0x6d6e, 4);
    if (!cd) cd = FindConfigDev(NULL, 0x6d6e, 3);
    CloseLibrary(ExpansionBase);
    if (!cd) { printf("no ZZ9000 found\n"); return 10; }
    pg = (volatile UBYTE *)cd->cd_BoardAddr + APERTURE_OFFSET + DIAG_OFFSET;

    for (tries = 0; tries < 20; tries++) {      /* copy only a stable, even generation */
        gen1 = be32(pg + 4);
        if (gen1 & 1) continue;
        for (i = 0; i < 4096; i++) copy[i] = pg[i];
        gen2 = be32(pg + 4);
        if (gen1 == gen2) break;
    }
    if (be32(copy) != DIAG_MAGIC) {
        printf("no diagnostic page yet (magic %08lX) - it appears after the first v2 command\n", (unsigned long)be32(copy));
        return 5;
    }
    printf("ZZ9000 USB diagnostics: generation %lu, epoch %lu, last request %lu, capabilities %08lX\n",
           (unsigned long)be32(copy + 4), (unsigned long)be32(copy + 20), (unsigned long)be32(copy + 24),
           (unsigned long)be32(copy + 16));
    printf("queue state %08lX, controller/schedule bits %08lX (USBSTS<<8 | USBCMD)\n",
           (unsigned long)be32(copy + 40), (unsigned long)be32(copy + 44));
    printf("counters: requests %lu, completions %lu, timeouts %lu, late %lu, cancelled %lu, resets %lu, EHCI errors %lu, "
           "recoveries %lu, stale %lu\n",
           (unsigned long)be32(copy + 48), (unsigned long)be32(copy + 52), (unsigned long)be32(copy + 56),
           (unsigned long)be32(copy + 60), (unsigned long)be32(copy + 64), (unsigned long)be32(copy + 68),
           (unsigned long)be32(copy + 72), (unsigned long)be32(copy + 76), (unsigned long)be32(copy + 80));
    next = be32(copy + 28);
    count = be32(copy + 32);
    if (count > 64) count = 64;
    printf("%lu events (%lu lost)%s:\n", (unsigned long)count, (unsigned long)be32(copy + 36),
           all ? "" : ", port resets and errors only (DIAG ALL for every one)");
    first = count < 64 ? 0 : next % 64;
    for (i = 0; i < count; i++) {
        const UBYTE *e = copy + 128 + ((first + i) % 64) * 32;
        UWORD type = be16(e + 20), status = be16(e + 22);
        ULONG detail = be32(e + 12);
        char w[200];
        if (!all && type != 6 && type != 3 && type != 7 && type != 8) continue;
        printf("  #%lu %s, status %02X, addr %u ep %u, request %lu, t=%lu: ", (unsigned long)be32(e), type_name(type), status,
               be16(e + 24), e[28], (unsigned long)be32(e + 4), (unsigned long)be32(e + 16));
        if (type == 1) printf("%s\n", cmd_name(detail));
        else if (type == 6) { zzp_portsc_words(detail, w); printf("%s\n", w); }
        else printf("detail %08lX\n", (unsigned long)detail);
    }
    return 0;
}
