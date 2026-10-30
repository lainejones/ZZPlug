/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * zzplug.c - the ZZPlug command.
 *
 *   ZZPlug [LIST] [VERBOSE]      what is plugged into the ZZ9000's USB port
 *   ZZPlug START [QUIET]         run: USB mice and keyboards work (use Run >NIL:)
 *   ZZPlug STOP                  stop the running ZZPlug
 *   ZZPlug RESET                 one root-port reset, then its diagnostic record (ZZPlug stopped)
 *   ZZPlug DIAG [ALL]            the ZZ9000 firmware's USB diagnostic page (read only)
 *
 * While ZZPlug runs it owns the port, so LIST shows what it found instead
 * of scanning again.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "core.h"
#include "mailbox.h"
#include "report.h"
#include "daemon.h"
#include "device.h"
#include "diag.h"

static const char ver[] __attribute__((used)) = "$VER: ZZPlug 0.7 (29.10.2026)";

static struct zzp_mailbox MB;
static struct zzp_transport T;
static struct zzp_bus B;

static int running(void)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort((STRPTR)ZZP_PORTNAME);
    if (p) Signal(p->mp_SigTask, 0);            /* (only proves it is there) */
    Permit();
    return p != NULL;
}

static int stop(void)
{
    struct MsgPort *p;
    int i;
    {
        char nm[32];
        if (zzd_mounted(nm, sizeof(nm))) {
            printf("ZZPlug keeps running: %s: is mounted on a USB stick and ZZPlug serves it\n", nm);
            return RETURN_WARN;
        }
    }
    Forbid();
    p = FindPort((STRPTR)ZZP_PORTNAME);
    if (p) Signal(p->mp_SigTask, SIGBREAKF_CTRL_C);
    Permit();
    if (!p) { printf("ZZPlug is not running\n"); return RETURN_WARN; }
    for (i = 0; i < 50 && running(); i++)       /* wait up to 5 s for it to let go */
        Delay(5);
    printf(running() ? "ZZPlug did not stop\n" : "ZZPlug stopped\n");
    return running() ? RETURN_ERROR : RETURN_OK;
}

static int show_running(void)
{
    char line[256];
    FILE *f = fopen(ZZP_ENVFILE, "r");
    if (!f) { printf("ZZPlug is running\n"); return RETURN_OK; }
    while (fgets(line, sizeof(line), f))
        fputs(line, stdout);
    fclose(f);
    return RETURN_OK;
}

int main(void)
{
    LONG args[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    int n, verbose, quiet, start, stopit;
    const char * volatile keepver = ver;

    (void)keepver;
    rda = ReadArgs((STRPTR)"LIST/S,VERBOSE/S,START/S,STOP/S,QUIET/S,DIAG/S,ALL/S,RESET/S", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"ZZPlug"); return RETURN_FAIL; }
    verbose = args[1] != 0;
    start = args[2] != 0;
    stopit = args[3] != 0;
    quiet = args[4] != 0;
    FreeArgs(rda);
    if (args[5]) return zzp_diag_print(args[6] != 0);

    if (stopit) return stop();
    if (running()) {
        if (start) { printf("ZZPlug: already running\n"); return RETURN_WARN; }
        return show_running();
    }
    if (!mb_open(&MB)) {
        printf("ZZPlug: %s\n", MB.why);
        return RETURN_WARN;
    }
    if (args[7]) {                              /* one port reset, for the firmware's diagnostic record */
        int speed = 0, r;
        mb_transport(&MB, &T);
        r = T.root_reset(T.ctx, &speed);
        printf("RESET_PORT: %s, speed %d\n", zzp_status_name(r), speed);
        mb_close(&MB);
        return zzp_diag_print(0);
    }
    if (start) {
        n = zzp_daemon(&MB, quiet);
        mb_close(&MB);
        return n;
    }
    mb_transport(&MB, &T);
    zzp_bus_init(&B, &T);
    n = zzp_scan(&B);
    printf("ZZ9000 USB (Zorro %d, protocol v2, capabilities 0x%03lx)\n", MB.zorro, (unsigned long)MB.caps);
    zzp_print_tree(stdout, &B, verbose);
    mb_close(&MB);
    return n ? RETURN_OK : RETURN_WARN;
}
