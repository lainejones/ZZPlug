/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * core.c - enumeration. The root port is reset, the device on it gets
 * address 1 and is read; a hub has each of its ports powered, checked and
 * reset, and whatever is on them is enumerated the same way. Devices that
 * are not high speed but sit behind a high-speed hub are reached with split
 * transactions through that hub's transaction translator.
 */
#include <string.h>
#include "core.h"
#include "usbdefs.h"

#define CTRL_TIMEOUT 500

static void note(char *dst, const char *s)
{
    strncpy(dst, s, ZZP_NOTELEN - 1);
    dst[ZZP_NOTELEN - 1] = 0;
}

void zzp_bus_init(struct zzp_bus *b, struct zzp_transport *t)
{
    memset(b, 0, sizeof(*b));
    b->t = t;
    b->next_addr = 1;
}

/* a control transfer to a device; d->addr 0 is the default address */
static int control_raw(struct zzp_bus *b, uint8_t addr, uint8_t speed, uint8_t maxpkt,
                       uint8_t tt_hub, uint8_t tt_port, uint8_t tt_multi, uint8_t tt_think,
                       uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                       void *data, uint16_t len, uint32_t *actual)
{
    struct zzp_xfer x;
    int r;
    memset(&x, 0, sizeof(x));
    x.type = XT_CONTROL;
    x.addr = addr; x.ep = 0;
    x.dir_in = (rt & RT_IN) ? 1 : 0;
    x.speed = speed;
    x.maxpkt = maxpkt ? maxpkt : 8;
    if (speed != SPEED_HIGH && tt_hub) {
        x.tt_hub = tt_hub; x.tt_port = tt_port; x.tt_multi = tt_multi; x.tt_think = tt_think;
    }
    x.timeout_ms = CTRL_TIMEOUT;
    x.setup[0] = rt; x.setup[1] = req;
    x.setup[2] = (uint8_t)value; x.setup[3] = (uint8_t)(value >> 8);
    x.setup[4] = (uint8_t)index; x.setup[5] = (uint8_t)(index >> 8);
    x.setup[6] = (uint8_t)len;   x.setup[7] = (uint8_t)(len >> 8);
    x.data = (uint8_t *)data;
    x.len = len;
    b->xfers++;
    r = b->t->xfer(b->t->ctx, &x);
    if (r != XS_OK) b->errors++;
    if (actual) *actual = x.actual;
    return r;
}

int zzp_control(struct zzp_bus *b, const struct zzp_dev *d, uint8_t rt, uint8_t req,
                uint16_t value, uint16_t index, void *data, uint16_t len, uint32_t *actual)
{
    return control_raw(b, d->addr, d->speed, d->maxpkt0, d->tt_hub, d->tt_port, d->tt_multi, d->tt_think,
                       rt, req, value, index, data, len, actual);
}

