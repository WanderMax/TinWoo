/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT Filesystem module  R0.15                               /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2022, ChaN, all right reserved.
/
/ FatFs module is an open source software. Redistribution and use of FatFs in
/ source and binary forms, with or without modification, are permitted provided
/ that the following condition is met:

/ 1. Redistributions of source code must retain the above copyright notice,
/    this condition and the following disclaimer.
/
/ This software is provided by the copyright holder and contributors "AS IS"
/ and any warranties related to this software are DISCLAIMED.
/ The copyright owner or contributors be NOT LIABLE for any damages caused
/ by use of this software.
/
/----------------------------------------------------------------------------*/


#ifndef FF_DEFINED
#define FF_DEFINED	80286	/* Revision ID */

#ifdef __cplusplus
extern "C" {
#endif

#include "ffconf.h"		/* FatFs configuration options */

#if FF_DEFINED != FFCONF_DEF
#error Wrong configuration file (ffconf.h).
#endif


/* Integer types used for FatFs API */

#if defined(_WIN32)		/* Windows VC++ (for development only) */
#define FF_INTDEF 2
#include <windows.h>
typedef unsigned __int64 QWORD;
#include <float.h>
#define isnan(v) _isnan(v)
#define isinf(v) (!_finite(v))

#elif (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L) || defined(__cplusplus)	/* C99 or later */
#define FF_INTDEF 2
#include <stdint.h>
typedef unsigned int	UINT;	/* int must be 16-bit or 32-bit */
typedef unsigned char	BYTE;	/* char must be 8-bit */
typedef uint16_t		WORD;	/* 16-bit unsigned integer */
typedef uint32_t		DWORD;	/* 32-bit unsigned integer */
typedef uint64_t		QWORD;	/* 64-bit unsigned integer */
typedef WORD			WCHAR;	/* UTF-16 character type */

#else  	/* Earlier than C99 */
#define FF_INTDEF 1
typedef unsigned int	UINT;	/* int must be 16-bit or 32-bit */
typedef unsigned char	BYTE;	/* char must be 8-bit */
typedef unsigned short	WORD;	/* 16-bit unsigned integer */
typedef unsigned long	DWORD;	/* 32-bit unsigned integer */
typedef WORD			WCHAR;	/* UTF-16 character type */
#endif


/* Type of file size and LBA variables */

#if FF_FS_EXFAT
#if FF_INTDEF != 2
#error exFAT feature wants C99 or later
#endif
typedef QWORD FSIZE_t;
#if FF_LBA64
typedef QWORD LBA_t;
#else
typedef DWORD LBA_t;
#endif
#else
#if FF_LBA64
#error exFAT needs to be enabled when enable 64-bit LBA
#endif
typedef DWORD FSIZE_t;
typedef DWORD LBA_t;
#endif



/* Type of path name strings on FatFs API (TCHAR) */

#if FF_USE_LFN && FF_LFN_UNICODE == 1 	/* Unicode in UTF-16 encoding */
typedef WCHAR TCHAR;
#define _T(x) L ## x
#define _TEXT(x) L ## x
#elif FF_USE_LFN && FF_LFN_UNICODE == 2	/* Unicode in UTF-8 encoding */
typedef char TCHAR;
#define _T(x) u8 ## x
#define _TEXT(x) u8 ## x
#elif FF_USE_LFN && FF_LFN_UNICODE == 3	/* Unicode in UTF-32 encoding */
typedef DWORD TCHAR;
#define _T(x) U ## x
#define _TEXT(x) U ## x
#elif FF_USE_LFN && (FF_LFN_UNICODE < 0 || FF_LFN_UNICODE > 3)
#error Wrong FF_LFN_UNICODE setting
#else									/* ANSI/OEM code in SBCS/DBCS */
typedef char TCHAR;
#define _T(x) x
#define _TEXT(x) x
#endif



/* Filesystem object structure (FATFS) */

typedef struct {
	BYTE	fs_type;		/* Filesystem type (0:not mounted) */
	BYTE	pdrv;			/* Volume hosting physical drive */
#if FF_FS_REENTRANT
	BYTE	ldrv;			/* Logical drive number */
#endif
	BYTE	ro_flag;		/* Read-only flag */
	BYTE	n_fats;			/* Number of FATs (1 or 2) */
	BYTE	wflag;			/* win[] status (b0:dirty) */
	BYTE	fsi_flag;		/* FSINFO status (b7:disabled, b0:dirty) */
	WORD	id;				/* Volume mount ID */
	WORD	n_rootdir;		/* Number of root directory entries (FAT12/16) */
	WORD	csize;			/* Cluster size [sectors] */
#if FF_MAX_SS != FF_MIN_SS
	WORD	ssize;			/* Sector size (512, 1024, 2048 or 4096) */
#endif
#if FF_USE_LFN
	WCHAR*	lfnbuf;			/* LFN working buffer */
#endif
#if FF_FS_EXFAT
	BYTE*	dirbuf;			/* Directory entry block scratchpad buffer for exFAT */
#endif
	DWORD	last_clst;		/* Last allocated cluster */
	DWORD	free_clst;		/* Number of free clusters */
	DWORD	cdir;			/* Current directory start cluster (0:root) */
#if FF_FS_EXFAT
	DWORD	cdc_scl;		/* Containing directory start cluster (invalid when cdir is 0) */
	DWORD	cdc_size;		/* b31-b8:Size of containing directory, b7-b0: Chain status */
	DWORD	cdc_ofs;		/* Offset in the containing directory (invalid when cdir is 0) */
#endif
	DWORD	n_fatent;		/* Number of FAT entries (number of clusters + 2) */
	DWORD	fsize;			/* Number of sectors per FAT */
	LBA_t	volbase;		/* Volume base sector */
	LBA_t	fatbase;		/* FAT base sector */
	LBA_t	dirbase;		/* Root directory base sector (FAT12/16) or cluster (FAT32/exFAT) */
	LBA_t	database;		/* Data base sector */
#if FF_FS_EXFAT
	LBA_t	bitbase;		/* Allocation bitmap base sector */
#endif
	LBA_t	winsect;		/* Current sector appearing in the win[] */
	BYTE	win[FF_MAX_SS];	/* Disk access window for Directory, FAT (and file data at tiny cfg) */
} FATFS;



/* Object ID and allocation information (FFOBJID) */

typedef struct {
	FATFS*	fs;				/* Pointer to the hosting volume of this object */
	WORD	id;				/* Hosting volume's mount ID */
	BYTE	attr;			/* Object attribute */
	BYTE	stat;			/* Object chain status (b1-0: =0:not contiguous, =2:contiguous, =3:fragmented in this session, b2:sub-directory stretched) */
	DWORD	sclust;			/* Object data start cluster (0:no cluster or root directory) */
	FSIZE_t	objsize;		/* Object size (valid when sclust != 0) */
#if FF_FS_EXFAT
	DWORD	n_cont;			/* Size of first fragment - 1 (valid when stat == 3) */
	DWORD	n_frag;			/* Size of last fragment needs to be written to FAT (valid when not zero) */
	DWORD	c_scl;			/* Containing directory start cluster (valid when sclust != 0) */
	DWORD	c_size;			/* b31-b8:Size of containing directory, b7-b0: Chain status (valid when c_scl != 0) */
	DWORD	c_ofs;			/* Offset in the containing directory (valid when file object and sclust != 0) */
#endif
#if FF_FS_LOCK
	UINT	lockid;			/* File lock ID origin from 1 (index of file semaphore table Files[]) */
#endif
} FFOBJID;



/* File object structure (FIL) */

typedef struct {
	FFOBJID	obj;			/* Object identifier (must be the 1st member to detect invalid object pointer) */
	BYTE	flag;			/* File status flags */
	BYTE	err;			/* Abort flag (error code) */
	FSIZE_t	fptr;			/* File read/write pointer (Zeroed on file open) */
	DWORD	clust;			/* Current cluster of fpter (invalid when fptr is 0) */
	LBA_t	sect;			/* Sector number appearing in buf[] (0:invalid) */
	LBA_t	dir_sect;		/* Sector number containing the directory entry (not used at exFAT) */
	BYTE*	dir_ptr;		/* Pointer to the directory entry in the win[] (not used at exFAT) */
#if FF_USE_FASTSEEK
	DWORD*	cltbl;			/* Pointer to the cluster link map table (nulled on open, set by application) */
#endif
#if !FF_FS_TINY
	BYTE	buf[FF_MAX_SS];	/* File private data read/write window */
#endif
} FIL;



/* Directory object structure (DIR) */

typedef struct {
	FFOBJID	obj;			/* Object identifier */
	DWORD	dptr;			/* Current read/write offset */
	DWORD	clust;			/* Current cluster */
	LBA_t	sect;			/* Current sector (0:Read operation has terminated) */
	BYTE*	dir;			/* Pointer to the directory item in the win[] */
	BYTE	fn[12];			/* SFN (in/out) {body[8],ext[3],status[1]} */
#if FF_USE_LFN
	DWORD	blk_ofs;		/* Offset of current entry block being processed (0xFFFFFFFF:Invalid) */
#endif
#if FF_USE_FIND
	const TCHAR* pat;		/* Pointer to the name matching pattern */
#endif
} DIR;



/* File information structure (FILINFO) */

typedef struct {
	FSIZE_t	fsize;			/* File size */
	WORD	fdate;			/* Modified date */
	WORD	ftime;			/* Modified time */
	BYTE	fattrib;		/* File attribute */
#if FF_USE_LFN
	TCHAR	altname[FF_SFN_BUF + 1];/* Alternative file name */
	TCHAR	fname[FF_LFN_BUF + 1];	/* Primary file name */
#else
	TCHAR	fname[12 + 1];	/* File name */
#endif
} FILINFO;



/* File function return code (FRESULT) */

typedef enum {
	FR_OK = 0,				/* (0) Succeeded */
	FR_DISK_ERR,			/* (1) A hard error occurred in the low level disk I/O layer */
	FR_INT_ERR,				/* (2) Assertion failed */
	FR_NOT_READY,			/* (3) The physical drive cannot work */
	FR_NO_FILE,				/* (4) Could not find the file */
	FR_NO_PATH,				/* (5) Could not find the path */
	FR_INVALID_NAME,		/* (6) The path name format is invalid */
	FR_DENIED,				/* (7) Access denied due to prohibited access or directory full */
	FR_EXIST,				/* (8) Access denied due to prohibited access */
	FR_INVALID_OBJECT,		/* (9) The file/directory object is invalid */
	FR_WRITE_PROTECTED,		/* (10) The physical drive is write protected */
	FR_INVALID_DRIVE,		/* (11) The logical drive number is invalid */
	FR_NOT_ENABLED,			/* (12) The volume has no work area */
	FR_NO_FILESYSTEM,		/* (13) There is no valid FAT volume */
	FR_TIMEOUT,				/* (14) Could not get a grant to access the volume within defined period */
	FR_LOCKED,				/* (15) The operation is rejected according to the file sharing policy */
	FR_NOT_ENOUGH_CORE,		/* (16) LFN working buffer could not be allocated */
	FR_TOO_MANY_OPEN_FILES,	/* (17) Number of open files > FF_FS_LOCK */
	FR_INVALID_PARAMETER	/* (18) Given parameter is invalid */
} FRESULT;



/*--------------------------------------------------------------*/
/* FatFs Module Application Interface                           */
/*--------------------------------------------------------------*/

FRESULT ff_open (FIL* fp, const TCHAR* path, BYTE mode);				/* Open or create a file */
FRESULT ff_close (FIL* fp);											/* Close an open file object */
FRESULT ff_read (FIL* fp, void* buff, UINT btr, UINT* br);			/* Read data from the file */
FRESULT ff_write (FIL* fp, const void* buff, UINT btw, UINT* bw);	/* Write data to the file */
FRESULT ff_lseek (FIL* fp, FSIZE_t ofs);								/* Move file pointer of the file object */
FRESULT ff_truncate (FIL* fp);										/* Truncate the file */
FRESULT ff_sync (FIL* fp);											/* Flush cached data of the writing file */
FRESULT ff_opendir (DIR* dp, const TCHAR* path);						/* Open a directory */
FRESULT ff_closedir (DIR* dp);										/* Close an open directory */
FRESULT ff_readdir (DIR* dp, FILINFO* fno);							/* Read a directory item */
FRESULT ff_findfirst (DIR* dp, FILINFO* fno, const TCHAR* path, const TCHAR* pattern);	/* Find first file */
FRESULT ff_findnext (DIR* dp, FILINFO* fno);							/* Find next file */
FRESULT ff_mkdir (const TCHAR* path);								/* Create a sub directory */
FRESULT ff_unlink (const TCHAR* path);								/* Delete an existing file or directory */
FRESULT _ff_rename (const TCHAR* path_old, const TCHAR* path_new);	/* Rename/Move a file or directory */
FRESULT ff_stat (const TCHAR* path, FILINFO* fno);					/* Get file status */
FRESULT ff_chmod (const TCHAR* path, BYTE attr, BYTE mask);			/* Change attribute of a file/dir */
FRESULT ff_utime (const TCHAR* path, const FILINFO* fno);			/* Change timestamp of a file/dir */
FRESULT ff_getfree (const TCHAR* path, DWORD* nclst, FATFS** fatfs);	/* Get number of free clusters on the drive */
FRESULT ff_getlabel (const TCHAR* path, TCHAR* label, DWORD* vsn);	/* Get volume label */
FRESULT ff_setlabel (const TCHAR* label);							/* Set volume label */
FRESULT ff_forward (FIL* fp, UINT(*func)(const BYTE*,UINT), UINT btf, UINT* bf);	/* Forward data to the stream */
FRESULT ff_expand (FIL* fp, FSIZE_t fsz, BYTE opt);					/* Allocate a contiguous block to the file */
FRESULT ff_mount (FATFS* fs, const TCHAR* path, BYTE opt);			/* Mount/Unmount a logical drive */
FRESULT ff_setcp (WORD cp);											/* Set current code page */
int ff_putc (TCHAR c, FIL* fp);										/* Put a character to the file */
int ff_puts (const TCHAR* str, FIL* cp);								/* Put a string to the file */
int ff_printf (FIL* fp, const TCHAR* str, ...);						/* Put a formatted string to the file */
TCHAR* ff_gets (TCHAR* buff, int len, FIL* fp);						/* Get a string from the file */

/* Some API fucntions are implemented as macro */

#define ff_eof(fp) ((int)((fp)->fptr == (fp)->obj.objsize))
#define ff_error(fp) ((fp)->err)
#define ff_tell(fp) ((fp)->fptr)
#define ff_size(fp) ((fp)->obj.objsize)
#define ff_rewind(fp) ff_lseek((fp), 0)
#define ff_rewinddir(dp) ff_readdir((dp), 0)
#define ff_rmdir(path) ff_unlink(path)
#define ff_unmount(path) ff_mount(0, path, 0)




/*--------------------------------------------------------------*/
/* Additional Functions                                         */
/*--------------------------------------------------------------*/

/* RTC function (provided by user) */
#define FAT_TIMESTAMP(year, mon, mday, hour, min, sec)	({ \
	DWORD fat_year = (DWORD)year, fat_mon = (DWORD)mon, fat_mday = (DWORD)mday, fat_hour = (DWORD)hour, fat_min = (DWORD)min, fat_sec = (DWORD)sec; \
	if (fat_year < 1980 || fat_year > 2107) fat_year = FF_NORTC_YEAR; \
	fat_year -= 1980; \
	if (fat_mon < 1 || fat_mon > 12) fat_mon = FF_NORTC_MON; \
	if (fat_mday < 1 || fat_mday > 31) fat_mday = FF_NORTC_MDAY; \
	if (fat_hour > 23) fat_hour = 0; \
	if (fat_min > 59) fat_min = 0; \
	if (fat_sec > 58) fat_sec = 58; \
	fat_sec /= 2; \
	DWORD timestamp = (((fat_year << 25) & (DWORD)0xFE000000) | ((fat_mon << 21) & (DWORD)0x01E00000) | ((fat_mday << 16) & (DWORD)0x001F0000) | ((fat_hour << 11) & (DWORD)0x0000F800) | ((fat_min << 5) & (DWORD)0x000007E0) | (fat_sec & (DWORD)0x0000001F)); \
	timestamp; \
})

