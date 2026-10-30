/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * prefs.c - ZZPlugPrefs: a small window over the running ZZPlug.
 *
 *  - what is plugged in and what ZZPlug does with each device, kept current;
 *    pick one to see its details
 *  - start or stop ZZPlug
 *  - which filesystem FAT sticks mount with (ENV:ZZPlug.dostype)
 *  - the game pads' buttons, in a window of their own: every pad plugged in,
 *    each Amiga button set by pressing the pad's button, kept per kind of pad
 *    (ENV:ZZPlug.padbuttons.VVVV-PPPP); a live readout of the pad
 *  - Record a device: runs ZZPlug DUMP in a window of its own, for a device
 *    ZZPlug does not know yet (the file goes with a bug report)
 *  - Save (ENVARC: too) / Use (ENV: only) / Cancel, as Amiga prefs editors do
 *  - menus as the system's prefs editors have them: Project (About, Save,
 *    Use, Quit), Edit (Reset to defaults, Last saved, Restore), ZZPlug
 *  - a compact layout when the normal one does not fit (640x200, Topaz 8)
 *
 * It asks ZZPlug through its public port (zzpmsg.h); nothing here touches
 * the USB hardware.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include "zzpmsg.h"
#include "amigui.h"

#define PREFS_VERSION "0.8"
static const char ver[] __attribute__((used)) = "$VER: ZZPlugPrefs " PREFS_VERSION " (29.10.2026)";
static const char about_text[] =
    "The prefs editor of ZZPlug, a small USB stack\n"
    "for the ZZ9000: what is plugged in, Start/Stop,\n"
    "FAT sticks and the game pads' buttons.\n\n"
    "GPL-3.0-or-later";

#define PORTNAME "ZZPlug"
#define VAR_DOSTYPE "ZZPlug.dostype"
#define VAR_PAD "ZZPlug.padbuttons"
#define DOSTYPE_CROSSDOS 0x4D534400UL           /* "MSD\0" */
#define DOSTYPE_FAT95    0x46415400UL           /* "FAT\0" */

enum { G_STATE = 1, G_START, G_STOP, G_DEVS, G_DETAIL, G_FAT, G_PADSET, G_PADWIN, G_PADNOW, G_RECTEXT, G_RECORD, G_MSG,
       G_SAVE, G_USE, G_CANCEL };

#define MAXLINES 40
#define LINELEN  120

struct lines {
    struct List list;
    struct Node node[MAXLINES];
    char text[MAXLINES][LINELEN];
    int n;
};

static struct Screen *scr;
static APTR vi;
static struct Window *win;
static struct Gadget *glist, *gState, *gStart, *gStop, *gDevs, *gDetail, *gFat, *gPadSet, *gPadNow, *gMsg;
static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct TextAttr *font;

static struct lines devs, detail;
static int devidx[MAXLINES];                    /* the bus index each device line stands for, -1 = a note */
static int selected = -1;                       /* the line picked in the device list */
static int seldev = -1;                         /* and its bus index */
static char lastlist[4096], lastdetail[2048];
static int running = -1;

static struct MsgPort *reply;

/* ---- the menus: Project / Edit as the system's prefs editors have them ---- */

enum { MN_ABOUT = 1, MN_SAVE, MN_USE, MN_QUIT, MN_DEFAULTS, MN_LASTSAVED, MN_RESTORE,
       MN_START, MN_STOP, MN_PADS, MN_RECORD };

