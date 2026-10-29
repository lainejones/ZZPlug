/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * keytest.c - KeyTest [SECS=n] [TO=file]: a window that shows and records
 * every key it receives (raw key code, up/down, qualifiers and the character
 * it maps to), for checking a USB keyboard through ZZPlug. Close it or wait
 * SECS (default 300).
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <intuition/intuition.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/keymap.h>

static const char ver[] __attribute__((used)) = "$VER: KeyTest 1.0 (29.10.2026)";
struct Library *KeymapBase;

static void quals(UWORD q, char *o)
{
    static const struct { UWORD b; const char *n; } t[] = {
        { IEQUALIFIER_LSHIFT, "lshift" }, { IEQUALIFIER_RSHIFT, "rshift" }, { IEQUALIFIER_CAPSLOCK, "caps" },
        { IEQUALIFIER_CONTROL, "ctrl" }, { IEQUALIFIER_LALT, "lalt" }, { IEQUALIFIER_RALT, "ralt" },
        { IEQUALIFIER_LCOMMAND, "lamiga" }, { IEQUALIFIER_RCOMMAND, "ramiga" }, { IEQUALIFIER_NUMERICPAD, "numpad" },
        { IEQUALIFIER_REPEAT, "repeat" } };
    int i;
    o[0] = 0;
    for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++)
        if (q & t[i].b) { strcat(o, " "); strcat(o, t[i].n); }
}

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda = ReadArgs((STRPTR)"SECS/K/N,TO/K", args, NULL);
    ULONG secs = 300, start, now, dummy;
    const char *to = "RAM:keytest.txt";
    struct Window *w;
    FILE *f;
    int done = 0, line = 0;
    const char * volatile keepver = ver;
    (void)keepver;
    if (!rda) { PrintFault(IoErr(), (STRPTR)"KeyTest"); return RETURN_FAIL; }
    if (args[0]) secs = *(LONG *)args[0];
    if (args[1]) to = (const char *)args[1];
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 37);
    if (!IntuitionBase || !GfxBase || !(KeymapBase = OpenLibrary((STRPTR)"keymap.library", 37))) return RETURN_FAIL;
    if (!(f = fopen(to, "w"))) { CloseLibrary(KeymapBase); return RETURN_FAIL; }
    w = OpenWindowTags(NULL, WA_Title, (ULONG)"KeyTest - type here", WA_Left, 40, WA_Top, 40, WA_Width, 520, WA_Height, 150,
                       WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
                       WA_IDCMP, (ULONG)(IDCMP_RAWKEY | IDCMP_CLOSEWINDOW | IDCMP_INTUITICKS), TAG_END);
    if (!w) { fclose(f); CloseLibrary(KeymapBase); return RETURN_FAIL; }
    CurrentTime(&start, &dummy);
    fprintf(f, "KeyTest: raw code, up/down, qualifiers, character\n");
    fflush(f);
    while (!done) {
        struct IntuiMessage *im;
        WaitPort(w->UserPort);
        while ((im = (struct IntuiMessage *)GetMsg(w->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code, q = im->Qualifier;
            struct InputEvent ie;
            char buf[8], qs[80], ln[160];
            LONG n = 0;
            if (cls == IDCMP_RAWKEY) {
                memset(&ie, 0, sizeof(ie));
                ie.ie_Class = IECLASS_RAWKEY;
                ie.ie_Code = code;
                ie.ie_Qualifier = q;
                ie.ie_EventAddress = (APTR *)*((ULONG *)im->IAddress);
                if (!(code & IECODE_UP_PREFIX)) n = MapRawKey(&ie, (STRPTR)buf, sizeof(buf) - 1, NULL);
                if (n < 0) n = 0;
                buf[n] = 0;
                quals(q, qs);
                sprintf(ln, "%02X %s%s", code & 0x7f, code & IECODE_UP_PREFIX ? "up  " : "down", qs);
                if (n == 1 && buf[0] >= 32 && buf[0] < 127) sprintf(ln + strlen(ln), " '%c'", buf[0]);
                else if (n) { int k; strcat(ln, " ["); for (k = 0; k < n; k++) sprintf(ln + strlen(ln), k ? " %02X" : "%02X", (UBYTE)buf[k]); strcat(ln, "]"); }
                fprintf(f, "%s\n", ln);
                fflush(f);
                if (!(code & IECODE_UP_PREFIX)) {
                    SetAPen(w->RPort, 1);
                    if (line >= 10) { ScrollRaster(w->RPort, 0, 10, w->BorderLeft, w->BorderTop + 2, w->Width - w->BorderRight - 1, w->Height - w->BorderBottom - 1); line = 9; }
                    Move(w->RPort, w->BorderLeft + 6, w->BorderTop + 10 + line * 10);
                    Text(w->RPort, (STRPTR)ln, strlen(ln) > 60 ? 60 : strlen(ln));
                    line++;
                }
            }
            if (cls == IDCMP_CLOSEWINDOW) done = 1;
            ReplyMsg((struct Message *)im);
        }
        CurrentTime(&now, &dummy);
        if (now - start >= secs) done = 1;
    }
    fprintf(f, "end\n");
    fclose(f);
    CloseWindow(w);
    CloseLibrary(KeymapBase);
    return RETURN_OK;
}
