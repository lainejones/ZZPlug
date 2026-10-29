/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * hid.c - boot-protocol reports to Amiga input events.
 *
 * Mouse: byte 0 buttons (bit 0 left, 1 right, 2 middle), bytes 1-2 signed
 * X/Y movement, byte 3 the wheel when the mouse sends one.
 * Keyboard: byte 0 modifiers, byte 1 reserved, bytes 2-7 the keys held.
 * The Amiga wants key down/up events, so held keys are compared with the
 * previous report. Caps Lock is a toggle on the Amiga: a USB press flips it.
 */
#include <string.h>
#include "hid.h"

/* HID usage (keyboard page) -> Amiga raw key, US positions. 0xff = none. */
static const uint8_t usage_map[0x68] = {
    /* 00 */ 0xff, 0xff, 0xff, 0xff,
    /* 04 a-z */ 0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24, 0x25, 0x17, 0x26, 0x27, 0x28, 0x37,
                 0x36, 0x18, 0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11, 0x32, 0x15, 0x31,
    /* 1e 1-9,0 */ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    /* 28 enter esc bksp tab space */ 0x44, 0x45, 0x41, 0x42, 0x40,
    /* 2d - = [ ] \ #~ ; ' ` , . / */ 0x0b, 0x0c, 0x1a, 0x1b, 0x0d, 0x2b, 0x29, 0x2a, 0x00, 0x38, 0x39, 0x3a,
    /* 39 caps */ 0x62,
    /* 3a F1-F12 */ 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0xff, 0xff,
    /* 46 prtsc scrlk pause */ 0xff, 0xff, 0xff,
    /* 49 insert home pgup delete end pgdn */ 0x5f, 0xff, 0xff, 0x46, 0xff, 0xff,
    /* 4f right left down up */ 0x4e, 0x4f, 0x4d, 0x4c,
    /* 53 numlock kp/ kp* kp- kp+ kpenter */ 0xff, 0x5c, 0x5d, 0x4a, 0x5e, 0x43,
    /* 59 kp1-9 kp0 kp. */ 0x1d, 0x1e, 0x1f, 0x2d, 0x2e, 0x2f, 0x3d, 0x3e, 0x3f, 0x0f, 0x3c,
    /* 64 non-US \ (left of Z), application (Help) */ 0x30, 0x5f,
    /* 66 power, 67 kp= */ 0xff, 0xff,
};

/* modifier bit -> raw key and qualifier */
static const uint8_t mod_key[8]   = { 0x63, 0x60, 0x64, 0x66, 0x63, 0x61, 0x65, 0x67 };
static const uint16_t mod_qual[8] = { ZQ_CONTROL, ZQ_LSHIFT, ZQ_LALT, ZQ_LAMIGA, ZQ_CONTROL, ZQ_RSHIFT, ZQ_RALT, ZQ_RAMIGA };

uint8_t zzp_hid_to_rawkey(uint8_t usage)
{
    return usage < sizeof(usage_map) ? usage_map[usage] : 0xff;
}

static int is_numpad(uint8_t raw)
{
    return raw == 0x0f || raw == 0x1d || raw == 0x1e || raw == 0x1f || raw == 0x2d || raw == 0x2e || raw == 0x2f ||
           raw == 0x3d || raw == 0x3e || raw == 0x3f || raw == 0x3c || raw == 0x43 || raw == 0x4a ||
           (raw >= 0x5a && raw <= 0x5e);
}

static uint16_t qual(struct zzp_hid_shared *sh)
{
    return (uint16_t)(sh->keyq | sh->buttonq);
}

static void add(struct zzp_event *ev, int *n, uint8_t cls, uint8_t code, uint16_t q, int dx, int dy)
{
    if (*n >= ZZP_MAXEVENTS) return;
    ev[*n].cls = cls; ev[*n].code = code; ev[*n].qual = q;
    ev[*n].dx = (int16_t)dx; ev[*n].dy = (int16_t)dy;
    (*n)++;
}

/* ---- mouse ---------------------------------------------------------------- */

void zzp_mouse_init(struct zzp_mouse *m, struct zzp_hid_shared *sh)
{
    memset(m, 0, sizeof(*m));
    m->sh = sh;
}

int zzp_mouse_report(struct zzp_mouse *m, const uint8_t *r, int len, struct zzp_event *ev)
{
    static const uint8_t code[3] = { ZC_LBUTTON, ZC_RBUTTON, ZC_MBUTTON };
    static const uint16_t bq[3] = { ZQ_LBUTTON, ZQ_RBUTTON, ZQ_MBUTTON };
    int n = 0, i, dx, dy, moved;
    uint8_t b;

    if (len < 3) return 0;
    b = (uint8_t)(r[0] & 7);
    dx = (int8_t)r[1];
    dy = (int8_t)r[2];
    moved = dx || dy;
    for (i = 0; i < 3; i++) {
        uint8_t was = (uint8_t)(m->buttons & (1 << i)), now = (uint8_t)(b & (1 << i));
        if (was == now) continue;
        if (now) m->sh->buttonq |= bq[i]; else m->sh->buttonq &= (uint16_t)~bq[i];
        /* the movement rides on the first event so a drag starts where it should */
        add(ev, &n, ZE_RAWMOUSE, (uint8_t)(code[i] | (now ? 0 : ZC_UP)), (uint16_t)(qual(m->sh) | ZQ_RELMOUSE),
            moved ? dx : 0, moved ? dy : 0);
        moved = 0;
    }
    m->buttons = b;
    if (moved)
        add(ev, &n, ZE_RAWMOUSE, ZC_NOBUTTON, (uint16_t)(qual(m->sh) | ZQ_RELMOUSE), dx, dy);
    if (len >= 4 && r[3]) {                       /* the wheel, as NewMouse raw keys */
        int w = (int8_t)r[3], k;
        uint8_t c = w > 0 ? ZC_WHEEL_UP : ZC_WHEEL_DOWN;
        if (w < 0) w = -w;
        for (k = 0; k < w && k < 4; k++) {
            add(ev, &n, ZE_RAWKEY, c, qual(m->sh), 0, 0);
            add(ev, &n, ZE_RAWKEY, (uint8_t)(c | ZC_UP), qual(m->sh), 0, 0);
        }
    }
    return n;
}

