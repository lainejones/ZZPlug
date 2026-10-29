/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * hidrep.c - the HID report descriptor walked item by item (USB HID 1.11,
 * section 6.2.2): global items set the usage page, logical range, size,
 * count and report ID; local items name the usages; each Input item lays
 * out `count` fields of `size` bits. We keep the first X, Y, hat switch and
 * the first 16 buttons.
 */
#include <string.h>
#include "hidrep.h"

#define PAGE_DESKTOP 0x01
#define PAGE_BUTTON  0x09
#define U_X   0x30
#define U_Y   0x31
#define U_HAT 0x39

static int32_t sval(const uint8_t *p, int n)
{
    if (n == 1) return (int8_t)p[0];
    if (n == 2) return (int16_t)(p[0] | (p[1] << 8));
    if (n == 4) return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    return 0;
}

static uint32_t uval(const uint8_t *p, int n)
{
    if (n == 1) return p[0];
    if (n == 2) return (uint32_t)(p[0] | (p[1] << 8));
    if (n == 4) return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return 0;
}

static void set(struct zzp_field *f, uint8_t id, uint16_t bit, uint16_t size, int32_t lmin, int32_t lmax)
{
    if (f->present) return;                     /* the first one wins */
    f->present = 1; f->report_id = id; f->bit = bit; f->size = size; f->lmin = lmin; f->lmax = lmax;
}

