/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/* simtest.c - ZZPlug's USB logic against the simulated bus. */
#include <stdio.h>
#include <string.h>
#include "sim.h"
#include "../src/core.h"
#include "../src/usbdefs.h"

static int fails;
#define CHECK(c, m) do { if (c) printf("  ok   %s\n", m); else { printf("  FAIL %s\n", m); fails++; } } while (0)

static struct sim S;
static struct zzp_transport T;
static struct zzp_bus B;

static int find_kind(const char *product)
{
    int i;
    for (i = 0; i < B.ndev; i++)
        if (B.dev[i].used && !strcmp(B.dev[i].product, product)) return i;
    return -1;
}

int main(void)
{
    int n, h, m, f;

    printf("[hub with a mouse and a flash drive]\n");
    sim_init(&S, SIM_HUB);
    sim_plug(&S, 4, SIM_MOUSE);
    sim_plug(&S, 1, SIM_FLASH);
    sim_transport(&S, &T);
    zzp_bus_init(&B, &T);
    n = zzp_scan(&B);
    CHECK(n == 3, "three devices found");
    h = 0;
    CHECK(B.dev[h].is_hub && B.dev[h].ok && B.dev[h].addr == 1 && B.dev[h].speed == SPEED_HIGH && B.dev[h].hub_ports == 4,
          "the hub: address 1, high speed, 4 ports");
    CHECK(B.dev[h].vid == 0x1a40 && B.dev[h].pid == 0x0101 && !B.dev[h].hub_multi_tt, "the hub's IDs, single TT");
    m = find_kind("USB Optical Mouse");
    f = find_kind("DataTraveler 3.0");
    CHECK(m >= 0 && f >= 0, "both named by their string descriptors");
    if (m >= 0) {
        CHECK(B.dev[m].speed == SPEED_LOW && B.dev[m].tt_hub == 1 && B.dev[m].tt_port == 4 && B.dev[m].parent == 0 && B.dev[m].port == 4,
              "the mouse: low speed, behind hub port 4, split through the hub");
        CHECK(zzp_find_if(&B.dev[m], CLASS_HID, 1, 2) != NULL && B.dev[m].ifs[0].nep == 1 && B.dev[m].ifs[0].ep[0].addr == 0x81 &&
              (B.dev[m].ifs[0].ep[0].attr & 3) == 3, "the mouse: a boot-protocol HID mouse with an interrupt IN endpoint");
        CHECK(!strcmp(B.dev[m].manufacturer, "Logitech") && B.dev[m].vid == 0x046d && B.dev[m].pid == 0xc05a && B.dev[m].ok,
              "the mouse: Logitech 046D:C05A, configured");
    }
    if (f >= 0) {
        CHECK(B.dev[f].speed == SPEED_HIGH && B.dev[f].tt_hub == 0 && B.dev[f].maxpkt0 == 64, "the flash drive: high speed, no split route");
        CHECK(zzp_find_if(&B.dev[f], CLASS_STORAGE, 6, 0x50) != NULL && B.dev[f].ifs[0].nep == 2 && B.dev[f].ifs[0].ep[0].maxpkt == 512,
              "the flash drive: SCSI bulk-only storage, two 512-byte bulk endpoints");
        CHECK(!strcmp(B.dev[f].serial, "E0D55E6C09F01A11"), "the flash drive's serial number");
    }
    CHECK(S.bad_split == 0 && S.bad_speed == 0 && S.bad_setup == 0 && S.bad_maxpkt == 0,
          "every transfer used the right speed, split route, setup and packet size");
    CHECK(B.errors == 0, "no transfer failed");
    printf("  %u transfers, %d ms of waiting\n", (unsigned)B.xfers, S.waited_ms);

    printf("[the same bus scanned again]\n");
    n = zzp_scan(&B);
    CHECK(n == 3 && B.dev[0].addr == 1 && B.errors == 0, "a rescan resets and finds the same three again");

    printf("[nothing plugged in]\n");
    sim_init(&S, SIM_NONE);
    sim_transport(&S, &T);
    zzp_bus_init(&B, &T);
    n = zzp_scan(&B);
    CHECK(n == 0 && B.root_status == XS_OFFLINE && strstr(B.note, "nothing is plugged"), "no devices, and it says so");

    printf("[a mouse straight into the ZZ9000]\n");
    sim_init(&S, SIM_MOUSE);
    sim_transport(&S, &T);
    zzp_bus_init(&B, &T);
    n = zzp_scan(&B);
    CHECK(n == 0 && strstr(B.note, "behind a USB 2.0 hub") && S.xfers == 0, "not touched, and the note says to use a hub");

    printf("[a flash drive straight into the ZZ9000]\n");
    sim_init(&S, SIM_FLASH);
    sim_transport(&S, &T);
    zzp_bus_init(&B, &T);
    n = zzp_scan(&B);
    CHECK(n == 1 && B.dev[0].ok && B.dev[0].parent == -1 && zzp_find_if(&B.dev[0], CLASS_STORAGE, -1, -1), "one storage device on the root port");

    printf("[a device that refuses an address]\n");
    sim_init(&S, SIM_HUB);
    m = sim_plug(&S, 2, SIM_MOUSE);
    S.dev[m].refuse_address = 1;
    sim_transport(&S, &T);
    zzp_bus_init(&B, &T);
    n = zzp_scan(&B);
    CHECK(n == 2 && B.dev[0].ok && !B.dev[1].ok && strstr(B.dev[1].note, "refused an address"), "the hub works, the mouse is listed with the reason");

    printf("%s: %d failures\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