int zzp_mouse_release(struct zzp_mouse *m, struct zzp_event *ev)
{
    uint8_t r[3] = { 0, 0, 0 };
    return zzp_mouse_report(m, r, 3, ev);
}

/* ---- keyboard ------------------------------------------------------------- */

void zzp_kbd_init(struct zzp_kbd *k, struct zzp_hid_shared *sh)
{
    memset(k, 0, sizeof(*k));
    k->sh = sh;
    k->repeat_key = 0xff;
}

static int held(const uint8_t *keys, uint8_t usage)
{
    int i;
    for (i = 0; i < 6; i++)
        if (keys[i] == usage) return 1;
    return 0;
}

static void key_event(struct zzp_kbd *k, struct zzp_event *ev, int *n, uint8_t raw, int down)
{
    uint16_t q = qual(k->sh);
    if (is_numpad(raw)) q |= ZQ_NUMPAD;
    add(ev, n, ZE_RAWKEY, (uint8_t)(raw | (down ? 0 : ZC_UP)), q, 0, 0);
}

int zzp_kbd_report(struct zzp_kbd *k, const uint8_t *r, int len, struct zzp_event *ev)
{
    int n = 0, i;
    uint8_t mods, keys[6];

    if (len < 8) return 0;
    if (r[2] == 0x01) return 0;                   /* phantom state (too many keys): keep the last report */
    mods = r[0];
    memcpy(keys, r + 2, 6);

    /* modifiers first, so a key pressed in the same report is shifted */
    for (i = 0; i < 8; i++) {
        uint8_t was = (uint8_t)(k->mods & (1 << i)), now = (uint8_t)(mods & (1 << i));
        if (was == now) continue;
        if (now) k->sh->keyq |= mod_qual[i];
        else if (!(mods & (1 << (i ^ 4))) || mod_qual[i] != ZQ_CONTROL)   /* both Ctrls share a qualifier */
            k->sh->keyq &= (uint16_t)~mod_qual[i];
        key_event(k, ev, &n, mod_key[i], now != 0);
    }
    k->mods = mods;

    /* released keys */
    for (i = 0; i < 6; i++) {
        uint8_t u = k->keys[i], raw;
        if (u < 4 || held(keys, u)) continue;
        raw = zzp_hid_to_rawkey(u);
        if (raw == 0xff || raw == 0x62) continue;  /* caps lock acts on press only */
        key_event(k, ev, &n, raw, 0);
        if (raw == k->repeat_key) { k->repeat_key = 0xff; k->repeating = 0; }
    }
    /* pressed keys */
    for (i = 0; i < 6; i++) {
        uint8_t u = keys[i], raw;
        if (u < 4 || held(k->keys, u)) continue;
        raw = zzp_hid_to_rawkey(u);
        if (raw == 0xff) continue;
        if (raw == 0x62) {                         /* caps lock: an Amiga lock key */
            k->caps = !k->caps;
            if (k->caps) k->sh->keyq |= ZQ_CAPSLOCK; else k->sh->keyq &= (uint16_t)~ZQ_CAPSLOCK;
            key_event(k, ev, &n, 0x62, k->caps);
            continue;
        }
        key_event(k, ev, &n, raw, 1);
        k->repeat_key = raw; k->repeat_ms = 0; k->repeating = 0;
    }
    memcpy(k->keys, keys, 6);
    return n;
}

int zzp_kbd_tick(struct zzp_kbd *k, int ms, struct zzp_event *ev)
{
    int n = 0;
    if (k->repeat_key == 0xff) return 0;
    k->repeat_ms = (uint16_t)(k->repeat_ms + ms);
    while (n < ZZP_MAXEVENTS && k->repeat_ms >= (k->repeating ? REPEAT_PERIOD_MS : REPEAT_DELAY_MS)) {
        uint16_t q;
        k->repeat_ms = (uint16_t)(k->repeat_ms - (k->repeating ? REPEAT_PERIOD_MS : REPEAT_DELAY_MS));
        k->repeating = 1;
        q = (uint16_t)(qual(k->sh) | ZQ_REPEAT);
        if (is_numpad(k->repeat_key)) q |= ZQ_NUMPAD;
        add(ev, &n, ZE_RAWKEY, k->repeat_key, q, 0, 0);
    }
    return n;
}

uint8_t zzp_kbd_leds(const struct zzp_kbd *k)
{
    return (uint8_t)(k->caps ? 0x02 : 0x00);
}

int zzp_kbd_release(struct zzp_kbd *k, struct zzp_event *ev)
{
    uint8_t r[8];
    int n;
    memset(r, 0, sizeof(r));
    n = zzp_kbd_report(k, r, 8, ev);
    k->repeat_key = 0xff;
    return n;
}
