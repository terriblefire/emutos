/*
 * amigafs.c - read-only GEMDOS access to Amiga OFS/FFS volumes
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * The partitions come from an Amiga Rigid Disk Block (see bios/disk.c).
 * They are accessed through logical Rwabs(), with the BIOS logical sector
 * size set to the filesystem block size, so the normal BDOS buffer cache
 * (getbcb()) is used for metadata, and usrio() for whole data blocks.
 *
 * Mapping onto the BDOS structures:
 *  DMD     m_amiroot etc describe the volume; m_recsiz is the block size
 *  DND     d_dirpos is the directory's header block
 *  OFD     o_dirbyt is the file's header block; o_currec/o_curcl cache the
 *          current data block list (file header or extension block) and
 *          its index in the list chain
 *  DTA     the private area holds the header block of the last entry found
 *
 * Amiga names that are not valid 8.3 names are shortened to the first
 * SHORTNAME_KEPT valid characters, '~' and a 4-digit hash of the full name,
 * e.g. "Workbench.info" -> "WOR~1C2D.INF".  Names that fit are only
 * converted to upper case.
 */

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "asm.h"
#include "fs.h"
#include "gemerror.h"
#include "string.h"
#include "mem.h"
#include "ahdi.h"
#include "tosvars.h"
#include "biosbind.h"
#include "biosext.h"
#include "intmath.h"

#if CONF_WITH_AMIGA_FS

/* DMDs, DNDs and OFDs are allocated from 64-byte blocks (see osmem.c) */
typedef char dmd_size_check[(sizeof(DMD) <= 64) ? 1 : -1];

#define DOSTYPE_MASK        0xffffff00UL
#define DOSTYPE_DOS         0x444f5300UL    /* 'DOS\0' */
#define DOSTYPE_MAX_FFS     5               /* DOS\6 & DOS\7 have long names */
#define DOSTYPE_FFS_BIT     1               /* set for FFS, clear for OFS */
#define DOSTYPE_INTL        2               /* DOS\2 and above: international */

/* block types */
#define T_HEADER            2
#define T_LIST              16

/* secondary block types */
#define ST_ROOT             1
#define ST_USERDIR          2
#define ST_LINKDIR          4
#define ST_FILE             (-3)
#define ST_LINKFILE         (-4)

/* longword offsets from the start of a header or list block */
#define HDR_TYPE            0
#define HDR_HIGHSEQ         2
#define HDR_TABLE           6               /* hash table or data block list */

/* longword offsets from the end of a header or list block */
#define TAIL_SECTYPE        1
#define TAIL_EXTENSION      2
#define TAIL_PARENT         3
#define TAIL_HASHCHAIN      4
#define TAIL_REALENTRY      11
#define TAIL_NAME           20
#define TAIL_DATE           23              /* days, minutes, ticks */
#define TAIL_BMEXT          24
#define TAIL_BYTESIZE       47
#define TAIL_PROTECT        48
#define TAIL_BMPAGES        49

#define TAIL(dm,blk,n)      ((blk)[((dm)->m_recsiz >> 2) - (n)])
#define HTSIZE(dm)          (((dm)->m_recsiz >> 2) - 56)
#define NUM_BMPAGES         25
#define MAX_NAME_LEN        30
#define OFS_DATA_HEADER     24              /* bytes before data in OFS blocks */

#define FIBF_ARCHIVE        0x10

#define TICKS_PER_SECOND    50
#define AMIGA_EPOCH         1978
#define DOS_EPOCH           1980
#define DOS_MAX_YEAR        (DOS_EPOCH + 127)

/* search position after the volume label has been returned */
#define VOLUME_KEY          0xfffffffeUL

#define SHORTNAME_KEPT      3

/* the FCB returned by amifs_scan() and amifs_next() */
static FCB fcbuf;


/*
 * get a metadata block of the given type, checking its checksum
 *
 * returns NULL if the block number or the contents are invalid
 */