static struct NewMenu main_menu_def[] = {
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"About...", NULL, 0, 0, (APTR)MN_ABOUT },
    { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Save", (STRPTR)"S", 0, 0, (APTR)MN_SAVE },
    { NM_ITEM, (STRPTR)"Use", (STRPTR)"U", 0, 0, (APTR)MN_USE },
    { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)MN_QUIT },
    { NM_TITLE, (STRPTR)"Edit", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Reset to defaults", (STRPTR)"D", 0, 0, (APTR)MN_DEFAULTS },
    { NM_ITEM, (STRPTR)"Last saved", (STRPTR)"L", 0, 0, (APTR)MN_LASTSAVED },
    { NM_ITEM, (STRPTR)"Restore", (STRPTR)"R", 0, 0, (APTR)MN_RESTORE },
    { NM_TITLE, (STRPTR)"ZZPlug", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Start ZZPlug", NULL, 0, 0, (APTR)MN_START },
    { NM_ITEM, (STRPTR)"Stop ZZPlug", NULL, 0, 0, (APTR)MN_STOP },
    { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Game pad buttons...", (STRPTR)"B", 0, 0, (APTR)MN_PADS },
    { NM_ITEM, (STRPTR)"Record a device...", NULL, 0, 0, (APTR)MN_RECORD },
    { NM_END, NULL, NULL, 0, 0, NULL } };

/* the game pad window's own: Edit works on the pad shown */
static struct NewMenu pad_menu_def[] = {
    { NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"About...", NULL, 0, 0, (APTR)MN_ABOUT },
    { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Save", (STRPTR)"S", 0, 0, (APTR)MN_SAVE },
    { NM_ITEM, (STRPTR)"Use", (STRPTR)"U", 0, 0, (APTR)MN_USE },
    { NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Close (Cancel)", (STRPTR)"Q", 0, 0, (APTR)MN_QUIT },
    { NM_TITLE, (STRPTR)"Edit", NULL, 0, 0, NULL },
    { NM_ITEM, (STRPTR)"Reset to defaults", (STRPTR)"D", 0, 0, (APTR)MN_DEFAULTS },
    { NM_ITEM, (STRPTR)"Last saved", (STRPTR)"L", 0, 0, (APTR)MN_LASTSAVED },
    { NM_ITEM, (STRPTR)"Restore", (STRPTR)"R", 0, 0, (APTR)MN_RESTORE },
    { NM_END, NULL, NULL, 0, 0, NULL } };

static struct Menu *main_menu, *pad_menu;

static struct Menu *make_menu(struct NewMenu *def)
{
    struct Menu *m = CreateMenus(def, TAG_END);
    if (m && !LayoutMenus(m, vi, GTMN_NewLookMenus, TRUE, TAG_END)) { FreeMenus(m); m = NULL; }
    return m;
}

static void about(struct Window *w)
{
    amg_about(w, "ZZPlugPrefs", PREFS_VERSION, about_text);
}

/* the text of ENVARC:<name> (what Save left), 0 = none */
static int read_envarc(const char *name, char *buf, LONG size)
{
    char path[80];
    BPTR fh;
    LONG n;
    strcpy(path, "ENVARC:");
    strncat(path, name, sizeof(path) - 8);
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) return 0;
    n = Read(fh, buf, size - 1);
    Close(fh);
    if (n < 0) n = 0;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
    return n > 0;
}

/* the title bar's height on this screen (the screen's font, not the gadgets') */
static int title_h(void)
{
    return scr->WBorTop + scr->Font->ta_YSize + 1;
}

/* the settings as edited */
static ULONG dostype;                           /* 0 = ZZPlug's default (CrossDOS) */
static char fatlabel_other[40];
static STRPTR fatlabels[4];
static int fatactive;

static char statetext[80], padsettext[80], padnowtext[120], msgtext[120];

/* ------------------------------------------------------------------ */

static int ask(UWORD cmd, LONG arg, char *buf, ULONG len, ULONG *out)
{
    struct zzp_msg m;
    struct MsgPort *p;
    memset(&m, 0, sizeof(m));
    m.m.mn_Node.ln_Type = NT_MESSAGE;
    m.m.mn_ReplyPort = reply;
    m.m.mn_Length = sizeof(m);
    m.cmd = cmd;
    m.arg = arg;
    m.buf = buf;
    m.buflen = len;
    if (buf && len) buf[0] = 0;
    Forbid();
    p = FindPort((STRPTR)PORTNAME);
    if (p) PutMsg(p, &m.m);
    Permit();
    if (!p) return -2;                          /* ZZPlug is not running */
    WaitPort(reply);
    GetMsg(reply);
    if (out) { out[0] = m.out[0]; out[1] = m.out[1]; out[2] = m.out[2]; out[3] = m.out[3]; }
    return m.rc;
}

static int zzplug_running(void)
{
    struct MsgPort *p;
    Forbid();
    p = FindPort((STRPTR)PORTNAME);
    Permit();
    return p != NULL;
}

static void settext(struct Gadget *g, char *buf, const char *s)
{
    if (buf != s) { strncpy(buf, s, 119); buf[119] = 0; }
    if (g && win) GT_SetGadgetAttrs(g, win, NULL, GTTX_Text, (ULONG)buf, TAG_END);
}

static void message(const char *s)
{
    strncpy(msgtext, s, sizeof(msgtext) - 1);
    msgtext[sizeof(msgtext) - 1] = 0;
    settext(gMsg, msgtext, msgtext);
}

static void lines_init(struct lines *l)
{
    l->list.lh_Head = (struct Node *)&l->list.lh_Tail;
    l->list.lh_Tail = NULL;
    l->list.lh_TailPred = (struct Node *)&l->list.lh_Head;
    l->n = 0;
}

static void lines_add(struct lines *l, const char *s)
{
    struct Node *nd;
    if (l->n >= MAXLINES) return;
    strncpy(l->text[l->n], s, LINELEN - 1);
    l->text[l->n][LINELEN - 1] = 0;
    nd = &l->node[l->n];
    memset(nd, 0, sizeof(*nd));
    nd->ln_Name = l->text[l->n];
    AddTail(&l->list, nd);
    l->n++;
}

/* ------------------------------------------------------------------ */

static void show_detail(int force)
{
    static char buf[2048];
    char *p, *e;
    if (seldev < 0) buf[0] = 0;
    else if (ask(ZZP_MSG_DETAIL, seldev, buf, sizeof(buf), NULL) < -1) buf[0] = 0;
    if (!force && !strcmp(buf, lastdetail)) return;
    strcpy(lastdetail, buf);
    if (gDetail) GT_SetGadgetAttrs(gDetail, win, NULL, GTLV_Labels, ~0UL, TAG_END);
    lines_init(&detail);
    for (p = buf; *p; p = e) {
        e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        { char t[LINELEN]; int n = (int)(e - p) < LINELEN - 1 ? (int)(e - p) : LINELEN - 1;
          memcpy(t, p, n); t[n] = 0; lines_add(&detail, t); }
        if (*e) e++;
    }
    if (gDetail) GT_SetGadgetAttrs(gDetail, win, NULL, GTLV_Labels, (ULONG)&detail.list, TAG_END);
}

static void refresh_state(void)
{
    int r = zzplug_running();
    if (r == running) return;
    if (r && running == 0 && !strncmp(msgtext, "Starting", 8)) message("ZZPlug started.");
    running = r;
    settext(gState, statetext, r ? "ZZPlug is running." : "ZZPlug is not running.");
    if (gStart) GT_SetGadgetAttrs(gStart, win, NULL, GA_Disabled, r, TAG_END);
    if (gStop) GT_SetGadgetAttrs(gStop, win, NULL, GA_Disabled, !r, TAG_END);
}

static void refresh_list(int force)
{
    static char buf[4096];
    char *p, *e;
    int i, newsel = -1;

    refresh_state();
    if (ask(ZZP_MSG_LIST, 0, buf, sizeof(buf), NULL) == -2)
        strcpy(buf, "-1\t0\tZZPlug is not running: Start runs it.\t\n");
    if (!force && !strcmp(buf, lastlist)) { show_detail(0); return; }
    strcpy(lastlist, buf);
    if (gDevs) GT_SetGadgetAttrs(gDevs, win, NULL, GTLV_Labels, ~0UL, TAG_END);
    lines_init(&devs);
    for (p = buf; *p && devs.n < MAXLINES; p = e) {
        char *f[4], t[LINELEN];
        int k, depth;
        e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if (*e) *e++ = 0;
        f[0] = p;
        for (k = 1; k < 4; k++) {
            char *tab = strchr(f[k - 1], '\t');
            if (tab) { *tab = 0; f[k] = tab + 1; } else f[k] = (char *)"";
        }
        depth = atoi(f[1]);
        t[0] = 0;
        for (k = 0; k < depth && k < 6; k++) strcat(t, "  ");
        strncat(t, f[2], LINELEN - 1 - strlen(t));
        if (f[3][0]) { strncat(t, " - ", LINELEN - 1 - strlen(t)); strncat(t, f[3], LINELEN - 1 - strlen(t)); }
        devidx[devs.n] = atoi(f[0]);
        if (devidx[devs.n] == seldev && seldev >= 0) newsel = devs.n;
        lines_add(&devs, t);
    }
    if (newsel < 0) {                           /* the device picked before is gone: pick the first one */
        for (i = 0; i < devs.n && devidx[i] < 0; i++) ;
        newsel = i < devs.n ? i : -1;
    }
    selected = newsel;
    seldev = newsel >= 0 ? devidx[newsel] : -1;
    if (gDevs) GT_SetGadgetAttrs(gDevs, win, NULL, GTLV_Labels, (ULONG)&devs.list,
                                 GTLV_Selected, (ULONG)(selected >= 0 ? selected : ~0UL), TAG_END);
    show_detail(1);
}

/* ------------------------------------------------------------------ */

static void pad_words(ULONG s, char *out)
{
    static const struct { ULONG bit; const char *name; } w[] = {
        { 1UL << 3, "up" }, { 1UL << 2, "down" }, { 1UL << 1, "left" }, { 1UL << 0, "right" },
        { 1UL << 22, "red" }, { 1UL << 23, "blue" }, { 1UL << 20, "green" }, { 1UL << 21, "yellow" },
        { 1UL << 18, "reverse" }, { 1UL << 19, "forward" }, { 1UL << 17, "play" } };
    int i;
    out[0] = 0;
    for (i = 0; i < (int)(sizeof(w) / sizeof(w[0])); i++)
        if (s & w[i].bit) { if (out[0]) strcat(out, ", "); strcat(out, w[i].name); }
    if (!out[0]) strcpy(out, "nothing pressed");
}

/* the variable a pad's buttons are kept in: one per kind of pad, so pads with
 * different layouts can be plugged in together */
static const char *pad_var(ULONG id)
{
    static char name[40];
    if (!id) return VAR_PAD;
    sprintf(name, VAR_PAD ".%04lX-%04lX", (unsigned long)(id >> 16), (unsigned long)(id & 0xffff));
    return name;
}

static void poll_pad(void)
{
    ULONG out[4];
    char t[120], w[100];
    int r = ask(ZZP_MSG_PAD, 0, NULL, 0, out), i;
    if (r != 0) {
        settext(gPadNow, padnowtext, r == -2 ? "-" : "No game pad plugged in");
        return;
    }
    pad_words(out[1], w);
    t[0] = 0;
    for (i = 0; i < 16; i++)
        if (out[0] & (1UL << i)) sprintf(t + strlen(t), t[0] ? " %d" : "%d", i + 1);
    if (t[0]) { char u[120]; sprintf(u, "%s  (pad button %s)", w, t); strcpy(t, u); } else strcpy(t, w);
    if (strcmp(t, padnowtext)) settext(gPadNow, padnowtext, t);
}

/* "2 plugged in: ..." beside the Game pads... button */
static void pads_summary(void)
{
    static char buf[400];
    char t[120];
    ULONG out[4];
    int i, n = 0;
    t[0] = 0;
    for (i = 0; i < 2; i++)
        if (ask(ZZP_MSG_PAD, i, buf, sizeof(buf), out) == 0) {
            char *e = buf;
            while (*e && *e != 10) e++;
            *e = 0;
            if (n++) strcat(t, ", ");
            strncat(t, buf, 40);
        }
    if (!n) strcpy(t, "None plugged in");
    else { char u[120]; sprintf(u, "%d plugged in: %s", n, t); strcpy(t, u); }
    if (strcmp(t, padsettext)) settext(gPadSet, padsettext, t);
}

/* ---- the Game pad window: every pad plugged in, and which of its buttons
 * plays each Amiga button. Set one row by pressing the button, or Learn all;
 * a light shows each row while its button is held. ---- */

static int tlen(const char *s);

#define MAXPADS 2
#define NPLACES 16                              /* places in a layout (hidrep.h PAD_MAXBUTTONS) */
#define NROWS 7

enum { P_PAD = 100, P_FIELD, P_SET = P_FIELD + NROWS, P_NOW = P_SET + NROWS, P_LEARN, P_BUILTIN, P_INFO,
       P_SAVE, P_USE, P_CANCEL };

struct pad {
    int present;
    ULONG id;                                   /* vendor << 16 | product */
    char name[48];
    int port;                                   /* its joystick port */
    UBYTE pos[NPLACES];                         /* as edited: the pad's button in each place, 0 = none */
    UBYTE builtin[NPLACES];
    int own;                                    /* 1 = a layout of its own (ENV:), 0 = built-in */
    int changed;
};

static struct pad pads[MAXPADS];
static int npads_now, curpad;

static const char *const row_name[NROWS] = { "Red (fire)", "Blue", "Green", "Yellow", "Reverse", "Forward", "Play" };
static const int row_place[NROWS] = { 0, 1, 2, 3, 4, 5, 8 };   /* each row's place in the layout */
static const signed char row_more[NROWS][2] = {                 /* more places that play the same part */
    { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { 6, -1 }, { 7, -1 }, { 9, 12 } };

static struct Window *pwin;
static struct Gadget *pglist, *pgPad, *pgField[NROWS], *pgSet[NROWS], *pgNow, *pgInfo;
static char pfield[NROWS][40], pnow[120], pinfo[120];
static char padlabel[MAXPADS][90];
static STRPTR padlabels[MAXPADS + 1];
static int lamp_x, lamp_w, row_y[NROWS], row_h, sw_x, head_y;
static int lamp_lit[NROWS];
static LONG pen_lit = -1, pen_off = -1, pen_sw[4] = { -1, -1, -1, -1 };
static int setrow = -1, setwait, learnall;
static ULONG rawnow;

/* what ZZPlug says about pad n; 0 = there is no such pad */
static int pad_read(int n, struct pad *p, ULONG *out)
{
    static char buf[400];
    char *l[4], *s;
    int k, i;
    if (ask(ZZP_MSG_PAD, n, buf, sizeof(buf), out) != 0) return 0;
    for (s = buf, k = 0; k < 4; k++) {          /* name, "port N", layout now, built-in layout */
        l[k] = s;
        while (*s && *s != 10) s++;
        if (*s) *s++ = 0;
    }
    memset(p, 0, sizeof(*p));
    p->present = 1;
    p->id = out[3];
    strncpy(p->name, l[0], sizeof(p->name) - 1);
    p->port = atoi(l[1] + 5);
    for (k = 2; k < 4; k++)
        for (s = l[k], i = 0; i < NPLACES; i++) {
            UBYTE v = (UBYTE)strtol(s, &s, 10);
            if (k == 2) p->pos[i] = v; else p->builtin[i] = v;
        }
    {
        char v[64];
        p->own = GetVar((STRPTR)pad_var(p->id), (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0;
    }
    return 1;
}

static void row_text(const struct pad *p, int r, char *out)
{
    int b[3], n = 0, i, k;
    b[n++] = p->pos[row_place[r]];
    for (k = 0; k < 2; k++) if (row_more[r][k] >= 0) b[n++] = p->pos[(int)row_more[r][k]];
    out[0] = 0;
    for (i = 0; i < n; i++) {
        int dup = 0;
        for (k = 0; k < i; k++) if (b[k] == b[i]) dup = 1;
        if (!b[i] || dup) continue;
        sprintf(out + strlen(out), out[0] ? ", %d" : "%d", b[i]);
    }
    if (!out[0]) strcpy(out, "none");
    else if (strchr(out, ',')) { char t[40]; sprintf(t, "buttons %s", out); strcpy(out, t); }
    else { char t[40]; sprintf(t, "button %s", out); strcpy(out, t); }
}

static int row_held(const struct pad *p, int r, ULONG raw)
{
    int k, b = p->pos[row_place[r]];
    if (b && (raw & (1UL << (b - 1)))) return 1;
    for (k = 0; k < 2; k++)
        if (row_more[r][k] >= 0 && (b = p->pos[(int)row_more[r][k]]) && (raw & (1UL << (b - 1)))) return 1;
    return 0;
}

static void draw_lamp(int r, int lit)
{
    struct RastPort *rp = pwin->RPort;
    int y = row_y[r];
    DrawBevelBox(rp, lamp_x, y, lamp_w, row_h, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_END);
    SetAPen(rp, lit ? (pen_lit >= 0 ? pen_lit : 3) : (pen_off >= 0 ? pen_off : 1));
    RectFill(rp, lamp_x + 2, y + 1, lamp_x + lamp_w - 3, y + row_h - 2);
    lamp_lit[r] = lit;
}

/* the colour swatches and the tape-deck marks in front of the rows */
static void draw_marks(void)
{
    struct RastPort *rp = pwin->RPort;
    int r, k, s = row_h - 4, x = sw_x, cy;
    for (r = 0; r < NROWS; r++) {
        int y = row_y[r] + 2;
        if (r < 4) {
            DrawBevelBox(rp, x, y, s + 4, s, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_END);
            SetAPen(rp, pen_sw[r] >= 0 ? pen_sw[r] : 3);
            RectFill(rp, x + 2, y + 1, x + s + 1, y + s - 2);
            continue;
        }
        SetAPen(rp, 1);
        cy = y + s / 2;
        for (k = 0; k <= s / 2; k++) {          /* a triangle (two for reverse/forward) */
            int h = s / 2 - k;
            if (r == 4) { Move(rp, x + k, cy - (s / 2 - h)); Draw(rp, x + k, cy + (s / 2 - h));
                          Move(rp, x + k + s / 2 + 1, cy - (s / 2 - h)); Draw(rp, x + k + s / 2 + 1, cy + (s / 2 - h)); }
            else if (r == 5) { Move(rp, x + k, cy - h); Draw(rp, x + k, cy + h);
                               Move(rp, x + k + s / 2 + 1, cy - h); Draw(rp, x + k + s / 2 + 1, cy + h); }
            else { Move(rp, x + 2 + k, cy - h); Draw(rp, x + 2 + k, cy + h); }
        }
    }
    for (r = 0; r < NROWS; r++) draw_lamp(r, lamp_lit[r]);
}

/* settext() for this window's gadgets */
static void psettext(struct Gadget *g, char *buf, const char *s)
{
    if (buf != s) { strncpy(buf, s, 119); buf[119] = 0; }
    if (g && pwin) GT_SetGadgetAttrs(g, pwin, NULL, GTTX_Text, (ULONG)buf, TAG_END);
}

static void pad_info(const char *s)
{
    strncpy(pinfo, s, sizeof(pinfo) - 1);
    psettext(pgInfo, pinfo, pinfo);
}

static void show_pad(void)
{
    struct pad *p = &pads[curpad];
    int r, off = !p->present;
    char t[120];
    for (r = 0; r < NROWS; r++) {
        if (off) strcpy(pfield[r], "-");
        else if (r == setrow) strcpy(pfield[r], "Press a button now...");
        else row_text(p, r, pfield[r]);
        GT_SetGadgetAttrs(pgField[r], pwin, NULL, GTTX_Text, (ULONG)pfield[r], TAG_END);
        GT_SetGadgetAttrs(pgSet[r], pwin, NULL, GA_Disabled, off, TAG_END);
    }
    if (off) strcpy(t, "Plug a game pad in to set its buttons.");
    else sprintf(t, "%04lX:%04lX: %s%s", (unsigned long)(p->id >> 16), (unsigned long)(p->id & 0xffff),
                 p->own ? "its own buttons" : "the built-in buttons", p->changed ? " (changed)" : "");
    if (setrow < 0) pad_info(t);
}

static void make_padlabels(void)
{
    int i;
    for (i = 0; i < npads_now; i++) {
        sprintf(padlabel[i], "%s [%04lX:%04lX], joystick port %d", pads[i].name,
                (unsigned long)(pads[i].id >> 16), (unsigned long)(pads[i].id & 0xffff), pads[i].port);
        padlabels[i] = (STRPTR)padlabel[i];
    }
    if (!npads_now) { strcpy(padlabel[0], "No game pad plugged in"); padlabels[0] = (STRPTR)padlabel[0]; i = 1; }
    padlabels[i] = NULL;
}

/* which pads are plugged in; a pad that stays keeps what was edited */
static void pads_refresh(int force)
{
    struct pad p;
    ULONG out[4];
    int i, n = 0, same = 1;
    for (i = 0; i < MAXPADS; i++) {
        if (pad_read(i, &p, out)) {
            if (!pads[n].present || pads[n].id != p.id || pads[n].port != p.port) { pads[n] = p; same = 0; }
            n++;
        }
    }
    for (i = n; i < MAXPADS; i++) { if (pads[i].present) same = 0; pads[i].present = 0; }
    if (same && n == npads_now && !force) return;
    npads_now = n;
    if (curpad >= n) curpad = 0;
    setrow = -1; learnall = 0;
    make_padlabels();
    if (pwin) {
        GT_SetGadgetAttrs(pgPad, pwin, NULL, GTCY_Labels, (ULONG)padlabels, GTCY_Active, (ULONG)curpad,
                          GA_Disabled, n < 1, TAG_END);
        show_pad();
    }
}

static void set_start(int r)
{
    setrow = r; setwait = 1;
    show_pad();
    { char t[100]; sprintf(t, "Press the pad's button for %s (Set again leaves it).", row_name[r]); pad_info(t); }
}

static void set_done(int b)
{
    struct pad *p = &pads[curpad];
    int r = setrow, k;
    if (b) {
        p->pos[row_place[r]] = (UBYTE)b;
        for (k = 0; k < 2; k++) if (row_more[r][k] >= 0) p->pos[(int)row_more[r][k]] = 0;
        p->own = 1; p->changed = 1;
    }
    setrow = -1;
    if (learnall && r + 1 < NROWS) { set_start(r + 1); return; }
    learnall = 0;
    show_pad();
}

static void pad_poll(void)
{
    struct pad *p = &pads[curpad];
    ULONG out[4];
    char t[120], w[100];
    int r, i;
    if (!p->present || ask(ZZP_MSG_PAD, curpad, NULL, 0, out) != 0) { pads_refresh(0); return; }
    rawnow = out[0];
    for (r = 0; r < NROWS; r++) {
        int lit = row_held(p, r, rawnow);
        if (lit != lamp_lit[r]) draw_lamp(r, lit);
    }
    if (setrow >= 0) {
        if (!rawnow) setwait = 0;
        else if (!setwait) {
            for (i = 0; i < 16 && !(rawnow & (1UL << i)); i++) ;
            setwait = 1;
            set_done(i + 1);
        }
    }
    pad_words(out[1], w);
    t[0] = 0;
    for (i = 0; i < 16; i++)
        if (rawnow & (1UL << i)) sprintf(t + strlen(t), t[0] ? " %d" : "%d", i + 1);
    if (t[0]) { char u[120]; sprintf(u, "%s  (pad button %s)", w, t); strcpy(t, u); } else strcpy(t, w);
    if (strcmp(t, pnow)) psettext(pgNow, pnow, t);
}

/* Use (save = 0) or Save (1) every pad's buttons */
static int pads_apply(int save)
{
    ULONG fl = GVF_GLOBAL_ONLY | (save ? GVF_SAVE_VAR : 0);
    int i, k, any = 0, ok = 1;
    for (i = 0; i < MAXPADS; i++) {
        struct pad *p = &pads[i];
        char v[64];
        if (!p->present || !p->changed) continue;
        any = 1;
        if (!p->own) {
            DeleteVar((STRPTR)pad_var(p->id), GVF_GLOBAL_ONLY);
            if (save) DeleteVar((STRPTR)pad_var(p->id), GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
            continue;
        }
        v[0] = 0;
        for (k = 0; k < NPLACES; k++) {
            if (k) strcat(v, " ");
            if (p->pos[k]) sprintf(v + strlen(v), "%d", p->pos[k]); else strcat(v, "-");
        }
        ok &= SetVar((STRPTR)pad_var(p->id), (STRPTR)v, -1, fl) != 0;
    }
    if (any) ask(ZZP_MSG_RESCAN, 0, NULL, 0, NULL);   /* pads read their buttons when found */
    return ok;
}

/* Edit > Last saved: the buttons ENVARC: holds for this kind of pad (none
 * there = the built-in ones). 0 = no pad. */
static int pad_last_saved(struct pad *p)
{
    char v[96], *s = v;
    int k;
    if (!p->present) return 0;
    if (!read_envarc(pad_var(p->id), v, sizeof(v))) {
        memcpy(p->pos, p->builtin, NPLACES);
        p->own = 0;
    } else {
        for (k = 0; k < NPLACES; k++) {
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '-') { p->pos[k] = 0; s++; }
            else if (*s >= '0' && *s <= '9') p->pos[k] = (UBYTE)strtol(s, &s, 10);
            else p->pos[k] = 0;
        }
        p->own = 1;
    }
    p->changed = 1;
    return 1;
}

static int make_padgadgets(int *pw, int *ph, int compact)
{
    struct NewGadget ng;
    struct Gadget *g;
    int fh = font->ta_YSize, gh = fh + (compact ? 4 : 6), row = gh + (compact ? 2 : 4);
    int gap = compact ? 2 : 6;
    int bl = scr->WBorLeft + 8, bt = title_h() + (compact ? 3 : 6);
    int labw = tlen("Red (fire)") + 16 + fh + 8, setw = tlen("Set") + 24, iw, fw, y, r, bw;
    int cw = tlen("abcdefghijklmnopqrstuvwxyzABCDEFGHIJ") / 36 + 1;

    iw = cw * 60;
    if (iw < 460) iw = 460;
    if (iw > scr->Width - bl - 8 - scr->WBorRight) iw = scr->Width - bl - 8 - scr->WBorRight;
    lamp_w = tlen("Held") + 8;
    fw = iw - labw - setw - lamp_w - 16;
    row_h = gh;

    g = CreateContext(&pglist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = font;
    ng.ng_VisualInfo = vi;

    y = bt;
    ng.ng_LeftEdge = bl + tlen("Pad") + 12; ng.ng_TopEdge = y; ng.ng_Width = iw - tlen("Pad") - 12; ng.ng_Height = gh;
    ng.ng_GadgetText = (STRPTR)"Pad"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = P_PAD;
    g = pgPad = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)padlabels, GTCY_Active, (ULONG)curpad,
                             GA_Disabled, npads_now < 1, TAG_END);
    y += row;
    head_y = y + (compact ? 0 : 2);             /* the column heads */
    y += fh + (compact ? 2 : 6);

    sw_x = bl;
    lamp_x = bl + iw - lamp_w;
    for (r = 0; r < NROWS; r++) {
        row_y[r] = y;
        ng.ng_LeftEdge = bl + labw; ng.ng_TopEdge = y; ng.ng_Width = fw; ng.ng_Height = gh;
        ng.ng_GadgetText = (STRPTR)row_name[r]; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = P_FIELD + r;
        g = pgField[r] = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)pfield[r], GTTX_Border, TRUE, TAG_END);
        ng.ng_LeftEdge = bl + labw + fw + 8; ng.ng_Width = setw;
        ng.ng_GadgetText = (STRPTR)"Set"; ng.ng_Flags = 0; ng.ng_GadgetID = P_SET + r;
        g = pgSet[r] = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
        y += row;
    }
    y += gap;
    ng.ng_LeftEdge = bl + tlen("Pad now") + 12; ng.ng_TopEdge = y; ng.ng_Width = iw - tlen("Pad now") - 12;
    ng.ng_GadgetText = (STRPTR)"Pad now"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = P_NOW;
    g = pgNow = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)pnow, GTTX_Border, TRUE, TAG_END);
    y += row;
    bw = tlen("Learn all") + 24;
    ng.ng_LeftEdge = bl; ng.ng_TopEdge = y; ng.ng_Width = bw; ng.ng_Flags = 0;
    ng.ng_GadgetText = (STRPTR)"Learn all"; ng.ng_GadgetID = P_LEARN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = bl + bw + 8; ng.ng_GadgetText = (STRPTR)"Built-in"; ng.ng_GadgetID = P_BUILTIN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = bl + 2 * bw + 16; ng.ng_Width = iw - 2 * bw - 16; ng.ng_GadgetText = NULL; ng.ng_GadgetID = P_INFO;
    g = pgInfo = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)pinfo, TAG_END);
    y += row + gap;

    bw = tlen("Cancel") + 32;
    if (bw < 90) bw = 90;
    ng.ng_TopEdge = y; ng.ng_Width = bw;
    ng.ng_LeftEdge = bl; ng.ng_GadgetText = (STRPTR)"Save"; ng.ng_GadgetID = P_SAVE;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = bl + (iw - bw) / 2; ng.ng_GadgetText = (STRPTR)"Use"; ng.ng_GadgetID = P_USE;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = bl + iw - bw; ng.ng_GadgetText = (STRPTR)"Cancel"; ng.ng_GadgetID = P_CANCEL;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    y += gh;

    *pw = bl + iw + 8 + scr->WBorRight;
    *ph = y + (compact ? 3 : 6) + scr->WBorBottom;
    return g != NULL;
}

