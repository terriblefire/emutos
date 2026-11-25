/*
 * romdiskimg.S - disk image linked into the ROM
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

#include "asmdefs.h"

#if CONF_WITH_ROMDISK

        .globl  _romdisk_start
        .globl  _romdisk_end

        .text
        .balign 4
_romdisk_start:
        .incbin ROMDISK_IMAGE
_romdisk_end:

#endif /* CONF_WITH_ROMDISK */
