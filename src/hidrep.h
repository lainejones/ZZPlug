/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * hidrep.h - a HID report descriptor read just far enough to drive a game
 * controller: where X, Y, a hat switch and the buttons sit in the report.
 * Portable; host-tested.
 */
#ifndef ZZP_HIDREP_H
#define ZZP_HIDREP_H
#include <stdint.h>

struct zzp_field {
    uint8_t present;
    uint8_t report_id;          /* 0 = the device uses no report IDs */
    uint16_t bit, size;         /* where in the report (after the ID byte) */
    int32_t lmin, lmax;
};

#define PAD_MAXBUTTONS 16

struct zzp_padmap {
    uint8_t uses_ids;
    struct zzp_field x, y, hat;
    struct zzp_field wheel, pan;                /* a mouse's wheel and tilt wheel (AC Pan) */
    struct zzp_field button[PAD_MAXBUTTONS];
    int nbuttons;
};

/* directions and buttons as lowlevel.library reports them (libraries/lowlevel.h) */
#define PAD_RIGHT  (1UL << 0)
#define PAD_LEFT   (1UL << 1)
#define PAD_DOWN   (1UL << 2)
#define PAD_UP     (1UL << 3)
#define PAD_PLAY   (1UL << 17)
#define PAD_REVERSE (1UL << 18)
#define PAD_FORWARD (1UL << 19)
#define PAD_GREEN  (1UL << 20)
#define PAD_YELLOW (1UL << 21)
#define PAD_RED    (1UL << 22)
#define PAD_BLUE   (1UL << 23)

int  zzp_padmap_parse(struct zzp_padmap *m, const uint8_t *desc, int len);   /* 1 = looks like a pad/joystick */
int  zzp_mousemap_parse(struct zzp_padmap *m, const uint8_t *desc, int len); /* 1 = a mouse: X, Y and buttons */
int32_t zzp_field_value(const struct zzp_field *f, const uint8_t *r, int len, int uses_ids);   /* 0 if absent */
/* which HID button plays which part: order[0] = the button for red (fire), then blue, green, yellow,
 * reverse, forward, ... (1-based HID button numbers, 0 = leave, PAD_NONE = no button there).
 * Returns the number applied. */
#define PAD_NONE 0xff
int  zzp_padmap_reorder(struct zzp_padmap *m, const uint8_t *order, int n);
int  zzp_padmap_order_for(uint16_t vid, uint16_t pid, uint8_t *order);  /* a known pad's layout: n, 0 = none */
/* the other way round: which of raw's buttons is in each of m's places (1-based, 0 = none) */
void zzp_padmap_layout(const struct zzp_padmap *m, const struct zzp_padmap *raw, uint8_t *pos);
uint32_t zzp_pad_state(const struct zzp_padmap *m, const uint8_t *r, int len, uint16_t *buttons_out);

#endif
