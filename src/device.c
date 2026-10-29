/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Laine Jones */
/*
 * device.c - zzusbdisk.device. MakeLibrary() with our own six vectors and
 * AddDevice(): the device lives in ZZPlug's memory as long as ZZPlug runs.
 * BeginIO runs in the caller's task: quick commands are answered there,
 * reads and writes are queued for ZZPlug's task (zzd_service).
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/interrupts.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <proto/exec.h>
#include "device.h"

struct zzd_unit zzd_units[ZZD_UNITS];
static struct Device *dev;
static struct MsgPort *port;

#define UNIT_OF(io) ((ULONG)(io)->io_Unit)

/* ---- the vectors (run in the caller's task) -------------------------------- */

static void d_open(struct IORequest *io __asm("a1"), ULONG unit __asm("d0"), ULONG flags __asm("d1"), struct Device *d __asm("a6"))
{
    (void)flags;
    if (unit >= ZZD_UNITS) { io->io_Error = IOERR_OPENFAIL; return; }
    io->io_Unit = (struct Unit *)unit;          /* the unit number itself; units are ours to define */
    io->io_Device = d;
    io->io_Error = 0;
    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    d->dd_Library.lib_OpenCnt++;
    d->dd_Library.lib_Flags &= ~LIBF_DELEXP;
}

static BPTR d_close(struct IORequest *io __asm("a1"), struct Device *d __asm("a6"))
{
    io->io_Device = (struct Device *)-1;
    io->io_Unit = (struct Unit *)-1;
    d->dd_Library.lib_OpenCnt--;
    return 0;
}

static BPTR d_expunge(struct Device *d __asm("a6"))
{
    d->dd_Library.lib_Flags |= LIBF_DELEXP;     /* ZZPlug removes it itself when it stops */
    return 0;
}

static ULONG d_null(void) { return 0; }

static void done(struct IOStdReq *io)
{
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);
}

static void d_beginio(struct IOStdReq *io __asm("a1"), struct Device *d __asm("a6"))
{
    struct zzd_unit *u = &zzd_units[UNIT_OF(io)];
    (void)d;
    io->io_Error = 0;
    io->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    switch (io->io_Command) {
    case TD_CHANGENUM:   io->io_Actual = u->changenum; break;
    case TD_CHANGESTATE: io->io_Actual = u->present ? 0 : 1; break;
    case TD_PROTSTATUS:  io->io_Actual = (u->present && u->wp) ? 1 : 0; break;
    case TD_GETDRIVETYPE: io->io_Actual = DRIVE3_5; break;
    case TD_MOTOR: case TD_SEEK: case TD_SEEK64: case NSCMD_TD_SEEK64: case CMD_CLEAR: case TD_REMOVE: case TD_EJECT:
        io->io_Actual = 0; break;
    case TD_GETGEOMETRY: {
        struct DriveGeometry *g = (struct DriveGeometry *)io->io_Data;
        if (!u->present) { io->io_Error = TDERR_DiskChanged; break; }
        if (io->io_Length < sizeof(*g)) { io->io_Error = IOERR_BADLENGTH; break; }
        memset(g, 0, sizeof(*g));
        g->dg_SectorSize = u->blksize;
        g->dg_TotalSectors = u->blocks;
        g->dg_Cylinders = u->blocks;
        g->dg_CylSectors = 1;
        g->dg_Heads = 1;
        g->dg_TrackSectors = 1;
        g->dg_BufMemType = MEMF_PUBLIC;
        g->dg_DeviceType = DG_DIRECT_ACCESS;
        g->dg_Flags = DGF_REMOVABLE;
        io->io_Actual = sizeof(*g);
        break;
    }
    case TD_ADDCHANGEINT:
        Forbid();
        AddTail(&u->ints, (struct Node *)io);
        Permit();
        io->io_Flags &= ~IOF_QUICK;             /* held until TD_REMCHANGEINT */
        return;
    case TD_REMCHANGEINT: {
        struct IOStdReq *add = (struct IOStdReq *)io->io_Data;
        Forbid();
        if (add) Remove((struct Node *)add);
        Permit();
        break;
    }
    case NSCMD_DEVICEQUERY: {
        static const UWORD cmds[] = { CMD_READ, CMD_WRITE, CMD_UPDATE, CMD_CLEAR, TD_MOTOR, TD_SEEK, TD_FORMAT, TD_CHANGENUM,
                                      TD_CHANGESTATE, TD_PROTSTATUS, TD_GETDRIVETYPE, TD_ADDCHANGEINT, TD_REMCHANGEINT,
                                      TD_GETGEOMETRY, TD_READ64, TD_WRITE64, TD_SEEK64, TD_FORMAT64,
                                      NSCMD_DEVICEQUERY, NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_SEEK64, NSCMD_TD_FORMAT64, 0 };
        struct NSDeviceQueryResult *q = (struct NSDeviceQueryResult *)io->io_Data;
        if (io->io_Length < 16) { io->io_Error = IOERR_BADLENGTH; break; }
        q->nsdqr_DevQueryFormat = 0;
        q->nsdqr_SizeAvailable = 16;
        q->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
        q->nsdqr_DeviceSubType = 0;
        q->nsdqr_SupportedCommands = (UWORD *)cmds;
        io->io_Actual = 16;
        break;
    }
    case CMD_READ: case CMD_WRITE: case TD_FORMAT: case CMD_UPDATE:
    case TD_READ64: case TD_WRITE64: case TD_FORMAT64:
    case NSCMD_TD_READ64: case NSCMD_TD_WRITE64: case NSCMD_TD_FORMAT64:
        io->io_Flags &= ~IOF_QUICK;
        PutMsg(port, &io->io_Message);
        return;
    default:
        io->io_Error = IOERR_NOCMD;
        break;
    }
    done(io);
}