#if !FF_FS_NORTC
DWORD get_fattime (void);	/* Get current time */
#endif


/* LFN support functions (defined in ffunicode.c) */

#if FF_USE_LFN >= 1
WCHAR ff_oem2uni (WCHAR oem, WORD cp);	/* OEM code to Unicode conversion */
WCHAR ff_uni2oem (DWORD uni, WORD cp);	/* Unicode to OEM code conversion */
DWORD ff_wtoupper (DWORD uni);			/* Unicode upper-case conversion */
#endif


/* O/S dependent functions (samples available in ffsystem.c) */

#if FF_USE_LFN == 3		/* Dynamic memory allocation */
void* ff_memalloc (UINT msize);		/* Allocate memory block */
void ff_memfree (void* mblock);		/* Free memory block */
#endif
#if FF_FS_REENTRANT	/* Sync functions */
int ff_mutex_create (int vol);		/* Create a sync object */
void ff_mutex_delete (int vol);		/* Delete a sync object */
int ff_mutex_take (int vol);		/* Lock sync object */
void ff_mutex_give (int vol);		/* Unlock sync object */
#endif




/*--------------------------------------------------------------*/
/* Flags and Offset Address                                     */
/*--------------------------------------------------------------*/

/* File access mode and open method flags (3rd argument of ff_open) */
#define	FA_READ				0x01
#define	FA_WRITE			0x02
#define	FA_OPEN_EXISTING	0x00
#define	FA_CREATE_NEW		0x04
#define	FA_CREATE_ALWAYS	0x08
#define	FA_OPEN_ALWAYS		0x10
#define	FA_OPEN_APPEND		0x30

/* Fast seek controls (2nd argument of ff_lseek) */
#define CREATE_LINKMAP	((FSIZE_t)0 - 1)

/* Filesystem type (FATFS.fs_type) */
#define FS_FAT12	1
#define FS_FAT16	2
#define FS_FAT32	3
#define FS_EXFAT	4

/* File attribute bits for directory entry (FILINFO.fattrib) */
#define	AM_RDO	0x01	/* Read only */
#define	AM_HID	0x02	/* Hidden */
#define	AM_SYS	0x04	/* System */
#define AM_DIR	0x10	/* Directory */
#define AM_ARC	0x20	/* Archive */


#ifdef __cplusplus
}
#endif

#endif /* FF_DEFINED */
