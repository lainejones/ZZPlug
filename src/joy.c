/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
#include <exec/types.h>
#include <libraries/lowlevel.h>
#include <proto/exec.h>
#include "joy.h"

#define LVO_READJOYPORT (-30)

typedef ULONG (*rjp_fn)(ULONG port __asm("d0"), struct Library *base __asm("a6"));

static struct Library *ll;
static rjp_fn old_rjp;
static volatile ULONG pad_state[JOY_SLOTS];
static volatile UBYTE pad_present[JOY_SLOTS];
static const UBYTE slot_port[JOY_SLOTS] = { 1, 2 };

/* runs in whatever task calls ReadJoyPort */
static ULONG new_rjp(ULONG port __asm("d0"), struct Library *base __asm("a6"))
{
    int i;
    for (i = 0; i < JOY_SLOTS; i++)
        if (port == slot_port[i] && pad_present[i])
            return JP_TYPE_GAMECTLR | pad_state[i];
    return old_rjp(port, base);
}

int joy_open(void)
{
    if (old_rjp) return 1;
    if (!(ll = OpenLibrary((STRPTR)"lowlevel.library", 40)))
        return 0;
    Forbid();
    old_rjp = (rjp_fn)SetFunction(ll, LVO_READJOYPORT, (APTR)new_rjp);
    Permit();
    return 1;
}

/* is the vector still ours? Someone who patched after us calls into our code,
 * so ZZPlug must not unload then */
int joy_can_close(void)
{
    int ours;
    if (!ll) return 1;
    Forbid();
    ours = *(APTR *)((UBYTE *)ll + LVO_READJOYPORT + 2) == (APTR)new_rjp;
    Permit();
    return ours;
}

void joy_close(void)
{
    if (!ll) return;
    pad_present[0] = pad_present[1] = 0;
    Forbid();
    if (*(APTR *)((UBYTE *)ll + LVO_READJOYPORT + 2) == (APTR)new_rjp)
        SetFunction(ll, LVO_READJOYPORT, (APTR)old_rjp);
    Permit();
    CloseLibrary(ll);
    ll = NULL;
    old_rjp = NULL;
}

void joy_set(int slot, int present, ULONG state)
{
    if (slot < 0 || slot >= JOY_SLOTS) return;
    pad_state[slot] = state;
    pad_present[slot] = (UBYTE)(present ? 1 : 0);
}

int joy_port_of(int slot)
{
    return slot >= 0 && slot < JOY_SLOTS ? slot_port[slot] : -1;
}
