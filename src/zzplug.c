/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * zzplug.c - the ZZPlug command.
 *
 *   ZZPlug [LIST] [VERBOSE]
 *
 * LIST (the default) resets the ZZ9000's USB port, finds everything on it
 * and prints the tree: hubs, what hangs on which port, names, speeds, what
 * kind of device it is, and in plain words anything that went wrong.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "core.h"
#include "mailbox.h"
#include "usbdefs.h"

static const char ver[] __attribute__((used)) = "$VER: ZZPlug 0.1 (29.10.2026)";

static struct zzp_mailbox MB;
static struct zzp_transport T;
static struct zzp_bus B;

static const char *ep_type(uint8_t attr)
{
    switch (attr & 3) { case 0: return "control"; case 1: return "iso"; case 2: return "bulk"; default: return "interrupt"; }
}

static void indent(int depth)
{
    int i;
    for (i = 0; i < depth; i++) printf("    ");
}

static void what(const struct zzp_dev *d, char *buf)
{
    int i;
    buf[0] = 0;
    if (d->cls && d->cls != CLASS_VENDOR) {
        strcpy(buf, zzp_class_name(d->cls, d->subclass, d->proto));
        return;
    }
    for (i = 0; i < d->nif; i++) {
        const struct zzp_if *f = &d->ifs[i];
        const char *n = zzp_class_name(f->cls, f->subclass, f->proto);
        if (strstr(buf, n)) continue;
        if (buf[0]) strcat(buf, ", ");
        strcat(buf, n);
    }
    if (!buf[0]) strcpy(buf, "unknown kind");
}

static void print_dev(int i, int verbose)
{
    const struct zzp_dev *d = &B.dev[i];
    char kind[80];
    int k, j;

    indent(d->depth);
    if (d->parent >= 0) printf("port %d: ", d->port);
    else printf("ZZ9000 port: ");
    if (!d->ok && !d->vid) {
        printf("a %s device that could not be read - %s\n", zzp_speed_name(d->speed), d->note);
        return;
    }
    what(d, kind);
    printf("%s", d->product[0] ? d->product : "(no name)");
    if (d->manufacturer[0]) printf(" by %s", d->manufacturer);
    printf(" [%04X:%04X], %s, %s", d->vid, d->pid, zzp_speed_name(d->speed), kind);
    if (d->is_hub) printf(", %d ports%s", d->hub_ports, d->hub_multi_tt ? ", multi-TT" : "");
    printf("\n");
    if (!d->ok && d->note[0]) { indent(d->depth); printf("    problem: %s\n", d->note); }
    if (verbose) {
        indent(d->depth);
        printf("    address %d, USB %x.%02x, EP0 %d bytes, %s, up to %d mA",
               d->addr, d->bcd_usb >> 8, d->bcd_usb & 0xff, d->maxpkt0,
               d->self_powered ? "self-powered" : "bus-powered", d->max_power_ma);
        if (d->tt_hub) printf(", split via hub %d port %d", d->tt_hub, d->tt_port);
        if (d->serial[0]) printf(", serial %s", d->serial);
        printf("\n");
        for (k = 0; k < d->nif; k++) {
            const struct zzp_if *f = &d->ifs[k];
            indent(d->depth);
            printf("    interface %d: %s (class %d/%d/%d)", f->number, zzp_class_name(f->cls, f->subclass, f->proto),
                   f->cls, f->subclass, f->proto);
            for (j = 0; j < f->nep; j++)
                printf(", %s %s %d", ep_type(f->ep[j].attr), (f->ep[j].addr & 0x80) ? "in" : "out", f->ep[j].maxpkt);
            printf("\n");
        }
    }
    for (k = 0; k < B.ndev; k++)
        if (B.dev[k].used && B.dev[k].parent == i)
            print_dev(k, verbose);
}

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda;
    int n, i, verbose;

    rda = ReadArgs((STRPTR)"LIST/S,VERBOSE/S", args, NULL);
    if (!rda) { PrintFault(IoErr(), (STRPTR)"ZZPlug"); return RETURN_FAIL; }
    verbose = args[1] != 0;
    FreeArgs(rda);

    if (!mb_open(&MB)) {
        printf("ZZPlug: %s\n", MB.why);
        return RETURN_WARN;
    }
    mb_transport(&MB, &T);
    zzp_bus_init(&B, &T);
    n = zzp_scan(&B);
    printf("ZZ9000 USB (Zorro %d, protocol v2, capabilities 0x%03lx)\n", MB.zorro, (unsigned long)MB.caps);
    if (!n)
        printf("  %s\n", B.note[0] ? B.note : "nothing found");
    for (i = 0; i < B.ndev; i++)
        if (B.dev[i].used && B.dev[i].parent < 0)
            print_dev(i, verbose);
    if (verbose)
        printf("(%lu transfers, %lu failed)\n", (unsigned long)B.xfers, (unsigned long)B.errors);
    mb_close(&MB);
    return n ? RETURN_OK : RETURN_WARN;
}