static void pad_heads(void)
{
    struct RastPort *rp = pwin->RPort;
    int y = head_y + rp->TxBaseline;
    SetAPen(rp, 2);                             /* SHINEPEN on 3.x: the highlighted label look */
    Move(rp, sw_x, y); Text(rp, (STRPTR)"Amiga button", 12);
    Move(rp, pgField[0]->LeftEdge, y); Text(rp, (STRPTR)"Pad button", 10);
    Move(rp, lamp_x + 4, y); Text(rp, (STRPTR)"Held", 4);
}

static LONG best_pen(ULONG r, ULONG g, ULONG b)
{
    if (GfxBase->LibNode.lib_Version < 39) return -1;
    return ObtainBestPen(scr->ViewPort.ColorMap, r * 0x01010101UL, g * 0x01010101UL, b * 0x01010101UL,
                         OBP_Precision, PRECISION_GUI, TAG_END);
}

/* a gadget of the pad window (or the menu item standing for one); 1 = close it */
static int pad_action(int i, UWORD code)
{
    if (i >= P_SET && i < P_SET + NROWS) {
        if (setrow == i - P_SET) { learnall = 0; set_done(0); }   /* Set again: leave it */
        else { learnall = 0; set_start(i - P_SET); }
        return 0;
    }
    switch (i) {
    case P_PAD:
        if (code < npads_now) { curpad = code; setrow = -1; learnall = 0; show_pad(); }
        break;
    case P_LEARN:
        if (pads[curpad].present) { learnall = 1; set_start(0); }
        break;
    case P_BUILTIN:
        if (pads[curpad].present) {
            memcpy(pads[curpad].pos, pads[curpad].builtin, NPLACES);
            pads[curpad].own = 0; pads[curpad].changed = 1;
            setrow = -1; learnall = 0;
            show_pad();
        }
        break;
    case P_SAVE:
        if (!pads_apply(1)) { pad_info("Could not save the buttons."); break; }
        message("Game pad buttons saved."); return 1;
    case P_USE:
        if (!pads_apply(0)) { pad_info("Could not set the buttons."); break; }
        message("Game pad buttons in use until the next reboot."); return 1;
    case P_CANCEL: return 1;
    }
    return 0;
}

