/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/* report.c - the device tree in plain words, to any stdio stream (portable). */
#include <stdio.h>
#include <string.h>
#include "core.h"
#include "usbdefs.h"
#include "report.h"

static const char *ep_type(uint8_t attr)
{
    switch (attr & 3) { case 0: return "control"; case 1: return "iso"; case 2: return "bulk"; default: return "interrupt"; }
}

static void indent(FILE *o, int depth)
{
    int i;
    for (i = 0; i < depth; i++) fputs("    ", o);
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

static void print_dev(FILE *o, const struct zzp_bus *b, int i, int verbose)
{
    const struct zzp_dev *d = &b->dev[i];
    char kind[80];
    int k, j;

    indent(o, d->depth);
    if (d->parent >= 0) fprintf(o, "port %d: ", d->port);
    else fputs("ZZ9000 port: ", o);
    if (!d->ok && !d->vid) {
        fprintf(o, "a %s device that could not be read - %s\n", zzp_speed_name(d->speed), d->note);
        return;
    }
    what(d, kind);
    fputs(d->product[0] ? d->product : "(no name)", o);
    if (d->manufacturer[0]) fprintf(o, " by %s", d->manufacturer);
    fprintf(o, " [%04X:%04X], %s, %s", d->vid, d->pid, zzp_speed_name(d->speed), kind);
    if (d->is_hub) fprintf(o, ", %d ports%s", d->hub_ports, d->hub_multi_tt ? ", multi-TT" : "");
    fputc('\n', o);
    if (!d->ok && d->note[0]) { indent(o, d->depth); fprintf(o, "    problem: %s\n", d->note); }
    if (verbose) {
        indent(o, d->depth);
        fprintf(o, "    address %d, USB %x.%02x, EP0 %d bytes, %s, up to %d mA",
                d->addr, d->bcd_usb >> 8, d->bcd_usb & 0xff, d->maxpkt0,
                d->self_powered ? "self-powered" : "bus-powered", d->max_power_ma);
        if (d->tt_hub) fprintf(o, ", split via hub %d port %d", d->tt_hub, d->tt_port);
        if (d->serial[0]) fprintf(o, ", serial %s", d->serial);
        fputc('\n', o);
        for (k = 0; k < d->nif; k++) {
            const struct zzp_if *f = &d->ifs[k];
            indent(o, d->depth);
            fprintf(o, "    interface %d: %s (class %d/%d/%d)", f->number, zzp_class_name(f->cls, f->subclass, f->proto),
                    f->cls, f->subclass, f->proto);
            for (j = 0; j < f->nep; j++)
                fprintf(o, ", %s %s %d", ep_type(f->ep[j].attr), (f->ep[j].addr & 0x80) ? "in" : "out", f->ep[j].maxpkt);
            fputc('\n', o);
        }
    }
    for (k = 0; k < b->ndev; k++)
        if (b->dev[k].used && b->dev[k].parent == i)
            print_dev(o, b, k, verbose);
}

void zzp_print_tree(FILE *o, const struct zzp_bus *b, int verbose)
{
    int i, n = 0;
    for (i = 0; i < b->ndev; i++)
        if (b->dev[i].used) n++;
    if (!n)
        fprintf(o, "  %s\n", b->note[0] ? b->note : "nothing found");
    for (i = 0; i < b->ndev; i++)
        if (b->dev[i].used && b->dev[i].parent < 0)
            print_dev(o, b, i, verbose);
    if (verbose)
        fprintf(o, "(%lu transfers, %lu failed)\n", (unsigned long)b->xfers, (unsigned long)b->errors);
}
