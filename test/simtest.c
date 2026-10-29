/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/* simtest.c - ZZPlug's USB logic against the simulated bus. */
#include <stdio.h>
#include <string.h>
#include "sim.h"
#include "../src/core.h"
#include "../src/usbdefs.h"
#include "../src/hid.h"

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

    printf("[mouse reports]\n");
    {
        struct zzp_hid_shared sh = { 0, 0 };
        struct zzp_mouse mo;
        struct zzp_event ev[ZZP_MAXEVENTS];
        uint8_t r1[3] = { 0, 5, (uint8_t)-3 }, r2[3] = { 1, 2, 0 }, r3[3] = { 1, 0, 0 }, r4[3] = { 0, 0, 0 };
        uint8_t r5[4] = { 0, 0, 0, (uint8_t)-1 };
        zzp_mouse_init(&mo, &sh);
        n = zzp_mouse_report(&mo, r1, 3, ev);
        CHECK(n == 1 && ev[0].cls == ZE_RAWMOUSE && ev[0].code == ZC_NOBUTTON && ev[0].dx == 5 && ev[0].dy == -3 &&
              (ev[0].qual & ZQ_RELMOUSE), "movement: one relative event, signed deltas");
        n = zzp_mouse_report(&mo, r2, 3, ev);
        CHECK(n == 1 && ev[0].code == ZC_LBUTTON && ev[0].dx == 2 && (ev[0].qual & ZQ_LBUTTON),
              "left button down carries the movement and the button qualifier");
        n = zzp_mouse_report(&mo, r3, 3, ev);
        CHECK(n == 0, "still held, no movement: nothing");
        n = zzp_mouse_report(&mo, r4, 3, ev);
        CHECK(n == 1 && ev[0].code == (ZC_LBUTTON | ZC_UP) && !(ev[0].qual & ZQ_LBUTTON), "left button up");
        n = zzp_mouse_report(&mo, r5, 4, ev);
        CHECK(n == 2 && ev[0].cls == ZE_RAWKEY && ev[0].code == ZC_WHEEL_DOWN && ev[1].code == (ZC_WHEEL_DOWN | ZC_UP),
              "a wheel click down is a NewMouse wheel-down key press and release");
    }

    printf("[keyboard reports]\n");
    {
        struct zzp_hid_shared sh = { 0, 0 };
        struct zzp_kbd kb;
        struct zzp_mouse mo;
        struct zzp_event ev[ZZP_MAXEVENTS];
        uint8_t a[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };          /* a */
        uint8_t sa[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };      /* left shift + a */
        uint8_t none[8] = { 0 };
        uint8_t caps[8] = { 0, 0, 0x39, 0, 0, 0, 0, 0 };
        uint8_t kp1[8] = { 0, 0, 0x59, 0, 0, 0, 0, 0 };
        uint8_t roll[8] = { 0, 0, 1, 1, 1, 1, 1, 1 };
        uint8_t click[3] = { 1, 0, 0 };
        zzp_kbd_init(&kb, &sh);
        zzp_mouse_init(&mo, &sh);
        n = zzp_kbd_report(&kb, a, 8, ev);
        CHECK(n == 1 && ev[0].cls == ZE_RAWKEY && ev[0].code == 0x20 && ev[0].qual == 0, "a down = raw key 0x20");
        n = zzp_kbd_report(&kb, sa, 8, ev);
        CHECK(n == 1 && ev[0].code == 0x60 && (ev[0].qual & ZQ_LSHIFT), "shift pressed while a is held: shift down, a not repeated");
        n = zzp_kbd_report(&kb, none, 8, ev);
        CHECK(n == 2 && ev[0].code == (0x60 | ZC_UP) && ev[1].code == (0x20 | ZC_UP) && !(ev[1].qual & ZQ_LSHIFT),
              "both released: shift up, then a up unshifted");
        n = zzp_kbd_report(&kb, sa, 8, ev);
        CHECK(n == 2 && ev[0].code == 0x60 && ev[1].code == 0x20 && (ev[1].qual & ZQ_LSHIFT),
              "shift and a in one report: shift first, a carries the shift qualifier");
        n = zzp_kbd_report(&kb, none, 8, ev);
        n = zzp_kbd_report(&kb, caps, 8, ev);
        CHECK(n == 1 && ev[0].code == 0x62 && (ev[0].qual & ZQ_CAPSLOCK) && zzp_kbd_leds(&kb) == 2, "caps lock press: lock on, LED on");
        n = zzp_kbd_report(&kb, none, 8, ev);
        CHECK(n == 0, "caps lock release: nothing (it is a lock)");
        n = zzp_kbd_report(&kb, caps, 8, ev);
        CHECK(n == 1 && ev[0].code == (0x62 | ZC_UP) && !(ev[0].qual & ZQ_CAPSLOCK) && zzp_kbd_leds(&kb) == 0, "pressed again: lock off");
        zzp_kbd_report(&kb, none, 8, ev);
        n = zzp_kbd_report(&kb, kp1, 8, ev);
        CHECK(n == 1 && ev[0].code == 0x1d && (ev[0].qual & ZQ_NUMPAD), "keypad 1 = 0x1d with the numeric pad qualifier");
        n = zzp_kbd_report(&kb, roll, 8, ev);
        CHECK(n == 0, "a phantom (rollover) report changes nothing");
        n = zzp_kbd_tick(&kb, 400, ev);
        CHECK(n == 0, "no repeat before 500 ms");
        n = zzp_kbd_tick(&kb, 180, ev);
        CHECK(n == 3 && ev[0].code == 0x1d && (ev[0].qual & ZQ_REPEAT), "580 ms held: the first repeat and two more at 40 ms");
        n = zzp_kbd_release(&kb, ev);
        CHECK(n == 1 && ev[0].code == (0x1d | ZC_UP) && zzp_kbd_tick(&kb, 1000, ev) == 0, "release on unplug, repeat stops");
        zzp_kbd_report(&kb, sa, 8, ev);
        n = zzp_mouse_report(&mo, click, 3, ev);
        CHECK(n == 1 && (ev[0].qual & ZQ_LSHIFT) && (ev[0].qual & ZQ_LBUTTON), "shift-click: the click carries the keyboard's shift");
        CHECK(zzp_hid_to_rawkey(0x1d) == 0x31 && zzp_hid_to_rawkey(0x29) == 0x45 && zzp_hid_to_rawkey(0x52) == 0x4c &&
              zzp_hid_to_rawkey(0x64) == 0x30, "z, Esc, cursor up and the international key map to their Amiga codes");
    }

    printf("%s: %d failures\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