static LONG d_abortio(struct IOStdReq *io __asm("a1"), struct Device *d __asm("a6"))
{
    struct Node *n;
    (void)d;
    Forbid();
    for (n = port->mp_MsgList.lh_Head; n->ln_Succ; n = n->ln_Succ)
        if (n == (struct Node *)io) {
            Remove(n);
            io->io_Error = IOERR_ABORTED;
            ReplyMsg(&io->io_Message);
            break;
        }
    if (io->io_Command == TD_ADDCHANGEINT) {   /* (remove if still on a change list) */
        ULONG k;
        for (k = 0; k < ZZD_UNITS; k++)
            for (n = zzd_units[k].ints.lh_Head; n->ln_Succ; n = n->ln_Succ)
                if (n == (struct Node *)io) { Remove(n); io->io_Error = IOERR_ABORTED; ReplyMsg(&io->io_Message); break; }
    }
    Permit();
    return 0;
}

/* ---- ZZPlug's side ---------------------------------------------------------- */

int zzd_open(void)
{
    static APTR vectors[] = { (APTR)d_open, (APTR)d_close, (APTR)d_expunge, (APTR)d_null,
                              (APTR)d_beginio, (APTR)d_abortio, (APTR)-1 };
    int i;
    for (i = 0; i < ZZD_UNITS; i++) {
        struct List *l = &zzd_units[i].ints;
        memset(&zzd_units[i], 0, sizeof(zzd_units[i]));
        l->lh_Head = (struct Node *)&l->lh_Tail;      /* NewList() is in amiga.lib */
        l->lh_Tail = NULL;
        l->lh_TailPred = (struct Node *)&l->lh_Head;
    }
    if (!(port = CreateMsgPort())) return 0;
    Forbid();
    if (FindName(&SysBase->DeviceList, (STRPTR)ZZD_NAME)) { Permit(); return 0; }
    Permit();
    dev = (struct Device *)MakeLibrary(vectors, NULL, NULL, sizeof(struct Device), 0);
    if (!dev) return 0;
    dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
    dev->dd_Library.lib_Node.ln_Name = (char *)ZZD_NAME;
    dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->dd_Library.lib_Version = 0;
    dev->dd_Library.lib_Revision = 4;
    dev->dd_Library.lib_IdString = (APTR)"zzusbdisk.device 0.4 (29.10.2026) ZZPlug USB storage";
    AddDevice(dev);
    return 1;
}