static const ULONG *get_block(DMD *dm, ULONG key, ULONG type)
{
    const ULONG *blk;
    ULONG sum;
    UWORD i;

    if ((key < dm->m_amireserved) || (key >= dm->m_amiblocks))
        return NULL;

    blk = (const ULONG *)getbcb(dm, BT_DATA, key)->b_bufr;
    if (blk[HDR_TYPE] != type)
        return NULL;

    for (i = 0, sum = 0; i < (dm->m_recsiz >> 2); i++)
        sum += blk[i];

    return sum ? NULL : blk;
}


/*
 * Amiga filesystem upper-case conversion
 */
static UBYTE ami_toupper(UBYTE c, BOOL intl)
{
    if (((c >= 'a') && (c <= 'z'))
     || (intl && (c >= 0xe0) && (c <= 0xfe) && (c != 0xf7)))
        return c - ('a' - 'A');

    return c;
}


/*
 * compute the hash table slot for a name
 */
static UWORD ami_hash(DMD *dm, const UBYTE *name, UWORD len)
{
    BOOL intl = dm->m_amitype >= DOSTYPE_INTL;
    ULONG hash = len;

    while (len--)
        hash = (hash * 13 + ami_toupper(*name++, intl)) & 0x7ff;

    return hash % HTSIZE(dm);
}


/*
 * get the name of a header block as a pointer & length
 */
static const UBYTE *entry_name(DMD *dm, const ULONG *blk, UWORD *len)
{
    const UBYTE *bstr = (const UBYTE *)&TAIL(dm, blk, TAIL_NAME);

    *len = min(bstr[0], MAX_NAME_LEN);

    return bstr + 1;
}


/*
 * check for a character that may be used in a GEMDOS filename
 */
static BOOL valid_fname_char(UBYTE c)
{
    return (c > ' ') && (c < 0x7f) && !strchr("*,:;<=>?[]|\\/.\"", c);
}


/*
 * copy up to 'max' valid characters, converted to upper case
 *
 * returns the number of characters copied
 */
static int copy_valid(char *dst, const UBYTE *src, const UBYTE *end, int max)
{
    int n;

    for (n = 0; (src < end) && (n < max); src++)
        if (valid_fname_char(*src))
            dst[n++] = toupper(*src);

    return n;
}


/*
 * convert an Amiga name to the FCB name format, shortening it if necessary
 */
static void make_fcbname(char *fcbname, const UBYTE *name, UWORD len)
{
    const UBYTE *end = name + len;
    const UBYTE *dot = NULL, *p;
    BOOL fits = TRUE;
    UWORD hash;
    int n, i;

    for (p = name; p < end; p++)
    {
        if (*p == '.')
        {
            if (dot)
                fits = FALSE;   /* more than one dot */
            dot = p;
        }
        else if (!valid_fname_char(*p))
            fits = FALSE;
    }
    if (!dot)
        dot = end;

    n = dot - name;
    if ((n < 1) || (n > LEN_ZNODE) || (end - dot - 1 > LEN_ZEXT) || (end - dot == 1))
        fits = FALSE;

    memset(fcbname, ' ', FNAMELEN);
    n = copy_valid(fcbname, name, dot, fits ? LEN_ZNODE : SHORTNAME_KEPT);
    if (dot < end)
        copy_valid(fcbname+LEN_ZNODE, dot+1, end, LEN_ZEXT);

    if (fits)
        return;

    for (p = name, hash = 0; p < end; p++)
        hash = hash * 31 + ami_toupper(*p, TRUE);

    fcbname[n++] = '~';
    for (i = 12; i >= 0; i -= 4)
        fcbname[n++] = "0123456789ABCDEF"[(hash >> i) & 0x0f];
}


/*
 * convert an Amiga date stamp (days since 1978, minutes, ticks) to
 * DOS format, stored little-endian as in an FCB
 */
