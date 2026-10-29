/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * prefs.c - ZZPlugPrefs: a small window over the running ZZPlug.
 *
 *  - what is plugged in and what ZZPlug does with each device, kept current;
 *    pick one to see its details
 *  - start or stop ZZPlug
 *  - which filesystem FAT sticks mount with (ENV:ZZPlug.dostype)
 *  - the game pad's buttons (ENV:ZZPlug.padbuttons): automatic, or learned
 *    by pressing them one after another; a live readout of the pad
 *  - Save (ENVARC: too) / Use (ENV: only) / Cancel, as Amiga prefs editors do
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

static const char ver[] __attribute__((used)) = "$VER: ZZPlugPrefs 0.6 (29.10.2026)";

#define PORTNAME "ZZPlug"
#define VAR_DOSTYPE "ZZPlug.dostype"
#define VAR_PAD "ZZPlug.padbuttons"
#define DOSTYPE_CROSSDOS 0x4D534400UL           /* "MSD\0" */
#define DOSTYPE_FAT95    0x46415400UL           /* "FAT\0" */

enum { G_STATE = 1, G_START, G_STOP, G_DEVS, G_DETAIL, G_FAT, G_PADSET, G_LEARN, G_AUTO, G_PADNOW, G_MSG,
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
static struct Gadget *glist, *gState, *gStart, *gStop, *gDevs, *gDetail, *gFat, *gPadSet, *gLearn, *gPadNow, *gMsg;
static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct TextAttr *font;

static struct lines devs, detail;
static int devidx[MAXLINES];                    /* the bus index each device line stands for, -1 = a note */
static int selected = -1;                       /* the line picked in the device list */
static int seldev = -1;                         /* and its bus index */
static char lastlist[4096], lastdetail[2048];
static int running = -1;

static struct MsgPort *reply;

/* the settings as edited */
static ULONG dostype;                           /* 0 = ZZPlug's default (CrossDOS) */
static char padset[64];                         /* "" = automatic */
static char padset_was[64];
static char fatlabel_other[40];
static STRPTR fatlabels[4];
static int fatactive;

/* learning the pad's buttons */
static const char *const learn_name[] = { "RED (fire)", "BLUE", "GREEN", "YELLOW", "REVERSE (left shoulder)",
                                          "FORWARD (right shoulder)", "PLAY (start)" };
static const int learn_pos[] = { 0, 1, 2, 3, 4, 5, 8 };
#define LEARN_STEPS 7
static int learn = -1, learn_wait;
static int learned[16];

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
    if (out) { out[0] = m.out[0]; out[1] = m.out[1]; out[2] = m.out[2]; }
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

static void show_padset(void)
{
    if (padset[0]) sprintf(padsettext, "Learned: %s", padset);
    else strcpy(padsettext, "Automatic");
    settext(gPadSet, padsettext, padsettext);
}

static void learn_prompt(void)
{
    char t[120];
    sprintf(t, "Press the button for %s (Learn again skips it)", learn_name[learn]);
    settext(gPadNow, padnowtext, t);
}

static void learn_finish(void)
{
    int i, n = 0, any = 0;
    char *p = padset;
    int order[10];
    for (i = 0; i < 10; i++) order[i] = 0;
    for (i = 0; i < LEARN_STEPS; i++) {
        order[learn_pos[i]] = learned[i];
        if (learned[i]) any = 1;
    }
    if (learned[6]) order[9] = learned[6];      /* both play positions on the one button */
    learn = -1;
    if (!any) { message("Nothing learned; the pad keeps its old buttons."); return; }
    for (n = 10; n > 0 && !order[n - 1]; n--) ;
    p[0] = 0;
    for (i = 0; i < n; i++) sprintf(p + strlen(p), i ? " %d" : "%d", order[i]);
    show_padset();
    message("Learned. Use or Save puts the new buttons to work.");
}

static void poll_pad(void)
{
    ULONG out[3];
    char t[120], w[100];
    int r = ask(ZZP_MSG_PAD, 0, NULL, 0, out);
    if (r != 0) {
        if (learn >= 0) { learn = -1; message("The game pad went away; learning stopped."); }
        settext(gPadNow, padnowtext, r == -2 ? "-" : "No game pad plugged in");
        return;
    }
    if (learn >= 0) {
        if (!out[0]) { learn_wait = 0; return; }
        if (learn_wait) return;                 /* still held from the last step */
        {
            int b = 0, i;
            while (b < 16 && !(out[0] & (1UL << b))) b++;
            for (i = 0; i < learn; i++)
                if (learned[i] == b + 1) {
                    sprintf(t, "Button %d is %s already - press another", b + 1, learn_name[i]);
                    settext(gPadNow, padnowtext, t);
                    learn_wait = 1;
                    return;
                }
            learned[learn] = b + 1;
            learn_wait = 1;
            if (++learn >= LEARN_STEPS) { learn_finish(); return; }
            learn_prompt();
        }
        return;
    }
    pad_words(out[1], w);
    {
        int i;
        char raw[60] = "";
        for (i = 0; i < 16; i++)
            if (out[0] & (1UL << i)) sprintf(raw + strlen(raw), raw[0] ? " %d" : "%d", i + 1);
        if (raw[0]) sprintf(t, "%s  (pad button %s)", w, raw);
        else strcpy(t, w);
    }
    if (strcmp(t, padnowtext)) settext(gPadNow, padnowtext, t);
}

/* ------------------------------------------------------------------ */

static void load_settings(void)
{
    char v[64];
    LONG n = GetVar((STRPTR)VAR_DOSTYPE, (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY);
    dostype = 0;
    if (n > 0) {
        const char *p = v;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        dostype = strtoul(p, NULL, 16);
    }
    n = GetVar((STRPTR)VAR_PAD, (STRPTR)padset, sizeof(padset), GVF_GLOBAL_ONLY);
    if (n <= 0) padset[0] = 0;
    strcpy(padset_was, padset);
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
    if (padset[0]) ok &= SetVar((STRPTR)VAR_PAD, (STRPTR)padset, -1, fl) != 0;
    else { DeleteVar((STRPTR)VAR_PAD, GVF_GLOBAL_ONLY); if (save) DeleteVar((STRPTR)VAR_PAD, GVF_GLOBAL_ONLY | GVF_SAVE_VAR); }
    if (strcmp(padset, padset_was)) ask(ZZP_MSG_RESCAN, 0, NULL, 0, NULL);   /* pads read their buttons when found */
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

static int make_gadgets(int *pw, int *ph)
{
    struct NewGadget ng;
    struct Gadget *g;
    int fh = font->ta_YSize, gh = fh + 6, row = gh + 4;
    int cw = tlen("abcdefghijklmnopqrstuvwxyzABCDEFGHIJ") / 36 + 1;
    int bl = scr->WBorLeft + 6, bt = scr->WBorTop + fh + 1 + 6;
    int iw = cw * 84, lw, lvh, y, labw, bw, x;

    if (iw < 560) iw = 560;
    lw = iw * 52 / 100;
    labw = tlen("Game pad buttons") + 12;
    lvh = (fh + 1) * 9 + 4;

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
    y += row + fh + 4;
    ng.ng_LeftEdge = bl; ng.ng_TopEdge = y; ng.ng_Width = lw; ng.ng_Height = lvh;
    ng.ng_GadgetText = (STRPTR)"Plugged in"; ng.ng_Flags = PLACETEXT_ABOVE | NG_HIGHLABEL; ng.ng_GadgetID = G_DEVS;
    g = gDevs = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&devs.list, GTLV_ShowSelected, 0UL,
                             GTLV_Selected, (ULONG)(selected >= 0 ? selected : ~0UL), TAG_END);
    ng.ng_LeftEdge = bl + lw + 8; ng.ng_Width = iw - lw - 8;
    ng.ng_GadgetText = (STRPTR)"Details"; ng.ng_GadgetID = G_DETAIL;
    g = gDetail = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&detail.list, GTLV_ReadOnly, TRUE, TAG_END);
    y += lvh + 8;

    /* settings */
    ng.ng_Flags = PLACETEXT_LEFT;
    x = bl + labw;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y; ng.ng_Width = iw - labw; ng.ng_Height = gh;
    ng.ng_GadgetText = (STRPTR)"FAT sticks use"; ng.ng_GadgetID = G_FAT;
    g = gFat = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)fatlabels, GTCY_Active, (ULONG)fatactive, TAG_END);
    y += row;
    bw = tlen("Automatic") + 20;
    ng.ng_TopEdge = y; ng.ng_Width = iw - labw - 2 * bw - 16;
    ng.ng_GadgetText = (STRPTR)"Game pad buttons"; ng.ng_GadgetID = G_PADSET;
    g = gPadSet = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)padsettext, GTTX_Border, TRUE, TAG_END);
    ng.ng_Flags = 0;
    ng.ng_LeftEdge = bl + iw - 2 * bw - 8; ng.ng_Width = bw; ng.ng_GadgetText = (STRPTR)"Learn"; ng.ng_GadgetID = G_LEARN;
    g = gLearn = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = bl + iw - bw; ng.ng_GadgetText = (STRPTR)"Automatic"; ng.ng_GadgetID = G_AUTO;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    y += row;
    ng.ng_Flags = PLACETEXT_LEFT;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y; ng.ng_Width = iw - labw;
    ng.ng_GadgetText = (STRPTR)"Game pad now"; ng.ng_GadgetID = G_PADNOW;
    g = gPadNow = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)padnowtext, GTTX_Border, TRUE, TAG_END);
    y += row;
    ng.ng_LeftEdge = bl; ng.ng_Width = iw; ng.ng_GadgetText = NULL; ng.ng_GadgetID = G_MSG;
    ng.ng_TopEdge = y;
    g = gMsg = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)msgtext, TAG_END);
    y += row + 4;

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
    *ph = y + 6 + scr->WBorBottom;
    return g != NULL;
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
    show_padset();
    strcpy(padnowtext, "-");
    if (!(scr = LockPubScreen(NULL)) || !(vi = GetVisualInfo(scr, TAG_END))) goto out;
    font = scr->Font;
    running = zzplug_running();
    statetext[0] = 0;
    if (!make_gadgets(&w, &h) || w > scr->Width || h > scr->Height) {   /* too big: the small font */
        FreeGadgets(glist); glist = NULL;
        font = &topaz8;
        if (!make_gadgets(&w, &h)) goto out;
    }
    win = OpenWindowTags(NULL,
        WA_Title, (ULONG)"ZZPlug - USB on the ZZ9000",
        WA_Left, (ULONG)((scr->Width - w) / 2), WA_Top, (ULONG)(scr->BarHeight + 1),
        WA_Width, (ULONG)w, WA_Height, (ULONG)h,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_SimpleRefresh, TRUE,
        WA_IDCMP, (ULONG)(IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY | BUTTONIDCMP | LISTVIEWIDCMP | CYCLEIDCMP),
        WA_Gadgets, (ULONG)glist,
        WA_PubScreen, (ULONG)scr,
        TAG_END);
    if (!win) goto out;
    GT_RefreshWindow(win, NULL);
    running = -1;
    refresh_list(1);

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
            if (++ticks >= 10) { ticks = 0; refresh_list(0); }
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
            case IDCMP_GADGETUP:
                switch (gad->GadgetID) {
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
                case G_LEARN:
                    if (learn < 0) {
                        int i;
                        for (i = 0; i < 16; i++) learned[i] = 0;
                        learn = 0; learn_wait = 1;
                        message("Learning the game pad's buttons.");
                        learn_prompt();
                    } else {
                        learned[learn] = 0;
                        learn_wait = 1;
                        if (++learn >= LEARN_STEPS) learn_finish(); else learn_prompt();
                    }
                    break;
                case G_AUTO:
                    learn = -1;
                    padset[0] = 0;
                    show_padset();
                    message("Automatic buttons. Use or Save puts them to work.");
                    break;
                case G_SAVE:
                    if (!apply(1)) { message("Could not save the settings."); break; }
                    done = 1; break;
                case G_USE:
                    if (!apply(0)) { message("Could not set the settings."); break; }
                    done = 1; break;
                case G_CANCEL: done = 1; break;
                }
                break;
            }
        }
    }
    rc = RETURN_OK;

out:
    if (sent) {
        AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
    }
    if (timer_open) CloseDevice((struct IORequest *)treq);
    if (treq) DeleteIORequest((struct IORequest *)treq);
    if (tport) DeleteMsgPort(tport);
    if (win) CloseWindow(win);
    if (glist) FreeGadgets(glist);
    if (vi) FreeVisualInfo(vi);
    if (scr) UnlockPubScreen(NULL, scr);
    if (reply) DeleteMsgPort(reply);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
