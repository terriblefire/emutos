/*
 * romdisk.h - header for the read-only disk linked into the ROM
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */
#ifndef _ROMDISK_H
#define _ROMDISK_H

#if CONF_WITH_ROMDISK

/* driver functions */
LONG romdisk_ioctl(UWORD drv,UWORD ctrl,void *arg);
LONG romdisk_rw(WORD rw,LONG sector,WORD count,UBYTE *buf,WORD dev);

#endif /* CONF_WITH_ROMDISK */

#endif /* _ROMDISK_H */