int zzp_padmap_parse(struct zzp_padmap *m, const uint8_t *d, int len)
{
    uint32_t page = 0, rsize = 0, rcount = 0, usages[32], umin = 0, umax = 0;
    int32_t lmin = 0, lmax = 0;
    int nus = 0, i = 0, have_range = 0, is_pad = 0;
    uint8_t id = 0;
    uint16_t offset[256];

    memset(m, 0, sizeof(*m));
    memset(offset, 0, sizeof(offset));
    while (i < len) {
        uint8_t b = d[i], type, tag;
        int n = b & 3;
        const uint8_t *p;
        if (n == 3) n = 4;
        if (b == 0xfe) {                        /* a long item: skip */
            if (i + 2 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        if (i + 1 + n > len) break;
        p = d + i + 1;
        type = (uint8_t)((b >> 2) & 3);
        tag = (uint8_t)(b >> 4);
        i += 1 + n;
        if (type == 1) {                        /* global */
            switch (tag) {
            case 0: page = uval(p, n); break;
            case 1: lmin = sval(p, n); break;
            case 2: lmax = (lmin >= 0 && n < 4) ? (int32_t)uval(p, n) : sval(p, n); break;
            case 7: rsize = uval(p, n); break;
            case 8: id = (uint8_t)uval(p, n); m->uses_ids = 1; break;
            case 9: rcount = uval(p, n); break;
            }
        } else if (type == 2) {                 /* local */
            uint32_t u = uval(p, n);
            if (n == 4) { page = u >> 16; u &= 0xffff; }
            if (tag == 0 && nus < 32) usages[nus++] = u;
            else if (tag == 1) { umin = u; have_range = 1; }
            else if (tag == 2) { umax = u; have_range = 1; }
        } else if (type == 0) {                 /* main */
            if (tag == 0x0a && page == PAGE_DESKTOP && nus && (usages[0] == 0x04 || usages[0] == 0x05))
                is_pad = 1;                     /* a joystick or game pad collection */
            if (tag == 0x08) {                  /* Input */
                uint32_t flags = uval(p, n), k;
                int constant = flags & 1;
                for (k = 0; k < rcount; k++) {
                    uint32_t u;
                    uint16_t bit = offset[id];
                    if (nus) u = usages[k < (uint32_t)nus ? k : (uint32_t)nus - 1];
                    else if (have_range) u = umin + k > umax ? umax : umin + k;
                    else u = 0;
                    if (!constant) {
                        if (page == PAGE_DESKTOP && u == U_X) set(&m->x, id, bit, (uint16_t)rsize, lmin, lmax);
                        else if (page == PAGE_DESKTOP && u == U_Y) set(&m->y, id, bit, (uint16_t)rsize, lmin, lmax);
                        else if (page == PAGE_DESKTOP && u == U_HAT) set(&m->hat, id, bit, (uint16_t)rsize, lmin, lmax);
                        else if (page == PAGE_BUTTON && u >= 1 && u <= PAD_MAXBUTTONS) {
                            set(&m->button[u - 1], id, bit, (uint16_t)rsize, lmin, lmax);
                            if ((int)u > m->nbuttons) m->nbuttons = (int)u;
                        }
                    }
                    offset[id] = (uint16_t)(offset[id] + rsize);
                }
            }
            if (tag == 0x08 || tag == 0x09 || tag == 0x0b || tag == 0x0a) {   /* locals end with a main item */
                nus = 0; have_range = 0; umin = umax = 0;
            }
        }
    }
    return is_pad && (m->x.present || m->hat.present) && m->nbuttons > 0;
}

int zzp_padmap_reorder(struct zzp_padmap *m, const uint8_t *order, int n)
{
    struct zzp_field old[PAD_MAXBUTTONS];
    int i, done = 0;
    memcpy(old, m->button, sizeof(old));
    for (i = 0; i < n && i < PAD_MAXBUTTONS; i++)
        if (order[i] >= 1 && order[i] <= PAD_MAXBUTTONS) {
            m->button[i] = old[order[i] - 1];
            done++;
        }
    return done;
}

int zzp_padmap_order_for(uint16_t vid, uint16_t pid, uint8_t *order)
{
    /* SNES-style pad, HID buttons 1-4 = X (top, blue cap), A (right, red cap),
     * B (bottom, yellow cap), Y (left, green cap): each Amiga colour on the cap
     * of the same colour, so red (fire) is the right-hand A button */
    if (vid == 0x081f && pid == 0xe401) {
        static const uint8_t o[4] = { 2, 1, 4, 3 };
        memcpy(order, o, sizeof(o));
        return 4;
    }
    return 0;
}

static int32_t field(const struct zzp_field *f, const uint8_t *r, int len, int uses_ids)
{
    uint32_t bit, v = 0, k;
    int base = uses_ids ? 1 : 0;
    if (!f->present || f->size == 0 || f->size > 32) return 0;
    if (uses_ids && (len < 1 || r[0] != f->report_id)) return 0;
    for (k = 0; k < f->size; k++) {
        bit = f->bit + k;
        if ((int)(base + bit / 8) >= len) return 0;
        if (r[base + bit / 8] & (1u << (bit % 8))) v |= 1u << k;
    }
    if (f->lmin < 0 && f->size < 32 && (v & (1u << (f->size - 1))))   /* sign-extend */
        v |= ~0u << f->size;
    return (int32_t)v;
}

/* an axis read as -1, 0 or +1: the outer quarters of the range count */
static int axis(const struct zzp_field *f, const uint8_t *r, int len, int ids)
{
    int32_t v, mid, q;
    if (!f->present || f->lmax <= f->lmin) return 0;
    v = field(f, r, len, ids);
    mid = (f->lmin + f->lmax) / 2;
    q = (f->lmax - f->lmin) / 4;
    if (v < mid - q) return -1;
    if (v > mid + q) return 1;
    return 0;
}

uint32_t zzp_pad_state(const struct zzp_padmap *m, const uint8_t *r, int len, uint16_t *buttons_out)
{
    static const uint32_t pad_bits[PAD_MAXBUTTONS] = {
        PAD_RED, PAD_BLUE, PAD_GREEN, PAD_YELLOW, PAD_REVERSE, PAD_FORWARD, PAD_REVERSE, PAD_FORWARD,
        PAD_PLAY, PAD_PLAY, 0, 0, PAD_PLAY, 0, 0, 0 };
    uint32_t s = 0;
    uint16_t buttons = 0;
    int i, dx, dy;

    if (m->uses_ids) {                          /* only the report that carries the pad */
        uint8_t want = m->x.present ? m->x.report_id : m->hat.report_id;
        if (len < 1 || r[0] != want) return 0xffffffffUL;
    }
    dx = axis(&m->x, r, len, m->uses_ids);
    dy = axis(&m->y, r, len, m->uses_ids);
    if (m->hat.present) {
        int32_t h = field(&m->hat, r, len, m->uses_ids) - m->hat.lmin;
        static const signed char hx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, hy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        if (h >= 0 && h < 8) { if (!dx) dx = hx[h]; if (!dy) dy = hy[h]; }
    }
    if (dx < 0) s |= PAD_LEFT;
    if (dx > 0) s |= PAD_RIGHT;
    if (dy < 0) s |= PAD_UP;
    if (dy > 0) s |= PAD_DOWN;
    for (i = 0; i < m->nbuttons && i < PAD_MAXBUTTONS; i++)
        if (field(&m->button[i], r, len, m->uses_ids)) {
            buttons |= (uint16_t)(1u << i);
            s |= pad_bits[i];
        }
    if (buttons_out) *buttons_out = buttons;
    return s;
}