int zzd_in_use(void)
{
    return dev && dev->dd_Library.lib_OpenCnt > 0;
}

void zzd_close(void)
{
    if (dev) {
        /* not RemDevice(): that calls our expunge vector, which leaves the node on the list */
        Forbid();
        Remove(&dev->dd_Library.lib_Node);
        Permit();
        FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize, dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
        dev = NULL;
    }
    if (port) { DeleteMsgPort(port); port = NULL; }
}

ULONG zzd_sigmask(void)
{
    return port ? 1UL << port->mp_SigBit : 0;
}

static void notify(struct zzd_unit *u)
{
    struct Node *n;
    Forbid();
    for (n = u->ints.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct IOStdReq *io = (struct IOStdReq *)n;
        if (io->io_Data) Cause((struct Interrupt *)io->io_Data);
    }
    Permit();
}

void zzd_set_media(int unit, struct zzp_msd *msd)
{
    struct zzd_unit *u = &zzd_units[unit];
    int was = u->present;
    u->msd = msd;
    u->present = msd && msd->ready;
    if (u->present) { u->blocks = msd->blocks; u->blksize = msd->blksize; u->wp = msd->write_protected; }
    if (was != u->present) {
        u->changenum++;
        notify(u);
    }
}

static int map_error(int r)
{
    if (r == MSD_NOMEDIA) return TDERR_DiskChanged;
    if (r == MSD_RANGE) return IOERR_BADADDRESS;
    return TDERR_NotSpecified;
}

static void one(struct IOStdReq *io)
{
    struct zzd_unit *u = &zzd_units[UNIT_OF(io)];
    uint64_t off = io->io_Offset;
    int r, write, is64;

    is64 = io->io_Command == TD_READ64 || io->io_Command == TD_WRITE64 || io->io_Command == TD_FORMAT64 ||
           io->io_Command == NSCMD_TD_READ64 || io->io_Command == NSCMD_TD_WRITE64 || io->io_Command == NSCMD_TD_FORMAT64;
    if (is64) off |= (uint64_t)io->io_Actual << 32;    /* the 64-bit commands carry the offset's high half here */
    io->io_Actual = 0;
    if (!u->present || !u->msd) { io->io_Error = TDERR_DiskChanged; return; }
    if (io->io_Command == CMD_UPDATE) { zzp_msd_sync(u->msd); return; }
    write = io->io_Command == CMD_WRITE || io->io_Command == TD_FORMAT || io->io_Command == TD_WRITE64 ||
            io->io_Command == TD_FORMAT64 || io->io_Command == NSCMD_TD_WRITE64 || io->io_Command == NSCMD_TD_FORMAT64;
    if ((off % u->blksize) || (io->io_Length % u->blksize)) { io->io_Error = IOERR_BADLENGTH; return; }
    if (write && u->wp) { io->io_Error = TDERR_WriteProt; return; }
    if (!io->io_Length) return;
    r = write ? zzp_msd_write(u->msd, (uint32_t)(off / u->blksize), io->io_Length / u->blksize, io->io_Data)
              : zzp_msd_read(u->msd, (uint32_t)(off / u->blksize), io->io_Length / u->blksize, io->io_Data);
    if (r == MSD_OK) {
        io->io_Actual = io->io_Length;
        if (write) u->writes++; else u->reads++;
    } else {
        io->io_Error = (BYTE)map_error(r);
        u->errors++;
    }
}

void zzd_service(void)
{
    struct IOStdReq *io;
    if (!port) return;
    while ((io = (struct IOStdReq *)GetMsg(port))) {
        io->io_Error = 0;
        one(io);
        ReplyMsg(&io->io_Message);
    }
}
