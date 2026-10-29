/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * joytest.c - what a program sees through lowlevel.library.
 *
 *   JoyTest [PORT=n] [SECS=n]
 *
 * Prints ReadJoyPort(n) (default 1 = joystick port 2) every time it
 * changes, for SECS seconds (default 15).
 */
#include <stdio.h>
#include <stdlib.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/lowlevel.h>

static const char ver[] __attribute__((used)) = "$VER: JoyTest 1.0 (29.10.2026)";
struct Library *LowLevelBase;

static void show(ULONG v)
{
    static const char *const type[] = { "not available", "game controller", "mouse", "joystick" };
    ULONG t = (v & JP_TYPE_MASK) >> 28;
    printf("%08lX %s:", (unsigned long)v, t < 4 ? type[t] : "unknown");
    if (v & JPF_JOY_UP) printf(" up");
    if (v & JPF_JOY_DOWN) printf(" down");
    if (v & JPF_JOY_LEFT) printf(" left");
    if (v & JPF_JOY_RIGHT) printf(" right");
    if (v & JPF_BUTTON_RED) printf(" red");
    if (v & JPF_BUTTON_BLUE) printf(" blue");
    if (v & JPF_BUTTON_GREEN) printf(" green");
    if (v & JPF_BUTTON_YELLOW) printf(" yellow");
    if (v & JPF_BUTTON_FORWARD) printf(" forward");
    if (v & JPF_BUTTON_REVERSE) printf(" reverse");
    if (v & JPF_BUTTON_PLAY) printf(" play");
    printf("\n");
    fflush(stdout);
}

int main(void)
{
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda = ReadArgs((STRPTR)"PORT/K/N,SECS/K/N", args, NULL);
    ULONG port = 1, secs = 15, last = 0xffffffffUL, v, t;
    const char * volatile keepver = ver;
    (void)keepver;
    if (!rda) { PrintFault(IoErr(), (STRPTR)"JoyTest"); return RETURN_FAIL; }
    if (args[0]) port = *(LONG *)args[0];
    if (args[1]) secs = *(LONG *)args[1];
    FreeArgs(rda);
    if (!(LowLevelBase = OpenLibrary((STRPTR)"lowlevel.library", 40))) { printf("JoyTest: no lowlevel.library\n"); return RETURN_FAIL; }
    printf("ReadJoyPort(%lu) for %lu s:\n", (unsigned long)port, (unsigned long)secs);
    for (t = 0; t < secs * 50; t++) {
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) break;
        v = ReadJoyPort(port);
        if (v != last) { show(v); last = v; }
        Delay(1);
    }
    CloseLibrary(LowLevelBase);
    return RETURN_OK;
}
