/*
 * romdisk.c - read-only disk whose image is linked into the ROM
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "biosdefs.h"
#include "disk.h"
#include "gemerror.h"
#include "romdisk.h"
#include "string.h"

#if CONF_WITH_ROMDISK

/* Image boundaries, defined in romdiskimg.S */
extern const UBYTE romdisk_start[];
extern const UBYTE romdisk_end[];

#define ROMDISK_SECTORS ((ULONG)(romdisk_end - romdisk_start) / SECTOR_SIZE)

LONG romdisk_ioctl(UWORD drv,UWORD ctrl,void *arg)
{
    ULONG *info = arg;

    if (drv || (ROMDISK_SECTORS == 0))
        return EUNDEV;

    switch(ctrl) {
    case GET_DISKINFO:
        info[0] = ROMDISK_SECTORS;
        info[1] = SECTOR_SIZE;
        break;
    case GET_DISKNAME:
        strcpy(arg,"ROM disk");
        break;
    case GET_MEDIACHANGE:
        return MEDIANOCHANGE;
    default:
        return ERR;
    }

    return E_OK;
}

LONG romdisk_rw(WORD rw,LONG sector,WORD count,UBYTE *buf,WORD dev)
{
    ULONG first = (ULONG)sector;
    ULONG n = (UWORD)count;

    if (dev || (ROMDISK_SECTORS == 0))
        return EUNDEV;

    if (rw & RW_RW)
        return EWRPRO;

    if ((first > ROMDISK_SECTORS) || (n > ROMDISK_SECTORS - first))
        return ESECNF;

    KDEBUG(("romdisk_rw(): sector=%lu count=%lu buf=%p\n", first, n, buf));
    memcpy(buf, romdisk_start + first * SECTOR_SIZE, n * SECTOR_SIZE);

    return E_OK;
}

#endif /* CONF_WITH_ROMDISK */
