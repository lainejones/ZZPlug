/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * hid.h - boot-protocol mice and keyboards turned into Amiga input events.
 * Portable: the events carry the input.device numbers (raw key codes,
 * IECODE/IEQUALIFIER values) but no Amiga headers are needed, so the host
 * tests check every translation.
 */
#ifndef ZZP_HID_H
#define ZZP_HID_H
#include <stdint.h>

/* input.device classes and codes (devices/inputevent.h) */
#define ZE_RAWKEY   0x01
#define ZE_RAWMOUSE 0x02
#define ZC_UP       0x80        /* IECODE_UP_PREFIX */
#define ZC_LBUTTON  0x68
#define ZC_RBUTTON  0x69
#define ZC_MBUTTON  0x6a
#define ZC_NOBUTTON 0xff
#define ZC_WHEEL_UP   0x7a      /* NewMouse raw keys */
#define ZC_WHEEL_DOWN 0x7b
/* qualifiers */
#define ZQ_LSHIFT   0x0001
#define ZQ_RSHIFT   0x0002
#define ZQ_CAPSLOCK 0x0004
#define ZQ_CONTROL  0x0008
#define ZQ_LALT     0x0010
#define ZQ_RALT     0x0020
#define ZQ_LAMIGA   0x0040
#define ZQ_RAMIGA   0x0080
#define ZQ_NUMPAD   0x0100
#define ZQ_REPEAT   0x0200
#define ZQ_MBUTTON  0x1000
#define ZQ_RBUTTON  0x2000
#define ZQ_LBUTTON  0x4000
#define ZQ_RELMOUSE 0x8000
#define ZQ_KEYS     0x00ff      /* the keyboard's own qualifier bits */
#define ZQ_BUTTONS  0x7000

struct zzp_event {
    uint8_t  cls;               /* ZE_* */
    uint8_t  code;
    uint16_t qual;
    int16_t  dx, dy;
};

#define ZZP_MAXEVENTS 24

/* the qualifiers both devices share: shift-click must work */
struct zzp_hid_shared {
    uint16_t keyq;              /* keyboard part */
    uint16_t buttonq;           /* mouse part */
};

struct zzp_mouse {
    uint8_t buttons;
    struct zzp_hid_shared *sh;
};

struct zzp_kbd {
    uint8_t mods;
    uint8_t keys[6];
    uint8_t caps;               /* caps lock is on */
    uint8_t leds;               /* the LED byte last sent */
    uint8_t repeat_key;         /* raw key held for repeat, 0xff = none */
    uint16_t repeat_ms;         /* time it has been held / since the last repeat */
    uint8_t repeating;
    uint16_t caps_quiet;        /* ms left in which another Caps Lock press is a bounce */
    struct zzp_hid_shared *sh;
};

#define REPEAT_DELAY_MS  500
#define REPEAT_PERIOD_MS 40
#define CAPS_QUIET_MS    250

void zzp_mouse_init(struct zzp_mouse *m, struct zzp_hid_shared *sh);
void zzp_kbd_init(struct zzp_kbd *k, struct zzp_hid_shared *sh);
/* a report in, events out: the number of events */
int  zzp_mouse_report(struct zzp_mouse *m, const uint8_t *r, int len, struct zzp_event *ev);
int  zzp_kbd_report(struct zzp_kbd *k, const uint8_t *r, int len, struct zzp_event *ev);
/* time passes: key repeat events */
int  zzp_kbd_tick(struct zzp_kbd *k, int ms, struct zzp_event *ev);
/* the LED byte the keyboard should show: bit 0 num lock (always on - the Amiga keypad always
 * gives numbers), bit 1 caps lock */
uint8_t zzp_kbd_leds(const struct zzp_kbd *k);
/* release everything held (the device went away): events out */
int  zzp_kbd_release(struct zzp_kbd *k, struct zzp_event *ev);
int  zzp_mouse_release(struct zzp_mouse *m, struct zzp_event *ev);

uint8_t zzp_hid_to_rawkey(uint8_t usage);   /* 0xff = no Amiga key */

#endif