static void make_dostime(DOSTIME *td, const ULONG *stamp)
{
    static const UBYTE month_days[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    ULONG days = stamp[0];
    UWORD year, month, ylen, mlen, mins, secs;

    for (year = AMIGA_EPOCH; year <= DOS_MAX_YEAR; year++, days -= ylen)
    {
        ylen = ((year % 4) || (!(year % 100) && (year % 400))) ? 365 : 366;
        if (days < ylen)
            break;
    }

    if (year < DOS_EPOCH)
    {
        td->date = (1 << 5) | 1;            /* 1980-01-01 */
        td->time = 0;
    }
    else if (year > DOS_MAX_YEAR)
    {
        td->date = (127 << 9) | (12 << 5) | 31;
        td->time = (23 << 11) | (59 << 5) | 29;
    }
    else
    {
        for (month = 0; ; month++, days -= mlen)
        {
            mlen = month_days[month] + ((month == 1) && (ylen == 366));
            if (days < mlen)
                break;
        }
        mins = stamp[1] % (24 * 60);
        secs = (stamp[2] / TICKS_PER_SECOND) % 60;
        td->date = ((year - DOS_EPOCH) << 9) | ((month + 1) << 5) | (days + 1);
        td->time = ((mins / 60) << 11) | ((mins % 60) << 5) | (secs / 2);
    }

    swpw(td->time);
    swpw(td->date);
}


/*
 * return the header block number stored in an FCB made by make_fcb()
 */
ULONG amifs_fcbkey(const FCB *fcb)
{
    ULONG key;

    memcpy(&key, fcb->f_fill, sizeof(key));

    return key;
}


/*
 * build an FCB in fcbuf for the entry in header block 'blk' (number 'key')
 *
 * returns NULL for entries that cannot be accessed (e.g. soft links)
 */
static FCB *make_fcb(DMD *dm, ULONG key, const ULONG *blk)
{
    const UBYTE *name;
    UWORD len;
    LONG sectype;

    bzero(&fcbuf, sizeof(fcbuf));
    name = entry_name(dm, blk, &len);
    make_fcbname(fcbuf.f_name, name, len);
    make_dostime(&fcbuf.f_td, &TAIL(dm, blk, TAIL_DATE));

    sectype = TAIL(dm, blk, TAIL_SECTYPE);
    if ((sectype == ST_LINKFILE) || (sectype == ST_LINKDIR))
    {
        key = TAIL(dm, blk, TAIL_REALENTRY);
        blk = get_block(dm, key, T_HEADER);
        if (!blk)
            return NULL;
        sectype = TAIL(dm, blk, TAIL_SECTYPE);
    }

    if (sectype == ST_USERDIR)
        fcbuf.f_attrib = FA_SUBDIR;
    else if (sectype == ST_FILE)
    {
        fcbuf.f_attrib = FA_RO;     /* the volume is read-only */
        if (!(TAIL(dm, blk, TAIL_PROTECT) & FIBF_ARCHIVE))
            fcbuf.f_attrib |= FA_ARCHIVE;
        fcbuf.f_fileln = TAIL(dm, blk, TAIL_BYTESIZE);
        swpl(fcbuf.f_fileln);
    }
    else
        return NULL;

    memcpy(fcbuf.f_fill, &key, sizeof(key));

    return &fcbuf;
}


/*
 * build an FCB in fcbuf for the volume label
 */
static FCB *volume_fcb(DMD *dm)
{
    const ULONG *root = get_block(dm, dm->m_amiroot, T_HEADER);
    const UBYTE *name;
    UWORD len;

    if (!root)
        return NULL;

    bzero(&fcbuf, sizeof(fcbuf));
    memset(fcbuf.f_name, ' ', FNAMELEN);
    name = entry_name(dm, root, &len);
    copy_valid(fcbuf.f_name, name, name + len, FNAMELEN);
    make_dostime(&fcbuf.f_td, &TAIL(dm, root, TAIL_DATE));
    fcbuf.f_attrib = FA_VOL;

    return &fcbuf;
}


/*
 * check if a search pattern is a plain name, i.e. one that can only
 * match an Amiga name that is identical apart from case
 */
static BOOL is_plain_name(const char *pattern, UBYTE *name, UWORD *len)
{
    int i, n;

    if (*pattern == ERASE_MARKER)
        return FALSE;

    for (i = 0; i < FNAMELEN; i++)
        if ((pattern[i] == '?') || (pattern[i] == '~'))
            return FALSE;

    for (i = n = 0; (i < LEN_ZNODE) && (pattern[i] != ' '); i++)
        name[n++] = pattern[i];
    if (pattern[LEN_ZNODE] != ' ')
    {
        name[n++] = '.';
        for (i = LEN_ZNODE; (i < FNAMELEN) && (pattern[i] != ' '); i++)
            name[n++] = pattern[i];
    }
    *len = n;

    return n > 0;
}


/*
 * search for the next entry matching 'pattern' (an FCB-style name
 * followed by the attribute byte)
 *
 * if 'key' is 0, the search starts at the beginning of directory 'dirkey',
 * otherwise it continues after the entry with header block 'key'
 *
 * on success, *posp is updated with the header block of the entry found
 */
static FCB *search(DMD *dm, ULONG dirkey, ULONG key, char *pattern, LONG *posp)
{
    const ULONG *blk = NULL, *dir;
    const UBYTE *name;
    UBYTE plain[LEN_ZFNAME];
    UWORD slot = 0, len;
    ULONG next, count;
    BOOL single_chain = FALSE;
    FCB *fcb;

    KDEBUG(("amifs search(%lu,%lu,%.11s,0x%02x)\n",dirkey,key,pattern,(UBYTE)pattern[FNAMELEN]));

    if (!key && (pattern[FNAMELEN] & FA_VOL) && (dirkey == dm->m_amiroot))
    {
        *posp = VOLUME_KEY;
        fcb = volume_fcb(dm);
        if (fcb && match(pattern, fcb->f_name))
            return fcb;
    }

    if (key == VOLUME_KEY)
    {
        key = 0;
        dirkey = dm->m_amiroot;
    }

    if (key)
    {
        blk = get_block(dm, key, T_HEADER);
        if (!blk)
            return NULL;
        dirkey = TAIL(dm, blk, TAIL_PARENT);
    }
    else if (is_plain_name(pattern, plain, &len))
    {
        /* only the hash chain for this name can contain a match */
        single_chain = TRUE;
        slot = ami_hash(dm, plain, len);
    }

    /* a directory cannot have more entries than the volume has blocks */
    for (count = 0; count < dm->m_amiblocks; count++)
    {
        next = blk ? TAIL(dm, blk, TAIL_HASHCHAIN) : 0;
        if (!next)
        {
            if (blk)
            {
                if (single_chain)
                    return NULL;
                name = entry_name(dm, blk, &len);
                slot = ami_hash(dm, name, len) + 1;
            }
            dir = get_block(dm, dirkey, T_HEADER);
            if (!dir)
                return NULL;
            for ( ; slot < HTSIZE(dm); slot++)
                if ((next = dir[HDR_TABLE+slot]) || single_chain)
                    break;
            if (!next)
                return NULL;
        }

        blk = get_block(dm, next, T_HEADER);
        if (!blk)
            return NULL;

        fcb = make_fcb(dm, next, blk);
        if (fcb && match(pattern, fcb->f_name))
        {
            *posp = next;
            return fcb;
        }
    }

    return NULL;
}


/*
 * find or create the DND for the subdirectory described by an FCB
 */
static DND *get_dnd(DND *parent, const FCB *fcb)
{
    ULONG key = amifs_fcbkey(fcb);
    DND *dnd;

    for (dnd = parent->d_left; dnd; dnd = dnd->d_right)
        if (dnd->d_dirpos == key)
            return dnd;

    /* don't let the parent be scavenged while we allocate */
    parent->d_flag |= DND_LOCKED;
    dnd = MGET(DND);            /* MGET(DND) only returns if it succeeds */
    parent->d_flag &= ~DND_LOCKED;

    dnd->d_parent = parent;
    dnd->d_right = parent->d_left;
    parent->d_left = dnd;
    dnd->d_drv = parent->d_drv;
    dnd->d_dirpos = key;
    dnd->d_td = fcb->f_td;
    memcpy(dnd->d_name, fcb->f_name, FNAMELEN);

    return dnd;
}


/*
 * amifs_scan - the Amiga version of scan()
 *
 * *posp is 0 to search from the start of the directory, or -1 to find a
 * subdirectory and return its DND
 */
FCB *amifs_scan(DND *dnd, const char *n, WORD att, LONG *posp)
{
    char pattern[FNAMELEN+1];
    LONG pos = 0;
    FCB *fcb;

    builds(n, pattern);
    pattern[FNAMELEN] = att;

    if (*posp != -1L)
        return search(dnd->d_drv, dnd->d_dirpos, *posp, pattern, posp);

    fcb = search(dnd->d_drv, dnd->d_dirpos, 0, pattern, &pos);

    return fcb ? (FCB *)get_dnd(dnd, fcb) : NULL;
}


/*
 * amifs_next - continue a search after the entry at *posp
 */
FCB *amifs_next(DMD *dm, char *pattern, LONG *posp)
{
    return search(dm, 0, *posp, pattern, posp);
}


/*
 * get the block holding data block number 'idx' of a file
 *
 * returns 0 if the file has no such block
 */
static ULONG data_block(OFD *p, ULONG idx)
{
    DMD *dm = p->o_dmd;
    const ULONG *blk;
    ULONG key, list = idx / HTSIZE(dm);
    UWORD n;

    if (p->o_currec && (list == p->o_curcl))
    {
        /* the cached list block was checked when it was cached */
        blk = (const ULONG *)getbcb(dm, BT_DATA, p->o_currec)->b_bufr;
    }
    else
    {
        /* start from the cached list block if we can */
        if (p->o_currec && (list > p->o_curcl))
        {
            key = p->o_currec;
            n = p->o_curcl;
        }
        else
        {
            key = p->o_dirbyt;
            n = 0;
        }

        for (;;)
        {
            blk = get_block(dm, key, n ? T_LIST : T_HEADER);
            if (!blk)
                return 0;
            if (n == list)
                break;
            key = TAIL(dm, blk, TAIL_EXTENSION);
            n++;
        }

        p->o_currec = key;
        p->o_curcl = n;
    }

    idx -= list * HTSIZE(dm);
    if (idx >= blk[HDR_HIGHSEQ])
        return 0;

    return blk[HDR_TABLE + HTSIZE(dm) - 1 - idx];
}


/*
 * amifs_read - read 'len' bytes from the current position of a file
 *
 * the caller has already limited 'len' to the bytes left in the file
 */
long amifs_read(OFD *p, long len, char *ubufr)
{
    DMD *dm = p->o_dmd;
    UWORD hdrlen = (dm->m_amitype & DOSTYPE_FFS_BIT) ? 0 : OFS_DATA_HEADER;
    ULONG dbsize = dm->m_recsiz - hdrlen;
    ULONG idx, blk, n, offset;
    long done, nbytes;
    const UBYTE *data;

    for (done = 0; len > 0; done += nbytes)
    {
        idx = p->o_bytnum / dbsize;
        offset = p->o_bytnum % dbsize;
        blk = data_block(p, idx);
        if (!blk || (blk >= dm->m_amiblocks))
            break;              /* file is shorter than its size says */

        if (!hdrlen && !offset && (len >= dbsize))
        {
            /* read a run of consecutive blocks straight into the user buffer */
            for (n = 1; (n < len / dbsize) && (n < CNTMAX); n++)
                if (data_block(p, idx+n) != blk+n)
                    break;
            usrio(0, n, blk, ubufr, dm);
            nbytes = n * dbsize;
        }
        else
        {
            data = getbcb(dm, BT_DATA, blk)->b_bufr + hdrlen;
            nbytes = min((ULONG)len, dbsize - offset);
            memcpy(ubufr, data + offset, nbytes);
        }

        ubufr += nbytes;
        len -= nbytes;
        p->o_bytnum += nbytes;
    }

    return done;
}


/*
 * amifs_getfree - Dfree() for an Amiga volume
 */
long amifs_getfree(long *buf, DMD *dm)
{
    const ULONG *list, *bm, *pages;
    ULONG listkey = dm->m_amiroot, bmkey, nbits, freeblocks, w;
    UWORD longs = dm->m_recsiz >> 2;
    UWORD i, n, npages, page = 0;
    BOOL root = TRUE;

    nbits = dm->m_amiblocks - dm->m_amireserved;

    /*
     * the bitmap block numbers are listed in the root block, then in a
     * chain of extension blocks; a set bit means the block is free
     */
    for (freeblocks = 0; nbits; )
    {
        list = root ? get_block(dm, listkey, T_HEADER)
                    : (const ULONG *)getbcb(dm, BT_DATA, listkey)->b_bufr;
        if (!list)
            break;
        pages = root ? &TAIL(dm, list, TAIL_BMPAGES) : list;
        npages = root ? NUM_BMPAGES : longs - 1;

        if (page == npages)
        {
            listkey = root ? TAIL(dm, list, TAIL_BMEXT) : list[longs-1];
            if (!listkey || (listkey >= dm->m_amiblocks))
                break;
            root = FALSE;
            page = 0;
            continue;
        }

        bmkey = pages[page++];
        if (!bmkey || (bmkey >= dm->m_amiblocks))
            break;
        bm = (const ULONG *)getbcb(dm, BT_DATA, bmkey)->b_bufr;
        for (i = 1; (i < longs) && nbits; i++, nbits -= n)
        {
            n = min(nbits, 32UL);
            w = bm[i];
            if (n < 32)
                w &= (1UL << n) - 1;
            for ( ; w; w &= w - 1)
                freeblocks++;
        }
    }

    *buf++ = freeblocks;
    *buf++ = dm->m_amiblocks - dm->m_amireserved;
    *buf++ = dm->m_recsiz;
    *buf = 1;

    return E_OK;
}


/*
 * amifs_mount - log in an Amiga volume on drive 'drv'
 *
 * returns 0 if OK, 1 if this is not a volume we support, or ENSMEM
 */
WORD amifs_mount(int drv)
{
    const AMIGA_PARTINFO *pi = blkdev_amiga_partinfo(drv);
    const ULONG *root;
    DMD *dm;

    if (!pi || ((pi->dostype & DOSTYPE_MASK) != DOSTYPE_DOS)
     || ((pi->dostype & 0xff) > DOSTYPE_MAX_FFS)
     || (pi->blocksize > pun_ptr->max_sect_siz)
     || (pi->blocks <= pi->reserved))
    {
        KDEBUG(("amifs_mount(%d): unsupported partition\n",drv));
        return 1;
    }

    if (!(dm = getdmd(drv)))
        return ENSMEM;

    dm->m_recsiz = pi->blocksize;
    dm->m_amiblocks = pi->blocks;
    dm->m_amireserved = pi->reserved;
    dm->m_amitype = pi->dostype;
    dm->m_amiroot = (pi->blocks - 1 + pi->reserved) / 2;
    dm->m_dtl->d_dirpos = dm->m_amiroot;

    root = get_block(dm, dm->m_amiroot, T_HEADER);
    if (!root || (TAIL(dm, root, TAIL_SECTYPE) != ST_ROOT))
    {
        KDEBUG(("amifs_mount(%d): no root block at %lu\n",drv,dm->m_amiroot));
        xmfreblk(dm->m_dtl);
        xmfreblk(dm);
        drvtbl[drv] = NULL;
        return 1;
    }

    KDEBUG(("amifs_mount(%d): DOS\\%d, %lu blocks of %u bytes, root %lu\n",
            drv,dm->m_amitype,dm->m_amiblocks,dm->m_recsiz,dm->m_amiroot));

    return E_OK;
}

#endif /* CONF_WITH_AMIGA_FS */