/* the device's string, as plain ASCII (anything else becomes '?') */
static void get_string(struct zzp_bus *b, struct zzp_dev *d, uint8_t idx, uint16_t lang, char *out)
{
    uint8_t buf[255];
    uint32_t got = 0;
    int i, n;
    out[0] = 0;
    if (!idx) return;
    if (zzp_control(b, d, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR,
                    (DT_STRING << 8) | idx, lang, buf, sizeof(buf), &got) != XS_OK || got < 2)
        return;
    if (buf[0] < got) got = buf[0];
    n = 0;
    for (i = 2; i + 1 < (int)got && n < ZZP_STRLEN - 1; i += 2) {
        uint16_t c = (uint16_t)(buf[i] | (buf[i + 1] << 8));
        out[n++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    while (n > 0 && out[n - 1] == ' ') n--;    /* many devices pad with spaces */
    out[n] = 0;
}

/* walk a configuration descriptor and keep the interfaces and endpoints */
static void parse_config(struct zzp_dev *d, const uint8_t *p, uint32_t len)
{
    uint32_t i = 0;
    struct zzp_if *cur = NULL;
    d->nif = 0;
    while (i + 2 <= len) {
        uint8_t l = p[i], t = p[i + 1];
        if (l < 2 || i + l > len) break;
        if (t == DT_CONFIG && l >= 9) {
            d->config = p[i + 5];
            d->self_powered = (p[i + 7] & 0x40) ? 1 : 0;
            d->max_power_ma = (uint16_t)(p[i + 8] * 2);
        } else if (t == DT_INTERFACE && l >= 9) {
            cur = NULL;
            if (p[i + 3] == 0 && d->nif < ZZP_MAXIF) {      /* alternate setting 0 only */
                cur = &d->ifs[d->nif++];
                memset(cur, 0, sizeof(*cur));
                cur->number = p[i + 2];
                cur->cls = p[i + 5]; cur->subclass = p[i + 6]; cur->proto = p[i + 7];
            }
        } else if (t == DT_ENDPOINT && l >= 7 && cur && cur->nep < ZZP_MAXEP) {
            struct zzp_ep *e = &cur->ep[cur->nep++];
            e->addr = p[i + 2];
            e->attr = p[i + 3];
            e->maxpkt = (uint16_t)((p[i + 4] | (p[i + 5] << 8)) & 0x7ff);
            e->interval = p[i + 6];
        }
        i += l;
    }
}

static int new_dev(struct zzp_bus *b)
{
    int i;
    for (i = 0; i < ZZP_MAXDEV; i++)
        if (!b->dev[i].used) {
            memset(&b->dev[i], 0, sizeof(b->dev[i]));
            b->dev[i].used = 1;
            if (i + 1 > b->ndev) b->ndev = i + 1;
            return i;
        }
    return -1;
}

static void enumerate_hub(struct zzp_bus *b, int hi);

/*
 * The device just reset on (parent, port) answers at address 0: read its
 * first 8 bytes for the EP0 packet size, give it an address, read the rest.
 */
static int enumerate(struct zzp_bus *b, int parent, uint8_t port, uint8_t speed)
{
    int di = new_dev(b);
    struct zzp_dev *d;
    uint8_t buf[512];
    uint32_t got = 0;
    uint16_t lang = 0x0409, total;
    int r, addr;

    if (di < 0) return -1;
    d = &b->dev[di];
    d->parent = (int8_t)parent;
    d->port = port;
    d->speed = speed;
    d->depth = parent < 0 ? 0 : (uint8_t)(b->dev[parent].depth + 1);
    d->maxpkt0 = speed == SPEED_HIGH ? 64 : 8;
    if (parent >= 0 && speed != SPEED_HIGH) {
        struct zzp_dev *h = &b->dev[parent];
        if (h->speed == SPEED_HIGH) {          /* this hub's own transaction translator */
            d->tt_hub = h->addr; d->tt_port = port; d->tt_multi = h->hub_multi_tt; d->tt_think = h->hub_think;
        } else {                               /* a full-speed hub: the route of the hub above it */
            d->tt_hub = h->tt_hub; d->tt_port = h->tt_port; d->tt_multi = h->tt_multi; d->tt_think = h->tt_think;
        }
    }

    /* 1. the first 8 bytes of the device descriptor, at address 0 */
    d->addr = 0;
    r = zzp_control(b, d, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_DEVICE << 8, 0, buf, 8, &got);
    if (r != XS_OK || got < 8) {
        note(d->note, r == XS_OK ? "the device sent a short descriptor" : "the device did not answer at address 0");
        return di;
    }
    if (buf[7] == 8 || buf[7] == 16 || buf[7] == 32 || buf[7] == 64)
        d->maxpkt0 = buf[7];

    /* 2. an address */
    if (b->next_addr > 127) { note(d->note, "no USB addresses left"); return di; }
    addr = b->next_addr;
    r = zzp_control(b, d, RT_OUT | RT_STANDARD | RT_DEVICE, REQ_SET_ADDRESS, (uint16_t)addr, 0, NULL, 0, NULL);
    if (r != XS_OK) { note(d->note, "the device refused an address"); return di; }
    b->next_addr++;
    d->addr = (uint8_t)addr;
    b->t->delay_ms(b->t->ctx, 5);              /* SET_ADDRESS recovery interval is 2 ms */

    /* 3. the whole device descriptor */
    r = zzp_control(b, d, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_DEVICE << 8, 0, buf, 18, &got);
    if (r != XS_OK || got < 18) { note(d->note, "could not read the device descriptor"); return di; }
    d->bcd_usb = (uint16_t)(buf[2] | (buf[3] << 8));
    d->cls = buf[4]; d->subclass = buf[5]; d->proto = buf[6];
    d->vid = (uint16_t)(buf[8] | (buf[9] << 8));
    d->pid = (uint16_t)(buf[10] | (buf[11] << 8));
    d->bcd_dev = (uint16_t)(buf[12] | (buf[13] << 8));
    {
        uint8_t imanu = buf[14], iprod = buf[15], iser = buf[16], nconf = buf[17];
        uint8_t lb[4];

        /* 4. the first configuration, its length first */
        if (nconf == 0) { note(d->note, "the device has no configuration"); return di; }
        r = zzp_control(b, d, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_CONFIG << 8, 0, buf, 9, &got);
        if (r != XS_OK || got < 9) { note(d->note, "could not read the configuration"); return di; }
        total = (uint16_t)(buf[2] | (buf[3] << 8));
        if (total > sizeof(buf)) total = sizeof(buf);
        r = zzp_control(b, d, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_CONFIG << 8, 0, buf, total, &got);
        if (r != XS_OK) { note(d->note, "could not read the configuration"); return di; }
        parse_config(d, buf, got);

        /* 5. names: the first language the device offers */
        if (imanu || iprod || iser) {
            if (zzp_control(b, d, RT_IN | RT_STANDARD | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_STRING << 8, 0, lb, 4, &got) == XS_OK && got >= 4)
                lang = (uint16_t)(lb[2] | (lb[3] << 8));
            get_string(b, d, imanu, lang, d->manufacturer);
            get_string(b, d, iprod, lang, d->product);
            get_string(b, d, iser, lang, d->serial);
        }
    }

    /* 6. select it */
    r = zzp_control(b, d, RT_OUT | RT_STANDARD | RT_DEVICE, REQ_SET_CONFIGURATION, d->config, 0, NULL, 0, NULL);
    if (r != XS_OK) { note(d->note, "the device refused its configuration"); d->config = 0; return di; }
    d->ok = 1;

    if (d->cls == CLASS_HUB || zzp_find_if(d, CLASS_HUB, -1, -1)) {
        d->is_hub = 1;
        enumerate_hub(b, di);
    }
    return di;
}

static int hub_port_status(struct zzp_bus *b, struct zzp_dev *h, uint8_t port, uint16_t *status, uint16_t *change)
{
    uint8_t s[4];
    uint32_t got = 0;
    int r = zzp_control(b, h, RT_IN | RT_CLASS | RT_OTHER, REQ_GET_STATUS, 0, port, s, 4, &got);
    if (r != XS_OK || got < 4) return r == XS_OK ? XS_ERROR : r;
    *status = (uint16_t)(s[0] | (s[1] << 8));
    *change = (uint16_t)(s[2] | (s[3] << 8));
    return XS_OK;
}

static void hub_feature(struct zzp_bus *b, struct zzp_dev *h, int set, uint16_t feature, uint8_t port)
{
    zzp_control(b, h, RT_OUT | RT_CLASS | RT_OTHER, set ? REQ_SET_FEATURE : REQ_CLEAR_FEATURE, feature, port, NULL, 0, NULL);
}

static void enumerate_hub(struct zzp_bus *b, int hi)
{
    struct zzp_dev *h = &b->dev[hi];
    uint8_t buf[16];
    uint32_t got = 0;
    int port, r, tries;

    r = zzp_control(b, h, RT_IN | RT_CLASS | RT_DEVICE, REQ_GET_DESCRIPTOR, DT_HUB << 8, 0, buf, sizeof(buf), &got);
    if (r != XS_OK || got < 7) { note(h->note, "could not read the hub descriptor"); return; }
    h->hub_ports = buf[2];
    h->hub_think = (uint8_t)((buf[3] >> 5) & 3);    /* wHubCharacteristics bits 5-6 */
    h->hub_pwr_ms = buf[5];                           /* bPwrOn2PwrGood, in 2 ms units */
    h->hub_multi_tt = (h->proto == 2 || zzp_find_if(h, CLASS_HUB, -1, 2)) ? 1 : 0;
    if (h->hub_ports > 15) h->hub_ports = 15;

    for (port = 1; port <= h->hub_ports; port++)
        hub_feature(b, h, 1, PORT_POWER, (uint8_t)port);
    b->t->delay_ms(b->t->ctx, h->hub_pwr_ms * 2 + 100);   /* power good, then devices need 100 ms to settle */

    for (port = 1; port <= h->hub_ports; port++) {
        uint16_t st = 0, ch = 0;
        uint8_t speed;
        if (hub_port_status(b, h, (uint8_t)port, &st, &ch) != XS_OK) continue;
        if (ch & PC_CONNECTION) hub_feature(b, h, 0, C_PORT_CONNECTION, (uint8_t)port);
        if (!(st & PS_CONNECTION)) continue;
        hub_feature(b, h, 1, PORT_RESET, (uint8_t)port);
        for (tries = 0; tries < 20; tries++) {            /* reset takes 10-20 ms */
            b->t->delay_ms(b->t->ctx, 10);
            if (hub_port_status(b, h, (uint8_t)port, &st, &ch) == XS_OK && (ch & PC_RESET)) break;
        }
        hub_feature(b, h, 0, C_PORT_RESET, (uint8_t)port);
        if (!(st & PS_ENABLE)) continue;                   /* reset failed or the device went away */
        b->t->delay_ms(b->t->ctx, 10);                    /* reset recovery */
        speed = (st & PS_LOW_SPEED) ? SPEED_LOW : (st & PS_HIGH_SPEED) ? SPEED_HIGH : SPEED_FULL;
        enumerate(b, hi, (uint8_t)port, speed);
        h = &b->dev[hi];                                   /* (the array does not move; kept for clarity) */
    }
}

int zzp_scan(struct zzp_bus *b)
{
    int speed = SPEED_FULL, r, n, i;

    memset(b->dev, 0, sizeof(b->dev));
    b->ndev = 0;
    b->next_addr = 1;
    b->note[0] = 0;
    b->xfers = b->errors = 0;
    r = b->t->root_check(b->t->ctx, &speed);
    if (r == XS_OK)
        r = b->t->root_reset(b->t->ctx, &speed);
    b->root_status = r;
    if (r == XS_NOBOARD) { note(b->note, "no ZZ9000 USB port could be opened"); return 0; }
    if (r == XS_OFFLINE) { note(b->note, "nothing is plugged into the ZZ9000's USB port"); return 0; }
    if (r != XS_OK) { note(b->note, "the ZZ9000's USB port did not reset"); return 0; }
    if (speed == SPEED_LOW) {
        note(b->note, "a low-speed device (a mouse or keyboard) is plugged straight into the ZZ9000: "
                      "it only works behind a USB 2.0 hub");
        return 0;
    }
    b->t->delay_ms(b->t->ctx, 20);
    enumerate(b, -1, 0, (uint8_t)speed);
    for (i = 0, n = 0; i < b->ndev; i++)
        if (b->dev[i].used) n++;
    return n;
}

const struct zzp_if *zzp_find_if(const struct zzp_dev *d, uint8_t cls, int subclass, int proto)
{
    int i;
    for (i = 0; i < d->nif; i++) {
        const struct zzp_if *f = &d->ifs[i];
        if (f->cls == cls && (subclass < 0 || f->subclass == subclass) && (proto < 0 || f->proto == proto))
            return f;
    }
    return NULL;
}

const char *zzp_class_name(uint8_t cls, uint8_t subclass, uint8_t proto)
{
    switch (cls) {
    case CLASS_PER_INTERFACE: return "(see interfaces)";
    case CLASS_AUDIO:   return "audio";
    case CLASS_COMM:    return "communications";
    case CLASS_HID:
        if (subclass == 1 && proto == 1) return "keyboard";
        if (subclass == 1 && proto == 2) return "mouse";
        return "input device";
    case CLASS_PRINTER: return "printer";
    case CLASS_STORAGE: return "storage";
    case CLASS_HUB:     return "hub";
    case CLASS_CDC_DATA: return "data";
    case CLASS_VIDEO:   return "video";
    case CLASS_VENDOR:  return "vendor-specific";
    default:            return "other";
    }
}

const char *zzp_speed_name(uint8_t speed)
{
    return speed == SPEED_HIGH ? "high speed" : speed == SPEED_FULL ? "full speed" : "low speed";
}

const char *zzp_status_name(int status)
{
    switch (status) {
    case XS_OK:          return "ok";
    case XS_TIMEOUT:     return "timed out";
    case XS_STALL:       return "stalled";
    case XS_NAK:         return "no data";
    case XS_OFFLINE:     return "nothing connected";
    case XS_BADPARAM:    return "refused by the firmware";
    case XS_UNSUPPORTED: return "not supported by the firmware";
    case XS_STALE:       return "stale (the port was reset)";
    case XS_HOSTERROR:   return "host error";
    case XS_NOBOARD:     return "no ZZ9000";
    default:             return "error";
    }
}