static void pad_window(struct MsgPort *tport, struct timerequest *treq)
{
    int w, h, done = 0, r, left, top;
    struct TextFont *tf = NULL;

    pads_refresh(1);
    curpad = 0;
    for (r = 0; r < NROWS; r++) { pfield[r][0] = 0; lamp_lit[r] = 0; }
    strcpy(pnow, "-"); pinfo[0] = 0;
    /* a small screen (640x200, say): the compact layout */
    if (!make_padgadgets(&w, &h, 0) || h > scr->Height - scr->BarHeight - 1) {
        FreeGadgets(pglist); pglist = NULL;
        if (!make_padgadgets(&w, &h, 1)) { FreeGadgets(pglist); pglist = NULL; message("Could not open the game pad window."); return; }
    }
    left = win->LeftEdge + 20 < scr->Width - w ? win->LeftEdge + 20 : 0;
    top = win->TopEdge + 20 < scr->Height - h ? win->TopEdge + 20 : scr->BarHeight + 1;
    if (top + h > scr->Height) top = scr->Height - h > 0 ? scr->Height - h : 0;
    pwin = OpenWindowTags(NULL,
        WA_Title, (ULONG)"ZZPlug - Game pad buttons",
        WA_Left, (ULONG)left, WA_Top, (ULONG)top,
        WA_Width, (ULONG)w, WA_Height, (ULONG)h,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_SimpleRefresh, TRUE, WA_NewLookMenus, TRUE,
        WA_IDCMP, (ULONG)(IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | IDCMP_MENUPICK |
                          BUTTONIDCMP | CYCLEIDCMP),
        WA_Gadgets, (ULONG)pglist,
        WA_PubScreen, (ULONG)scr,
        TAG_END);
    if (!pwin) { FreeGadgets(pglist); pglist = NULL; message("Could not open the game pad window."); return; }
    if (pad_menu) SetMenuStrip(pwin, pad_menu);
    if (main_menu) ClearMenuStrip(win);         /* the main window waits meanwhile */
    if ((tf = OpenFont(font))) SetFont(pwin->RPort, tf);
    pen_lit = best_pen(60, 220, 90); pen_off = best_pen(80, 80, 80);
    pen_sw[0] = best_pen(220, 40, 40); pen_sw[1] = best_pen(40, 80, 220);
    pen_sw[2] = best_pen(30, 170, 60); pen_sw[3] = best_pen(230, 200, 30);
    if (IntuitionBase->LibNode.lib_Version >= 39) SetWindowPointer(win, WA_BusyPointer, TRUE, TAG_END);
    GT_RefreshWindow(pwin, NULL);
    pad_heads();
    draw_marks();
    show_pad();

    while (!done) {
        ULONG pmask = 1UL << pwin->UserPort->mp_SigBit, wmask = 1UL << win->UserPort->mp_SigBit;
        ULONG tmask = 1UL << tport->mp_SigBit;
        ULONG sig = Wait(pmask | wmask | tmask | SIGBREAKF_CTRL_C);
        struct IntuiMessage *im;
        if (sig & SIGBREAKF_CTRL_C) done = 1;
        if (GetMsg(tport)) {
            static int t;
            pad_poll();
            if (++t >= 10) { t = 0; pads_refresh(0); }
            treq->tr_node.io_Command = TR_ADDREQUEST;
            treq->tr_time.tv_secs = 0; treq->tr_time.tv_micro = 100000;
            SendIO((struct IORequest *)treq);
        }
        while ((im = GT_GetIMsg(win->UserPort))) {    /* the main window waits; it may need redrawing */
            ULONG cls = im->Class;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); }
        }
        while ((im = GT_GetIMsg(pwin->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            switch (cls) {
            case IDCMP_CLOSEWINDOW: done = 1; break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(pwin); pad_heads(); draw_marks(); GT_EndRefresh(pwin, TRUE);
                break;
            case IDCMP_VANILLAKEY:
                if (code == 27) done = 1;
                break;
            case IDCMP_MENUPICK:
                while (code != MENUNULL && !done) {
                    struct MenuItem *it = ItemAddress(pad_menu, code);
                    if (!it) break;
                    switch ((ULONG)GTMENUITEM_USERDATA(it)) {
                    case MN_ABOUT: about(pwin); break;
                    case MN_SAVE: done = pad_action(P_SAVE, 0); break;
                    case MN_USE: done = pad_action(P_USE, 0); break;
                    case MN_QUIT: done = 1; break;
                    case MN_DEFAULTS: pad_action(P_BUILTIN, 0); break;
                    case MN_LASTSAVED:
                        if (pad_last_saved(&pads[curpad])) {
                            setrow = -1; learnall = 0; show_pad();
                            pad_info(pads[curpad].own ? "The buttons last saved for this pad." :
                                                        "None saved for this pad: the built-in buttons.");
                        }
                        break;
                    case MN_RESTORE: {
                        struct pad p;
                        ULONG out[4];
                        if (pads[curpad].present && pad_read(curpad, &p, out)) {
                            pads[curpad] = p;
                            setrow = -1; learnall = 0; show_pad();
                        }
                        break; }
                    }
                    code = it->NextSelect;
                }
                break;
            case IDCMP_GADGETUP:
                done = pad_action(gad->GadgetID, code);
                break;
            }
        }
    }
    if (main_menu) SetMenuStrip(win, main_menu);
    if (IntuitionBase->LibNode.lib_Version >= 39) SetWindowPointer(win, WA_Pointer, 0UL, TAG_END);
    if (GfxBase->LibNode.lib_Version >= 39) {
        struct ColorMap *cm = scr->ViewPort.ColorMap;
        if (pen_lit >= 0) ReleasePen(cm, pen_lit);
        if (pen_off >= 0) ReleasePen(cm, pen_off);
        for (r = 0; r < 4; r++) if (pen_sw[r] >= 0) ReleasePen(cm, pen_sw[r]);
    }
    pen_lit = pen_off = -1;
    for (r = 0; r < 4; r++) pen_sw[r] = -1;
    if (pad_menu) ClearMenuStrip(pwin);
    CloseWindow(pwin); pwin = NULL;
    if (tf) CloseFont(tf);
    FreeGadgets(pglist); pglist = NULL;
    setrow = -1; learnall = 0;
}

