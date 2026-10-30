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
 *   ZZPlug DUMP [SECS=n] [TO=file]  record a device ZZPlug does not know yet: its
 *                                descriptors and what it sends while you use it, in
 *                                one file to send with a bug report
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
#include <dos/datetime.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "core.h"
#include "mailbox.h"
#include "report.h"
#include "daemon.h"
#include "device.h"
#include "diag.h"
#include "zzpmsg.h"

static const char ver[] __attribute__((used)) = "$VER: ZZPlug 0.8 (29.10.2026)";

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

/* ---- ZZPlug DUMP ---- */
/* "when": the Amiga's date and time, as DOS writes it (a dead clock shows 1978) */
static void when_text(char *out)
{
    struct DateTime dt;
    char day[16], date[16], time[16];
    memset(&dt, 0, sizeof(dt));
    DateStamp(&dt.dat_Stamp);
    dt.dat_Format = FORMAT_DOS;
    dt.dat_StrDay = (STRPTR)day; dt.dat_StrDate = (STRPTR)date; dt.dat_StrTime = (STRPTR)time;
    day[0] = date[0] = time[0] = 0;
    DateToStr(&dt);
    sprintf(out, "%s %s %s", day, date, time);
}

static int ask_daemon(UWORD cmd, LONG arg, char *buf, ULONG len, ULONG *out, struct MsgPort *reply)
{
    struct zzp_msg m;
    struct MsgPort *p;
    memset(&m, 0, sizeof(m));
    m.m.mn_Node.ln_Type = NT_MESSAGE;
    m.m.mn_ReplyPort = reply;
    m.m.mn_Length = sizeof(m);
    m.cmd = cmd; m.arg = arg; m.buf = buf; m.buflen = len;
    if (buf && len) buf[0] = 0;
    Forbid();
    p = FindPort((STRPTR)ZZP_PORTNAME);
    if (p) PutMsg(p, &m.m);
    Permit();
    if (!p) return -2;
    WaitPort(reply);
    GetMsg(reply);
    if (out) { out[0] = m.out[0]; out[1] = m.out[1]; }
    return m.rc;
}

static int dump(LONG secs, const char *to)
{
    struct MsgPort *reply = CreateMsgPort();
    char *buf = AllocMem(32768, MEMF_ANY);
    char line[256];
    FILE *f, *e;
    ULONG out[2], lines = 0, lost = 0;
    LONG ticks, left, shown = -1;
    int r, rc = RETURN_OK;

    if (!reply || !buf) { printf("ZZPlug DUMP: out of memory\n"); rc = RETURN_FAIL; goto done; }
    if (!running()) {
        printf("ZZPlug is not running: start it first (ZZPlugPrefs, or ZZPlug START), plug the device\n"
               "in, and run ZZPlug DUMP again.\n");
        rc = RETURN_WARN; goto done;
    }
    if (!(f = fopen(to, "w"))) { printf("ZZPlug DUMP: cannot write %s\n", to); rc = RETURN_ERROR; goto done; }
    if (secs < 5) secs = 5;
    if (secs > 600) secs = 600;

    when_text(line);
    fprintf(f, "ZZPlug DUMP - %s, Kickstart %d.%d\nRecorded: %s\n", ver + 6,
            SysBase->LibNode.lib_Version, SysBase->SoftVer, line);
    fprintf(f, "Send this file with a report at https://github.com/lainejones/ZZPlug/issues\n\n");
    if ((e = fopen(ZZP_ENVFILE, "r"))) {        /* what is plugged in, as LIST VERBOSE shows it */
        while (fgets(line, sizeof(line), e)) fputs(line, f);
        fclose(e);
        fputc('\n', f);
    }
    ask_daemon(ZZP_MSG_DUMP, 0, buf, 32768, NULL, reply);
    fputs(buf, f);
    fprintf(f, "\n== what the devices sent (only reports that changed; seconds since the start)\n");

    printf("ZZPlug DUMP - recording what your USB devices send, for %ld seconds.\n\n"
           "Now use the device, one thing at a time, with a short pause in between:\n"
           "  - press each button once, and hold it a moment\n"
           "  - move each stick, wheel, slider or dial all the way both ways\n"
           "Write down the order you did things in - it helps us match them up.\n"
           "Ctrl-C stops early.\n\n", (long)secs);
    for (ticks = secs * 50; ticks > 0; ticks -= 10) {
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) { printf("\n(stopped early)"); break; }
        Delay(10);
        r = ask_daemon(ZZP_MSG_DUMP, 1, buf, 32768, out, reply);
        if (r == -2) { printf("\nZZPlug stopped"); break; }
        fputs(buf, f);
        lines += out[0];
        lost = out[1];
        if (r == 1) { printf("\nA device was plugged in or out - the recording ended there."); break; }
        left = (ticks + 49) / 50;
        if (left != shown) {
            shown = left;
            printf("\r  %3ld s left, %lu reports recorded ", (long)left, (unsigned long)lines);
            fflush(stdout);
        }
    }
    ask_daemon(ZZP_MSG_DUMP, 1, buf, 32768, out, reply);     /* what came in the last moment */
    fputs(buf, f);
    lines += out[0];
    lost = out[1];
    ask_daemon(ZZP_MSG_DUMP, 2, NULL, 0, NULL, reply);
    fprintf(f, "== end: %lu reports%s\n", (unsigned long)lines, lost ? " (some were lost: too many at once)" : "");
    fclose(f);
    printf("\n\nDone: %lu reports. Saved to %s\n\n"
           "Please send it with a report at\n  https://github.com/lainejones/ZZPlug/issues\n"
           "(New issue, then \"A device that doesn't work\"): what the device is, what\n"
           "happens, when, where, how - and what you pressed or moved in which order.\n",
           (unsigned long)lines, to);
done:
    if (buf) FreeMem(buf, 32768);
    if (reply) DeleteMsgPort(reply);
    return rc;
}

int main(void)
{
    LONG args[11] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda;
    int n, verbose, quiet, start, stopit;
    const char * volatile keepver = ver;

    (void)keepver;
    rda = ReadArgs((STRPTR)"LIST/S,VERBOSE/S,START/S,STOP/S,QUIET/S,DIAG/S,ALL/S,RESET/S,DUMP/S,SECS/K/N,TO/K", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"ZZPlug"); return RETURN_FAIL; }
    if (args[8]) {
        char to[256];
        LONG secs = args[9] ? *(LONG *)args[9] : 30;
        strncpy(to, args[10] ? (char *)args[10] : "RAM:ZZPlug-dump.txt", sizeof(to) - 1);
        to[sizeof(to) - 1] = 0;
        FreeArgs(rda);
        return dump(secs, to);
    }
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
