/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/* simtest.c - ZZPlug's USB logic against the simulated bus. */
#include <stdio.h>
#include <string.h>
#include "sim.h"
#include "../src/core.h"
#include "../src/usbdefs.h"
#include "../src/hid.h"
#include "../src/msd.h"
#include "../src/hidrep.h"

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

    printf("[a USB stick: bulk-only storage]\n");
    {
        struct zzp_msd ms;
        static uint8_t buf[40 * 512], back[40 * 512];
        struct zzp_part parts[MAXPART];
        int fi, r, k;
        sim_init(&S, SIM_HUB);
        f = sim_plug(&S, 1, SIM_FLASH);
        sim_disk(&S.dev[f], 4096, 1, 0x0c);
        sim_transport(&S, &T);
        zzp_bus_init(&B, &T);
        n = zzp_scan(&B);
        fi = find_kind("DataTraveler 3.0");
        CHECK(n == 2 && fi >= 0 && zzp_msd_find(&B.dev[fi]) == 0, "the stick is found with a bulk-only SCSI interface");
        r = zzp_msd_init(&ms, &B, fi);
        CHECK(r == MSD_OK && ms.blocks == 4096 && ms.blksize == 512 && ms.removable && !strcmp(ms.vendor, "Kingston"),
              "ready after the unit attention: 4096 blocks of 512, removable, Kingston");
        CHECK(ms.ep_in == 0x81 && ms.ep_out == 0x02 && ms.mps_in == 512 && ms.max_lun == 0, "endpoints, and one LUN when GET_MAX_LUN stalls");
        r = zzp_msd_read(&ms, 0, 1, buf);
        CHECK(r == MSD_OK && buf[510] == 0x55 && buf[511] == 0xaa, "block 0 read: an MBR signature");
        k = zzp_parse_parts(buf, ms.blocks, parts);
        CHECK(k == 1 && parts[0].start == 63 && parts[0].count == 4096 - 63 && parts[0].type == 0x0c && !strcmp(zzp_fat_name(0x0c), "FAT32"),
              "one FAT32 partition from block 63 to the end");
        for (k = 0; k < (int)sizeof(buf); k++) buf[k] = (uint8_t)(k * 7 + 3);
        r = zzp_msd_write(&ms, 1000, 40, buf);
        CHECK(r == MSD_OK, "40 blocks written (two commands: 32 + 8)");
        memset(back, 0, sizeof(back));
        r = zzp_msd_read(&ms, 1000, 40, back);
        CHECK(r == MSD_OK && !memcmp(buf, back, sizeof(buf)), "read back identical");
        r = zzp_msd_read(&ms, 4090, 10, back);
        CHECK(r == MSD_RANGE, "a read past the end is refused before it is sent");
        S.dev[f].stall_data_once = 1;
        r = zzp_msd_read(&ms, 0, 1, back);
        CHECK(r == MSD_FAILED && ms.sense_key == 3, "a stalled data stage: halt cleared, status read, sense says medium error");
        r = zzp_msd_read(&ms, 0, 1, back);
        CHECK(r == MSD_OK && back[510] == 0x55, "and the next read works");
        CHECK(S.bad_setup == 0 && S.bad_split == 0 && S.bad_speed == 0, "every wrapper and transfer was well formed");
        /* a stick formatted without a partition table */
        sim_disk(&S.dev[f], 2048, 0, 0);
        zzp_msd_init(&ms, &B, fi);
        zzp_msd_read(&ms, 0, 1, buf);
        k = zzp_parse_parts(buf, ms.blocks, parts);
        CHECK(k == 1 && parts[0].start == 0 && parts[0].count == 2048 && parts[0].type == 0, "no partition table: the whole stick is one FAT volume");
    }

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

    printf("[mouse in report protocol: wheel and side buttons]\n");
    {
        /* a usual 5-button wheel mouse: buttons 1-5, 3 bits padding, X, Y, wheel (8 bits, signed) */
        static const uint8_t md[] = {
            0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
            0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
            0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
            0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
            0xC0, 0xC0 };
        struct zzp_hid_shared sh = { 0, 0 };
        struct zzp_mouse mo;
        struct zzp_event ev[ZZP_MAXEVENTS];
        uint8_t m1[4] = { 0x08, 5, (uint8_t)-3, 1 }, m2[4] = { 0x00, 0, 0, 0 }, m3[4] = { 0x10, 0, 0, (uint8_t)-2 };
        zzp_mouse_init(&mo, &sh);
        CHECK(zzp_mousemap_parse(&mo.map, md, sizeof(md)) && mo.map.wheel.present && mo.map.nbuttons == 5,
              "the descriptor reads as a mouse with 5 buttons and a wheel");
        CHECK(!zzp_padmap_parse(&mo.map, md, sizeof(md)), "a mouse is not taken for a game pad");
        zzp_mousemap_parse(&mo.map, md, sizeof(md));
        mo.full = 1;
        n = zzp_mouse_report(&mo, m1, 4, ev);
        CHECK(n == 4 && ev[0].cls == ZE_RAWKEY && ev[0].code == ZC_BUTTON4 && ev[1].cls == ZE_RAWMOUSE &&
              ev[1].dx == 5 && ev[1].dy == -3 && ev[2].code == ZC_WHEEL_UP && ev[3].code == (ZC_WHEEL_UP | ZC_UP),
              "button 4 down, the movement, one wheel click up");
        n = zzp_mouse_report(&mo, m2, 4, ev);
        CHECK(n == 1 && ev[0].code == (ZC_BUTTON4 | ZC_UP), "button 4 up");
        n = zzp_mouse_report(&mo, m3, 4, ev);
        CHECK(n == 5 && ev[0].code == ZC_BUTTON5 && ev[1].code == ZC_WHEEL_DOWN && ev[3].code == ZC_WHEEL_DOWN,
              "button 5 down and two wheel clicks down");
        n = zzp_mouse_release(&mo, ev);
        CHECK(n == 1 && ev[0].code == (ZC_BUTTON5 | ZC_UP) && mo.full, "unplugged: button 5 goes up, the mouse stays full");
        {   /* like a Logitech 046D:C05A: the mouse report has no ID, a later collection has Report ID 0x10 */
            static const uint8_t ld[] = {
                0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
                0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
                0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
                0xC0, 0xC0,
                0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x10, 0x75, 0x08, 0x95, 0x06, 0x15, 0x00, 0x26, 0xFF, 0x00,
                0x09, 0x01, 0x81, 0x00, 0xC0 };
            uint8_t up[4] = { 0x00, 0x00, 0xC7, 0x00 };     /* straight up: Y -57, no buttons */
            zzp_mouse_init(&mo, &sh);
            CHECK(zzp_mousemap_parse(&mo.map, ld, sizeof(ld)) && mo.map.uses_ids && mo.map.x.report_id == 0,
                  "a mouse report without an ID, in a descriptor that has one elsewhere");
            mo.full = 1;
            n = zzp_mouse_report(&mo, up, 4, ev);
            CHECK(n == 1 && ev[0].cls == ZE_RAWMOUSE && ev[0].code == ZC_NOBUTTON && ev[0].dx == 0 && ev[0].dy == -57,
                  "it is read without an ID byte: straight up, no button");
        }
        {   /* like 1EA7:0066: the descriptor has Report ID 1, the receiver sends boot reports anyway */
            uint8_t br[4] = { 0x00, 0xFE, 0x00, 0x00 }, side[4] = { 0x08, 0, 0, 0 };
            int k;
            zzp_mouse_init(&mo, &sh);
            zzp_mousemap_parse(&mo.map, md, sizeof(md));
            mo.map.uses_ids = 1; mo.map.x.report_id = 1;     /* as if the descriptor said Report ID 1 */
            mo.full = 1;
            for (k = 0; k < 2; k++) CHECK(zzp_mouse_report(&mo, br, 4, ev) == 0, "a boot-looking report: not yet");
            n = zzp_mouse_report(&mo, br, 4, ev);
            CHECK(!mo.full && n == 1 && ev[0].dx == -2, "the third: read as a boot mouse from then on");
            n = zzp_mouse_report(&mo, side, 4, ev);
            CHECK(n == 1 && ev[0].code == ZC_BUTTON4, "a boot report's bit 3 is the fourth button");
        }
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
        CHECK(n == 1 && ev[0].code == 0x62 && (ev[0].qual & ZQ_CAPSLOCK) && zzp_kbd_leds(&kb) == 3, "caps lock press: lock on, LED on (num lock LED always on)");
        n = zzp_kbd_report(&kb, none, 8, ev);
        CHECK(n == 0, "caps lock release: nothing (it is a lock)");
        n = zzp_kbd_report(&kb, caps, 8, ev);
        CHECK(n == 0 && zzp_kbd_leds(&kb) == 3, "a second caps press within 250 ms is a bounce: still on");
        zzp_kbd_report(&kb, none, 8, ev);
        zzp_kbd_tick(&kb, 300, ev);
        n = zzp_kbd_report(&kb, caps, 8, ev);
        CHECK(n == 1 && ev[0].code == (0x62 | ZC_UP) && !(ev[0].qual & ZQ_CAPSLOCK) && zzp_kbd_leds(&kb) == 1, "pressed again later: lock off");
        zzp_kbd_report(&kb, none, 8, ev);
        {
            uint8_t menu[8] = { 0, 0, 0x65, 0, 0, 0, 0, 0 }, menua[8] = { 0, 0, 0x65, 0x04, 0, 0, 0, 0 };
            n = zzp_kbd_report(&kb, menu, 8, ev);
            CHECK(n == 1 && ev[0].code == 0x67 && (ev[0].qual & ZQ_RAMIGA), "the menu key is right Amiga");
            n = zzp_kbd_report(&kb, menua, 8, ev);
            CHECK(n == 1 && ev[0].code == 0x20 && (ev[0].qual & ZQ_RAMIGA), "menu + a: a carries right Amiga (menu shortcut)");
            n = zzp_kbd_report(&kb, none, 8, ev);
            CHECK(n == 2 && !(ev[1].qual & ZQ_RAMIGA) && zzp_kbd_tick(&kb, 1000, ev) == 0, "released: qualifier gone, no repeat");
        }
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

    printf("[game pad report descriptors]\n");
    {
        /* a common cheap pad: 5 axis bytes (X and Y last two), a hat, 12 buttons, padding */
        static const uint8_t d1[] = {
            0x05,0x01, 0x09,0x05, 0xa1,0x01, 0xa1,0x02,
            0x75,0x08, 0x95,0x05, 0x15,0x00, 0x26,0xff,0x00, 0x09,0x32, 0x09,0x35, 0x09,0x33, 0x09,0x30, 0x09,0x31, 0x81,0x02,
            0x75,0x04, 0x95,0x01, 0x25,0x07, 0x46,0x3b,0x01, 0x65,0x14, 0x09,0x39, 0x81,0x42,
            0x65,0x00, 0x75,0x01, 0x95,0x0c, 0x25,0x01, 0x45,0x01, 0x05,0x09, 0x19,0x01, 0x29,0x0c, 0x81,0x02,
            0x06,0x00,0xff, 0x75,0x01, 0x95,0x08, 0x25,0x01, 0x45,0x01, 0x09,0x01, 0x81,0x02,
            0xc0, 0xc0 };
        /* a pad with a report ID and signed axes */
        static const uint8_t d2[] = {
            0x05,0x01, 0x09,0x04, 0xa1,0x01, 0x85,0x03,
            0x05,0x09, 0x19,0x01, 0x29,0x08, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
            0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7f, 0x75,0x08, 0x95,0x02, 0x81,0x02, 0xc0 };
        struct zzp_padmap pm;
        uint16_t bt;
        uint8_t centre[8] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x0f, 0x00, 0x00 };
        uint8_t leftup_a[8] = { 0x80, 0x80, 0x80, 0x00, 0x00, 0x1f, 0x00, 0x00 };   /* X 0, Y 0, hat centre, button 1 */
        uint8_t hat_right[8] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x02, 0x00, 0x00 };  /* hat 2 = east */
        uint8_t start[8] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x0f, 0x20, 0x00 };      /* button 10: bit 44 + 9 = byte 6 bit 5 */
        uint8_t r2[4] = { 3, 0x02, 0x7f, 0x81 };                                     /* id 3, button 2, X +127, Y -127 */
        uint8_t other[4] = { 1, 0xff, 0x00, 0x00 };
        uint32_t s;

        CHECK(zzp_padmap_parse(&pm, d1, sizeof(d1)) && pm.x.bit == 24 && pm.y.bit == 32 && pm.hat.bit == 40 &&
              pm.hat.size == 4 && pm.nbuttons == 12 && pm.button[0].bit == 44 && !pm.uses_ids,
              "a pad descriptor: X at byte 3, Y at byte 4, a 4-bit hat, 12 buttons after it");
        s = zzp_pad_state(&pm, centre, 8, &bt);
        CHECK(s == 0 && bt == 0, "centred, no buttons: nothing");
        s = zzp_pad_state(&pm, leftup_a, 8, &bt);
        CHECK(s == (PAD_LEFT | PAD_UP | PAD_RED) && bt == 1, "X and Y at 0 with button 1: left, up, red (fire)");
        s = zzp_pad_state(&pm, hat_right, 8, &bt);
        CHECK(s == PAD_RIGHT, "the hat pointing east: right");
        s = zzp_pad_state(&pm, start, 8, &bt);
        CHECK(s == PAD_PLAY && bt == 0x200, "button 10 (start): play");
        CHECK(zzp_padmap_parse(&pm, d2, sizeof(d2)) && pm.uses_ids && pm.x.report_id == 3 && pm.x.lmin == -127 && pm.x.bit == 8,
              "a joystick with report ID 3 and signed axes");
        s = zzp_pad_state(&pm, r2, 4, &bt);
        CHECK(s == (PAD_RIGHT | PAD_UP | PAD_BLUE), "X +127, Y -127, button 2: right, up, blue");
        CHECK(zzp_pad_state(&pm, other, 4, &bt) == 0xffffffffUL, "another report ID is not the pad's");
        {
            uint8_t order[16];
            int n2;
            zzp_padmap_parse(&pm, d1, sizeof(d1));
            n2 = zzp_padmap_order_for(0x081f, 0xe401, order);
            CHECK(n2 == 4 && zzp_padmap_reorder(&pm, order, n2) == 4, "the 081F:E401 pad has a known layout");
            s = zzp_pad_state(&pm, leftup_a, 8, &bt);       /* HID button 1 = the top face button */
            CHECK((s & (PAD_BLUE | PAD_GREEN | PAD_RED)) == PAD_BLUE, "after reordering, its top (blue-cap) button is blue and not fire");
            zzp_padmap_parse(&pm, d1, sizeof(d1));
            n2 = zzp_padmap_order_for(0x0810, 0xe501, order);
            CHECK(n2 == 4 && zzp_padmap_reorder(&pm, order, n2) == 4, "the 0810:E501 NES pad has a known layout");
            s = zzp_pad_state(&pm, leftup_a, 8, &bt);       /* HID button 1 = B, on the left */
            CHECK((s & (PAD_BLUE | PAD_RED)) == PAD_BLUE, "after reordering, its left B button is blue and right A is fire");
            CHECK(zzp_padmap_order_for(0x0079, 0x0006, order) == 0, "the shared 0079:0006 chip keeps the pad's own order");
            {   /* the game pad window's layouts: "-" (PAD_NONE) empties a place; a layout reads back */
                struct zzp_padmap raw;
                uint8_t pos[PAD_MAXBUTTONS], o2[3] = { 2, PAD_NONE, 1 };
                zzp_padmap_parse(&pm, d1, sizeof(d1));
                raw = pm;
                CHECK(zzp_padmap_reorder(&pm, o2, 3) == 3, "a layout with an empty place applies");
                zzp_padmap_layout(&pm, &raw, pos);
                CHECK(pos[0] == 2 && pos[1] == 0 && pos[2] == 1 && pos[3] == 4, "it reads back as 2, none, 1, then as the pad numbers them");
                s = zzp_pad_state(&pm, leftup_a, 8, &bt);   /* HID button 1 held: now green, not red or blue */
                CHECK((s & (PAD_RED | PAD_BLUE | PAD_GREEN)) == PAD_GREEN, "button 1 moved to green; blue has no button");
            }
        }
    }

    printf("%s: %d failures\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