static ULONG parse_dostype(const char *p)
{
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    return strtoul(p, NULL, 16);
}

static ULONG dostype_start;                     /* as it was when the window opened (Edit > Restore) */

static void load_settings(void)
{
    char v[64];
    LONG n = GetVar((STRPTR)VAR_DOSTYPE, (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY);
    dostype = n > 0 ? parse_dostype(v) : 0;
    dostype_start = dostype;
    fatlabels[0] = (STRPTR)"CrossDOS (comes with AmigaOS)";
    fatlabels[1] = (STRPTR)"fat95 (L:fat95)";
    fatlabels[2] = NULL;
    fatactive = dostype == DOSTYPE_FAT95 ? 1 : 0;
    if (dostype && dostype != DOSTYPE_CROSSDOS && dostype != DOSTYPE_FAT95) {
        sprintf(fatlabel_other, "Other: DosType 0x%08lX", (unsigned long)dostype);
        fatlabels[2] = (STRPTR)fatlabel_other;
        fatlabels[3] = NULL;
        fatactive = 2;
    }
}

/* Edit menu: show d (0 = the default, CrossDOS) in the FAT gadget. A DosType
 * other than CrossDOS/fat95 can only be shown if the window opened with one
 * (the cycle gadget's choices are fixed on 2.04/3.0); 0 = it could not. */
static int set_dostype(ULONG d)
{
    if (d == DOSTYPE_CROSSDOS) d = 0;
    if (!d) fatactive = 0;
    else if (d == DOSTYPE_FAT95) fatactive = 1;
    else if (fatlabels[2]) {
        dostype = d;
        sprintf(fatlabel_other, "Other: DosType 0x%08lX", (unsigned long)dostype);
        fatactive = 2;
    } else return 0;
    if (gFat) GT_SetGadgetAttrs(gFat, win, NULL, GTCY_Active, (ULONG)fatactive, TAG_END);
    return 1;
}

static void edit_menu(ULONG id)
{
    char v[64];
    switch (id) {
    case MN_DEFAULTS:
        set_dostype(0);
        message("Defaults: FAT sticks mount with CrossDOS.");
        break;
    case MN_LASTSAVED:
        if (!read_envarc(VAR_DOSTYPE, v, sizeof(v))) { set_dostype(0); message("Last saved: CrossDOS (none saved)."); }
        else if (set_dostype(parse_dostype(v))) message("The settings last saved.");
        else message("Last saved is a DosType this window cannot show - left as it is.");
        break;
    case MN_RESTORE:
        set_dostype(dostype_start);
        message("The settings as they were when ZZPlugPrefs opened.");
        break;
    }
}

static int apply(int save)
{
    char v[24];
    ULONG fl = GVF_GLOBAL_ONLY | (save ? GVF_SAVE_VAR : 0);
    int ok = 1;
    if (fatactive == 0) {
        DeleteVar((STRPTR)VAR_DOSTYPE, GVF_GLOBAL_ONLY);
        if (save) DeleteVar((STRPTR)VAR_DOSTYPE, GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
    } else {
        ULONG d = fatactive == 1 ? DOSTYPE_FAT95 : dostype;
        sprintf(v, "0x%08lX", (unsigned long)d);
        ok &= SetVar((STRPTR)VAR_DOSTYPE, (STRPTR)v, -1, fl) != 0;
    }
    return ok;
}

static void start_zzplug(void)
{
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    BPTR lk = Lock((STRPTR)"C:ZZPlug", ACCESS_READ);
    if (!lk) { message("C:ZZPlug is not there - copy ZZPlug into C: first."); if (in) Close(in); if (out) Close(out); return; }
    UnLock(lk);
    if (SystemTags((STRPTR)"C:ZZPlug START QUIET", SYS_Input, in, SYS_Output, out, SYS_Asynch, TRUE,
                   NP_StackSize, 16384, TAG_DONE) == -1) {
        Close(in); Close(out);
        message("Could not start C:ZZPlug.");
        return;
    }
    message("Starting ZZPlug...");
}

/* ZZPlug DUMP in a console window: it says what to do, counts down and saves the file */
static void record_device(void)
{
    BPTR con;
    if (!zzplug_running()) { message("Start ZZPlug first, then plug the device in."); return; }
    con = Open((STRPTR)"CON:20/30/620/200/ZZPlug - record a device/CLOSE/WAIT", MODE_NEWFILE);
    if (!con) { message("Could not open a window for the recording."); return; }
    if (SystemTags((STRPTR)"C:ZZPlug DUMP", SYS_Input, con, SYS_Output, 0UL, SYS_Asynch, TRUE,
                   NP_StackSize, 16384, TAG_DONE) == -1) {
        Close(con);
        message("Could not start C:ZZPlug DUMP.");
        return;
    }
    message("Recording: follow the steps in the new window.");
}

static void stop_zzplug(void)
{
    char why[160];
    int r = ask(ZZP_MSG_STOP, 0, why, sizeof(why), NULL);
    if (r == 1) {
        char t[200];
        sprintf(t, "ZZPlug keeps running: %s", why);
        message(t);
    } else
        message("ZZPlug stopped.");
}

/* ------------------------------------------------------------------ */

static int tlen(const char *s)
{
    struct RastPort rp;
    struct TextFont *tf = OpenFont(font);
    int n;
    InitRastPort(&rp);
    if (tf) SetFont(&rp, tf);
    n = TextLength(&rp, (STRPTR)s, strlen(s));
    if (tf) CloseFont(tf);
    return n;
}

/* compact = the layout for a small screen (640x200 with Topaz 8): tighter
 * rows, and the lists get what height is left */
static int make_gadgets(int *pw, int *ph, int compact)
{
    struct NewGadget ng;
    struct Gadget *g;
    int fh = font->ta_YSize, gh = fh + (compact ? 4 : 6), row = gh + (compact ? 2 : 4);
    int cw = tlen("abcdefghijklmnopqrstuvwxyzABCDEFGHIJ") / 36 + 1;
    int bl = scr->WBorLeft + 6, bt = title_h() + (compact ? 3 : 6);
    int iw = cw * 84, lw, lvh, y, labw, bw, x, lines = 9;

    if (iw < 560) iw = 560;
    if (iw > scr->Width - bl - 6 - scr->WBorRight) iw = scr->Width - bl - 6 - scr->WBorRight;
    lw = iw * 52 / 100;
    labw = tlen("Game pad now") + 12;
    if (compact) {                              /* everything but the lists, then lines to fill the rest */
        int fixed = bt + row + (fh + 2) + 4 + 5 * row + gh + 3 + scr->WBorBottom;
        int room = scr->Height - scr->BarHeight - 1 - fixed;
        lines = (room - 4) / (fh + 1);
        if (lines > 9) lines = 9;
        if (lines < 3) lines = 3;
    }
    lvh = (fh + 1) * lines + 4;

    g = CreateContext(&glist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr = font;
    ng.ng_VisualInfo = vi;

    /* row: the state and Start / Stop */
    y = bt;
    bw = tlen("Start ZZPlug") + 20;
    ng.ng_LeftEdge = bl; ng.ng_TopEdge = y; ng.ng_Width = iw - 2 * bw - 16; ng.ng_Height = gh;
    ng.ng_GadgetID = G_STATE;
    g = gState = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)statetext, TAG_END);
    ng.ng_LeftEdge = bl + iw - 2 * bw - 8; ng.ng_Width = bw; ng.ng_GadgetText = (STRPTR)"Start ZZPlug"; ng.ng_GadgetID = G_START;
    g = gStart = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, running == 1, TAG_END);
    ng.ng_LeftEdge = bl + iw - bw; ng.ng_GadgetText = (STRPTR)"Stop ZZPlug"; ng.ng_GadgetID = G_STOP;
    g = gStop = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, running != 1, TAG_END);

    /* the device list and the details */
    y += row + fh + (compact ? 2 : 4);
    ng.ng_LeftEdge = bl; ng.ng_TopEdge = y; ng.ng_Width = lw; ng.ng_Height = lvh;
    ng.ng_GadgetText = (STRPTR)"Plugged in"; ng.ng_Flags = PLACETEXT_ABOVE | NG_HIGHLABEL; ng.ng_GadgetID = G_DEVS;
    g = gDevs = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&devs.list, GTLV_ShowSelected, 0UL,
                             GTLV_Selected, (ULONG)(selected >= 0 ? selected : ~0UL), TAG_END);
    ng.ng_LeftEdge = bl + lw + 8; ng.ng_Width = iw - lw - 8;
    ng.ng_GadgetText = (STRPTR)"Details"; ng.ng_GadgetID = G_DETAIL;
    g = gDetail = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&detail.list, GTLV_ReadOnly, TRUE, TAG_END);
    y += lvh + (compact ? 4 : 8);

    /* settings */
    ng.ng_Flags = PLACETEXT_LEFT;
    x = bl + labw;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y; ng.ng_Width = iw - labw; ng.ng_Height = gh;
    ng.ng_GadgetText = (STRPTR)"FAT sticks use"; ng.ng_GadgetID = G_FAT;
    g = gFat = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)fatlabels, GTCY_Active, (ULONG)fatactive, TAG_END);
    y += row;
    bw = tlen("Buttons...") + 24;
    ng.ng_TopEdge = y; ng.ng_Width = iw - labw - bw - 8;
    ng.ng_GadgetText = (STRPTR)"Game pads"; ng.ng_GadgetID = G_PADSET;
    g = gPadSet = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)padsettext, GTTX_Border, TRUE, TAG_END);
    ng.ng_Flags = 0;
    ng.ng_LeftEdge = bl + iw - bw; ng.ng_Width = bw; ng.ng_GadgetText = (STRPTR)"Buttons..."; ng.ng_GadgetID = G_PADWIN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    y += row;
    ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y; ng.ng_Width = iw - labw;
    ng.ng_GadgetText = (STRPTR)"Game pad now"; ng.ng_GadgetID = G_PADNOW;
    g = gPadNow = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)padnowtext, GTTX_Border, TRUE, TAG_END);
    y += row;
    bw = tlen("Record a device...") + 24;
    ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y; ng.ng_Width = iw - labw - bw - 8;
    ng.ng_GadgetText = (STRPTR)"Not working?"; ng.ng_GadgetID = G_RECTEXT;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"Record what a device sends, for a bug report", TAG_END);
    ng.ng_Flags = 0;
    ng.ng_LeftEdge = bl + iw - bw; ng.ng_Width = bw; ng.ng_GadgetText = (STRPTR)"Record a device..."; ng.ng_GadgetID = G_RECORD;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    y += row;
    ng.ng_LeftEdge = bl; ng.ng_Width = iw; ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_MSG;
    ng.ng_TopEdge = y;
    g = gMsg = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)msgtext, TAG_END);
    y += row + (compact ? 0 : 4);

    /* Save / Use / Cancel */
    bw = tlen("Cancel") + 32;
    if (bw < 90) bw = 90;
    ng.ng_Flags = 0; ng.ng_TopEdge = y; ng.ng_Width = bw;
    ng.ng_LeftEdge = bl; ng.ng_GadgetText = (STRPTR)"Save"; ng.ng_GadgetID = G_SAVE;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = bl + (iw - bw) / 2; ng.ng_GadgetText = (STRPTR)"Use"; ng.ng_GadgetID = G_USE;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = bl + iw - bw; ng.ng_GadgetText = (STRPTR)"Cancel"; ng.ng_GadgetID = G_CANCEL;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    y += gh;

    *pw = bl + iw + 6 + scr->WBorRight;
    *ph = y + (compact ? 3 : 6) + scr->WBorBottom;
    return g != NULL;
}

/* a gadget of the main window (or the menu item standing for one); 1 = quit */
static int main_action(int id, UWORD code, struct MsgPort *tport, struct timerequest *treq)
{
    switch (id) {
    case G_DEVS:
        if (code < devs.n && devidx[code] >= 0) { selected = code; seldev = devidx[code]; show_detail(1); }
        break;
    case G_START: start_zzplug(); refresh_list(1); break;
    case G_STOP: stop_zzplug(); Delay(25); refresh_list(1); break;
    case G_FAT:
        fatactive = code;
        if (code == 1) {
            BPTR lk = Lock((STRPTR)"L:fat95", ACCESS_READ);
            if (lk) { UnLock(lk); message("New FAT sticks will mount with fat95."); }
            else message("L:fat95 is not installed - sticks would not mount with it.");
        } else
            message(code ? "" : "New FAT sticks will mount with CrossDOS.");
        break;
    case G_RECORD:
        record_device();
        break;
    case G_PADWIN:
        pad_window(tport, treq);
        pads_summary();
        break;
    case G_SAVE:
        if (!apply(1)) { message("Could not save the settings."); break; }
        return 1;
    case G_USE:
        if (!apply(0)) { message("Could not set the settings."); break; }
        return 1;
    case G_CANCEL: return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    struct MsgPort *tport = NULL;
    struct timerequest *treq = NULL;
    int timer_open = 0, sent = 0, done = 0, ticks = 0, w, h, rc = RETURN_FAIL;
    const char * volatile keepver = ver;
    (void)keepver;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37);
    GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
    if (!IntuitionBase || !GfxBase || !GadToolsBase) goto out;
    if (!(reply = CreateMsgPort())) goto out;
    if (!(tport = CreateMsgPort()) || !(treq = (struct timerequest *)CreateIORequest(tport, sizeof(*treq))) ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) != 0)
        goto out;
    timer_open = 1;

    lines_init(&devs);
    lines_init(&detail);
    load_settings();
    strcpy(padsettext, "-");
    strcpy(padnowtext, "-");
    if (!(scr = LockPubScreen(NULL)) || !(vi = GetVisualInfo(scr, TAG_END))) goto out;
    font = scr->Font;
    running = zzplug_running();
    statetext[0] = 0;
    {   /* the screen's font, then compact, then Topaz 8, then Topaz 8 compact: the first that fits */
        int k, made = 0, fits = 0;
        for (k = 0; k < 4 && !fits; k++) {          /* none fits: the last (smallest) one stays */
            if (glist) { FreeGadgets(glist); glist = NULL; }
            font = k < 2 ? scr->Font : &topaz8;
            made = make_gadgets(&w, &h, k & 1);
            fits = made && w <= scr->Width && h <= scr->Height - scr->BarHeight - 1;
        }
        if (!made) goto out;
    }
    win = OpenWindowTags(NULL,
        WA_Title, (ULONG)"ZZPlug - USB on the ZZ9000",
        WA_Left, (ULONG)(w < scr->Width ? (scr->Width - w) / 2 : 0),
        WA_Top, (ULONG)(h <= scr->Height - scr->BarHeight - 1 ? scr->BarHeight + 1 : 0),
        WA_Width, (ULONG)w, WA_Height, (ULONG)h,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_SimpleRefresh, TRUE, WA_NewLookMenus, TRUE,
        WA_IDCMP, (ULONG)(IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | IDCMP_MENUPICK |
                          BUTTONIDCMP | LISTVIEWIDCMP | CYCLEIDCMP),
        WA_Gadgets, (ULONG)glist,
        WA_PubScreen, (ULONG)scr,
        TAG_END);
    if (!win) goto out;
    if ((main_menu = make_menu(main_menu_def))) SetMenuStrip(win, main_menu);
    pad_menu = make_menu(pad_menu_def);
    GT_RefreshWindow(win, NULL);
    running = -1;
    refresh_list(1);
    pads_summary();

    treq->tr_node.io_Command = TR_ADDREQUEST;
    treq->tr_time.tv_secs = 0; treq->tr_time.tv_micro = 100000;
    SendIO((struct IORequest *)treq);
    sent = 1;

    while (!done) {
        ULONG wmask = 1UL << win->UserPort->mp_SigBit, tmask = 1UL << tport->mp_SigBit;
        ULONG sig = Wait(wmask | tmask | SIGBREAKF_CTRL_C);
        struct IntuiMessage *im;
        if (sig & SIGBREAKF_CTRL_C) done = 1;
        if (GetMsg(tport)) {
            poll_pad();
            if (++ticks >= 10) { ticks = 0; refresh_list(0); pads_summary(); }
            treq->tr_node.io_Command = TR_ADDREQUEST;
            treq->tr_time.tv_secs = 0; treq->tr_time.tv_micro = 100000;
            SendIO((struct IORequest *)treq);
        }
        while ((im = GT_GetIMsg(win->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            switch (cls) {
            case IDCMP_CLOSEWINDOW: done = 1; break;
            case IDCMP_REFRESHWINDOW: GT_BeginRefresh(win); GT_EndRefresh(win, TRUE); break;
            case IDCMP_VANILLAKEY:
                if (code == 27) done = 1;
                break;
            case IDCMP_MENUPICK:
                while (code != MENUNULL && !done) {
                    struct MenuItem *it = ItemAddress(main_menu, code);
                    ULONG id;
                    if (!it) break;
                    id = (ULONG)GTMENUITEM_USERDATA(it);
                    code = it->NextSelect;      /* read first: the pad window swaps the menu strip */
                    switch (id) {
                    case MN_ABOUT: about(win); break;
                    case MN_SAVE: done = main_action(G_SAVE, 0, tport, treq); break;
                    case MN_USE: done = main_action(G_USE, 0, tport, treq); break;
                    case MN_QUIT: done = 1; break;
                    case MN_DEFAULTS: case MN_LASTSAVED: case MN_RESTORE: edit_menu(id); break;
                    case MN_START: if (!zzplug_running()) main_action(G_START, 0, tport, treq); break;
                    case MN_STOP: if (zzplug_running()) main_action(G_STOP, 0, tport, treq); break;
                    case MN_PADS: main_action(G_PADWIN, 0, tport, treq); break;
                    case MN_RECORD: main_action(G_RECORD, 0, tport, treq); break;
                    }
                }
                break;
            case IDCMP_GADGETUP:
                done = main_action(gad->GadgetID, code, tport, treq);
                break;
            }
        }
    }
    rc = RETURN_OK;

out:
    if (win && main_menu) ClearMenuStrip(win);
    if (sent) {
        AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
    }
    if (timer_open) CloseDevice((struct IORequest *)treq);
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (win) CloseWindow(win);
    if (main_menu) FreeMenus(main_menu);
    if (pad_menu) FreeMenus(pad_menu);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (scr) UnlockPubScreen(NULL, scr);
    if (reply) DeleteMsgPort(reply);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
