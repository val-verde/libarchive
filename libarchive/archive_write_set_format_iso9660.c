/*-
 * Copyright (c) 2009,2010 Michihiro NAKAJIMA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "archive_platform.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <stdio.h>
#include <stdarg.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_ZLIB_H
#include <zlib.h>
#endif

#include "archive.h"
#include "archive_endian.h"
#include "archive_entry.h"
#include "archive_private.h"
#include "archive_write_private.h"

#if defined(_WIN32) && !defined(__CYGWIN__)
#define getuid()			0
#define getgid()			0
#endif

/*#define DEBUG 1*/
#ifdef DEBUG
/* To compare to the ISO image file made by mkisofs. */
#define COMPAT_MKISOFS		1
#endif

#define LOGICAL_BLOCK_SIZE			2048
#define PATH_TABLE_BLOCK_SIZE			4096

#define SYSTEM_AREA_BLOCK			16
#define PRIMARY_VOLUME_DESCRIPTOR_BLOCK 	1
#define SUPPLEMENTARY_VOLUME_DESCRIPTOR_BLOCK 	1
#define BOOT_RECORD_DESCRIPTOR_BLOCK	 	1
#define VOLUME_DESCRIPTOR_SET_TERMINATOR_BLOCK	1
#define NON_ISO_FILE_SYSTEM_INFORMATION_BLOCK	1
#define RRIP_ER_BLOCK				1
#define PADDING_BLOCK				150

#define FD_1_2M_SIZE		(1024 * 1200)
#define FD_1_44M_SIZE		(1024 * 1440)
#define FD_2_88M_SIZE		(1024 * 2880)
#define MULTI_EXTENT_SIZE	(ARCHIVE_LITERAL_LL(1) << 32)	/* 4Gi bytes. */
#define MAX_DEPTH		8
#define RR_CE_SIZE		28		/* SUSP "CE" extension size */

#define FILE_FLAG_EXISTENCE	0x01
#define FILE_FLAG_DIRECTORY	0x02
#define FILE_FLAG_ASSOCIATED	0x04
#define FILE_FLAG_RECORD	0x08
#define FILE_FLAG_PROTECTION	0x10
#define FILE_FLAG_MULTI_EXTENT	0x80

static const char rrip_identifier[] =
	"RRIP_1991A";
static const char rrip_descriptor[] =
	"THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR "
	"POSIX FILE SYSTEM SEMANTICS";
static const char rrip_source[] =
	"PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE.  "
	"SEE PUBLISHER IDENTIFIER IN PRIMARY VOLUME DESCRIPTOR FOR "
	"CONTACT INFORMATION.";
#define RRIP_ER_ID_SIZE		(sizeof(rrip_identifier)-1)
#define RRIP_ER_DSC_SIZE	(sizeof(rrip_descriptor)-1)
#define RRIP_ER_SRC_SIZE	(sizeof(rrip_source)-1)
#define RRIP_ER_SIZE		(8 + RRIP_ER_ID_SIZE + \
				RRIP_ER_DSC_SIZE + RRIP_ER_SRC_SIZE)

static const unsigned char zisofs_magic[8] = {
	0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07
};

#define ZF_HEADER_SIZE	16	/* zisofs header size. */
#define ZF_LOG2_BS	15	/* log2 block size; 32K bytes. */
#define ZF_BLOCK_SIZE	(1UL << ZF_LOG2_BS)

/*
 * Manage extra records.
 */
struct extr_rec {
	int		 location;
	int		 offset;
	unsigned char	 buf[LOGICAL_BLOCK_SIZE];
	struct extr_rec	*next;
};

struct ctl_extr_rec {
	int		 use_extr;
	unsigned char	*bp;
	struct isoent	*isoent;
	unsigned char	*ce_ptr;
	int		 cur_len;
	int		 dr_len;
	int		 limit;
	int		 extr_off;
	int		 extr_loc;
};
#define DR_SAFETY	RR_CE_SIZE
#define DR_LIMIT	(254 - DR_SAFETY)

/*
 * The relation of struct isofile and isoent and archive_entry.
 *
 * Primary volume tree  --> struct isoent
 *                                |
 *                                v
 *                          struct isofile --> archive_entry
 *                                ^
 *                                |
 * Joliet volume tree   --> struct isoent
 *
 * struct isoent has specific information for volume.
 */

struct isofile {
	/* Used for managing struct isofile list. */
	struct isofile		*allnext;
	/* Used for managing a hardlined struct isofile list. */
	struct isofile		*hlnext;
	struct isofile		*hardlink_target;

	struct archive_entry	*entry;
	int			 temp_fd;

	/*
	 * Used for making a directory tree.
	 */
	struct archive_string	 parentdir;
	struct archive_string	 basename;
	struct archive_string	 symlink;
	int			 dircnt;	/* The number of elements of 
						 * its parent directory */

	/*
	 * Used for a Directory Record.
	 */
	struct content {
		int64_t		 offset_of_temp;
		uint32_t 	 location;
		int64_t		 size;
		/*
		 * One extent equals one content.
		 * If this entry has multi extent, `next' variable points
		 * next content data.
		 */
		struct content	*next;		/* next content	*/
	} content, *cur_content;

	enum {
		NO = 0,
		BOOT_CATALOG,
		BOOT_IMAGE,
	} boot;

	/*
	 * Used for a zisofs.
	 */
	struct {
		unsigned char	 header_size;
		unsigned char	 log2_bs;
		uint32_t	 uncompressed_size;
		int		 keep_original;
		int64_t		 original_offset_of_temp;
	} zisofs;
};

struct isoent {
	struct isofile		*file;

	struct isoent		*parent;
	/* A list of sub directories. */
	struct {
		struct isoent	*first;
		struct isoent	**last;
		int		 cnt;
	}			 children;
	/* A sorted list of sub directories. */
	struct isoent		**children_sorted;
	/* Used for managing struct isoent list. */
	struct isoent		*chnext;
	struct isoent		*ptnext;

	/*
	 * Used for making a Directory Record.
	 */
	int			 dir_number;
	struct {
		int		 vd;
		int		 self;
		int		 parent;
		int		 normal;
	}			 dr_len;
	uint32_t 		 dir_location;
	int			 dir_block;

	/*
	 * Identifier:
	 *   on primary, ISO9660 file/directory name.
	 *   on joliet, UCS2 file/directory name.
	 * ext_off   : offset of identifier extension.
	 * ext_len   : length of identifier extension.
	 * id_len    : byte size of identifier.
	 *   on primary, this is ext_off + ext_len + version length.
	 *   on joliet, this is ext_off + ext_len.
	 * mb_len    : length of multibyte-character of identifier.
	 *   on primary, mb_len and id_len are always the same.
	 *   on joliet, mb_len and id_len are different.
	 */
	char			*identifier;
	int			 ext_off;
	int			 ext_len;
	int			 id_len;
	int			 mb_len;

	/*
	 * Used for making a Rockridge extension.
	 * This is a part of Directory Records.
	 */
	struct isoent		*rr_parent;
	struct isoent		*rr_child;

	/* Extra Record.(which we call in this source file)
	 * A maximum size of the Directory Record is 254.
	 * so, if generated RRIP data of a file cannot into a Directory
	 * Record because of its size, that surplus data relocate this
	 * Extra Record.
	 */
	struct {
		struct extr_rec	*first;
		struct extr_rec	**last;
		struct extr_rec	*current;
	}			 extr_rec_list;

	int			 virtual:1;
	/* If set to one, this file type is a directory.
	 * A convenience flag to be used as
	 * "archive_entry_filetype(isoent->file->entry) == AE_IFDIR".
	 */
	int			 dir:1;
};

#define HARDLINK_HASH_SIZE	(1 << 10)	/* 1024 */
#define HARDLINK_HASH_MASK	(HARDLINK_HASH_SIZE -1)
struct hardlink {
	struct hardlink		*next;
	int			 nlink;
	struct {
		struct isofile	*first;
		struct isofile	**last;
	}			 file_list;
};

/*
 * ISO writer options
 */
struct iso_option {
	/*
	 * Usage  : abstract-file=<value>
	 * Type   : string, max 37 bytes
	 * Default: Not specified
	 * COMPAT : mkisofs -abstract <value>
	 *
	 * Specifies Abstract Filename.
	 * This file shall be described in the Root Directory
	 * and containing a abstract statement.
	 */
	unsigned int	 abstract_file:1;
#define OPT_ABSTRACT_FILE_DEFAULT	0	/* Not specified */
#define ABSTRACT_FILE_SIZE		37

	/*
	 * Usage  : application-id=<value>
	 * Type   : string, max 128 bytes
	 * Default: Not specified
	 * COMPAT : mkisofs -A/-appid <value>.
	 *
	 * Specifies Application Identifier.
	 * If the first byte is set to '_'(5F), the remaining
	 * bytes of this option shall specify an identifier
	 * for a file containing the identification of the
	 * application.
	 * This file shall be described in the Root Directory.
	 */
	unsigned int	 application_id:1;
#define OPT_APPLICATION_ID_DEFAULT	0	/* Use default identifier */
#define APPLICATION_IDENTIFIER_SIZE	128

	/*
	 * Usage  : allow-ldots
	 * Type   : boolean
	 * Default: Disabled
	 *	  : Violates the ISO9660 standard if enable.
	 * COMPAT : mkisofs -allow-leading-dots/-ldots
	 *
	 * Allow filenames to begin with dot('.') character.
	 */
	unsigned int	 allow_ldots:1;
#define OPT_ALLOW_LDOTS_DEFAULT		0	/* Disabled */

	/*
	 * Usage  : allow-lowercase
	 * Type   : boolean
	 * Default: Disabled
	 *	  : Violates the ISO9660 standard if enable.
	 * COMPAT : mkisofs -allow-lowercase
	 *
	 * Allow filenames to use lower case characters.
	 */
	unsigned int	 allow_lowercase:1;
#define OPT_ALLOW_LOWERCASE_DEFAULT	0	/* Disabled */

	/*
	 * Usage  : allow-multidot
	 * Type   : boolean
	 * Default: Disabled
	 *	  : Violates the ISO9660 standard if enable.
	 * COMPAT : mkisofs -allow-multidot
	 *
	 * Allow filenames to use multiple dot('.') except
	 * begining dot(see allow_ldots).
	 */
	unsigned int	 allow_multidot:1;
#define OPT_ALLOW_MULTIDOT_DEFAULT	0	/* Disabled */

	/*
	 * Usage  : !allow-period
	 * Type   : boolean
	 * Default: Enabled
	 *	  : Violates the ISO9660 standard if disable.
	 * COMPAT : mkisofs -d
	 *
	 * Allow trailing period from filenames.
	 */
	unsigned int	 allow_period:1;
#define OPT_ALLOW_PERIOD_DEFAULT	1	/* Enabled */

	/*
	 * Usage  : allow-pvd-lowercase
	 * Type   : boolean
	 * Default: Disabled
	 *	  : Violates the ISO9660 standard if enable.
	 *	  : But mkisofs seems to use low-case characters
	 *	  : in PVD.
	 * COMPAT : NONE
	 *
	 * Allow Primary Volume Descriptor to use lower-case characters.
	 */
	unsigned int	 allow_pvd_lowercase:1;
#ifdef COMPAT_MKISOFS
#define OPT_ALLOW_PVD_LOWERCASE_DEFAULT	1	/* Enabled */
#else
#define OPT_ALLOW_PVD_LOWERCASE_DEFAULT	0	/* Disabled */
#endif

	/*
	 * Usage  : allow-sharp-tilde
	 * Type   : boolean
	 * Default: Disabled
	 *	  : Violates the ISO9660 standard if enable.
	 * COMPAT : mkisofs -no-iso-translate
	 *
	 * Allow filenames to use '#' and '~'.
	 */
	unsigned int	 allow_sharp_tilde:1;
#define OPT_ALLOW_SHARP_TILDE_DEFAULT	0	/* Disabled */

	/*
	 * Usage : !allow-vernum
	 * Type  : boolean
	 * Default: Enabled
	 *	  : Violates the ISO9660 standard if disable.
	 * COMPAT: mkisofs -N
	 *
	 * Allow filenames to use version numbers.
	 */
	unsigned int	 allow_vernum:1;
#define OPT_ALLOW_VERNUM_DEFAULT	1	/* Enabled */

	/*
	 * Usage  : biblio-file=<value>
	 * Type   : string, max 37 bytes
	 * Default: Not specified
	 * COMPAT : mkisofs -biblio <value>
	 *
	 * Specifies Bibliographic Filename.
	 * This file shall be described in the Root Directory
	 * and containing bibliographic records.
	 */
	unsigned int	 biblio_file:1;
#define OPT_BIBLIO_FILE_DEFAULT		0	/* Not specified */
#define BIBLIO_FILE_SIZE		37

	/*
	 * Usage  : boot=<value>
	 * Type   : string
	 * Default: Not specified
	 * COMPAT : mkisofs -b/-eltorito-boot <value>
	 *
	 * Specifies "El Torito" boot image file to make
	 * a bootable CD.
	 */
	unsigned int	 boot:1;
#define OPT_BOOT_DEFAULT		0	/* Not specified */

	/*
	 * Usage  : boot-catalog=<value>
	 * Type   : string
	 * Default: "boot.catalog"
	 * COMPAT : mkisofs -c/-eltorito-catalog <value>
	 *
	 * Specifies a fullpath of El Torito boot catalog.
	 */
	unsigned int	 boot_catalog:1;
#define OPT_BOOT_CATALOG_DEFAULT	0	/* Not specified */

	/*
	 * Usage  : boot-info-table
	 * Type   : boolean
	 * Default: Disabled
	 * COMPAT : mkisofs -boot-info-table
	 *
	 * Modify the boot image file specified by `boot'
	 * option; ISO writer stores boot file information
	 * into the boot file in ISO image at offset 8
	 * through offset 64.
	 */
	unsigned int	 boot_info_table:1;
#define OPT_BOOT_INFO_TABLE_DEFAULT	0	/* Disabled */

	/*
	 * Usage  : boot-load-seg=<value>
	 * Type   : hexadecimal
	 * Default: Not specified
	 * COMPAT : mkisofs -boot-load-seg <value>
	 *
	 * Specifies a load segment for boot image.
	 * This is used with no-emulation mode.
	 */
	unsigned int	 boot_load_seg:1;
#define OPT_BOOT_LOAD_SEG_DEFAULT	0	/* Not specified */

	/*
	 * Usage  : boot-load-size=<value>
	 * Type   : decimal
	 * Default: Not specified
	 * COMPAT : mkisofs -boot-load-size <value>
	 *
	 * Specifies a sector count for boot image.
	 * This is used with no-emulation mode.
	 */
	unsigned int	 boot_load_size:1;
#define OPT_BOOT_LOAD_SIZE_DEFAULT	0	/* Not specified */

	/*
	 * Usage  : boot-type=<boot-media-type>
	 *        : 'no-emulation' : 'no emulation' image
	 *        :           'fd' : floppy disk image
	 *        :    'hard-disk' : hard disk image
	 * Type   : string
	 * Default: Auto detect
	 *        : We check a size of boot image;
	 *        : If ths size is just 1.22M/1.44M/2.88M,
	 *        : we assume boot_type is 'fd';
	 *        : otherwise boot_type is 'no-emulation'.
	 * COMPAT : 
	 *    boot=no-emulation
	 *	mkisofs -no-emul-boot
	 *    boot=fd
	 *	This is a default on the mkisofs. 
	 *    boot=hard-disk
	 *	mkisofs -hard-disk-boot
	 *
	 * Specifies a type of "El Torito" boot image.
	 */
	unsigned int	 boot_type:2;
#define OPT_BOOT_TYPE_AUTO		0	/* auto detect		  */
#define OPT_BOOT_TYPE_NO_EMU		1	/* ``no emulation'' image */
#define OPT_BOOT_TYPE_FD		2	/* floppy disk image	  */
#define OPT_BOOT_TYPE_HARD_DISK		3	/* hard disk image	  */
#define OPT_BOOT_TYPE_DEFAULT		OPT_BOOT_TYPE_AUTO

	/*
	 * Usage  : compression-level=<value>
	 * Type   : decimal
	 * Default: Not specified
	 * COMPAT : NONE
	 *
	 * Specifies compression level for option zisofs=direct.
	 */
	unsigned int	 compression_level:1;
#define OPT_COMPRESSION_LEVEL_DEFAULT	0	/* Not specified */

	/*
	 * Usage  : copyright-file=<value>
	 * Type   : string, max 37 bytes
	 * Default: Not specified
	 * COMPAT : mkisofs -copyright <value>
	 *
	 * Specifies Copyright Filename.
	 * This file shall be described in the Root Directory
	 * and containing a copyright statement.
	 */
	unsigned int	 copyright_file:1;
#define OPT_COPYRIGHT_FILE_DEFAULT	0	/* Not specified */
#define COPYRIGHT_FILE_SIZE		37

	/*
	 * Usage  : gid=<value>
	 * Type   : decimal
	 * Default: Not specified
	 * COMPAT : mkisofs -gid <value>
	 *
	 * Specifies a group id to rewrite the group id of all files.
	 */
	unsigned int	 gid:1;
#define OPT_GID_DEFAULT			0	/* Not specified */

	/*
	 * Usage  : iso-level=[1234]
	 * Type   : decimal
	 * Default: 1
	 * COMPAT : mkisofs -iso-level <value>
	 *
	 * Specifies ISO9600 Level.
	 * Level 1: [DEFAULT]
	 *   - limits each file size less than 4Gi bytes;
	 *   - a File Name shall not contain more than eight
	 *     d-characters or eight d1-characters;
	 *   - a File Name Extension shall not contain more than
	 *     three d-characters or three d1-characters;
	 *   - a Directory Identifier shall not contain more
	 *     than eight d-characters or eight d1-characters.
	 * Level 2:
	 *   - limits each file size less than 4Giga bytes;
	 *   - a File Name shall not contain more than thirty
	 *     d-characters or thirty d1-characters;
	 *   - a File Name Extension shall not contain more than
	 *     thirty d-characters or thirty d1-characters;
	 *   - a Directory Identifier shall not contain more
	 *     than thirty-one d-characters or thirty-one
	 *     d1-characters.
	 * Level 3:
	 *   - no limit of file size; use multi extent.
	 * Level 4:
	 *   - this level 4 simulates mkisofs option
	 *     '-iso-level 4';
	 *   - crate a enhanced volume as mkisofs doing;
	 *   - it is the same effects that you specify options
	 *     "allow-ldots,allow-lowercase,allow-multidot,
	 *     !limit-depth,!allow-period,!allow-vernum,
	 *     relaxed-filenames", and that the maxinum length
	 *     of files and directories is raised to 193.
	 *     if rockridge option is disabled, raised to 207.
	 */
	unsigned int	 iso_level:3;
#define OPT_ISO_LEVEL_DEFAULT		1	/* ISO Level 1 */

	/*
	 * Usage  : joliet[=long]
	 *        : !joliet
	 *        :   Do not generate Joliet Volume and Records.
	 *        : joliet [DEFAULT]
	 *        :   Generates Joliet Volume and Directory Records.
	 *        :   [COMPAT: mkisofs -J/-joliet]
	 *        : joliet=long
	 *        :   The joliet filenames are up to 103 Unicode
	 *        :   characters.
	 *        :   This option breaks the Joliet specification.
	 *        :   [COMPAT: mkisofs -J -joliet-long]
	 * Type   : boolean/string
	 * Default: Enabled
	 * COMPAT : mkisofs -J / -joliet-long
	 *
	 * Generates Joliet Volume and Directory Records.
	 */
	unsigned int	 joliet:2;
#define OPT_JOLIET_DISABLE		0	/* Not generate Joliet Records. */
#define OPT_JOLIET_ENABLE		1	/* Generate Joliet Records.	*/
#define OPT_JOLIET_LONGNAME		2	/* Use long joliet filenames.	*/
#define OPT_JOLIET_DEFAULT		OPT_JOLIET_ENABLE

	/*
	 * Usage  : !limit-depth
	 * Type   : boolean
	 * Default: Enabled
	 *	  : Violates the ISO9660 standard if disable.
	 * COMPAT : mkisofs -D/-disable-deep-relocation
	 *
	 * The number of levels in hierarchy cannot exceed eight.
	 */
	unsigned int	 limit_depth:1;
#define OPT_LIMIT_DEPTH_DEFAULT		1	/* Enabled */

	/*
	 * Usage  : !limit-dirs
	 * Type   : boolean
	 * Default: Enabled
	 *	  : Violates the ISO9660 standard if disable.
	 * COMPAT : mkisofs -no-limit-pathtables
	 *
	 * Limits the number of directories less than 65536 due
	 * to the size of the Parent Directory Number of Path
	 * Table.
	 */
	unsigned int	 limit_dirs:1;
#define OPT_LIMIT_DIRS_DEFAULT		1	/* Enabled */

	/*
	 * Usage  : !pad
	 * Type   : boolean
	 * Default: Enabled
	 * COMPAT : -pad/-no-pad
	 *
	 * Pads the end of the ISO image by null of 300Ki bytes.
	 */
	unsigned int	 pad:1;
#define OPT_PAD_DEFAULT			1	/* Enabled */

	/*
	 * Usage  : publisher=<value>
	 * Type   : string, max 128 bytes
	 * Default: Not specified
	 * COMPAT : mkisofs -publisher <value>
	 *
	 * Specifies Publisher Identifier.
	 * If the first byte is set to '_'(5F), the remaining
	 * bytes of this option shall specify an identifier
	 * for a file containing the identification of the user.
	 * This file shall be described in the Root Directory.
	 */
	unsigned int	 publisher:1;
#define OPT_PUBLISHER_DEFAULT		0	/* Not specified */
#define PUBLISHER_IDENTIFIER_SIZE	128

	/*
	 * Usage  : relaxed-filenames
	 * Type   : boolean
	 * Default: Disabled
	 *	  : Violates the ISO9660 standard if enable.
	 * COMPAT : mkisofs -allow-lowercase
	 *
	 * Use all 7 bit ASCII characters except lower-case
	 * characters.
	 */
	unsigned int	 relaxed_filenames:1;
#define OPT_RELAXED_FILENAMES_DEFAULT	0	/* Disabled */

	/*
	 * Usage  : rockridge
	 *        : !rockridge
	 *        :    disable to generate SUSP and RR records.
	 *        : rockridge
	 *        :    the same as 'rockridge=useful'.
	 *        : rockridge=strict
	 *        :    generate SUSP and RR records.
	 *        :    [COMPAT: mkisofs -R]
	 *        : rockridge=useful [DEFAULT]
	 *        :    generate SUSP and RR records.
	 *        :    [COMPAT: mkisofs -r]
	 * Type   : boolean/string
	 * Default: Enabled as rockridge=useful
	 * COMPAT : mkisofs -r / -R
	 *
	 * Generates SUSP and RR records.
	 */
	unsigned int	 rr:2;
#define OPT_RR_DISABLED			0
#define OPT_RR_STRICT			1
#define OPT_RR_USEFUL			2
#define OPT_RR_DEFAULT			OPT_RR_USEFUL

	/*
	 * Usage  : uid=<value>
	 * Type   : decimal
	 * Default: Not specified
	 * COMPAT : mkisofs -uid <value>
	 *
	 * Specifies a user id to rewrite the user id of all files.
	 */
	unsigned int	 uid:1;
#define OPT_UID_DEFAULT			0	/* Not specified */

	/*
	 * Usage  : volume-id=<value>
	 * Type   : string, max 32 bytes
	 * Default: Not specified
	 * COMPAT : mkisofs -V <value>
	 *
	 * Specifies Volume Identifier.
	 */
	unsigned int	 volume_id:1;
#define OPT_VOLUME_ID_DEFAULT		0	/* Use default identifier */
#define VOLUME_IDENTIFIER_SIZE		32

	/*
	 * Usage  : zisofs[=indirect|direct]
	 *        : !zisofs [DEFAULT]
	 *        :    Disable to generate RRIP 'ZF' extension.
	 *        : zisofs=indirect
	 *        :    Generate RRIP 'ZF' extension for special files
	 *        :    which is made by mkzftree utility.
	 *        :    [COMPAT: mkisofs -z]
	 *        : zisofs
	 *        : zisofs=direct
	 *        :    Make files zisofs file and generate RRIP 'ZF'
 	 *        :    extension. So you do not need mkzftree utility
	 *        :    for making zisofs.
	 *        :    Sometimes we will choose an original file data
	 *        :    insteadof zisofs file data when following
	 *        :    condition:
	 *        :      o If zisofsed file size is more than an
	 *        :        original file size.
	 *        :      o If the number of Logical Blocks which an
	 *        :        original file will use in ISO-image file
	 *        :        is the same as or less than the number of
	 *        :        Logical Blocks which a zisofsed file will use.
	 *        :    Those won't reduce ISO-image size.
	 *        :   
	 *        :    When you specify option 'boot=<boot-image>', that
	 *        :    'boot-image' file won't be converted to zisofs file.
	 * Type   : boolean, string
	 * Default: Disabled
	 * COMPAT : mkisofs -z
	 *
	 * Generates RRIP 'ZF' System Use Entry.
	 */
	unsigned int	 zisofs:2;
#define OPT_ZISOFS_DISABLED		0
#define OPT_ZISOFS_INDIRECT		1
#define OPT_ZISOFS_DIRECT		2
#define OPT_ZISOFS_DEFAULT		OPT_ZISOFS_DISABLED

	/*
	 * Usage  : zisofs-exclude=<value>
	 *        :  You can specify this option as below
	 *        :  e.g.
	 *        :       zisofs-exclude=foo,zisofs-exclude=bar
	 *        :   or  zisofs-exclude=foo:bar
	 *        :       (':' character is separator)
	 * Type   : string
	 * Default: Not specified
	 *
	 * Specifies files which are excluded from converting to
	 * zisofs. This only works on using 'zisofs=direct' option.
	 */
	unsigned int	 zisofs_exclude:1;
#define OPT_ZISOFS_EXCLUDE_DEFAULT	0	/* Not specified */
};

struct iso9660 {
	/* The creation time of ISO image. */
	time_t			 birth_time;
	/* A file stream of a temporary file, which file contents
	 * save to until ISO iamge can be created. */
	int			 temp_fd;

	struct isofile		*cur_file;
	struct isoent		*cur_dirent;
	uint64_t		 bytes_remaining;
	int			 need_multi_extent;

	/* A list of all of struct isofile entries. */
	struct {
		struct isofile	*first;
		struct isofile	**last;
	}			 all_file_list;

	/* Used for managing to find hardlinking files. */
	struct {
		struct hardlink	*first;
		struct hardlink	**last;
	}			 hardlink[HARDLINK_HASH_SIZE];

	/* Used for making the Path Table Record. */
	struct vdd {
		/* the root of entry tree. */
		struct isoent	*rootent;
		enum vdd_type {
			VDD_PRIMARY,
			VDD_JOLIET,
			VDD_ENHANCED
		} vdd_type;

		struct path_table {
			struct isoent		*first;
			struct isoent		**last;
			struct isoent		**sorted;
			int			 cnt;
		} *pathtbl;
		int				 max_depth;

		int		 path_table_block;
		int		 path_table_size;
		int		 location_type_L_path_table;
		int		 location_type_M_path_table;
		int		 total_dir_block;
	} primary, joliet;

	/* Used for making a Volume Descriptor. */
	int			 volume_space_size;
	int			 volume_sequence_number;
	int			 total_file_block;
	struct archive_string	 volume_identifier;
	struct archive_string	 publisher_identifier;
	struct archive_string	 data_preparer_identifier;
	struct archive_string	 application_identifier;
	struct archive_string	 copyright_file_identifier;
	struct archive_string	 abstract_file_identifier;
	struct archive_string	 bibliographic_file_identifier;

	/* Used for making rockridge extensions. */
	int			 uid;
	int			 gid;
	int			 location_rrip_er;

	/* Used for making zisofs. */
	struct {
		int		 detect_magic:1;
		unsigned char	 magic_buffer[64];
		int		 magic_cnt;

#ifdef HAVE_ZLIB_H
		int		 making:1;
		int		 allzero:1;

		/*
		 * Copy a compressed file to iso9660.zisofs.temp_fd
		 * and also copy a uncompressed file(original file) to
		 * iso9660.temp_fd . If the number of logical block
		 * of the compressed file is less than the number of
		 * logical block of the uncompressed file, use it and
		 * remove the copy of the uncompressed file.
		 * but if not, we use uncompressed file and remove
		 * the copy of the compressed file.
		 */
		int		 temp_fd;
		int64_t		 offset_of_temp;
		uint32_t	*block_pointers;
		size_t		 block_pointers_allocated;
		int		 block_pointers_cnt;
		int		 block_pointers_idx;
		uint64_t	 total_size;
		uint64_t	 offset_of_block;

		z_stream	 stream;
		int		 stream_valid;
		int64_t		 remaining;
		int		 compression_level;
		struct nozf_file {
			struct archive_string	 parentdir;
			struct archive_string	 basename;
		}		*nozf_list;
		int		 nozf_cnt;
#endif
	} zisofs;

	struct isoent		*directories_too_deep;
	int			 dircnt_max;

	/* Write buffer. */
	unsigned char		 wbuff[LOGICAL_BLOCK_SIZE * 32];
	size_t			 wbuff_remaining;

	/* 'El Torito' boot data. */
	struct {
		/* boot catalog file */
		struct archive_string	 catalog_filename;
		struct isoent		*catalog;
		/* boot image file */
		struct archive_string	 boot_filename;
		struct isoent		*boot;

		unsigned char		 platform_id;
#define BOOT_PLATFORM_X86	0
#define BOOT_PLATFORM_PPC	1
#define BOOT_PLATFORM_MAC	2
		struct archive_string	 id;
		unsigned char		 media_type;
#define BOOT_MEDIA_NO_EMULATION		0
#define BOOT_MEDIA_1_2M_DISKETTE	1
#define BOOT_MEDIA_1_44M_DISKETTE	2
#define BOOT_MEDIA_2_88M_DISKETTE	3
#define BOOT_MEDIA_HARD_DISK		4
		unsigned char		 system_type;
		uint16_t		 boot_load_seg;
		uint16_t		 boot_load_size;
#define BOOT_LOAD_SIZE		4
	} el_torito;

	struct iso_option	 opt;
};

/*
 * Types of Volume Descriptor
 */
enum VD_type {
	VDT_BOOT_RECORD=0,	/* Boot Record Volume Descriptor 	*/
	VDT_PRIMARY=1,		/* Primary Volume Descriptor		*/
	VDT_SUPPLEMENTARY=2,	/* Supplementary Volume Descriptor	*/
	VDT_TERMINATOR=255	/* Volume Descriptor Set Terminator	*/
};

/*
 * Types of Directory Record
 */
enum dir_rec_type {
	DIR_REC_VD,		/* Stored in Volume Descriptor.	*/
	DIR_REC_SELF,		/* Stored as Current Directory.	*/
	DIR_REC_PARENT,		/* Stored as Parent Directory.	*/
	DIR_REC_NORMAL,		/* Stored as Child.		*/
};

/*
 * Kinds of Volume Descriptor Character
 */
enum vdc {
	VDC_STD,
	VDC_LOWERCASE,
	VDC_UCS2,
	VDC_UCS2_DIRECT,
};

/*
 * IDentifier Resolver.
 * Used for resolving duplicated filenames.
 */
struct idr {
	struct wlist {
		/* Chain wlist in flist. */
		struct wlist		*next;
		/* Chain wlist in llist. */
		struct {
			struct wlist	*next;
			struct wlist	**prev;
		} elqueue;
		struct isoent		*isoent;
		int			 weight;
		int			 noff;
	} *wlistpool;

	/* A list sorted by a filename of identifier. */
	struct flist {
		struct isoent		*isoent;
		int			 noff;
		struct flist_link {
			struct wlist	*first;
			struct wlist	**last;
		} pri, low;
	} *flistpool;

	int				 pool_size;
	int				 wlistidx;
	int				 flistidx;

	/* A list sorted by a length of identifier. */
#define IDR_LLIST_MAX			207
	struct llist {
		struct wlist		*first;
		struct wlist		**last;
	} llist[IDR_LLIST_MAX];

	char				 char_map[0x80];
};

enum char_type {
	A_CHAR,
	D_CHAR,
};


static int	iso9660_options(struct archive_write *,
		    const char *, const char *);
static int	iso9660_write_header(struct archive_write *,
		    struct archive_entry *);
static ssize_t	iso9660_write_data(struct archive_write *,
		    const void *, size_t);
static int	iso9660_finish_entry(struct archive_write *);
static int	iso9660_finish(struct archive_write *);
static int	iso9660_destroy(struct archive_write *);

static void	get_system_identitier(char *, size_t);
static void	set_str(unsigned char *, const char *, size_t, char,
		    const char *);
static inline int joliet_allowed_char(uint16_t);
static void	set_str_beutf16(unsigned char *, const char *, size_t,
		    uint16_t, enum vdc);
static void	set_str_a_characters_bp(unsigned char *, int, int,
		    const char *, enum vdc);
static void	set_str_d_characters_bp(unsigned char *, int, int,
		    const char *, enum  vdc);
static void	set_VD_bp(unsigned char *, enum VD_type, unsigned char);
static inline void set_unused_field_bp(unsigned char *, int, int);

static unsigned char *extra_open_record(unsigned char *, int,
		    struct isoent *, struct ctl_extr_rec *);
static void	extra_close_record(struct ctl_extr_rec *, int);
static unsigned char * extra_next_record(struct ctl_extr_rec *, int);
static unsigned char *extra_get_record(struct isoent *, int *, int *, int *);
static void	extra_tell_used_size(struct ctl_extr_rec *, int);
static int	extra_setup_location(struct isoent *, int);
static int	set_directory_record_rr(unsigned char *, int,
		    struct isoent *, struct iso9660 *, enum dir_rec_type);
static int	set_directory_record(unsigned char *, size_t,
		    struct isoent *, struct iso9660 *, enum dir_rec_type,
		    enum vdd_type);
static inline int get_dir_rec_size(struct iso9660 *, struct isoent *,
		    enum dir_rec_type, enum vdd_type);
static inline unsigned char *wb_buffptr(struct archive_write *);
static int	wb_write_out(struct archive_write *);
static int	wb_consume(struct archive_write *, size_t);
static int	write_null(struct archive_write *, size_t);
static int	write_VD_terminator(struct archive_write *);
static int	set_file_identifier(unsigned char *, int, int, enum vdc,
		    struct archive_write *, struct vdd *,
		    struct archive_string *, const char *, int,
		    enum char_type);
static int	write_VD(struct archive_write *, struct vdd *);
static int	write_VD_boot_record(struct archive_write *);
static int	write_information_block(struct archive_write *);
static int	write_path_table(struct archive_write *, int,
		    struct vdd *);
static int	write_directory_descriptors(struct archive_write *,
		    struct vdd *);
static int	write_file_descriptors(struct archive_write *);
static int	write_rr_ER(struct archive_write *);
static void	calculate_path_table_size(struct vdd *);

static void	isofile_init_entry_list(struct iso9660 *);
static void	isofile_add_entry(struct iso9660 *, struct isofile *);
static void	isofile_free_all_entries(struct iso9660 *);
static struct isofile * isofile_new(struct archive_entry *);
static void	isofile_free(struct isofile *);
static void	isofile_gen_utility_names(struct isofile *);
static void	get_parent_and_base(struct archive_string *,
		    struct archive_string *, const char *);
static int	isofile_register_hardlink(struct archive_write *,
		    struct isofile *);
static void	isofile_connect_hardlink_files(struct iso9660 *);
static void	isofile_init_hardlinks(struct iso9660 *);
static void	isofile_free_hardlinks(struct iso9660 *);

static struct isoent *isoent_new(struct isofile *);
static int	isoent_clone_tree(struct archive_write *,
		    struct isoent **, struct isoent *);
static void	_isoent_free(struct isoent *isoent);
static void	isoent_free_all(struct isoent *);
static struct isoent * isoent_create_virtual_dir(struct iso9660 *,
		    const char *);
static inline void isoent_add_child_head(struct isoent *, struct isoent *);
static inline void isoent_add_child_tail(struct isoent *, struct isoent *);
static void	isoent_trim_root_directory(struct iso9660 *);
static void	isoent_setup_directory_location(struct iso9660 *,
		    int, struct vdd *);
static void	isoent_setup_file_location(struct iso9660 *, int);
static int	get_path_component(char *, int, const char *);
static struct isoent *isoent_tree_add_child(struct archive_write *,
		    struct isoent *, struct isoent *);
static struct isoent *isoent_tree(struct archive_write *, struct isoent *);
static void	idr_relaxed_filenames(char *);
static void	idr_allow_sharp_tilde(char *);
static void	idr_init(struct iso9660 *, struct vdd *, struct idr *);
static void	idr_cleanup(struct idr *);
static inline void idr_llist_insert(struct llist *, struct wlist *);
static inline void idr_llist_remove(struct llist *, struct wlist *);
static int	idr_ensure_poolsize(struct archive_write *, struct idr *,
		    int);
static int	idr_start(struct archive_write *, struct idr *,
		    int, int);
static struct wlist * idr_flist_add(struct idr *, struct flist *,
		    struct isoent *, int, int);
static void	idr_register(struct idr *, struct isoent *, int,
		    int, int);
static struct llist * idr_extend_identifier(struct llist *,
		    struct wlist *, int, int);
static int	idr_has_same_identifier(struct llist *, struct wlist *);
static void	idr_resolve(struct idr *, int, int,
		    void (*)(unsigned char *, int));
static void	idr_set_num(unsigned char *, int);
static void	idr_set_num_beutf16(unsigned char *, int);
static int	isoent_gen_iso9660_identifier(struct archive_write *,
		    struct isoent *, struct idr *);
static int	isoent_gen_joliet_identifier(struct archive_write *,
		    struct isoent *, struct idr *);
static int	_compare_iso9660_identifier(const void *, const void *);
static int	_compare_joliet_identifier(const void *, const void *);
static inline void path_table_add_entry(struct path_table *, struct isoent *);
static inline struct isoent * path_table_last_entry(struct path_table *);
static int	isoent_make_path_table(struct archive_write *);
static int	isoent_create_boot_catalog(struct archive_write *,
		    struct isoent *);
static struct	isoent *isoent_find_entry(struct isoent *,
		    const char *);
static size_t	fd_boot_image_size(int);
static void	make_boot_catalog(struct iso9660 *, unsigned char *);
static int	setup_boot_information(struct archive_write *);

static void	utf16stobeutf16s(unsigned char *, size_t);
static size_t	mbstoutf16s(unsigned char *, size_t, const char *, int);
static size_t	mblen_of_utf16(const unsigned char *, size_t);

static int	zisofs_init(struct archive_write *, struct isofile *);
static void	zisofs_detect_magic(struct archive_write *,
		    const void *, size_t);
#ifdef HAVE_ZLIB_H
static void	zisofs_cancel(struct iso9660 *, struct isofile *);
static int	zisofs_write_to_temp(struct archive_write *,
		    const void *, size_t);
static int	zisofs_finish_entry(struct archive_write *);
static int	zisofs_add_nozf(struct archive_write *, const char *);
static int	zisofs_fix_nozf(struct archive_write *);
#endif

int
archive_write_set_format_iso9660(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	struct iso9660 *iso9660;

	archive_check_magic(_a, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_set_format_iso9660");

	/* If another format was already registered, unregister it. */
	if (a->format_destroy != NULL)
		(a->format_destroy)(a);

	iso9660 = malloc(sizeof(*iso9660));
	if (iso9660 == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate iso9660 data");
		return (ARCHIVE_FATAL);
	}
	iso9660->birth_time = 0;
	iso9660->temp_fd = -1;
	iso9660->cur_file = NULL;
	iso9660->cur_dirent = NULL;
	iso9660->primary.max_depth = 0;
	iso9660->primary.vdd_type = VDD_PRIMARY;
	iso9660->primary.pathtbl = NULL;
	iso9660->joliet.rootent = NULL;
	iso9660->joliet.max_depth = 0;
	iso9660->joliet.vdd_type = VDD_JOLIET;
	iso9660->joliet.pathtbl = NULL;
	isofile_init_entry_list(iso9660);
	isofile_init_hardlinks(iso9660);
	iso9660->directories_too_deep = NULL;
	iso9660->dircnt_max = 1;
	iso9660->wbuff_remaining = 0;

	/*
	 * Init Identifiers used for PVD and SVD.
	 */
	archive_string_init(&(iso9660->volume_identifier));
	archive_strcpy(&(iso9660->volume_identifier), "CDROM");
	archive_string_init(&(iso9660->publisher_identifier));
	archive_string_init(&(iso9660->data_preparer_identifier));
	archive_string_init(&(iso9660->application_identifier));
	archive_strcpy(&(iso9660->application_identifier),
	    archive_version_string());
	archive_string_init(&(iso9660->copyright_file_identifier));
	archive_string_init(&(iso9660->abstract_file_identifier));
	archive_string_init(&(iso9660->bibliographic_file_identifier));

	/*
	 * Init El Torito bootable CD variables.
	 */
	archive_string_init(&(iso9660->el_torito.catalog_filename));
	iso9660->el_torito.catalog = NULL;
	/* Set default file name of boot catalog  */
	archive_strcpy(&(iso9660->el_torito.catalog_filename),
	    "boot.catalog");
	archive_string_init(&(iso9660->el_torito.boot_filename));
	iso9660->el_torito.boot = NULL;
	iso9660->el_torito.platform_id = BOOT_PLATFORM_X86;
	archive_string_init(&(iso9660->el_torito.id));
	iso9660->el_torito.boot_load_seg = 0;
	iso9660->el_torito.boot_load_size = BOOT_LOAD_SIZE;

	/*
	 * Init zisofs variables.
	 */
#ifdef HAVE_ZLIB_H
	iso9660->zisofs.temp_fd = -1;
	iso9660->zisofs.block_pointers = NULL;
	iso9660->zisofs.block_pointers_allocated = 0;
	iso9660->zisofs.stream_valid = 0;
	iso9660->zisofs.compression_level = 9;
	iso9660->zisofs.nozf_list = NULL;
	iso9660->zisofs.nozf_cnt = 0;
	memset(&(iso9660->zisofs.stream), 0,
	    sizeof(iso9660->zisofs.stream));
#endif

	/*
	 * Set default value of iso9660 options.
	 */
	iso9660->opt.abstract_file = OPT_ABSTRACT_FILE_DEFAULT;
	iso9660->opt.application_id = OPT_APPLICATION_ID_DEFAULT;
	iso9660->opt.allow_ldots = OPT_ALLOW_LDOTS_DEFAULT;
	iso9660->opt.allow_lowercase = OPT_ALLOW_LOWERCASE_DEFAULT;
	iso9660->opt.allow_multidot = OPT_ALLOW_MULTIDOT_DEFAULT;
	iso9660->opt.allow_period = OPT_ALLOW_PERIOD_DEFAULT;
	iso9660->opt.allow_pvd_lowercase = OPT_ALLOW_PVD_LOWERCASE_DEFAULT;
	iso9660->opt.allow_sharp_tilde = OPT_ALLOW_SHARP_TILDE_DEFAULT;
	iso9660->opt.allow_vernum = OPT_ALLOW_VERNUM_DEFAULT;
	iso9660->opt.biblio_file = OPT_BIBLIO_FILE_DEFAULT;
	iso9660->opt.boot = OPT_BOOT_DEFAULT;
	iso9660->opt.boot_catalog = OPT_BOOT_CATALOG_DEFAULT;
	iso9660->opt.boot_info_table = OPT_BOOT_INFO_TABLE_DEFAULT;
	iso9660->opt.boot_load_seg = OPT_BOOT_LOAD_SEG_DEFAULT;
	iso9660->opt.boot_load_size = OPT_BOOT_LOAD_SIZE_DEFAULT;
	iso9660->opt.boot_type = OPT_BOOT_TYPE_DEFAULT;
	iso9660->opt.compression_level = OPT_COMPRESSION_LEVEL_DEFAULT;
	iso9660->opt.copyright_file = OPT_COPYRIGHT_FILE_DEFAULT;
	iso9660->opt.gid = OPT_GID_DEFAULT;
	iso9660->opt.iso_level = OPT_ISO_LEVEL_DEFAULT;
	iso9660->opt.joliet = OPT_JOLIET_DEFAULT;
	iso9660->opt.limit_depth = OPT_LIMIT_DEPTH_DEFAULT;
	iso9660->opt.limit_dirs = OPT_LIMIT_DIRS_DEFAULT;
	iso9660->opt.pad = OPT_PAD_DEFAULT;
	iso9660->opt.publisher = OPT_PUBLISHER_DEFAULT;
	iso9660->opt.relaxed_filenames = OPT_RELAXED_FILENAMES_DEFAULT;
	iso9660->opt.rr = OPT_RR_DEFAULT;
	iso9660->opt.uid = OPT_UID_DEFAULT;
	iso9660->opt.volume_id = OPT_VOLUME_ID_DEFAULT;
	iso9660->opt.zisofs = OPT_ZISOFS_DEFAULT;
	iso9660->opt.zisofs_exclude = OPT_ZISOFS_EXCLUDE_DEFAULT;

	/* Create the root directory. */
	iso9660->primary.rootent =
	    isoent_create_virtual_dir(iso9660, "");
	if (iso9660->primary.rootent == NULL) {
		free(iso9660);
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	iso9660->primary.rootent->parent = iso9660->primary.rootent;

	a->format_data = iso9660;

	a->pad_uncompressed = 0;
	a->format_name = "iso9660";
	a->format_options = iso9660_options;
	a->format_write_header = iso9660_write_header;
	a->format_write_data = iso9660_write_data;
	a->format_finish_entry = iso9660_finish_entry;
	a->format_finish = iso9660_finish;
	a->format_destroy = iso9660_destroy;
	a->archive.archive_format = ARCHIVE_FORMAT_ISO9660;
	a->archive.archive_format_name = "ISO9660";

	return (ARCHIVE_OK);
}

static int
get_str_opt(struct archive_write *a, struct archive_string *s,
    size_t maxsize, const char *key, const char *value)
{

	if (strlen(value) > maxsize) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Value is longer than %zu characters "
		    "for option ``%s''", maxsize, key);
		return (ARCHIVE_FATAL);
	}
	archive_strcpy(s, value);
	return (ARCHIVE_OK);
}

static int
get_num_opt(struct archive_write *a, int *num, int high, int low,
    const char *key, const char *value)
{
	const char *p = value;
	int data = 0;
	int neg = 0;

	if (p == NULL) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Invalid value(empty) for option ``%s''", key);
		return (ARCHIVE_FATAL);
	}
	if (*p == '-') {
		neg = 1;
		p++;
	}
	while (*p) {
		if (*p >= '0' && *p <= '9')
			data = data * 10 + *p - '0';
		else {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Invalid value for option ``%s''", key);
			return (ARCHIVE_FATAL);
		}
		if (data > high) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Invalid value(over %d) for "
			    "option ``%s''", high, key);
			return (ARCHIVE_FATAL);
		}
		if (data < low) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Invalid value(under %d) for "
			    "option ``%s''", low, key);
			return (ARCHIVE_FATAL);
		}
		p++;
	}
	if (neg)
		data *= -1;
	*num = data;

	return (ARCHIVE_OK);
}

static int
iso9660_options(struct archive_write *a, const char *key, const char *value)
{
	struct iso9660 *iso9660 = a->format_data;
	const char *p;
	int num, r;

	switch (key[0]) {
	case 'a':
		if (strcmp(key, "abstract-file") == 0) {
			r = get_str_opt(a,
			    &(iso9660->abstract_file_identifier),
			    ABSTRACT_FILE_SIZE, key, value);
			iso9660->opt.abstract_file = r == ARCHIVE_OK;
			return (r);
		}
		if (strcmp(key, "application-id") == 0) {
			r = get_str_opt(a,
			    &(iso9660->application_identifier),
			    APPLICATION_IDENTIFIER_SIZE, key, value);
			iso9660->opt.application_id = r == ARCHIVE_OK;
			return (r);
		}
		if (strcmp(key, "allow-dot-first") == 0) {
			iso9660->opt.allow_ldots = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "allow-lowercase") == 0) {
			iso9660->opt.allow_lowercase = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "allow-multidot") == 0) {
			iso9660->opt.allow_multidot = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "allow-period") == 0) {
			iso9660->opt.allow_period = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "allow-pvd-lowercase") == 0) {
			iso9660->opt.allow_pvd_lowercase = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "allow-sharp-tilde") == 0) {
			iso9660->opt.allow_sharp_tilde = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "allow-vernum") == 0) {
			iso9660->opt.allow_vernum = value != NULL;
			return (ARCHIVE_OK);
		}
		break;
	case 'b':
		if (strcmp(key, "biblio-file") == 0) {
			r = get_str_opt(a,
			    &(iso9660->bibliographic_file_identifier),
			    BIBLIO_FILE_SIZE, key, value);
			iso9660->opt.biblio_file = r == ARCHIVE_OK;
			return (r);
		}
		if (strcmp(key, "boot") == 0) {
			if (value == NULL)
				iso9660->opt.boot = 0;
			else {
				iso9660->opt.boot = 1;
				archive_strcpy(
				    &(iso9660->el_torito.boot_filename),
				    value);
#ifdef HAVE_ZLIB_H
				r = zisofs_add_nozf(a,
				    iso9660->el_torito.boot_filename.s);
				if (r != ARCHIVE_OK)
					return (r);
#endif
			}
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "boot-catalog") == 0) {
			r = get_str_opt(a,
			    &(iso9660->el_torito.catalog_filename),
			    1024, key, value);
			iso9660->opt.boot_catalog = r == ARCHIVE_OK;
			return (r);
		}
		if (strcmp(key, "boot-info-table") == 0) {
			iso9660->opt.boot_info_table = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "boot-load-seg") == 0) {
			uint32_t seg;

			iso9660->opt.boot_load_seg = 0;
			if (value == NULL)
				goto invalid_value;
			seg = 0;
			p = value;
			if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
				p += 2;
			while (*p) {
				if (*p >= 'A' && *p <= 'f')
					seg = (seg << 4) + *p - 'A' + 0x0a;
				else if (*p >= 'a' && *p <= 'f')
					seg = (seg << 4) + *p - 'a' + 0x0a;
				else if (*p >= '0' && *p <= '9')
					seg = (seg << 4) + *p - '0';
				else
					goto invalid_value;
				if (seg > 0xffff) {
					archive_set_error(&a->archive,
					    ARCHIVE_ERRNO_MISC,
					    "Invalid value(over 0xffff) for "
					    "option ``%s''", key);
					return (ARCHIVE_FATAL);
				}
				p++;
			}
			iso9660->el_torito.boot_load_seg = (uint16_t)seg;
			iso9660->opt.boot_load_seg = 1;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "boot-load-size") == 0) {
			r = get_num_opt(a, &num, 0xffff, 1, key, value);
			iso9660->opt.boot_load_size = r == ARCHIVE_OK;
			if (r != ARCHIVE_OK)
				return (ARCHIVE_FATAL);
			iso9660->el_torito.boot_load_size = (uint16_t)num;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "boot-type") == 0) {
			if (value == NULL)
				goto invalid_value;
			if (strcmp(value, "no-emulation") == 0)
				iso9660->opt.boot_type = OPT_BOOT_TYPE_NO_EMU;
			else if (strcmp(value, "fd") == 0)
				iso9660->opt.boot_type = OPT_BOOT_TYPE_FD;
			else if (strcmp(value, "hard-disk") == 0)
				iso9660->opt.boot_type = OPT_BOOT_TYPE_HARD_DISK;
			else
				goto invalid_value;
			return (ARCHIVE_OK);
		}
		break;
	case 'c':
		if (strcmp(key, "compression-level") == 0) {
#ifdef HAVE_ZLIB_H
			if (value == NULL ||
			    !(value[0] >= '0' && value[0] <= '9') ||
			    value[1] != '\0')
				goto invalid_value;
                	iso9660->zisofs.compression_level = value[0] - '0';
			iso9660->opt.compression_level = 1;
                	return (ARCHIVE_OK);
#else
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_MISC,
			    "Option ``%s'' "
			    "is not supported on this platform.", key);
			return (ARCHIVE_FATAL);
#endif
		}
		if (strcmp(key, "copyright-file") == 0) {
			r = get_str_opt(a,
			    &(iso9660->copyright_file_identifier),
			    COPYRIGHT_FILE_SIZE, key, value);
			iso9660->opt.copyright_file = r == ARCHIVE_OK;
			return (r);
		}
#ifdef DEBUG
		/* Specifies Volume creation date and time;
		 * year(4),month(2),day(2),hour(2),minute(2),second(2).
		 * e.g. "20090929033757"
		 */
		if (strcmp(key, "creation") == 0) {
			struct tm tm;
			char buf[5];

			p = value;
			if (p == NULL || strlen(p) < 14)
				goto invalid_value;
			memset(&tm, 0, sizeof(tm));
			memcpy(buf, p, 4); buf[4] = '\0'; p += 4;
			tm.tm_year = strtol(buf, NULL, 10) - 1900;
			memcpy(buf, p, 2); buf[2] = '\0'; p += 2;
			tm.tm_mon = strtol(buf, NULL, 10) - 1;
			memcpy(buf, p, 2); buf[2] = '\0'; p += 2;
			tm.tm_mday = strtol(buf, NULL, 10);
			memcpy(buf, p, 2); buf[2] = '\0'; p += 2;
			tm.tm_hour = strtol(buf, NULL, 10);
			memcpy(buf, p, 2); buf[2] = '\0'; p += 2;
			tm.tm_min = strtol(buf, NULL, 10);
			memcpy(buf, p, 2); buf[2] = '\0';
			tm.tm_sec = strtol(buf, NULL, 10);
			iso9660->birth_time = mktime(&tm);
			return (ARCHIVE_OK);
		}
#endif
		break;
	case 'g':
		if (strcmp(key, "gid") == 0) {
			r = get_num_opt(a, &num, INT_MAX, 0, key, value);
			if (r != ARCHIVE_OK)
				return (ARCHIVE_FATAL);
			iso9660->gid = num;
			iso9660->opt.gid = 1;
			return (ARCHIVE_OK);
		}
		break;
	case 'i':
		if (strcmp(key, "iso-level") == 0) {
			if (value != NULL && value[1] == '\0' &&
			    (value[0] >= '1' && value[0] <= '4')) {
				iso9660->opt.iso_level = value[0]-'0';
				return (ARCHIVE_OK);
			}
			goto invalid_value;
		}
		break;
	case 'j':
		if (strcmp(key, "joliet") == 0) {
			if (value == NULL)
				iso9660->opt.joliet = OPT_JOLIET_DISABLE;
			else if (strcmp(value, "1") == 0)
				iso9660->opt.joliet = OPT_JOLIET_ENABLE;
			else if (strcmp(value, "long") == 0)
				iso9660->opt.joliet = OPT_JOLIET_LONGNAME;
			else
				goto invalid_value;
			return (ARCHIVE_OK);
		}
		break;
	case 'l':
		if (strcmp(key, "limit-depth") == 0) {
			iso9660->opt.limit_depth = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "limit-dirs") == 0) {
			iso9660->opt.limit_dirs = value != NULL;
			return (ARCHIVE_OK);
		}
		break;
	case 'p':
		if (strcmp(key, "pad") == 0) {
			iso9660->opt.pad = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "publisher") == 0) {
			r = get_str_opt(a,
			    &(iso9660->publisher_identifier),
			    PUBLISHER_IDENTIFIER_SIZE, key, value);
			iso9660->opt.publisher = r == ARCHIVE_OK;
			return (r);
		}
		break;
	case 'r':
		if (strcmp(key, "relaxed-filenames") == 0) {
			iso9660->opt.relaxed_filenames = value != NULL;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "rockridge") == 0 ||
		    strcmp(key, "Rockridge") == 0) {
			if (value == NULL)
				iso9660->opt.rr = OPT_RR_DISABLED;
			else if (strcmp(value, "1") == 0)
				iso9660->opt.rr = OPT_RR_USEFUL;
			else if (strcmp(value, "strict") == 0)
				iso9660->opt.rr = OPT_RR_STRICT;
			else if (strcmp(value, "useful") == 0)
				iso9660->opt.rr = OPT_RR_USEFUL;
			else
				goto invalid_value;
			return (ARCHIVE_OK);
		}
		break;
	case 'u':
		if (strcmp(key, "uid") == 0) {
			r = get_num_opt(a, &num, INT_MAX, 0, key, value);
			if (r != ARCHIVE_OK)
				return (ARCHIVE_FATAL);
			iso9660->uid = num;
			iso9660->opt.uid = 1;
			return (ARCHIVE_OK);
		}
		break;
	case 'v':
		if (strcmp(key, "volume-id") == 0) {
			r = get_str_opt(a, &(iso9660->volume_identifier),
			    VOLUME_IDENTIFIER_SIZE, key, value);
			iso9660->opt.volume_id = r == ARCHIVE_OK;
			return (r);
		}
		break;
	case 'z':
		if (strcmp(key, "zisofs") == 0) {
			if (value == NULL)
				iso9660->opt.zisofs = OPT_ZISOFS_DISABLED;
			else if (strcmp(value, "1") == 0)
				iso9660->opt.zisofs = OPT_ZISOFS_DIRECT;
			else if (strcmp(value, "indirect") == 0)
				iso9660->opt.zisofs = OPT_ZISOFS_INDIRECT;
#ifdef HAVE_ZLIB_H
			else if (strcmp(value, "direct") == 0)
				iso9660->opt.zisofs = OPT_ZISOFS_DIRECT;
#else
			else if (strcmp(value, "direct") == 0) {
				archive_set_error(&a->archive,
				    ARCHIVE_ERRNO_MISC,
				    "``zisofs=%s'' "
				    "is not supported on this platform.",
				    value);
				return (ARCHIVE_FATAL);
			}
#endif
			else
				goto invalid_value;
			return (ARCHIVE_OK);
		}
		if (strcmp(key, "zisofs-exclude") == 0) {
#ifdef HAVE_ZLIB_H
			const char *xp;
			struct archive_string str;

			if (value == NULL)
				goto invalid_value;
			xp = value;
			archive_string_init(&str);
			do {
				p = strchr(xp, ':');
				if (p != NULL) {
					archive_strncpy(&str, xp, p - xp);
					xp = p + 1;
				} else {
					archive_strcpy(&str, xp);
					xp = NULL;
				}
				r = zisofs_add_nozf(a, str.s);
				if (r != ARCHIVE_OK) {
					archive_string_free(&str);
					return (r);
				}
			} while (xp != NULL);
			archive_string_free(&str);
			iso9660->opt.zisofs_exclude = 1;
			return (ARCHIVE_OK);
#else
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_MISC,
			    "Option ``%s'' "
			    "is not supported on this platform.", key);
			return (ARCHIVE_FATAL);
#endif
		}
		break;
	}

	return (ARCHIVE_WARN);
invalid_value:
	archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
	    "Invalid value for option ``%s''", key);
	return (ARCHIVE_FATAL);
}

static int
iso9660_write_header(struct archive_write *a, struct archive_entry *entry)
{
	struct iso9660 *iso9660;
	struct isofile *file;
	struct isoent *isoent;
	int r;

	iso9660 = a->format_data;

	iso9660->cur_file = NULL;
	iso9660->bytes_remaining = 0;
	iso9660->need_multi_extent = 0;
	if (archive_entry_filetype(entry) == AE_IFLNK
	    && iso9660->opt.rr == OPT_RR_DISABLED) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Ignore symlink file.");
		iso9660->cur_file = NULL;
		return (ARCHIVE_WARN);
	}
	if (archive_entry_filetype(entry) == AE_IFREG &&
	    archive_entry_size(entry) >= MULTI_EXTENT_SIZE) {
		if (iso9660->opt.iso_level < 3) {
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_MISC,
			    "Ignore over %lld bytes file. "
			    "This file too large.",
			    MULTI_EXTENT_SIZE);
				iso9660->cur_file = NULL;
			return (ARCHIVE_WARN);
		}
		iso9660->need_multi_extent = 1;
	}

	file = isofile_new(entry);
	if (file == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate data");
		return (ARCHIVE_FATAL);
	}
	isofile_gen_utility_names(file);
	isofile_add_entry(iso9660, file);
	isoent = isoent_new(file);
	if (isoent == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate data");
		return (ARCHIVE_FATAL);
	}
	if (isoent->file->dircnt > iso9660->dircnt_max)
		iso9660->dircnt_max = isoent->file->dircnt;

	/* Add the current file into tree */
	isoent = isoent_tree(a, isoent);
	if (isoent == NULL)
		return (ARCHIVE_FATAL);

	/* If there is the same file in tree and
	 * the current file is older than the file in tree.
	 * So we don't need the current file data anymore. */
	if (isoent->file != file)
		return (ARCHIVE_OK);

	/* Non regular files contents are unneeded to be saved to
	 * temporary files. */
	if (archive_entry_filetype(file->entry) != AE_IFREG)
		return (ARCHIVE_OK);

	/*
	 * Set the current file to cur_file to read its contents.
	 */
	iso9660->cur_file = file;

	if (archive_entry_nlink(file->entry) > 1) {
		r = isofile_register_hardlink(a, file);
		if (r != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
		if (archive_entry_hardlink(file->entry) != NULL)
			archive_entry_unset_size(file->entry);
	}

	/*
	 * Prepare to save the contents of the file.
	 */
	if (iso9660->temp_fd < 0) {
		iso9660->temp_fd = __archive_mktemp(NULL);
		if (iso9660->temp_fd < 0) {
			archive_set_error(&a->archive, errno,
			    "Couldn't create temporary file");
			return (ARCHIVE_FATAL);
		}
	}

	/* Save a offset of current file in temporary file. */
	file->temp_fd = iso9660->temp_fd;
	file->content.offset_of_temp = lseek(iso9660->temp_fd, 0, SEEK_CUR);
	file->cur_content = &(file->content);
	r = zisofs_init(a, file);
	iso9660->bytes_remaining =  archive_entry_size(file->entry);

	return (r);
}

static int
write_to_temp(struct archive_write *a, int fd, const void *buff, size_t s)
{
	ssize_t written;
	const unsigned char *b;

	b = (const unsigned char *)buff;
	while (s) {
		written = write(fd, b, s);
		if (written < 0) {
			archive_set_error(&a->archive, errno,
			    "Can't write to temporary file");
			return (ARCHIVE_FATAL);
		}
		s -= written;
		b += written;
	}
	return (ARCHIVE_OK);
}

static ssize_t
write_iso9660_data(struct archive_write *a, const void *buff, size_t s)
{
	struct iso9660 *iso9660 = a->format_data;
	size_t ws;

	if (iso9660->temp_fd < 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Couldn't create temporary file");
		return (ARCHIVE_FATAL);
	}

	ws = s;
	if (iso9660->need_multi_extent &&
	    (iso9660->cur_file->cur_content->size + ws) >=
	      (MULTI_EXTENT_SIZE - LOGICAL_BLOCK_SIZE)) {
		struct content *con;
		size_t ts;

		ts = MULTI_EXTENT_SIZE - LOGICAL_BLOCK_SIZE -
		    iso9660->cur_file->cur_content->size;
		if (write_to_temp(a, iso9660->temp_fd, buff, ts)
		    != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
		iso9660->cur_file->cur_content->size += ts;

		if (iso9660->zisofs.detect_magic)
			zisofs_detect_magic(a, buff, ts);
#ifdef HAVE_ZLIB_H
		if (iso9660->zisofs.making &&
		    zisofs_write_to_temp(a, buff, ts) != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
#endif
		/*
		 * Make next extent.
		 */
		ws -= ts;
		buff = (const void *)(((const unsigned char *)buff) + ts);
		/* Make a content for next extent. */
		con = calloc(1, sizeof(*con));
		if (con == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate content data");
			return (ARCHIVE_FATAL);
		}
		con->offset_of_temp =
		    lseek(iso9660->cur_file->temp_fd, 0, SEEK_CUR);
		iso9660->cur_file->cur_content->next = con;
		iso9660->cur_file->cur_content = con;
	}

	if (write_to_temp(a, iso9660->temp_fd, buff, ws) != ARCHIVE_OK)
		return (ARCHIVE_FATAL);
	iso9660->cur_file->cur_content->size += ws;

	if (iso9660->zisofs.detect_magic)
		zisofs_detect_magic(a, buff, ws);
#ifdef HAVE_ZLIB_H
	if (iso9660->zisofs.making &&
	    zisofs_write_to_temp(a, buff, ws) != ARCHIVE_OK)
		return (ARCHIVE_FATAL);
#endif
	return (s);
}

static ssize_t
iso9660_write_data(struct archive_write *a, const void *buff, size_t s)
{
	struct iso9660 *iso9660 = a->format_data;
	ssize_t r;

	if (iso9660->cur_file == NULL)
		return (0);
	if (archive_entry_filetype(iso9660->cur_file->entry) != AE_IFREG)
		return (0);
	if (s > iso9660->bytes_remaining)
		s = iso9660->bytes_remaining;
	if (s == 0)
		return (0);

	r = write_iso9660_data(a, buff, s);
	if (r > 0)
		iso9660->bytes_remaining -= r;
	return (r);
}

static int
iso9660_finish_entry(struct archive_write *a)
{
	struct iso9660 *iso9660 = a->format_data;

	if (iso9660->cur_file == NULL)
		return (ARCHIVE_OK);
	if (archive_entry_filetype(iso9660->cur_file->entry) != AE_IFREG)
		return (ARCHIVE_OK);

	/* If there are unwritten data, write null data instead. */
	while (iso9660->bytes_remaining > 0) {
		size_t s;

		s = (iso9660->bytes_remaining > a->null_length)?
		    a->null_length: (size_t)iso9660->bytes_remaining;
		if (write_iso9660_data(a, a->nulls, s) < 0)
			return (ARCHIVE_FATAL);
		iso9660->bytes_remaining -= s;
	}

#ifdef HAVE_ZLIB_H
	if (iso9660->zisofs.making &&
	    zisofs_finish_entry(a) != ARCHIVE_OK)
		return (ARCHIVE_FATAL);
#endif
	return (ARCHIVE_OK);
}

static int
iso9660_finish(struct archive_write *a)
{
	struct iso9660 *iso9660;
	int ret, blocks;

	iso9660 = a->format_data;

	/*
	 * Preparations...
	 */
#ifdef DEBUG
	if (iso9660->birth_time == 0)
#endif
		time(&(iso9660->birth_time));
	isoent_trim_root_directory(iso9660);
	if (iso9660->opt.boot) {
		ret = isoent_create_boot_catalog(a,
		    iso9660->primary.rootent);
		if (ret < 0)
			return (ret);
	}
#ifdef HAVE_ZLIB_H
	ret = zisofs_fix_nozf(a);
	if (ret < 0)
		return (ret);
#endif
	if (iso9660->opt.joliet) {
		/* Make a new tree for joliet. */
		ret = isoent_clone_tree(a, &(iso9660->joliet.rootent),
		    iso9660->primary.rootent);
		if (ret < 0)
			return (ret);
	}

	/*
	 * Make Path Tables.
	 */
	ret = isoent_make_path_table(a);
	if (ret < 0)
		return (ret);

	/*
	 * Calculate a total volume size and setup all locations of
	 * contents of a iso9660 image.
	 */
	blocks = SYSTEM_AREA_BLOCK
		+ PRIMARY_VOLUME_DESCRIPTOR_BLOCK
		+ VOLUME_DESCRIPTOR_SET_TERMINATOR_BLOCK
		+ NON_ISO_FILE_SYSTEM_INFORMATION_BLOCK;
	if (iso9660->opt.boot)
		blocks += BOOT_RECORD_DESCRIPTOR_BLOCK;
	if (iso9660->opt.joliet)
		blocks += SUPPLEMENTARY_VOLUME_DESCRIPTOR_BLOCK;
	if (iso9660->opt.iso_level == 4)
		blocks += SUPPLEMENTARY_VOLUME_DESCRIPTOR_BLOCK;

	/* Setup locations of Path Table. */
	iso9660->primary.location_type_L_path_table = blocks;
	blocks += iso9660->primary.path_table_block;
	iso9660->primary.location_type_M_path_table = blocks;
	blocks += iso9660->primary.path_table_block;
	if (iso9660->opt.joliet) {
		iso9660->joliet.location_type_L_path_table = blocks;
		blocks += iso9660->joliet.path_table_block;
		iso9660->joliet.location_type_M_path_table = blocks;
		blocks += iso9660->joliet.path_table_block;
	}

	/* Setup locations of directories. */
	isoent_setup_directory_location(iso9660, blocks,
	    &(iso9660->primary));
	blocks += iso9660->primary.total_dir_block;
	if (iso9660->opt.joliet) {
		isoent_setup_directory_location(iso9660, blocks,
		    &(iso9660->joliet));
		blocks += iso9660->joliet.total_dir_block;
	}

	if (iso9660->opt.rr) {
		iso9660->location_rrip_er = blocks;
		blocks += RRIP_ER_BLOCK;
	}

	/* Setup locations of all file contents. */
 	isoent_setup_file_location(iso9660, blocks);
	blocks += iso9660->total_file_block;
	if (iso9660->opt.boot && iso9660->opt.boot_info_table) {
		ret = setup_boot_information(a);
		if (ret < 0)
			return (ret);
	}

	/* Now we have a total volume size. */
	iso9660->volume_space_size = blocks;
	if (iso9660->opt.pad)
		iso9660->volume_space_size += PADDING_BLOCK;
	iso9660->volume_sequence_number = 1;


	/*
	 * Write ISO 9660 image.
	 */
	/* Start using wbuff as file buffer. */ 
	iso9660->wbuff_remaining = sizeof(iso9660->wbuff);

	/* Write The System Area */
	ret = write_null(a, SYSTEM_AREA_BLOCK * LOGICAL_BLOCK_SIZE);
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Write Primary Volume Descriptor */
	ret = write_VD(a, &(iso9660->primary));
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	if (iso9660->opt.boot) {
		/* Write Boot Record Volume Descriptor */
		ret = write_VD_boot_record(a);
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}

	if (iso9660->opt.iso_level == 4) {
		/* Write Enhanced Volume Descriptor */
		iso9660->primary.vdd_type = VDD_ENHANCED;
		ret = write_VD(a, &(iso9660->primary));
		iso9660->primary.vdd_type = VDD_PRIMARY;
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}

	if (iso9660->opt.joliet) {
		ret = write_VD(a, &(iso9660->joliet));
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}

	/* Write Volume Descriptor Set Terminator */
	ret = write_VD_terminator(a);
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Write Non-ISO File System Information */
	ret = write_information_block(a);
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Write Type L Path Table */
	ret = write_path_table(a, 0, &(iso9660->primary));
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Write Type M Path Table */
	ret = write_path_table(a, 1, &(iso9660->primary));
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	if (iso9660->opt.joliet) {
		/* Write Type L Path Table */
		ret = write_path_table(a, 0, &(iso9660->joliet));
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);

		/* Write Type M Path Table */
		ret = write_path_table(a, 1, &(iso9660->joliet));
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}

	/* Write Directory Descriptors */
	ret = write_directory_descriptors(a, &(iso9660->primary));
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	if (iso9660->opt.joliet) {
		ret = write_directory_descriptors(a, &(iso9660->joliet));
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}

	if (iso9660->opt.rr) {
		/* Write Rockridge ER(Extensions Reference) */
		ret = write_rr_ER(a);
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}

	/* Write File Descriptors */
	ret = write_file_descriptors(a);
	if (ret != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Write Padding  */
	if (iso9660->opt.pad) {
		ret = write_null(a, PADDING_BLOCK * LOGICAL_BLOCK_SIZE);
		if (ret != ARCHIVE_OK)
			return (ARCHIVE_FATAL);
	}
	
	if (iso9660->directories_too_deep != NULL) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "%s: Directories too deep.",
		    archive_entry_pathname(
			iso9660->directories_too_deep->file->entry));
		return (ARCHIVE_WARN);
	}

	/* Write remaining data out. */
	ret = wb_write_out(a);

	return (ret);
}

static int
iso9660_destroy(struct archive_write *a)
{
	struct iso9660 *iso9660;
	int i, ret;

	iso9660 = a->format_data;
	ret = ARCHIVE_OK;

	/* Close temporary file. */
	if (iso9660->temp_fd >= 0)
		close(iso9660->temp_fd);

#ifdef HAVE_ZLIB_H
	if (iso9660->zisofs.temp_fd >= 0)
		close(iso9660->zisofs.temp_fd);
	free(iso9660->zisofs.block_pointers);
	if (iso9660->zisofs.stream_valid) {
                switch (deflateEnd(&(iso9660->zisofs.stream))) {
                case Z_OK:
                        break;
                default:
                        archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                            "Failed to clean up compressor");
                        ret = ARCHIVE_FATAL;
                }
	}
	free(iso9660->zisofs.nozf_list);
#endif
		
	/* Remove directory entries in tree which includes file entries. */
	isoent_free_all(iso9660->primary.rootent);
	for (i = 0; i < iso9660->primary.max_depth; i++)
		free(iso9660->primary.pathtbl[i].sorted);
	free(iso9660->primary.pathtbl);

	if (iso9660->opt.joliet) {
		isoent_free_all(iso9660->joliet.rootent);
		for (i = 0; i < iso9660->joliet.max_depth; i++)
			free(iso9660->joliet.pathtbl[i].sorted);
		free(iso9660->joliet.pathtbl);
	}

	/* Remove isofile entries. */
	isofile_free_all_entries(iso9660);
	isofile_free_hardlinks(iso9660);

	archive_string_free(&(iso9660->volume_identifier));
	archive_string_free(&(iso9660->publisher_identifier));
	archive_string_free(&(iso9660->data_preparer_identifier));
	archive_string_free(&(iso9660->application_identifier));
	archive_string_free(&(iso9660->copyright_file_identifier));
	archive_string_free(&(iso9660->abstract_file_identifier));
	archive_string_free(&(iso9660->bibliographic_file_identifier));
	archive_string_free(&(iso9660->el_torito.catalog_filename));
	archive_string_free(&(iso9660->el_torito.boot_filename));
	archive_string_free(&(iso9660->el_torito.id));
	
	free(iso9660);
	a->format_data = NULL;

	return (ret);
}

/*
 * Get the System Identifier
 */
static void
get_system_identitier(char *system_id, size_t size)
{
#if defined(HAVE_SYS_UTSNAME_H)
	struct utsname u;

	uname(&u);
	strncpy(system_id, u.sysname, size-1);
	system_id[size-1] = '\0';
#elif defined(_WIN32) && !defined(__CYGWIN__)
	strncpy(system_id, "Windows", size-1);
	system_id[size-1] = '\0';
#else
#error no way to get the system identifier on your platform.
#endif
}

static void
set_str(unsigned char *p, const char *s, size_t l, char f, const char *map)
{
	unsigned char c;

	if (s == NULL)
		s = "";
	while ((c = *s++) != 0 && l > 0) {
		if (c >= 0x80 || map[c] == 0)
		 {
			/* illegal character */
			if (c >= 'a' && c <= 'z') {
				/* convert c from a-z to A-Z */
				c -= 0x20;
			} else
				c = 0x5f;
		}
		*p++ = c;
		l--;
	}
	/* If l isn't zero, fill p buffer by the character
	 * which indicated by f. */
	if (l > 0)
		memset(p , f, l);
}

static inline int
joliet_allowed_char(uint16_t utf16)
{

	if (utf16 <= 0x001F)
		return (0);

	switch (utf16) {
	case 0x002A: /* '*' */
	case 0x002F: /* '/' */
	case 0x003A: /* ':' */
	case 0x003B: /* ';' */
	case 0x003F: /* '?' */
	case 0x005C: /* '\' */
		return (0);/* Not allowed. */
	}
	return (1);
}

static void
set_str_beutf16(unsigned char *p, const char *s, size_t l, uint16_t uf,
    enum vdc vdc)
{
	size_t size, i;
	int onepad;

	if (s == NULL)
		s = "";
	if (l & 0x01) {
		onepad = 1;
		l &= ~1;
	} else
		onepad = 0;
	if (vdc == VDC_UCS2) {
		size = mbstoutf16s(p, l, s, strlen(s));
		utf16stobeutf16s(p, size);
	} else {
		const uint16_t *u16 = (const uint16_t *)s;

		size = 0;
		while (*u16++)
			size += 2;
		if (size > l)
			size = l;
		memcpy(p, s, size);
	}
	for (i = 0; i < size; i += 2, p += 2) {
		uint16_t utf16 = archive_be16dec(p);

		if (!joliet_allowed_char(utf16))
			archive_be16enc(p, 0x005F);/* '_' */
	}
	l -= size;
	while (l > 0) {
		archive_be16enc(p, uf);
		p += 2;
		l -= 2;
	}
	if (onepad)
		*p = 0;
}

static const char a_characters_map[0x80] = {
/*  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F          */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 00-0F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 10-1F */
    1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 20-2F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 30-3F */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 40-4F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,/* 50-5F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 60-6F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 70-7F */
};

static const char a1_characters_map[0x80] = {
/*  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F          */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 00-0F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 10-1F */
    1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 20-2F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 30-3F */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 40-4F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,/* 50-5F */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 60-6F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,/* 70-7F */
};

static const char d_characters_map[0x80] = {
/*  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F          */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 00-0F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 10-1F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 20-2F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,/* 30-3F */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 40-4F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,/* 50-5F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 60-6F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 70-7F */
};

static const char d1_characters_map[0x80] = {
/*  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F          */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 00-0F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 10-1F */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,/* 20-2F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,/* 30-3F */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 40-4F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,/* 50-5F */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,/* 60-6F */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,/* 70-7F */
};

static void
set_str_a_characters_bp(unsigned char *bp, int from, int to,
    const char *s, enum vdc vdc)
{

	switch (vdc) {
	case VDC_STD:
		set_str(bp+from, s, to - from + 1, 0x20,
		    a_characters_map);
		break;
	case VDC_LOWERCASE:
		set_str(bp+from, s, to - from + 1, 0x20,
		    a1_characters_map);
		break;
	case VDC_UCS2:
	case VDC_UCS2_DIRECT:
		set_str_beutf16(bp+from, s, to - from + 1,
		    0x0020, vdc);
		break;
	}
}

static void
set_str_d_characters_bp(unsigned char *bp, int from, int to,
    const char *s, enum  vdc vdc)
{

	switch (vdc) {
	case VDC_STD:
		set_str(bp+from, s, to - from + 1, 0x20,
		    d_characters_map);
		break;
	case VDC_LOWERCASE:
		set_str(bp+from, s, to - from + 1, 0x20,
		    d1_characters_map);
		break;
	case VDC_UCS2:
	case VDC_UCS2_DIRECT:
		set_str_beutf16(bp+from, s, to - from + 1,
		    0x0020, vdc);
		break;
	}
}

static void
set_VD_bp(unsigned char *bp, enum VD_type type, unsigned char ver)
{

	/* Volume Descriptor Type */
	bp[1] = (unsigned char)type;
	/* Standard Identifier */
	memcpy(bp + 2, "CD001", 5);
	/* Volume Descriptor Version */
	bp[7] = ver;
}

static inline void
set_unused_field_bp(unsigned char *bp, int from, int to)
{
	memset(bp + from, 0, to - from + 1);
}

/*
 * 8-bit unsigned numerical values.
 * ISO9660 Standard 7.1.1
 */
static inline void
set_num_711(unsigned char *p, unsigned char value)
{
	*p = value;
}

/*
 * 8-bit signed numerical values.
 * ISO9660 Standard 7.1.2
 */
static inline void
set_num_712(unsigned char *p, char value)
{
	*((char *)p) = value;
}

/*
 * Least significant byte first.
 * ISO9660 Standard 7.2.1
 */
static inline void
set_num_721(unsigned char *p, uint16_t value)
{
	archive_le16enc(p, value);
}

/*
 * Most significant byte first.
 * ISO9660 Standard 7.2.2
 */
static inline void
set_num_722(unsigned char *p, uint16_t value)
{
	archive_be16enc(p, value);
}

/*
 * Both-byte orders.
 * ISO9660 Standard 7.2.3
 */
static void
set_num_723(unsigned char *p, uint16_t value)
{
	archive_le16enc(p, value);
	archive_be16enc(p+2, value);
}

/*
 * Least significant byte first.
 * ISO9660 Standard 7.3.1
 */
static inline void
set_num_731(unsigned char *p, uint32_t value)
{
	archive_le32enc(p, value);
}

/*
 * Most significant byte first.
 * ISO9660 Standard 7.3.2
 */
static inline void
set_num_732(unsigned char *p, uint32_t value)
{
	archive_be32enc(p, value);
}

/*
 * Both-byte orders.
 * ISO9660 Standard 7.3.3
 */
static inline void
set_num_733(unsigned char *p, uint32_t value)
{
	archive_le32enc(p, value);
	archive_be32enc(p+4, value);
}

static void
set_digit(unsigned char *p, size_t s, int value)
{

	while (s--) {
		p[s] = '0' + (value % 10);
		value /= 10;
	}
}

#if defined(HAVE_STRUCT_TM_TM_GMTOFF)
#define get_gmoffset(tm)	((tm)->tm_gmtoff)
#elif defined(HAVE_STRUCT_TM___TM_GMTOFF)
#define get_gmoffset(tm)	((tm)->__tm_gmtoff)
#else
static long
get_gmoffset(struct tm *tm)
{
	long offset;

#if defined(HAVE__GET_TIMEZONE)
	_get_timezone(&offset);
#elif defined(__CYGWIN__) || defined(__MINGW32__)
	offset = _timezone;
#else
	offset = timezone;
#endif
	offset *= -1;
	if (tm->tm_isdst)
		offset += 3600;
	return (offset);
}
#endif

static void
get_tmfromtime(struct tm *tm, time_t *t)
{
#if HAVE_LOCALTIME_R
	tzset();
	localtime_r(t, tm);
#elif HAVE__LOCALTIME64_S
	_localtime64_s(tm, t);
#else
	memcpy(tm, localtime(t), sizeof(*tm));
#endif
}

/*
 * Date and Time Format.
 * ISO9660 Standard 8.4.26.1
 */
static void
set_date_time(unsigned char *p, time_t t)
{
	struct tm tm;

	get_tmfromtime(&tm, &t);
	set_digit(p, 4, tm.tm_year + 1900);
	set_digit(p+4, 2, tm.tm_mon + 1);
	set_digit(p+6, 2, tm.tm_mday);
	set_digit(p+8, 2, tm.tm_hour);
	set_digit(p+10, 2, tm.tm_min);
	set_digit(p+12, 2, tm.tm_sec);
	set_digit(p+14, 2, 0);
	set_num_712(p+16, get_gmoffset(&tm)/(60*15));
}

static void
set_date_time_null(unsigned char *p)
{
	memset(p, '0', 16);
	p[16] = 0;
}

static void
set_time_915(unsigned char *p, time_t t)
{
	struct tm tm;

	get_tmfromtime(&tm, &t);
	set_num_711(p+0, tm.tm_year);
	set_num_711(p+1, tm.tm_mon+1);
	set_num_711(p+2, tm.tm_mday);
	set_num_711(p+3, tm.tm_hour);
	set_num_711(p+4, tm.tm_min);
	set_num_711(p+5, tm.tm_sec);
	set_num_712(p+6, get_gmoffset(&tm)/(60*15));
}


/*
 * Write SUSP "CE" System Use Entry.
 */
static int
set_SUSP_CE(unsigned char *p, int location, int offset, int size)
{
	unsigned char *bp = p -1;
	/*  Extend the System Use Area
	 *   "CE" Format:
	 *               len  ver  
	 *    +----+----+----+----+-----------+-----------+
	 *    | 'C'| 'E'| 1C | 01 | LOCATION1 | LOCATION2 |
	 *    +----+----+----+----+-----------+-----------+
	 *    0    1    2    3    4          12          20
	 *    +-----------+
	 *    | LOCATION3 |
	 *    +-----------+
	 *   20          28
	 *   LOCATION1 : Location of Continuation of System Use Area.
	 *   LOCATION2 : Offset to Start of Continuation.
	 *   LOCATION3 : Length of the Continuation.
	 */

	bp[1] = 'C';
	bp[2] = 'E';
	bp[3] = RR_CE_SIZE;	/* length	*/
	bp[4] = 1;		/* version	*/
	set_num_733(bp+5, location);
	set_num_733(bp+13, offset);
	set_num_733(bp+21, size);
	return (RR_CE_SIZE);
}

/*
 * The functions, which names are beginning with extra_, are used to
 * control extra records.
 * The maximum size of a Directory Record is 254. When a filename is
 * very long, all of RRIP data of a file won't stored to the Directory
 * Record and so remaining RRIP data store to an extra record instead.
 */
static unsigned char *
extra_open_record(unsigned char *bp, int dr_len, struct isoent *isoent,
    struct ctl_extr_rec *ctl)
{
	ctl->bp = bp;
	if (bp != NULL)
		bp += dr_len;
	ctl->use_extr = 0;
	ctl->isoent = isoent;
	ctl->ce_ptr = NULL;
	ctl->cur_len = ctl->dr_len = dr_len;
	ctl->limit = DR_LIMIT;

	return (bp);
}

static void
extra_close_record(struct ctl_extr_rec *ctl, int ce_size)
{
	int padding = 0;

	if (ce_size > 0)
		extra_tell_used_size(ctl, ce_size);
	/* Padding. */
	if (ctl->cur_len & 0x01) {
		ctl->cur_len++;
		if (ctl->bp != NULL)
			ctl->bp[ctl->cur_len] = 0;
		padding = 1;
	}
	if (ctl->use_extr) {
		if (ctl->ce_ptr != NULL)
			set_SUSP_CE(ctl->ce_ptr, ctl->extr_loc,
			    ctl->extr_off, ctl->cur_len - padding);
	} else
		ctl->dr_len = ctl->cur_len;
}

#define extra_space(ctl)	((ctl)->limit - (ctl)->cur_len)

static unsigned char *
extra_next_record(struct ctl_extr_rec *ctl, int length)
{
	int cur_len = ctl->cur_len;/* save cur_len */

	/* Close the current extra record or Directory Record. */
	extra_close_record(ctl, RR_CE_SIZE);

	/* Get a next extra record. */
	ctl->use_extr = 1;
	if (ctl->bp != NULL) {
		/* Storing data into an extra record. */
		unsigned char *p;

		/* Save the pointer where a CE extension will be
		 * stored to. */
		ctl->ce_ptr = &ctl->bp[cur_len+1];
		p = extra_get_record(ctl->isoent,
		    &ctl->limit, &ctl->extr_off, &ctl->extr_loc);
		ctl->bp = p - 1;/* the base of bp offset is 1. */
	} else
		/* Calculating the size of an extra record. */
		(void)extra_get_record(ctl->isoent,
		    &ctl->limit, NULL, NULL);
	ctl->cur_len = 0;
	/* Check if an extra record is almost full.
	 * If so, get a next one. */
	if (extra_space(ctl) < length)
		(void)extra_next_record(ctl, length);

	return (ctl->bp);
}

static inline struct extr_rec *
extra_last_record(struct isoent *isoent)
{
	if (isoent->extr_rec_list.first == NULL)
		return (NULL);
	return ((struct extr_rec *)(void *)
		((char *)(isoent->extr_rec_list.last)
		    - offsetof(struct extr_rec, next)));
}

static unsigned char *
extra_get_record(struct isoent *isoent, int *space, int *off, int *loc)
{
	struct extr_rec *rec;

	isoent = isoent->parent;
	if (off != NULL) {
		/* Storing data into an extra record. */
		rec = isoent->extr_rec_list.current;
		if (DR_SAFETY > LOGICAL_BLOCK_SIZE - rec->offset)
			rec = rec->next;
	} else {
		/* Calculating the size of an extra record. */
		rec = extra_last_record(isoent);
		if (rec == NULL ||
		    DR_SAFETY > LOGICAL_BLOCK_SIZE - rec->offset) {
			rec = malloc(sizeof(*rec));
			if (rec == NULL)
				return (NULL);
			rec->location = 0;
			rec->offset = 0;
			/* Insert `rec` into the tail of isoent->extr_rec_list */
			rec->next = NULL;
			*isoent->extr_rec_list.last = rec;
			isoent->extr_rec_list.last = &(rec->next);
		}
	}
	*space = LOGICAL_BLOCK_SIZE - rec->offset - DR_SAFETY;
	if (*space & 0x01)
		*space -= 1;/* Keep padding space. */
	if (off != NULL)
		*off = rec->offset;
	if (loc != NULL)
		*loc = rec->location;
	isoent->extr_rec_list.current = rec;

	return (&rec->buf[rec->offset]);
}

static void
extra_tell_used_size(struct ctl_extr_rec *ctl, int size)
{
	struct isoent *isoent;
	struct extr_rec *rec;

	if (ctl->use_extr) {
		isoent = ctl->isoent->parent;
		rec = isoent->extr_rec_list.current;
		if (rec != NULL)
			rec->offset += size;
	}
	ctl->cur_len += size;
}

static int
extra_setup_location(struct isoent *isoent, int location)
{
	struct extr_rec *rec;
	int cnt;

	cnt = 0;
	rec = isoent->extr_rec_list.first;
	isoent->extr_rec_list.current = rec;
	while (rec) {
		cnt++;
		rec->location = location++;
		rec->offset = 0;
		rec = rec->next;
	}
	return (cnt);
}

/*
 * Create the RRIP entries.
 */
static int
set_directory_record_rr(unsigned char *bp, int dr_len,
    struct isoent *isoent, struct iso9660 *iso9660, enum dir_rec_type t)
{
	/* Flags(BP 5) of the Rockridge "RR" System Use Field */
	unsigned char rr_flag;
#define RR_USE_PX	0x01
#define RR_USE_PN	0x02
#define RR_USE_SL	0x04
#define RR_USE_NM	0x08
#define RR_USE_CL	0x10
#define RR_USE_PL	0x20
#define RR_USE_RE	0x40
#define RR_USE_TF	0x80
	int length;
	struct ctl_extr_rec ctl;
	struct isoent *rr_parent, *pxent;
	struct isofile *file;

	bp = extra_open_record(bp, dr_len, isoent, &ctl);

	if (t == DIR_REC_PARENT) {
		rr_parent = isoent->rr_parent;
		pxent = isoent->parent;
		if (rr_parent != NULL)
			isoent = rr_parent;
		else
			isoent = isoent->parent;
	} else {
		rr_parent = NULL;
		pxent = isoent;
	}
	file = isoent->file;

	if (t != DIR_REC_NORMAL) {
		rr_flag = RR_USE_PX | RR_USE_TF;
		if (rr_parent != NULL)
			rr_flag |= RR_USE_PL;
	} else {
		rr_flag = RR_USE_PX | RR_USE_NM | RR_USE_TF;
		if (archive_entry_filetype(file->entry) == AE_IFLNK)
			rr_flag |= RR_USE_SL;
		if (isoent->rr_parent != NULL)
			rr_flag |= RR_USE_RE;
		if (isoent->rr_child != NULL)
			rr_flag |= RR_USE_CL;
		if (archive_entry_filetype(file->entry) == AE_IFCHR ||
		    archive_entry_filetype(file->entry) == AE_IFBLK)
			rr_flag |= RR_USE_PN;
#ifdef COMPAT_MKISOFS
		/*
		 * mkisofs 2.01.01a63 records "RE" extension to
		 * the entry of "rr_moved" directory.
		 * I don't understand this behavior.
		 */
		if (isoent->virtual &&
		    isoent->parent == iso9660->primary.rootent &&
		    strcmp(isoent->file->basename.s, "rr_moved") == 0)
			rr_flag |= RR_USE_RE;
#endif
	}

	/* Write "SP" System Use Entry. */
	if (t == DIR_REC_SELF && isoent == isoent->parent) {
		length = 7;
		if (bp != NULL) {
			bp[1] = 'S';
			bp[2] = 'P';
			bp[3] = length;
			bp[4] = 1;	/* version	*/
			bp[5] = 0xBE;  /* Check Byte	*/
			bp[6] = 0xEF;  /* Check Byte	*/
			bp[7] = 0;
			bp += length;
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "RR" System Use Entry. */
	length = 5;
	if (extra_space(&ctl) < length)
		bp = extra_next_record(&ctl, length);
	if (bp != NULL) {
		bp[1] = 'R';
		bp[2] = 'R';
		bp[3] = length;
		bp[4] = 1;	/* version */
		bp[5] = rr_flag;
		bp += length;
	}
	extra_tell_used_size(&ctl, length);

	/* Write "NM" System Use Entry. */
	if (rr_flag & RR_USE_NM) {
		/*
		 *   "NM" Format:
		 *     e.g. a basename is 'foo'
		 *               len  ver  flg
		 *    +----+----+----+----+----+----+----+----+
		 *    | 'N'| 'M'| 08 | 01 | 00 | 'f'| 'o'| 'o'|
		 *    +----+----+----+----+----+----+----+----+
		 *    <----------------- len ----------------->
		 */
		size_t nmlen = file->basename.length;
		const char *nm = file->basename.s;
		size_t nmmax;

		if (extra_space(&ctl) < 6)
			bp = extra_next_record(&ctl, 6);
		if (bp != NULL) {
			bp[1] = 'N';
			bp[2] = 'M';
			bp[4] = 1;	    /* version	*/
		}
		nmmax = extra_space(&ctl);
		if (nmmax > 0xff)
			nmmax = 0xff;
		while (nmlen + 5 > nmmax) {
			length = nmmax;
			if (bp != NULL) {
				bp[3] = length;
				bp[5] = 0x01;/* Alternate Name continues
					       * in next "NM" field */
				memcpy(bp+6, nm, length - 5);
				bp += length;
			}
			nmlen -= length - 5;
			nm += length - 5;
			extra_tell_used_size(&ctl, length);
			if (extra_space(&ctl) < 6) {
				bp = extra_next_record(&ctl, 6);
				nmmax = extra_space(&ctl);
				if (nmmax > 0xff)
					nmmax = 0xff;
			}
			if (bp != NULL) {
				bp[1] = 'N';
				bp[2] = 'M';
				bp[4] = 1;    /* version */
			}
		}
		length = 5 + nmlen;
		if (bp != NULL) {
			bp[3] = length;
			bp[5] = 0;
			memcpy(bp+6, nm, nmlen);
			bp += length;
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "PX" System Use Entry. */
	if (rr_flag & RR_USE_PX) {
		/*
		 *   "PX" Format:
		 *               len  ver  
		 *    +----+----+----+----+-----------+-----------+
		 *    | 'P'| 'X'| 2C | 01 | FILE MODE |   LINKS   |
		 *    +----+----+----+----+-----------+-----------+
		 *    0    1    2    3    4          12          20
		 *    +-----------+-----------+------------------+
		 *    |  USER ID  | GROUP ID  |FILE SERIAL NUMBER|
		 *    +-----------+-----------+------------------+
		 *   20          28          36                 44
		 */
		length = 44;
		if (extra_space(&ctl) < length)
			bp = extra_next_record(&ctl, length);
		if (bp != NULL) {
			mode_t mode;
			uid_t uid;
			gid_t gid;

			mode = archive_entry_mode(file->entry);
			uid = archive_entry_uid(file->entry);
			gid = archive_entry_gid(file->entry);
			if (iso9660->opt.rr == OPT_RR_USEFUL) {
				/*
				 * This action is simular mkisofs -r option
				 */
				uid = 0;
				gid = 0;
				/* set all read bit ON */
				mode |= 0444;
#if !defined(_WIN32) && !defined(__CYGWIN__)
				if (mode & 0111)
#endif
					/* set all exec bit ON */
					mode |= 0111;
				/* clear all write bits. */
				mode &= ~0222;
				/* clear setuid,setgid,sticky bits. */
				mode &= ~07000;
			}
			if (iso9660->opt.uid)
				uid = iso9660->uid;
			if (iso9660->opt.gid)
				gid = iso9660->gid;

			bp[1] = 'P';
			bp[2] = 'X';
			bp[3] = length;
			bp[4] = 1;	/* version	*/
			/* file mode */
			set_num_733(bp+5, mode);
			/* file links (stat.st_nlink) */
			set_num_733(bp+13,
			    archive_entry_nlink(file->entry));
			set_num_733(bp+21, uid);
			set_num_733(bp+29, gid);
			/* File Serial Number */
			if (pxent->dir)
				set_num_733(bp+37, pxent->dir_location);
			else if (file->hardlink_target != NULL)
				set_num_733(bp+37,
				    file->hardlink_target->cur_content->location);
			else
				set_num_733(bp+37,
				    file->cur_content->location);
			bp += length;
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "SL" System Use Entry. */
	if (rr_flag & RR_USE_SL) {
		/*
		 *   "SL" Format:
		 *     e.g. a symbolic name is 'foo/bar'
		 *               len  ver  flg  
		 *    +----+----+----+----+----+------------+
		 *    | 'S'| 'L'| 0F | 01 | 00 | components |
		 *    +----+----+----+----+----+-----+------+
		 *    0    1    2    3    4    5  ...|...  15
		 *    <----------------- len --------+------>
		 *    components :                   |
		 *     cflg clen                     |
		 *    +----+----+----+----+----+     |
		 *    | 00 | 03 | 'f'| 'o'| 'o'| <---+
		 *    +----+----+----+----+----+     |
		 *    5    6    7    8    9   10     |
		 *     cflg clen                     |
		 *    +----+----+----+----+----+     |
		 *    | 00 | 03 | 'b'| 'a'| 'r'| <---+
		 *    +----+----+----+----+----+
		 *   10   11   12   13   14   15
		 *
	 	 *    - cflg : flag of componet
		 *    - clen : length of componet
		 */
		const char *sl;
		char sl_last;

		if (extra_space(&ctl) < 7)
			bp = extra_next_record(&ctl, 7);
		sl = file->symlink.s;
		sl_last = '\0';
		if (bp != NULL) {
			bp[1] = 'S';
			bp[2] = 'L';
			bp[4] = 1;	/* version	*/
		}
		for (;;) {
			unsigned char *nc, *cf,  *cl, cldmy = 0;
			int sllen, slmax;

			slmax = extra_space(&ctl);
			if (slmax > 0xff)
				slmax = 0xff;
			if (bp != NULL)
				nc = &bp[6];
			else
				nc = NULL;
			cf = cl = NULL;
			sllen = 0;
			while (*sl && sllen + 11 < slmax) {
				if (sl_last == '\0' && sl[0] == '/') {
					/*
					 *     flg  len 
					 *    +----+----+
					 *    | 08 | 00 | ROOT component.
					 *    +----+----+ ("/")
					 *
				 	 * Root component has to appear
				 	 * at the first component only.
					 */
					if (nc != NULL) {
						cf = nc++;
						*cf = 0x08; /* ROOT */
						*nc++ = 0;
					}
					sllen += 2;
					sl++;
					sl_last = '/';
					cl = NULL;
					continue;
				}
				if (((sl_last == '\0' || sl_last == '/') &&
				      sl[0] == '.' && sl[1] == '.' &&
				     (sl[2] == '/' || sl[2] == '\0')) ||
				    (sl[0] == '/' &&
				      sl[1] == '.' && sl[2] == '.' &&
				     (sl[3] == '/' || sl[3] == '\0'))) {
					/*
					 *     flg  len 
					 *    +----+----+
					 *    | 04 | 00 | PARENT component.
					 *    +----+----+ ("..")
					 */
					if (nc != NULL) {
						cf = nc++;
						*cf = 0x04; /* PARENT */
						*nc++ = 0;
					}
					sllen += 2;
					if (sl[0] == '/')
						sl += 3;/* skip "/.." */
					else
						sl += 2;/* skip ".." */
					sl_last = '.';
					cl = NULL;
					continue;
				}
				if (((sl_last == '\0' || sl_last == '/') &&
				      sl[0] == '.' &&
				     (sl[1] == '/' || sl[1] == '\0')) ||
				    (sl[0] == '/' && sl[1] == '.' &&
				     (sl[2] == '/' || sl[2] == '\0'))) {
					/*
					 *     flg  len 
					 *    +----+----+
					 *    | 02 | 00 | CURREENT component.
					 *    +----+----+ (".")
					 */
					if (nc != NULL) {
						cf = nc++;
						*cf = 0x02; /* CURRENT */
						*nc++ = 0;
					}
					sllen += 2;
					if (sl[0] == '/')
						sl += 2;/* skip "/." */
					else
						sl ++;  /* skip "." */
					sl_last = '.';
					cl = NULL;
					continue;
				}
				if (sl[0] == '/' || cl == NULL) {
					if (nc != NULL) {
						cf = nc++;
						*cf = 0;
						cl = nc++;
						*cl = 0;
					} else
						cl = &cldmy;
					sllen += 2;
					if (sl[0] == '/') {
						sl_last = *sl++;
						continue;
					}
				}
				sl_last = *sl++;
				if (nc != NULL) {
					*nc++ = sl_last;
					(*cl) ++;
				}
				sllen++;
			}
			if (*sl) {
				length = 5 + sllen;
				if (bp != NULL) {
					/*
					 * Mark flg as CONTINUE component.
					 */
					*cf |= 0x01;
					/*
					 *               len  ver  flg  
					 *    +----+----+----+----+----+-
					 *    | 'S'| 'L'| XX | 01 | 01 | 
					 *    +----+----+----+----+----+-
					 *                           ^
					 *           continues in next "SL"
					 */
					bp[3] = length;
					bp[5] = 0x01;/* This Symbolic Link
						      * continues in next
						      * "SL" field */
					bp += length;
				}
				extra_tell_used_size(&ctl, length);
				if (extra_space(&ctl) < 11)
					bp = extra_next_record(&ctl, 11);
				if (bp != NULL) {
					/* Next 'SL' */
					bp[1] = 'S';
					bp[2] = 'L';
					bp[4] = 1;    /* version */
				}
			} else {
				length = 5 + sllen;
				if (bp != NULL) {
					bp[3] = length;
					bp[5] = 0;
					bp += length;
				}
				extra_tell_used_size(&ctl, length);
				break;
			}
		}
	}

	/* Write "TF" System Use Entry. */
	if (rr_flag & RR_USE_TF) {
		/*
		 *   "TF" Format:
		 *               len  ver 
		 *    +----+----+----+----+-----+-------------+
		 *    | 'T'| 'F'| XX | 01 |FLAGS| TIME STAMPS |
		 *    +----+----+----+----+-----+-------------+
		 *    0    1    2    3    4     5            XX
		 *    TIME STAMPS : ISO 9660 Standard 9.1.5.
		 *                  If TF_LONG_FORM FLAGS is set, 
		 *                  use ISO9660 Standard 8.4.26.1.
		 */
#define TF_CREATION	0x01	/* Creation time recorded		*/
#define TF_MODIFY	0x02	/* Modification time recorded		*/
#define TF_ACCESS	0x04	/* Last Access time recorded		*/
#define TF_ATTRIBUTES	0x08	/* Last Attribute Change time recorded  */
#define TF_BACKUP	0x10	/* Last Backup time recorded		*/
#define TF_EXPIRATION	0x20	/* Expiration time recorded		*/
#define TF_EFFECTIVE	0x40	/* Effective time recorded		*/
#define TF_LONG_FORM	0x80	/ ISO 9660 17-byte time format used	*/
		unsigned char tf_flags;

		length = 5;
		tf_flags = 0;
#ifndef COMPAT_MKISOFS
		if (archive_entry_birthtime_is_set(file->entry) &&
		    archive_entry_birthtime(file->entry) <=
		    archive_entry_mtime(file->entry)) {
			length += 7;
			tf_flags |= TF_CREATION;
		}
#endif
		if (archive_entry_mtime_is_set(file->entry)) {
			length += 7;
			tf_flags |= TF_MODIFY;
		}
		if (archive_entry_atime_is_set(file->entry)) {
			length += 7;
			tf_flags |= TF_ACCESS;
		}
		if (archive_entry_ctime_is_set(file->entry)) {
			length += 7;
			tf_flags |= TF_ATTRIBUTES;
		}
		if (extra_space(&ctl) < length)
			bp = extra_next_record(&ctl, length);
		if (bp != NULL) {
			bp[1] = 'T';
			bp[2] = 'F';
			bp[3] = length;
			bp[4] = 1;	/* version	*/
			bp[5] = tf_flags;
			bp += 5;
			/* Creation time */
			if (tf_flags & TF_CREATION) {
				set_time_915(bp+1,
				    archive_entry_birthtime(file->entry));
				bp += 7;
			}
			/* Modification time */
			if (tf_flags & TF_MODIFY) {
				set_time_915(bp+1,
				    archive_entry_mtime(file->entry));
				bp += 7;
			}
			/* Last Access time */
			if (tf_flags & TF_ACCESS) {
				set_time_915(bp+1,
				    archive_entry_atime(file->entry));
				bp += 7;
			}
			/* Last Attribute Change time */
			if (tf_flags & TF_ATTRIBUTES) {
				set_time_915(bp+1,
				    archive_entry_ctime(file->entry));
				bp += 7;
			}
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "RE" System Use Entry. */
	if (rr_flag & RR_USE_RE) {
		/*
		 *   "RE" Format:
		 *               len  ver  
		 *    +----+----+----+----+
		 *    | 'R'| 'E'| 04 | 01 |
		 *    +----+----+----+----+
		 *    0    1    2    3    4
		 */
		length = 4;
		if (extra_space(&ctl) < length)
			bp = extra_next_record(&ctl, length);
		if (bp != NULL) {
			bp[1] = 'R';
			bp[2] = 'E';
			bp[3] = length;
			bp[4] = 1;	/* version	*/
			bp += length;
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "PL" System Use Entry. */
	if (rr_flag & RR_USE_PL) {
		/*
		 *   "PL" Format:
		 *               len  ver  
		 *    +----+----+----+----+------------+
		 *    | 'P'| 'L'| 0C | 01 | *LOCATION  |
		 *    +----+----+----+----+------------+
		 *    0    1    2    3    4           12
		 *    *LOCATION: location of parent directory
		 */
		length = 12;
		if (extra_space(&ctl) < length)
			bp = extra_next_record(&ctl, length);
		if (bp != NULL) {
			bp[1] = 'P';
			bp[2] = 'L';
			bp[3] = length;
			bp[4] = 1;	/* version	*/
			set_num_733(bp + 5,
			    rr_parent->dir_location);
			bp += length;
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "CL" System Use Entry. */
	if (rr_flag & RR_USE_CL) {
		/*
		 *   "CL" Format:
		 *               len  ver  
		 *    +----+----+----+----+------------+
		 *    | 'C'| 'L'| 0C | 01 | *LOCATION  |
		 *    +----+----+----+----+------------+
		 *    0    1    2    3    4           12
		 *    *LOCATION: location of child directory
		 */
		length = 12;
		if (extra_space(&ctl) < length)
			bp = extra_next_record(&ctl, length);
		if (bp != NULL) {
			bp[1] = 'C';
			bp[2] = 'L';
			bp[3] = length;
			bp[4] = 1;	/* version	*/
			set_num_733(bp + 5,
			    isoent->rr_child->dir_location);
			bp += length;
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "PN" System Use Entry. */
	if (rr_flag & RR_USE_PN) {
		/*
		 *   "PN" Format:
		 *               len  ver  
		 *    +----+----+----+----+------------+------------+
		 *    | 'P'| 'N'| 14 | 01 | dev_t high | dev_t low  |
		 *    +----+----+----+----+------------+------------+
		 *    0    1    2    3    4           12           20
		 */
		length = 20;
		if (extra_space(&ctl) < length)
			bp = extra_next_record(&ctl, length);
		if (bp != NULL) {
			uint64_t dev;

			bp[1] = 'P';
			bp[2] = 'N';
			bp[3] = length;
			bp[4] = 1;	/* version	*/
			dev = (uint64_t)archive_entry_rdev(file->entry);
			set_num_733(bp + 5, dev >> 32);
			set_num_733(bp + 13, dev & 0xFFFFFFFF);
			bp += length;
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "ZF" System Use Entry. */
	if (file->zisofs.header_size) {
		/*
		 *   "ZF" Format:
		 *               len  ver  
		 *    +----+----+----+----+----+----+-------------+
		 *    | 'Z'| 'F'| 10 | 01 | 'p'| 'z'| Header Size |
		 *    +----+----+----+----+----+----+-------------+
		 *    0    1    2    3    4    5    6             7
		 *    +--------------------+-------------------+
		 *    | Log2 of block Size | Uncompressed Size |
		 *    +--------------------+-------------------+
		 *    7                    8                   16
		 */
		length = 16;
		if (extra_space(&ctl) < length)
			bp = extra_next_record(&ctl, length);
		if (bp != NULL) {
			bp[1] = 'Z';
			bp[2] = 'F';
			bp[3] = length;
			bp[4] = 1;	/* version	*/
			bp[5] = 'p';
			bp[6] = 'z';
			bp[7] = file->zisofs.header_size;
			bp[8] = file->zisofs.log2_bs;
			set_num_733(bp + 9, file->zisofs.uncompressed_size);
			bp += length;
		}
		extra_tell_used_size(&ctl, length);
	}

	/* Write "CE" System Use Entry. */
	if (t == DIR_REC_SELF && isoent == isoent->parent) {
		length = RR_CE_SIZE;
		if (bp != NULL)
			set_SUSP_CE(bp+1, iso9660->location_rrip_er,
			    0, RRIP_ER_SIZE);
		extra_tell_used_size(&ctl, length);
	}

	extra_close_record(&ctl, 0);

	return (ctl.dr_len);
}

/*
 * Write data of a Directory Record or calculate writing bytes itself.
 * If parameter `p' is NULL, calculates the size of writing data, which
 * a Directory Record needs to write, then it saved and return
 * the calculated size.
 * Parameter `n' is a remaining size of buffer. when parameter `p' is
 * not NULL, check whether that `n' is not less than the saved size.
 * if that `n' is small, return zero.
 *
 * This format of the Directory Record is according to
 * ISO9660 Standard 9.1
 */
static int
set_directory_record(unsigned char *p, size_t n, struct isoent *isoent,
    struct iso9660 *iso9660, enum dir_rec_type t,
    enum vdd_type vdd_type)
{
	unsigned char *bp;
	size_t dr_len;
	size_t fi_len;

	if (p != NULL) {
		/*
		 * Check whether a write buffer size is less than the
		 * saved size which is needed to write this Directory
		 * Record.
		 */
		switch (t) {
		case DIR_REC_VD:
			dr_len = isoent->dr_len.vd; break;
		case DIR_REC_SELF:
			dr_len = isoent->dr_len.self; break;
		case DIR_REC_PARENT:
			dr_len = isoent->dr_len.parent; break;
		case DIR_REC_NORMAL:
		default:
			dr_len = isoent->dr_len.normal; break;
		}
		if (dr_len > n)
			return (0);/* Needs more buffer size. */
	}

	if (t == DIR_REC_NORMAL && isoent->identifier != NULL)
		fi_len = isoent->id_len;
	else
		fi_len = 1;

	if (p != NULL) {
		struct isoent *xisoent;
		struct isofile *file;
		unsigned char flag;

		if (t == DIR_REC_PARENT)
			xisoent = isoent->parent;
		else
			xisoent = isoent;
		file = isoent->file;
		if (file->hardlink_target != NULL)
			file = file->hardlink_target;
		/* Make a file flag. */
		if (xisoent->dir)
			flag = FILE_FLAG_DIRECTORY;
		else {
			if (file->cur_content->next != NULL)
				flag = FILE_FLAG_MULTI_EXTENT;
			else
				flag = 0;
		}

		bp = p -1;
		/* Extended Attribute Record Length */
		set_num_711(bp+2, 0);
		/* Location of Extent */
		if (xisoent->dir)
			set_num_733(bp+3, xisoent->dir_location);
		else
			set_num_733(bp+3, file->cur_content->location);
		/* Data Length */
		if (xisoent->dir)
			set_num_733(bp+11,
			    xisoent->dir_block * LOGICAL_BLOCK_SIZE);
		else
			set_num_733(bp+11, file->cur_content->size);
		/* Recording Date and Time */
		/* NOTE:
		 *  If a file type is symbolic link, you are seeing this
		 *  field value is different from a value mkisofs makes.
		 *  libarchive uses lstat to get this one, but it
		 *  seems mkisofs uses stat to get.
		 */
		set_time_915(bp+19,
		    archive_entry_mtime(xisoent->file->entry));
		/* File Flags */
		bp[26] = flag;
		/* File Unit Size */
		set_num_711(bp+27, 0);
		/* Interleave Gap Size */
		set_num_711(bp+28, 0);
		/* Volume Sequence Number */
		set_num_723(bp+29, iso9660->volume_sequence_number);
		/* Length of File Identifier */
		set_num_711(bp+33, fi_len);
		/* File Identifier */
		switch (t) {
		case DIR_REC_VD:
		case DIR_REC_SELF:
			set_num_711(bp+34, 0);
			break;
		case DIR_REC_PARENT:
			set_num_711(bp+34, 1);
			break;
		case DIR_REC_NORMAL:
			if (isoent->identifier != NULL)
				memcpy(bp+34, isoent->identifier, fi_len);
			else
				set_num_711(bp+34, 0);
			break;
		}
	} else
		bp = NULL;
	dr_len = 33 + fi_len;
	/* Padding Field */
	if (dr_len & 0x01) {
		dr_len ++;
		if (p != NULL)
			bp[dr_len] = 0;
	}

	/* Volume Descriptor does not record extension. */
	if (t == DIR_REC_VD) {
		if (p != NULL)
			/* Length of Directory Record */
			set_num_711(p, dr_len);
		else
			isoent->dr_len.vd = dr_len;
		return (dr_len);
	}

	/* Rockridge */
	if (iso9660->opt.rr && vdd_type != VDD_JOLIET)
		dr_len = set_directory_record_rr(bp, dr_len,
		    isoent, iso9660, t);

	if (p != NULL)
		/* Length of Directory Record */
		set_num_711(p, dr_len);
	else {
		/*
		 * Save the size which is needed to write this
		 * Directory Record.
		 */
		switch (t) {
		case DIR_REC_VD:
			/* This case does not come, but compiler
			 * complains that DIR_REC_VD not handled
			 *  in switch ....  */
			break;
		case DIR_REC_SELF:
			isoent->dr_len.self = dr_len; break;
		case DIR_REC_PARENT:
			isoent->dr_len.parent = dr_len; break;
		case DIR_REC_NORMAL:
			isoent->dr_len.normal = dr_len; break;
		}
	}

	return (dr_len);
}

/*
 * Calculate the size of a directory record.
 */
static inline int
get_dir_rec_size(struct iso9660 *iso9660, struct isoent *isoent,
    enum dir_rec_type t, enum vdd_type vdd_type)
{

	return (set_directory_record(NULL, SIZE_MAX,
	    isoent, iso9660, t, vdd_type));
}

/*
 * Manage to write ISO-image data with wbuff to reduce calling
 * __archive_write_output() for performance.
 */

#define wb_buffmax(a)	(sizeof(((struct iso9660 *)(a)->format_data)->wbuff))
#define wb_remaining(a)	(((struct iso9660 *)(a)->format_data)->wbuff_remaining)

static inline unsigned char *
wb_buffptr(struct archive_write *a)
{
	struct iso9660 *iso9660 = (struct iso9660 *)a->format_data;

	return (&(iso9660->wbuff[sizeof(iso9660->wbuff)
		- iso9660->wbuff_remaining]));
}

static int
wb_write_out(struct archive_write *a)
{
	struct iso9660 *iso9660 = (struct iso9660 *)a->format_data;
	size_t wsize, nw;
	int r;

	wsize = sizeof(iso9660->wbuff) - iso9660->wbuff_remaining;
	nw = wsize % LOGICAL_BLOCK_SIZE;
	r = __archive_write_output(a, iso9660->wbuff, wsize - nw);
	iso9660->wbuff_remaining = sizeof(iso9660->wbuff);
	if (nw) {
		iso9660->wbuff_remaining -= nw;
		memmove(iso9660->wbuff, iso9660->wbuff + wsize - nw, nw);
	}
	return (r);
}

static int
wb_consume(struct archive_write *a, size_t size)
{
	struct iso9660 *iso9660 = (struct iso9660 *)a->format_data;

	if (size > iso9660->wbuff_remaining ||
	    iso9660->wbuff_remaining == 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Internal Program error: iso9660:wb_consume()"
		    " size=%jd, wbuff_remaining=%jd",
		    (int64_t)size, (int64_t)iso9660->wbuff_remaining);
		return (ARCHIVE_FATAL);
	}
	iso9660->wbuff_remaining -= size;
	if (iso9660->wbuff_remaining < LOGICAL_BLOCK_SIZE)
		return (wb_write_out(a));
	return (ARCHIVE_OK);
}

static int
write_null(struct archive_write *a, size_t size)
{
	size_t remaining;
	unsigned char *p, *old;
	int r;

	remaining = wb_remaining(a);
	p = wb_buffptr(a);
	if (size <= remaining) {
		memset(p, 0, size);
		return (wb_consume(a, size));
	}
	memset(p, 0, remaining);
	r = wb_consume(a, remaining);
	if (r != ARCHIVE_OK)
		return (r);
	size -= remaining;
	old = p;
	p = wb_buffptr(a);
	memset(p, 0, old - p);
	remaining = wb_remaining(a);
	while (size) {
		size_t wsize = size;

		if (wsize > remaining)
			wsize = remaining;
		r = wb_consume(a, wsize);
		if (r != ARCHIVE_OK)
			return (r);
		size -= wsize;
	}
	return (ARCHIVE_OK);
}

/*
 * Write Volume Descriptor Set Terminator
 */
static int
write_VD_terminator(struct archive_write *a)
{
	unsigned char *bp;

	bp = wb_buffptr(a) -1;
	set_VD_bp(bp, VDT_TERMINATOR, 1);
	set_unused_field_bp(bp, 8, LOGICAL_BLOCK_SIZE);

	return (wb_consume(a, LOGICAL_BLOCK_SIZE));
}

static int
set_file_identifier(unsigned char *bp, int from, int to, enum vdc vdc,
    struct archive_write *a, struct vdd *vdd, struct archive_string *id,
    const char *label, int leading_under, enum char_type char_type)
{
	char identifier[256];
	struct isoent *isoent;
	const char *ids;
	size_t len;

	if (id->length > 0 && leading_under && id->s[0] != '_') {
		if (char_type == A_CHAR)
			set_str_a_characters_bp(bp, from, to, id->s, vdc);
		else
			set_str_d_characters_bp(bp, from, to, id->s, vdc);
	} else if (id->length > 0) {
		ids = id->s;
		if (leading_under)
			ids++;
		isoent = isoent_find_entry(vdd->rootent, ids);
		if (isoent == NULL) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Not Found %s `%s'.",
			    label, ids);
			return (ARCHIVE_FATAL);
		}
		len = isoent->ext_off + isoent->ext_len;
		if (vdd->vdd_type == VDD_JOLIET) {
			if (len > sizeof(identifier)-2)
				len = sizeof(identifier)-2;
		} else {
			if (len > sizeof(identifier)-1)
				len = sizeof(identifier)-1;
		}
		memcpy(identifier, isoent->identifier, len);
		identifier[len] = '\0';
		if (vdd->vdd_type == VDD_JOLIET) {
			identifier[len+1] = 0;
			vdc = VDC_UCS2_DIRECT;
		}
		if (char_type == A_CHAR)
			set_str_a_characters_bp(bp, from, to,
			    identifier, vdc);
		else
			set_str_d_characters_bp(bp, from, to,
			    identifier, vdc);
	} else {
		if (char_type == A_CHAR)
			set_str_a_characters_bp(bp, from, to, NULL, vdc);
		else
			set_str_d_characters_bp(bp, from, to, NULL, vdc);
	}
	return (ARCHIVE_OK);
}

/*
 * Write Primary/Supplementary Volume Descriptor
 */
static int
write_VD(struct archive_write *a, struct vdd *vdd)
{
	struct iso9660 *iso9660;
	unsigned char *bp;
	uint16_t volume_set_size = 1;
	char identifier[256];
	enum VD_type vdt;
	enum vdc vdc;
	unsigned char vd_ver, fst_ver;
	int r;

	iso9660 = a->format_data;
	switch (vdd->vdd_type) {
	case VDD_JOLIET:
		vdt = VDT_SUPPLEMENTARY;
		vd_ver = fst_ver = 1;
		vdc = VDC_UCS2;
		break;
	case VDD_ENHANCED:
		vdt = VDT_SUPPLEMENTARY;
		vd_ver = fst_ver = 2;
		vdc = VDC_LOWERCASE;
		break;
	case VDD_PRIMARY:
	default:
		vdt = VDT_PRIMARY;
		vd_ver = fst_ver = 1;
		if (iso9660->opt.allow_pvd_lowercase)
			vdc = VDC_LOWERCASE;
		else
			vdc = VDC_STD;
		break;
	}

	bp = wb_buffptr(a) -1;
	/* Volume Descriptor Type */
	set_VD_bp(bp, vdt, vd_ver);
	/* Unused Field */
	set_unused_field_bp(bp, 8, 8);
	/* System Identifier */
	get_system_identitier(identifier, sizeof(identifier));
	set_str_a_characters_bp(bp, 9, 40, identifier, vdc);
	/* Volume Identifier */
	set_str_d_characters_bp(bp, 41, 72,
	    iso9660->volume_identifier.s, vdc);
	/* Unused Field */
	set_unused_field_bp(bp, 73, 80);
	/* Volume Space Size */
	set_num_733(bp+81, iso9660->volume_space_size);
	if (vdd->vdd_type == VDD_JOLIET) {
		/* Escape Sequences */
		bp[89] = 0x25;/* UCS-2 Level 3 */
		bp[90] = 0x2F;
		bp[91] = 0x45;
		memset(bp + 92, 0, 120 - 92 + 1);
	} else {
		/* Unused Field */
		set_unused_field_bp(bp, 89, 120);
	}
	/* Volume Set Size */
	set_num_723(bp+121, volume_set_size);
	/* Volume Sequence Number */
	set_num_723(bp+125, iso9660->volume_sequence_number);
	/* Logical Block Size */
	set_num_723(bp+129, LOGICAL_BLOCK_SIZE);
	/* Path Table Size */
	set_num_733(bp+133, vdd->path_table_size);
	/* Location of Occurrence of Type L Path Table */
	set_num_731(bp+141, vdd->location_type_L_path_table);
	/* Location of Optional Occurrence of Type L Path Table */
	set_num_731(bp+145, 0);
	/* Location of Occurrence of Type M Path Table */
	set_num_732(bp+149, vdd->location_type_M_path_table);
	/* Location of Optional Occurrence of Type M Path Table */
	set_num_732(bp+153, 0);
	/* Directory Record for Root Directory(BP 157 to 190) */
	set_directory_record(bp+157, 190-157+1, vdd->rootent,
	    iso9660, DIR_REC_VD, vdd->vdd_type);
	/* Volume Set Identifier */
	set_str_d_characters_bp(bp, 191, 318, "", vdc);
	/* Publisher Identifier */
	r = set_file_identifier(bp, 319, 446, vdc, a, vdd,
	    &(iso9660->publisher_identifier),
	    "Publisher File", 1, A_CHAR);
	if (r != ARCHIVE_OK)
		return (r);
	/* Data Preparer Identifier */
	r = set_file_identifier(bp, 447, 574, vdc, a, vdd,
	    &(iso9660->data_preparer_identifier),
	    "Data Preparer File", 1, A_CHAR);
	if (r != ARCHIVE_OK)
		return (r);
	/* Application Identifier */
	r = set_file_identifier(bp, 575, 702, vdc, a, vdd,
	    &(iso9660->application_identifier),
	    "Application File", 1, A_CHAR);
	if (r != ARCHIVE_OK)
		return (r);
	/* Copyright File Identifier */
	r = set_file_identifier(bp, 703, 739, vdc, a, vdd,
	    &(iso9660->copyright_file_identifier),
	    "Copyright File", 0, D_CHAR);
	if (r != ARCHIVE_OK)
		return (r);
	/* Abstract File Identifier */
	r = set_file_identifier(bp, 740, 776, vdc, a, vdd,
	    &(iso9660->abstract_file_identifier),
	    "Abstract File", 0, D_CHAR);
	if (r != ARCHIVE_OK)
		return (r);
	/* Bibliongraphic File Identifier */
	r = set_file_identifier(bp, 777, 813, vdc, a, vdd,
	    &(iso9660->bibliographic_file_identifier),
	    "Bibliongraphic File", 0, D_CHAR);
	if (r != ARCHIVE_OK)
		return (r);
	/* Volume Creation Date and Time */
	set_date_time(bp+814, iso9660->birth_time);
	/* Volume Modification Date and Time */
	set_date_time(bp+831, iso9660->birth_time);
	/* Volume Expiration Date and Time(obsolete) */
	set_date_time_null(bp+848);
	/* Volume Effective Date and Time */
	set_date_time(bp+865, iso9660->birth_time);
	/* File Structure Version */
	bp[882] = fst_ver;
	/* Reserved */
	bp[883] = 0;
	/* Application Use */
	memset(bp + 884, 0x20, 1395 - 884 + 1);
	/* Reserved */
	set_unused_field_bp(bp, 1396, LOGICAL_BLOCK_SIZE);

	return (wb_consume(a, LOGICAL_BLOCK_SIZE));
}

/*
 * Write Boot Record Volume Descriptor
 */
static int
write_VD_boot_record(struct archive_write *a)
{
	struct iso9660 *iso9660;
	unsigned char *bp;

	iso9660 = a->format_data;
	bp = wb_buffptr(a) -1;
	/* Volume Descriptor Type */
	set_VD_bp(bp, VDT_BOOT_RECORD, 1);
	/* Boot System Identifier */
	memcpy(bp+8, "EL TORITO SPECIFICATION", 23);
	set_unused_field_bp(bp, 8+23, 39);
	/* Unused */
	set_unused_field_bp(bp, 40, 71);
	/* Absolute pointer to first sector of Boot Catalog */
	set_num_731(bp+72,
	    iso9660->el_torito.catalog->file->content.location);
	/* Unused */
	set_unused_field_bp(bp, 76, LOGICAL_BLOCK_SIZE);

	return (wb_consume(a, LOGICAL_BLOCK_SIZE));
}

enum keytype {
	KEY_FLG,
	KEY_STR,
	KEY_INT,
	KEY_HEX,
};
static void
set_option_info(struct archive_string *info, int *opt, const char *key,
    enum keytype type,  ...)
{
	va_list ap;
	char prefix;
	const char *s;
	int d;

	prefix = (*opt==0)? ' ':',';
	va_start(ap, type);
	switch (type) {
	case KEY_FLG:
		d = va_arg(ap, int);
		archive_string_sprintf(info, "%c%s%s",
		    prefix, (d == 0)?"!":"", key);
		break;
	case KEY_STR:
		s = va_arg(ap, const char *);
		archive_string_sprintf(info, "%c%s=%s",
		    prefix, key, s);
		break;
	case KEY_INT:
		d = va_arg(ap, int);
		archive_string_sprintf(info, "%c%s=%d",
		    prefix, key, d);
		break;
	case KEY_HEX:
		d = va_arg(ap, int);
		archive_string_sprintf(info, "%c%s=%x",
		    prefix, key, d);
		break;
	}
	va_end(ap);

	*opt = 1;
}

/*
 * Make Non-ISO File System Information
 */
static int
write_information_block(struct archive_write *a)
{
	struct iso9660 *iso9660;
	char buf[128];
	const char *v;
	int opt, r;
	struct archive_string info;
	size_t info_size = LOGICAL_BLOCK_SIZE *
			       NON_ISO_FILE_SYSTEM_INFORMATION_BLOCK;

	iso9660 = (struct iso9660 *)a->format_data;
	if (info_size > wb_remaining(a)) {
		r = wb_write_out(a);
		if (r != ARCHIVE_OK)
			return (r);
	}
	archive_string_init(&info);
	if (archive_string_ensure(&info, info_size) == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	memset(info.s, 0, info_size);
	opt = 0;
#if defined(HAVE__CTIME64_S)
	_ctime64_s(buf, sizeof(buf), &(iso9660->birth_time));
#elif defined(HAVE_CTIME_R)
	ctime_r(&(iso9660->birth_time), buf);
#else
	strncpy(buf, ctime(&(iso9660->birth_time)), sizeof(buf)-1);
	buf[sizeof(buf)-1] = '\0';
#endif
	archive_string_sprintf(&info,
	    "INFO %s%s", buf, archive_version_string());
	if (iso9660->opt.abstract_file != OPT_ABSTRACT_FILE_DEFAULT)
		set_option_info(&info, &opt, "abstract-file",
		    KEY_STR, iso9660->abstract_file_identifier.s);
	if (iso9660->opt.application_id != OPT_APPLICATION_ID_DEFAULT)
		set_option_info(&info, &opt, "application-id",
		    KEY_STR, iso9660->application_identifier.s);
	if (iso9660->opt.allow_ldots != OPT_ALLOW_LDOTS_DEFAULT)
		set_option_info(&info, &opt, "allow-dot-first",
		    KEY_FLG, iso9660->opt.allow_ldots);
	if (iso9660->opt.allow_lowercase != OPT_ALLOW_LOWERCASE_DEFAULT)
		set_option_info(&info, &opt, "allow-lowercase",
		    KEY_FLG, iso9660->opt.allow_lowercase);
	if (iso9660->opt.allow_multidot != OPT_ALLOW_MULTIDOT_DEFAULT)
		set_option_info(&info, &opt, "allow-multidot",
		    KEY_FLG, iso9660->opt.allow_multidot);
	if (iso9660->opt.allow_period != OPT_ALLOW_PERIOD_DEFAULT)
		set_option_info(&info, &opt, "allow-period",
		    KEY_FLG, iso9660->opt.allow_period);
	if (iso9660->opt.allow_pvd_lowercase != OPT_ALLOW_PVD_LOWERCASE_DEFAULT)
		set_option_info(&info, &opt, "allow-pvd-lowercase",
		    KEY_FLG, iso9660->opt.allow_pvd_lowercase);
	if (iso9660->opt.allow_sharp_tilde != OPT_ALLOW_SHARP_TILDE_DEFAULT)
		set_option_info(&info, &opt, "allow-sharp-tilde",
		    KEY_FLG, iso9660->opt.allow_sharp_tilde);
	if (iso9660->opt.allow_vernum != OPT_ALLOW_VERNUM_DEFAULT)
		set_option_info(&info, &opt, "allow-vernum",
		    KEY_FLG, iso9660->opt.allow_vernum);
	if (iso9660->opt.biblio_file != OPT_BIBLIO_FILE_DEFAULT)
		set_option_info(&info, &opt, "biblio-file",
		    KEY_STR, iso9660->bibliographic_file_identifier.s);
	if (iso9660->opt.boot != OPT_BOOT_DEFAULT)
		set_option_info(&info, &opt, "boot",
		    KEY_STR, iso9660->el_torito.boot_filename.s);
	if (iso9660->opt.boot_catalog != OPT_BOOT_CATALOG_DEFAULT)
		set_option_info(&info, &opt, "boot-catalog",
		    KEY_STR, iso9660->el_torito.catalog_filename.s);
	if (iso9660->opt.boot_info_table != OPT_BOOT_INFO_TABLE_DEFAULT)
		set_option_info(&info, &opt, "boot-info-table",
		    KEY_FLG, iso9660->opt.boot_info_table);
	if (iso9660->opt.boot_load_seg != OPT_BOOT_LOAD_SEG_DEFAULT)
		set_option_info(&info, &opt, "boot-load-seg",
		    KEY_HEX, iso9660->el_torito.boot_load_seg);
	if (iso9660->opt.boot_load_size != OPT_BOOT_LOAD_SIZE_DEFAULT)
		set_option_info(&info, &opt, "boot-load-size",
		    KEY_INT, iso9660->el_torito.boot_load_size);
	if (iso9660->opt.boot_type != OPT_BOOT_TYPE_DEFAULT) {
		v = "no-emulation";
		if (iso9660->opt.boot_type == OPT_BOOT_TYPE_FD)
			v = "fd";
		if (iso9660->opt.boot_type == OPT_BOOT_TYPE_HARD_DISK)
			v = "hard-disk";
		set_option_info(&info, &opt, "boot-type",
		    KEY_STR, v);
	}
#ifdef HAVE_ZLIB_H
	if (iso9660->opt.compression_level != OPT_COMPRESSION_LEVEL_DEFAULT)
		set_option_info(&info, &opt, "compression-level",
		    KEY_INT, iso9660->zisofs.compression_level);
#endif
	if (iso9660->opt.copyright_file != OPT_COPYRIGHT_FILE_DEFAULT)
		set_option_info(&info, &opt, "copyright-file",
		    KEY_STR, iso9660->copyright_file_identifier.s);
	if (iso9660->opt.gid != OPT_GID_DEFAULT)
		set_option_info(&info, &opt, "gid", KEY_INT,
		    iso9660->gid);
	if (iso9660->opt.iso_level != OPT_ISO_LEVEL_DEFAULT)
		set_option_info(&info, &opt, "iso-level",
		    KEY_INT, iso9660->opt.iso_level);
	if (iso9660->opt.joliet != OPT_JOLIET_DEFAULT) {
		if (iso9660->opt.joliet == OPT_JOLIET_LONGNAME)
			set_option_info(&info, &opt, "joliet",
			    KEY_STR, "long");
		else
			set_option_info(&info, &opt, "joliet",
			    KEY_FLG, iso9660->opt.joliet);
	}
	if (iso9660->opt.limit_depth != OPT_LIMIT_DEPTH_DEFAULT)
		set_option_info(&info, &opt, "limit-depth",
		    KEY_FLG, iso9660->opt.limit_depth);
	if (iso9660->opt.limit_dirs != OPT_LIMIT_DIRS_DEFAULT)
		set_option_info(&info, &opt, "limit-dirs",
		    KEY_FLG, iso9660->opt.limit_dirs);
	if (iso9660->opt.pad != OPT_PAD_DEFAULT)
		set_option_info(&info, &opt, "pad",
		    KEY_FLG, iso9660->opt.pad);
	if (iso9660->opt.publisher != OPT_PUBLISHER_DEFAULT)
		set_option_info(&info, &opt, "publisher",
		    KEY_STR, iso9660->publisher_identifier.s);
	if (iso9660->opt.relaxed_filenames != OPT_RELAXED_FILENAMES_DEFAULT)
		set_option_info(&info, &opt, "relaxed-filenames",
		    KEY_FLG, iso9660->opt.relaxed_filenames);
	if (iso9660->opt.rr != OPT_RR_DEFAULT) {
		if (iso9660->opt.rr == OPT_RR_DISABLED)
			set_option_info(&info, &opt, "rockridge",
			    KEY_FLG, iso9660->opt.rr);
		else if (iso9660->opt.rr == OPT_RR_STRICT)
			set_option_info(&info, &opt, "rockridge",
			    KEY_STR, "strict");
		else if (iso9660->opt.rr == OPT_RR_USEFUL)
			set_option_info(&info, &opt, "rockridge",
			    KEY_STR, "useful");
	}
	if (iso9660->opt.uid != OPT_UID_DEFAULT)
		set_option_info(&info, &opt, "uid", KEY_INT,
		    iso9660->uid);
	if (iso9660->opt.volume_id != OPT_VOLUME_ID_DEFAULT)
		set_option_info(&info, &opt, "volume-id",
		    KEY_STR, iso9660->volume_identifier.s);
	if (iso9660->opt.zisofs != OPT_ZISOFS_DEFAULT) {
		if (iso9660->opt.zisofs == OPT_ZISOFS_DISABLED ||
		    iso9660->opt.zisofs == OPT_ZISOFS_DIRECT)
			set_option_info(&info, &opt, "zisofs",
			    KEY_FLG, iso9660->opt.zisofs);
		else if (iso9660->opt.zisofs == OPT_ZISOFS_INDIRECT)
			set_option_info(&info, &opt, "zisofs",
			    KEY_STR, "direct");
	}
#ifdef HAVE_ZLIB_H
	if (iso9660->opt.zisofs_exclude != OPT_ZISOFS_EXCLUDE_DEFAULT) {
		int i;
		struct nozf_file *nzf;
		struct archive_string str;

		nzf = iso9660->zisofs.nozf_list;
		archive_string_init(&str);
		for (i = 0; i < iso9660->zisofs.nozf_cnt; i++) {
			if (i > 0)
				archive_strappend_char(&str, ':');
			if (nzf[i].parentdir.length > 0) {
				archive_string_concat(&str,
				     &nzf[i].parentdir);
				archive_strappend_char(&str, '/');
			}
			archive_string_concat(&str, &nzf[i].basename);
		}
		set_option_info(&info, &opt, "zisofs-exclude",
			    KEY_STR, str.s);
		archive_string_free(&str);
	}
#endif

	memcpy(wb_buffptr(a), info.s, info_size);
	archive_string_free(&info);
	return (wb_consume(a, info_size));
}

static int
write_rr_ER(struct archive_write *a)
{
	unsigned char *p;

	p = wb_buffptr(a);

	memset(p, 0, LOGICAL_BLOCK_SIZE);
	p[0] = 'E';
	p[1] = 'R';
	p[3] = 0x01;
	p[2] = RRIP_ER_SIZE;
	p[4] = RRIP_ER_ID_SIZE;
	p[5] = RRIP_ER_DSC_SIZE;
	p[6] = RRIP_ER_SRC_SIZE;
	p[7] = 0x01;
	memcpy(&p[8], rrip_identifier, p[4]);
	memcpy(&p[8+p[4]], rrip_descriptor, p[5]);
	memcpy(&p[8+p[4]+p[5]], rrip_source, p[6]);

	return (wb_consume(a, LOGICAL_BLOCK_SIZE));
}

static void
calculate_path_table_size(struct vdd *vdd)
{
	int depth, size;
	struct path_table *pt;

	pt = vdd->pathtbl;
	size = 0;
	for (depth = 0; depth < vdd->max_depth; depth++) {
		struct isoent **ptbl;
		int i, cnt;

		if ((cnt = pt[depth].cnt) == 0)
			break;

		ptbl = pt[depth].sorted;
		for (i = 0; i < cnt; i++) {
			int len;

			if (ptbl[i]->identifier == NULL)
				len = 1; /* root directory */
			else
				len = ptbl[i]->id_len;
			if (len & 0x01)
				len++; /* Padding Field */
			size += 8 + len;
		}
	}
	vdd->path_table_size = size;
	vdd->path_table_block =
	    ((size + PATH_TABLE_BLOCK_SIZE -1) /
	    PATH_TABLE_BLOCK_SIZE) *
	    (PATH_TABLE_BLOCK_SIZE / LOGICAL_BLOCK_SIZE);
}

static int
_write_path_table(struct archive_write *a, int type_m, int depth,
    struct vdd *vdd)
{
	unsigned char *bp, *wb;
	struct isoent **ptbl;
	size_t wbremaining;
	int i, r, wsize;

	if (vdd->pathtbl[depth].cnt == 0)
		return (0);

	wsize = 0;
	wb = wb_buffptr(a);
	wbremaining = wb_remaining(a);
	bp = wb - 1;
	ptbl = vdd->pathtbl[depth].sorted;
	for (i = 0; i < vdd->pathtbl[depth].cnt; i++) {
		struct isoent *np;
		size_t len;

		np = ptbl[i];
		if (np->identifier == NULL)
			len = 1; /* root directory */
		else
			len = np->id_len;
		if (wbremaining - ((bp+1) - wb) < (len + 1 + 8)) {
			r = wb_consume(a, (bp+1) - wb);
			if (r < 0)
				return (r);
			wb = wb_buffptr(a);
			wbremaining = wb_remaining(a);
			bp = wb -1;
		}
		/* Length of Directory Identifier */
		set_num_711(bp+1, len);
		/* Extended Attribute Record Length */
		set_num_711(bp+2, 0);
		/* Location of Extent */
		if (type_m)
			set_num_732(bp+3, np->dir_location);
		else
			set_num_731(bp+3, np->dir_location);
		/* Parent Directory Number */
		if (type_m)
			set_num_722(bp+7, np->parent->dir_number);
		else
			set_num_721(bp+7, np->parent->dir_number);
		/* Directory Identifier */
		if (np->identifier == NULL)
			bp[9] = 0;
		else
			memcpy(&bp[9], np->identifier, len);
		if (len & 0x01) {
			/* Padding Field */
			bp[9+len] = 0;
			len++;
		}
		wsize += 8 + len;
		bp += 8 + len;
	}
	if ((bp + 1) > wb) {
		r = wb_consume(a, (bp+1)-wb);
		if (r < 0)
			return (r);
	}
	return (wsize);
}

static int
write_path_table(struct archive_write *a, int type_m, struct vdd *vdd)
{
	int depth, r;
	size_t path_table_size;

	r = ARCHIVE_OK;
	path_table_size = 0;
	for (depth = 0; depth < vdd->max_depth; depth++) {
		r = _write_path_table(a, type_m, depth, vdd);
		if (r < 0)
			return (r);
		path_table_size += r;
	}

	/* Write padding data. */ 
	path_table_size = path_table_size % PATH_TABLE_BLOCK_SIZE;
	if (path_table_size > 0)
		r = write_null(a, PATH_TABLE_BLOCK_SIZE - path_table_size);
	return (r);
}

static int
calculate_directory_descriptors(struct iso9660 *iso9660, struct vdd *vdd,
    struct isoent *isoent, int depth)
{
	struct isoent **enttbl;
	int bs, block, i;

	block = 1;
	bs = get_dir_rec_size(iso9660, isoent, DIR_REC_SELF, vdd->vdd_type);
	bs += get_dir_rec_size(iso9660, isoent, DIR_REC_PARENT, vdd->vdd_type);

	if (isoent->children.cnt <= 0 || (vdd->vdd_type != VDD_JOLIET &&
	    !iso9660->opt.rr && depth + 1 >= vdd->max_depth))
		return (block);

	enttbl = isoent->children_sorted;
	for (i = 0; i < isoent->children.cnt; i++) {
		struct isoent *np = enttbl[i];
		struct isofile *file;

		file = np->file;
		if (file->hardlink_target != NULL)
			file = file->hardlink_target;
		file->cur_content = &(file->content);
		do {
			int dr_l;

			dr_l = get_dir_rec_size(iso9660, np, DIR_REC_NORMAL,
			    vdd->vdd_type);
			if ((bs + dr_l) > LOGICAL_BLOCK_SIZE) {
				block ++;
				bs = dr_l;
			} else
				bs += dr_l;
			file->cur_content = file->cur_content->next;
		} while (file->cur_content != NULL);
	}
	return (block);
}

static int
_write_directory_descriptors(struct archive_write *a, struct vdd *vdd,
    struct isoent *isoent, int depth)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isoent **enttbl;
	unsigned char *p, *wb;
	int i, r;
	int dr_l;

	p = wb = wb_buffptr(a);
#define WD_REMAINING	(LOGICAL_BLOCK_SIZE - (p - wb))
	p += set_directory_record(p, WD_REMAINING, isoent,
	    iso9660, DIR_REC_SELF, vdd->vdd_type);
	p += set_directory_record(p, WD_REMAINING, isoent,
	    iso9660, DIR_REC_PARENT, vdd->vdd_type);

	if (isoent->children.cnt <= 0 || (vdd->vdd_type != VDD_JOLIET &&
	    !iso9660->opt.rr && depth + 1 >= vdd->max_depth)) {
		memset(p, 0, WD_REMAINING);
		return (wb_consume(a, LOGICAL_BLOCK_SIZE));
	}

	enttbl = isoent->children_sorted;
	for (i = 0; i < isoent->children.cnt; i++) {
		struct isoent *np = enttbl[i];
		struct isofile *file = np->file;

		if (file->hardlink_target != NULL)
			file = file->hardlink_target;
		file->cur_content = &(file->content);
		do {
			dr_l = set_directory_record(p, WD_REMAINING,
			    np, iso9660, DIR_REC_NORMAL,
			    vdd->vdd_type);
			if (dr_l == 0) {
				memset(p, 0, WD_REMAINING);
				r = wb_consume(a, LOGICAL_BLOCK_SIZE);
				if (r < 0)
					return (r);
				p = wb = wb_buffptr(a);
				dr_l = set_directory_record(p,
				    WD_REMAINING, np, iso9660,
				    DIR_REC_NORMAL, vdd->vdd_type);
			}
			p += dr_l;
			file->cur_content = file->cur_content->next;
		} while (file->cur_content != NULL);
	}
	memset(p, 0, WD_REMAINING);
	return (wb_consume(a, LOGICAL_BLOCK_SIZE));
}

static int
write_directory_descriptors(struct archive_write *a, struct vdd *vdd)
{
	struct isoent *np;
	int depth, r;

	depth = 0;
	np = vdd->rootent;
	do {
		if (np->dir) {
			struct extr_rec *extr;

			r = _write_directory_descriptors(a, vdd, np, depth);
			if (r < 0)
				return (r);
			if (vdd->vdd_type != VDD_JOLIET) {
				/*
				 * This extract record is used by SUSP,RRIP.
				 * Not for joliet.
				 */
				for (extr = np->extr_rec_list.first;
				    extr != NULL;
				    extr = extr->next) {
					unsigned char *wb;

					wb = wb_buffptr(a);
					memcpy(wb, extr->buf, extr->offset);
					memset(wb + extr->offset, 0,
					    LOGICAL_BLOCK_SIZE - extr->offset);
					r = wb_consume(a, LOGICAL_BLOCK_SIZE);
					if (r < 0)
						return (r);
				}
			}

			if (np->children.first != NULL &&
			    depth + 1 < vdd->max_depth) {
				/* Enter to sub directories. */
				np = np->children.first;
				depth++;
				continue;
			}
		}
		while (np != np->parent) {
			if (np->chnext == NULL) {
				/* Return to the parent directory. */
				np = np->parent;
				depth--;
			} else {
				np = np->chnext;
				break;
			}
		}
	} while (np != np->parent);

	return (ARCHIVE_OK);
}

static int
_write_file_descriptors(struct archive_write *a, struct isoent *isoent)
{
	struct isoent **enttbl;
	int i, r;

	enttbl = isoent->children_sorted;
	for (i = 0; i < isoent->children.cnt; i++) {
		struct isoent *np;
		struct isofile *file;

		/*
		 * We do not write file contents if its file-type is
		 * a directory or boot-file, or empty file.
		 */
		np = enttbl[i];
		if (np->dir)
			continue;
		file = np->file;
		if (file->boot || file->content.size == 0)
			continue;

		file->cur_content = &(file->content);
		do {
			int64_t size;
			int64_t pad_size;

			lseek(file->temp_fd,
			    file->cur_content->offset_of_temp,
			    SEEK_SET);
			size = file->cur_content->size;
			pad_size = size % LOGICAL_BLOCK_SIZE;
			if (pad_size)
				pad_size = LOGICAL_BLOCK_SIZE - pad_size;
			while (size) {
				size_t rsize;
				ssize_t rs;
				unsigned char *wb;

				wb = wb_buffptr(a);
				if (size > wb_remaining(a))
					rsize = wb_remaining(a);
				else
					rsize = (size_t)size;

				rs = read(file->temp_fd, wb, rsize);
				if (rs <= 0) {
					archive_set_error(&a->archive,
					    errno,
					    "Can't read temporary file(%jd)",
					    (intmax_t)rs);
					return (ARCHIVE_FATAL);
				}
				size -= rs;
				if (size == 0 && pad_size) {
					/* All file contents must be aligned
					 * with LOGICAL_BLOCK_SIZE */ 
					memset(wb + rs, 0, pad_size);
					rs += pad_size;
				}
				r = wb_consume(a, rs);
				if (r < 0)
					return (r);
			}
			file->cur_content = file->cur_content->next;
		} while (file->cur_content != NULL);
	}
	return (ARCHIVE_OK);
}

static int
write_file_descriptors(struct archive_write *a)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isoent *np;
	int depth, r;
	int joliet;

	if (iso9660->el_torito.catalog != NULL) {
		make_boot_catalog(iso9660, wb_buffptr(a));
		r = wb_consume(a, LOGICAL_BLOCK_SIZE);
		if (r < 0)
			return (r);
	}
	if (iso9660->el_torito.boot != NULL) {
		int64_t size, padsize;

		np = iso9660->el_torito.boot;
		size = archive_entry_size(np->file->entry);
		lseek(np->file->temp_fd,
		    np->file->content.offset_of_temp, SEEK_SET);
		while (size) {
			size_t rsize;
			ssize_t rs;
			unsigned char *wb;

			wb = wb_buffptr(a);
			if (size > wb_remaining(a))
				rsize = wb_remaining(a);
			else
				rsize = (size_t)size;

			rs = read(np->file->temp_fd, wb, rsize);
			if (rs <= 0) {
				archive_set_error(&a->archive, errno,
				    "Can't read temporary file(%jd)",
				    (intmax_t)rs);
				return (ARCHIVE_FATAL);
			}
			size -= rs;
			r = wb_consume(a, rs);
			if (r < 0)
				return (r);
		}

		/*
		 * Write padding if needed.
		 */
		padsize = 0;
		size = fd_boot_image_size(iso9660->el_torito.media_type);
		if (size == 0)
			size = archive_entry_size(np->file->entry);
		else if (size > archive_entry_size(np->file->entry)) {
			/*
			 * If the boot image size is not just 1200KB,
			 * 1440KB,or 2880KB, add padding data.
			 */
			padsize = size - archive_entry_size(np->file->entry);
		}
		size = size % LOGICAL_BLOCK_SIZE;
		if (size != 0)
			padsize += LOGICAL_BLOCK_SIZE - size;
		if (padsize > 0) {
			r = write_null(a, (size_t)padsize);
			if (r < 0)
				return (r);
		}
	}

	depth = 0;
	if (!iso9660->opt.rr && iso9660->opt.joliet) {
		joliet = 1;
		np = iso9660->joliet.rootent;
	} else {
		joliet = 0;
		np = iso9660->primary.rootent;
	}
	do {
		if (np->dir) {
			r =  _write_file_descriptors(a, np);
			if (r < 0)
				return (r);

			if (np->children.first != NULL &&
			    (joliet ||
			    ((iso9660->opt.rr == OPT_RR_DISABLED &&
			      depth + 2 < iso9660->primary.max_depth) ||
			     (iso9660->opt.rr &&
			      depth + 1 < iso9660->primary.max_depth)))) {
				/* Enter to sub directories. */
				np = np->children.first;
				depth++;
				continue;
			}
		}
		while (np != np->parent) {
			if (np->chnext == NULL) {
				/* Return to the parent directory. */
				np = np->parent;
				depth--;
			} else {
				np = np->chnext;
				break;
			}
		}
	} while (np != np->parent);

	return (ARCHIVE_OK);
}

static void
isofile_init_entry_list(struct iso9660 *iso9660)
{
	iso9660->all_file_list.first = NULL;
	iso9660->all_file_list.last = &(iso9660->all_file_list.first);
}

static void
isofile_add_entry(struct iso9660 *iso9660, struct isofile *file)
{
	file->allnext = NULL;
	*iso9660->all_file_list.last = file;
	iso9660->all_file_list.last = &(file->allnext);
}

static void
isofile_free_all_entries(struct iso9660 *iso9660)
{
	struct isofile *file, *file_next;

	file = iso9660->all_file_list.first;
	while (file != NULL) {
		file_next = file->allnext;
		isofile_free(file);
		file = file_next;
	}
}

static struct isofile *
isofile_new(struct archive_entry *entry)
{
	struct isofile *file;

	file = calloc(1, sizeof(*file));
	if (file == NULL)
		return (NULL);

	if (entry != NULL)
		file->entry = archive_entry_clone(entry);
	else
		file->entry = archive_entry_new();
	if (file->entry == NULL) {
		free(file);
		return (NULL);
	}
	archive_string_init(&(file->parentdir));
	archive_string_init(&(file->basename));
	archive_string_init(&(file->symlink));
	file->cur_content = &(file->content);

	return (file);
}

static void
isofile_free(struct isofile *file)
{
	struct content *con, *tmp;

	con = file->content.next;
	while (con != NULL) {
		tmp = con;
		con = con->next;
		free(tmp);
	}
	archive_entry_free(file->entry);
	archive_string_free(&(file->parentdir));
	archive_string_free(&(file->basename));
	archive_string_free(&(file->symlink));
	free(file);
}

#if defined(_WIN32) || defined(__CYGWIN__)
static void
cleanup_backslash(char *p, size_t len)
{
	wchar_t wc;
	size_t l;
	mbstate_t ps;

	/* Convert a path-separator from '\' to  '/' */
	memset(&ps, 0, sizeof(ps));
        while (*p != '\0' && len) {
                l = mbrtowc(&wc, p, len, &ps);
                if (l == -1) {
                        while (*p != '\0') {
                                if (*p == '\\')
                                        *p = '/';
                                ++p;
                        }
                        break;
                }
                if (l == 1 && wc == L'\\')
                        *p = '/';
                p += l;
                len -= l;
        }
}
#else
#define cleanup_backslash(p, len)	/* nop */
#endif

/*
 * Generate a parent directory name and a base name from a pathname.
 */
static void
isofile_gen_utility_names(struct isofile *file)
{
	const char *pathname;
	char *p, *dirname, *slash;
	size_t len;

	archive_string_empty(&(file->parentdir));
	archive_string_empty(&(file->basename));
	archive_string_empty(&(file->symlink));

	pathname =  archive_entry_pathname(file->entry);
	archive_strcpy(&(file->parentdir), pathname);
	len = file->parentdir.length;
	if (len == 0) {/* virtual root */
		file->dircnt = 0;
		return;
	}
	p = dirname = file->parentdir.s;
	/*
	 * Convert a path-separator from '\' to  '/'
	 */
	cleanup_backslash(p, len);

	if (p[0] == '/') {
		p++;
		len--;
	}
	/*
	 * Remove leading '../' and './' elements
	 */
	while (*p) {
		if (p[0] != '.')
			break;
		if (p[1] == '.' && p[2] == '/') {
			p += 3;
			len -= 3;
		} else if (p[1] == '/') {
			p += 2;
			len -= 2;
		} else
			break;
	}
	if (p != dirname) {
		memmove(dirname, p, len+1);
		p = dirname;
	}
	/*
	 * Remove "/","/." and "/.." elements from tail.
	 */
	while (len > 0) {
		size_t ll = len;

		if (len > 0 && p[len-1] == '/') {
			p[len-1] = '\0';
			len--;
		}
		if (len > 1 && p[len-2] == '/' && p[len-1] == '.') {
			p[len-2] = '\0';
			len -= 2;
		}
		if (len > 2 && p[len-3] == '/' && p[len-2] == '.' &&
		    p[len-1] == '.') {
			p[len-3] = '\0';
			len -= 3;
		}
		if (ll == len)
			break;
	}
	while (*p) {
		if (p[0] == '/') {
			if (p[1] == '/')
				/* Convert '//' --> '/' */
				strcpy(p, p+1);
			else if (p[1] == '.' && p[2] == '/')
				/* Convert '/./' --> '/' */
				strcpy(p, p+2);
			else if (p[1] == '.' && p[2] == '.' && p[3] == '/') {
				/* Convert 'dir/dir1/../dir2/'
				 *     --> 'dir/dir2/'
				 */
				char *rp = p -1;
				while (rp >= dirname) {
					if (*rp == '/')
						break;
					--rp;
				}
				if (rp > dirname) {
					strcpy(rp, p+3);
					p = rp;
				} else {
					strcpy(dirname, p+4);
					p = dirname;
				}
			} else
				p++;
		} else
			p++;
	}
	p = dirname;
	len = strlen(p);

	if (archive_entry_filetype(file->entry) == AE_IFLNK) {
		/* Convert symlink name too. */
		pathname = archive_entry_symlink(file->entry);
		archive_strcpy(&(file->symlink),  pathname);
		cleanup_backslash(file->symlink.s, file->symlink.length);
	}
	/*
	 * - Count up directory elements.
	 * - Find out the position which points the last position of
	 *   path separator('/').
	 */
	slash = NULL;
	file->dircnt = 0;
	for (; *p != '\0'; p++)
		if (*p == '/') {
			slash = p;
			file->dircnt++;
		}
	if (slash == NULL) {
		/* The pathname doesn't have a parent directory. */
		file->parentdir.length = len;
		archive_string_copy(&(file->basename), &(file->parentdir));
		archive_string_empty(&(file->parentdir));
		file->parentdir.s = '\0';
		return;
	}

	/* Make a basename from dirname and slash */
	*slash  = '\0';
	file->parentdir.length = slash - dirname;
	archive_strcpy(&(file->basename),  slash + 1);
	if (archive_entry_filetype(file->entry) == AE_IFDIR)
		file->dircnt ++;
}

static void
get_parent_and_base(struct archive_string *parentdir,
    struct archive_string *basename, const char *pathname)
{
	char *p, *slash;

	archive_strcpy(parentdir, pathname);
	/*
	 * Convert a path-separator from '\' to  '/'
	 */
	cleanup_backslash(parentdir->s, parentdir->length);
	/*
	 * - Count up directory elements.
	 * - Find out the position which points the last position of
	 *   path separator('/').
	 */
	slash = NULL;
	p = parentdir->s + parentdir->length -1;
	if (*p == '/') {
	 	/* Remove '/' from the end of path. */
		*p-- = '\0';
		parentdir->length--;
	}
	for (;p >= parentdir->s; p--)
		if (*p == '/') {
			slash = p;
			break;
		}
	if (slash == NULL) {
		/* The pathname doesn't have a parent directory. */
		archive_string_copy(basename, parentdir);
		archive_string_empty(parentdir);
		parentdir->s = '\0';
	} else {
		/* Make a basename from dirname and slash */
		*slash  = '\0';
		parentdir->length = slash - parentdir->s;
		archive_strcpy(basename,  slash + 1);
	}
}

/*
 * Register a entry to get a hardlink target.
 */
static int
isofile_register_hardlink(struct archive_write *a, struct isofile *file)
{
	struct iso9660 *iso9660 = a->format_data;
	struct hardlink *hl;
	const char *pathname, *p;
	uint32_t hash;

	pathname = archive_entry_hardlink(file->entry);
	if (pathname == NULL)
		pathname = archive_entry_pathname(file->entry);
	hash = 0;
	for (p = pathname; *p; p++)
		hash =  (hash<<1) ^ *p;
	hash &= HARDLINK_HASH_MASK;
	for (hl = iso9660->hardlink[hash].first;
	    hl != NULL; hl = hl->next) {
		if (strcmp(pathname,
		    archive_entry_pathname(hl->file_list.first->entry)) == 0)
			break;
	}
	if (hl == NULL) {
		hl = malloc(sizeof(*hl));
		if (hl == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate memory");
			return (ARCHIVE_FATAL);
		}
		hl->nlink = 0;
		hl->file_list.first = NULL;
		hl->file_list.last = &(hl->file_list.first);
		/* Insert `hl` into the tail of iso9660->hardlink[hash] */
		hl->next = NULL;
		*iso9660->hardlink[hash].last = hl;
		iso9660->hardlink[hash].last = &(hl->next);
	}
	if (archive_entry_hardlink(file->entry) == NULL) {
		/* Insert `file` into the head.
		 * A hardlink target must be the first position. */
		if ((file->hlnext = hl->file_list.first) == NULL)
			hl->file_list.last = &(file->hlnext);
		hl->file_list.first = file;
	} else {
		/* Insert `file` into the tail. */
		file->hlnext = NULL;
		*hl->file_list.last = file;
		hl->file_list.last = &(file->hlnext);
	}
	hl->nlink++;

	return (ARCHIVE_OK);
}

/*
 * Hardlinked files have to have the same location of extent.
 * We have to find out hardlink target entries for the entries
 * which referrence that target entries.
 */
static void
isofile_connect_hardlink_files(struct iso9660 *iso9660)
{
	struct hardlink *hl;
	struct isofile *target, *nf;
	int i;

	for (i = 0; i < HARDLINK_HASH_SIZE; i++) {
		for (hl = iso9660->hardlink[i].first;
		    hl != NULL; hl = hl->next) {
			target = hl->file_list.first;
			if (archive_entry_hardlink(target->entry) != NULL)
				continue;
			archive_entry_set_nlink(target->entry, hl->nlink);
			for (nf = target->hlnext;
			    nf != NULL; nf = nf->hlnext) {
				nf->hardlink_target = target;
				archive_entry_set_nlink(
				    nf->entry, hl->nlink);
			}
		}
	}
}

static void
isofile_init_hardlinks(struct iso9660 *iso9660)
{
	int i;

	for (i = 0; i < HARDLINK_HASH_SIZE; i++) {
		iso9660->hardlink[i].first = NULL;
		iso9660->hardlink[i].last = &(iso9660->hardlink[i].first);
	}
}

static void
isofile_free_hardlinks(struct iso9660 *iso9660)
{
	struct hardlink *hl, *hl_next;
	int i;

	for (i = 0; i < HARDLINK_HASH_SIZE; i++) {
		hl = iso9660->hardlink[i].first;
		while (hl != NULL) {
			hl_next = hl->next;
			free(hl);
			hl = hl_next;
		}
	}
}

static struct isoent *
isoent_new(struct isofile *file)
{
	struct isoent *isoent;

	isoent = calloc(1, sizeof(*isoent));
	if (isoent == NULL)
		return (NULL);
	isoent->file = file;
	isoent->children.first = NULL;
	isoent->children.last = &(isoent->children.first);
	isoent->extr_rec_list.first = NULL;
	isoent->extr_rec_list.last = &(isoent->extr_rec_list.first);
	isoent->extr_rec_list.current = NULL;
	if (archive_entry_filetype(file->entry) == AE_IFDIR)
		isoent->dir = 1;

	return (isoent);
}

static inline struct isoent *
isoent_clone(struct isoent *src)
{
	return (isoent_new(src->file));
}

static void
_isoent_free(struct isoent *isoent)
{
	struct extr_rec *er, *er_next;

	free(isoent->children_sorted);
	free(isoent->identifier);
	er = isoent->extr_rec_list.first;
	while (er != NULL) {
		er_next = er->next;
		free(er);
		er = er_next;
	}
	free(isoent);
}

static void
isoent_free_all(struct isoent *isoent)
{
	struct isoent *np, *np_temp;

	if (isoent == NULL)
		return;
	np = isoent;
	for (;;) {
		if (np->dir) {
			if (np->children.first != NULL) {
				/* Enter to sub directories. */
				np = np->children.first;
				continue;
			}
		}
		for (;;) {
			np_temp = np;
			if (np->chnext == NULL) {
				/* Return to the parent directory. */
				np = np->parent;
				_isoent_free(np_temp);
				if (np == np_temp)
					return;
			} else {
				np = np->chnext;
				_isoent_free(np_temp);
				break;
			}
		}
	}
}

static struct isoent *
isoent_create_virtual_dir(struct iso9660 *iso9660, const char *pathname)
{
	struct isofile *file;
	struct isoent *isoent;

	file = isofile_new(NULL);
	if (file == NULL)
		return (NULL);
	archive_entry_set_pathname(file->entry, pathname);
	archive_entry_unset_mtime(file->entry);
	archive_entry_unset_atime(file->entry);
	archive_entry_unset_ctime(file->entry);
	archive_entry_set_uid(file->entry, getuid());
	archive_entry_set_gid(file->entry, getgid());
	archive_entry_set_mode(file->entry, 0555 | AE_IFDIR);
	archive_entry_set_nlink(file->entry, 2);
	isofile_gen_utility_names(file);
	isofile_add_entry(iso9660, file);

	isoent = isoent_new(file);
	if (isoent == NULL)
		return (NULL);
	isoent->dir = 1;
	isoent->virtual = 1;

	return (isoent);
}

static inline void
isoent_add_child_head(struct isoent *parent, struct isoent *child)
{
	if ((child->chnext = parent->children.first) == NULL)
		parent->children.last = &(child->chnext);	
	parent->children.first = child;
	parent->children.cnt++;
	child->parent = parent;
}

static inline void
isoent_add_child_tail(struct isoent *parent, struct isoent *child)
{
	child->chnext = NULL;
	*parent->children.last = child;
	parent->children.last = &(child->chnext);
	parent->children.cnt++;
	child->parent = parent;
}

/*
 * Trim extra directories.
 * If the root directory is a virtual directory and has one sub-directory
 * only, the sub-direcotry become a new root directory.
 */
static void
isoent_trim_root_directory(struct iso9660 *iso9660)
{
	struct isoent *rootent, *child;

	rootent = iso9660->primary.rootent;
	while (rootent->virtual) {
		if (rootent->children.cnt != 1) 
			break;
		child = rootent->children.first;
		if (child->dir) {
			_isoent_free(rootent);
			iso9660->primary.rootent = rootent = child;
			rootent->parent = rootent;
			iso9660->dircnt_max--;
		} else
			break;
	}
}

static int
isoent_clone_tree(struct archive_write *a, struct isoent **nroot,
    struct isoent *root)
{
	struct isoent *np, *xroot, *newent;

	np = root;
	xroot = NULL;
	do {
		newent = isoent_clone(np);
		if (newent == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate memory");
			return (ARCHIVE_FATAL);
		}
		if (xroot == NULL) {
			*nroot = xroot = newent;
			newent->parent = xroot;
		} else
			isoent_add_child_tail(xroot, newent);
		if (np->dir && np->children.first != NULL) {
			/* Enter to sub directories. */
			np = np->children.first;
			xroot = newent;
			continue;
		}
		while (np != np->parent) {
			if (np->chnext == NULL) {
				/* Return to the parent directory. */
				np = np->parent;
				xroot = xroot->parent;
			} else {
				np = np->chnext;
				break;
			}
		}
	} while (np != np->parent);

	return (ARCHIVE_OK);
}

/*
 * Setup directory locations.
 */
static void
isoent_setup_directory_location(struct iso9660 *iso9660, int location,
    struct vdd *vdd)
{
	struct isoent *np;
	int depth;

	vdd->total_dir_block = 0;
	depth = 0;
	np = vdd->rootent;
	do {
		if (np->dir) {
			int block;

			np->dir_block = calculate_directory_descriptors(
			    iso9660, vdd, np, depth);
			vdd->total_dir_block += np->dir_block;
			np->dir_location = location;
			location += np->dir_block;
			block = extra_setup_location(np, location);
			vdd->total_dir_block += block;
			location += block;

			if (np->children.first != NULL &&
			    depth + 1 < vdd->max_depth) {
				/* Enter to sub directories. */
				np = np->children.first;
				depth++;
				continue;
			}
		}
		while (np != np->parent) {
			if (np->chnext == NULL) {
				/* Return to the parent directory. */
				np = np->parent;
				depth--;
			} else {
				np = np->chnext;
				break;
			}
		}
	} while (np != np->parent);
}

static void
_isoent_file_location(struct iso9660 *iso9660, struct isoent *isoent,
    int *location, int *symlocation)
{
	struct isoent **children;
	int total_block;
	int n;

	if (isoent->children.cnt == 0)
		return;

	total_block = 0;
	children = isoent->children_sorted;
	for (n = 0; n < isoent->children.cnt; n++) {
		struct isoent *np;
		struct isofile *file;

		np = children[n];
		if (np->dir)
			continue;
		file = np->file;
		if (file->boot || file->hardlink_target != NULL)
			continue;
		if (archive_entry_filetype(file->entry) == AE_IFLNK ||
		    file->content.size == 0) {
			/*
			 * Do not point a valid location.
			 * Make sure entry is not hardlink file.
			 */
			file->content.location = (*symlocation)--;
			continue;
		}

		file->cur_content = &(file->content);
		do {
			int block;

			file->cur_content->location = *location;
			block = (file->cur_content->size
				  + LOGICAL_BLOCK_SIZE -1)
				/ LOGICAL_BLOCK_SIZE;
			*location += block;
			total_block += block;
			/* Next fragument */
			file->cur_content = file->cur_content->next;
		} while (file->cur_content != NULL);
	}
	iso9660->total_file_block += total_block;
}

/*
 * Setup file locations.
 */
static void
isoent_setup_file_location(struct iso9660 *iso9660, int location)
{
	struct isoent *isoent;
	struct isoent *np;
	size_t size;
	int block;
	int depth;
	int joliet;
	int symlocation;

	iso9660->total_file_block = 0;
	if ((isoent = iso9660->el_torito.catalog) != NULL) {
		isoent->file->content.location = location;
		block = (archive_entry_size(isoent->file->entry) +
		    LOGICAL_BLOCK_SIZE -1) / LOGICAL_BLOCK_SIZE;
		location += block;
		iso9660->total_file_block += block;
	}
	if ((isoent = iso9660->el_torito.boot) != NULL) {
		isoent->file->content.location = location;
		size = fd_boot_image_size(iso9660->el_torito.media_type);
		if (size == 0)
			size = archive_entry_size(isoent->file->entry);
		block = (size + LOGICAL_BLOCK_SIZE -1) / LOGICAL_BLOCK_SIZE;
		location += block;
		iso9660->total_file_block += block;
	}

	depth = 0;
	symlocation = -16;
	if (!iso9660->opt.rr && iso9660->opt.joliet) {
		joliet = 1;
		np = iso9660->joliet.rootent;
	} else {
		joliet = 0;
		np = iso9660->primary.rootent;
	}
	do {
		if (np->dir) {
			_isoent_file_location(iso9660, np,
			    &location, &symlocation);

			if (np->children.first != NULL &&
			    (joliet ||
			    ((iso9660->opt.rr == OPT_RR_DISABLED &&
			      depth + 2 < iso9660->primary.max_depth) ||
			     (iso9660->opt.rr &&
			      depth + 1 < iso9660->primary.max_depth)))) {
				/* Enter to sub directories. */
				np = np->children.first;
				depth++;
				continue;
			}
		}
		while (np != np->parent) {
			if (np->chnext == NULL) {
				/* Return to the parent directory. */
				np = np->parent;
				depth--;
			} else {
				np = np->chnext;
				break;
			}
		}
	} while (np != np->parent);
}

static int
get_path_component(char *name, int n, const char *fn)
{
	char *p;
	int l;

	p = strchr(fn, '/');
	if (p == NULL) {
		if ((l = strlen(fn)) == 0)
			return (0);
	} else
		l = p - fn;
	if (l > n -1)
		return (-1);
	memcpy(name, fn, l);
	name[l] = '\0';

	return (l);
}

static struct isoent *
isoent_tree_add_child(struct archive_write *a, struct isoent *parent,
    struct isoent *child)
{
	struct isoent *np;

	for (np = parent->children.first; np != NULL; np = np->chnext) {
		struct isofile *f1 = np->file;
		struct isofile *f2 = child->file;

		if (f1->basename.length != f2->basename.length)
			continue;
		if (memcmp(f1->basename.s, f2->basename.s,
		    f1->basename.length) != 0)
			continue;
		/*
		 * Right now, we have two entries names of which are
		 * the same.
		 */
		/* If entries' file types are different,
		 * we cannot handle. */
		if (archive_entry_filetype(f1->entry) !=
		    archive_entry_filetype(f2->entry)) {
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_MISC,
			    "Duplicate entries(`%s' and `%s') "
			    "whose file type are different at "
			    "`%s/%s'",
			    archive_entry_pathname(f1->entry),
			    archive_entry_pathname(f2->entry),
			    f1->parentdir.s,
			    f1->basename.s);
			_isoent_free(child);
			return (NULL);
		}
		/* Ignore a new entry which we are adding, if it's virtual.
		 * We don't replace a being entry by a virtual entry. */
		if (child->virtual)
			continue;
		if (archive_entry_mtime(f1->entry) <
		    archive_entry_mtime(f2->entry) || np->virtual) {
			/* Swap files. */
			np->file = f2;
			child->file = f1;
			np->virtual = 0;
		}
		_isoent_free(child);
		return (np);
	}

	isoent_add_child_tail(parent, child);
	return (child);
}

/*
 * Add a new entry into the tree.
 */
static struct isoent *
isoent_tree(struct archive_write *a, struct isoent *isoent)
{
#if defined(_WIN32) && !defined(__CYGWIN__)
	char name[_MAX_FNAME];/* Included null terminator size. */
#else
	char name[NAME_MAX+1];
#endif
	struct iso9660 *iso9660 = a->format_data;
	struct isoent *dent, *np;
	const char *fn, *p;
	int l;

	dent = iso9660->primary.rootent;
	if (isoent->file->parentdir.length > 0)
		fn = p = isoent->file->parentdir.s;
	else
		fn = p = "";
	if (iso9660->cur_dirent != NULL) {
		/*
		 * If isoent's parent directory is the same as a cur_dirent
		 * path, insert isoent into the cur_dirent.
		 */
		struct isoent *curdir = iso9660->cur_dirent;
		int plen = curdir->file->parentdir.length;

		if (plen == 0 ||
		     (strncmp(p, curdir->file->parentdir.s, plen) == 0 &&
		      p[plen] == '/')) {
			if (plen > 0)
				plen++;
			if (curdir->file->basename.length > 0 &&
			    strcmp(p+plen, curdir->file->basename.s) == 0)
				return (isoent_tree_add_child(a, curdir, isoent));
		}
	}

	l = get_path_component(name, sizeof(name), fn);
	np = dent->children.first;
	for (;;) {
		if (np == NULL && fn[0] != '\0') {
			/*
			 * Create a virtual directory.
			 */
			struct isoent *vp;
			struct archive_string as;

			archive_string_init(&as);
			__archive_string_append(&as, p, fn - p + l);
			if (as.s[as.length-1] == '/') {
				as.s[as.length-1] = '\0';
				as.length--;
			}
			vp = isoent_create_virtual_dir(iso9660, as.s);
			if (vp == NULL) {
				archive_string_free(&as);
				archive_set_error(&a->archive, ENOMEM,
				    "Can't allocate memory");
				_isoent_free(isoent);
				return (NULL);
			}
			archive_string_free(&as);

			if (vp->file->dircnt > iso9660->dircnt_max)
				iso9660->dircnt_max = vp->file->dircnt;
			isoent_add_child_tail(dent, vp);
			np = vp;
		}

		if (np != NULL && fn[0] != '\0') {
			/* Now we are finding a parent directory. */
			if (strcmp(name, np->file->basename.s) == 0) {
				/* Enter sub directories. */
				fn += l;
				if (fn[0] == '/')
					fn++;
				l = get_path_component(name,
				    sizeof(name), fn);
				dent = np;
				np = dent->children.first;
				continue;
			}
		} else {
			/* Found where isoent can be inserted. */
			iso9660->cur_dirent = dent;
			return (isoent_tree_add_child(a, dent, isoent));
		}
		np = np->chnext;
	}
}

/*
 * Following idr_* functions are used for resolving duplicated filenames
 * and unreceivable filenames to generate ISO9660/Joliet Identifiers.
 */

static void
idr_relaxed_filenames(char *map)
{
	int i;

	for (i = 0x21; i <= 0x2F; i++)
		map[i] = 1;
	for (i = 0x3A; i <= 0x41; i++)
		map[i] = 1;
	for (i = 0x5B; i <= 0x5E; i++)
		map[i] = 1;
	map[0x60] = 1;
	for (i = 0x7B; i <= 0x7E; i++)
		map[i] = 1;
}

static void
idr_allow_sharp_tilde(char *map)
{
	map[0x23] = 1;/* Allow '#' */
	map[0x7e] = 1;/* Allow '~' */
}

static void
idr_init(struct iso9660 *iso9660, struct vdd *vdd, struct idr *idr)
{

	idr->wlistpool = NULL;
	idr->flistpool = NULL;
	idr->pool_size = 0;
	if (vdd->vdd_type != VDD_JOLIET) {
		if (iso9660->opt.iso_level <= 3) {
			if (iso9660->opt.allow_lowercase)
				memcpy(idr->char_map, d1_characters_map,
				    sizeof(idr->char_map));
			else
				memcpy(idr->char_map, d_characters_map,
				    sizeof(idr->char_map));
			if (iso9660->opt.relaxed_filenames)
				idr_relaxed_filenames(idr->char_map);
			if (iso9660->opt.allow_sharp_tilde)
				idr_allow_sharp_tilde(idr->char_map);
		} else {
			memcpy(idr->char_map, d1_characters_map,
			    sizeof(idr->char_map));
			idr_relaxed_filenames(idr->char_map);
		}
	}
}

static void
idr_cleanup(struct idr *idr)
{
	free(idr->wlistpool);
	free(idr->flistpool);
}

static inline void
idr_llist_insert(struct llist *llist, struct wlist *wnp)
{
	if ((wnp->elqueue.next = llist->first) != NULL)
		llist->first->elqueue.prev = &(wnp->elqueue.next);
	else
		llist->last = &(wnp->elqueue.next);
	llist->first = wnp;
	wnp->elqueue.prev = &(llist->first);
}

static inline void
idr_llist_remove(struct llist *llist, struct wlist *wnp)
{
	if (wnp->elqueue.next != NULL)
		wnp->elqueue.next->elqueue.prev = wnp->elqueue.prev;
	else
		llist->last = wnp->elqueue.prev;
	*wnp->elqueue.prev = wnp->elqueue.next;
}

static int
idr_ensure_poolsize(struct archive_write *a, struct idr *idr,
    int cnt)
{

	if (idr->pool_size < cnt) {
		const int bk = (1 << 7) - 1;
		int psize;

		psize = (cnt + bk) & ~bk;
		free(idr->wlistpool);
		idr->wlistpool = malloc(sizeof(struct wlist) * psize);
		if (idr->wlistpool == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate memory");
			return (ARCHIVE_FATAL);
		}
		free(idr->flistpool);
		idr->flistpool = malloc(sizeof(struct flist) * psize);
		if (idr->flistpool == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate memory");
			return (ARCHIVE_FATAL);
		}
		idr->pool_size = psize;
	}
	return (ARCHIVE_OK);
}

static int
idr_start(struct archive_write *a, struct idr *idr, int cnt, int ffmax)
{
	int i, r;

	r = idr_ensure_poolsize(a, idr, cnt);
	if (r != ARCHIVE_OK)
		return (r);
	idr->wlistidx = 0;
	idr->flistidx = 0;
	if (ffmax > IDR_LLIST_MAX)
		ffmax = IDR_LLIST_MAX;
	for (i = 0; i < ffmax; i++) {
		idr->llist[i].first = NULL;
		idr->llist[i].last = &(idr->llist[i].first);
	}
	return (ARCHIVE_OK);
}

static inline void
idr_flist_link_add(struct flist_link *fll, struct wlist *wl)
{
	if ((wl->next = fll->first) == NULL)
		fll->last = &(wl->next);
	fll->first = wl;
}

static struct wlist *
idr_flist_add(struct idr *idr, struct flist *fl, struct isoent *ent,
    int weight, int noff)
{
	struct wlist *wlist;

	if (fl == NULL) {
		fl = &idr->flistpool[idr->flistidx++];
		fl->pri.first = NULL;
		fl->pri.last = &(fl->pri.first);
		fl->low.first = NULL;
		fl->low.last = &(fl->low.first);
		fl->isoent = ent;
		fl->noff = noff;
	}

	wlist = &idr->wlistpool[idr->wlistidx++];
	wlist->isoent = ent;
	wlist->weight = weight;
	wlist->noff = noff;
	if (weight < 0)
		idr_flist_link_add(&(fl->pri), wlist);
	else
		idr_flist_link_add(&(fl->low), wlist);
	return (wlist);
}

static void
idr_register(struct idr *idr, struct isoent *ent, int weight,
    int noff, int nsize)
{
	struct wlist *wlist;
	struct flist *fl;
	const char *id;
	int cmp, cmplen, ext_off;
	int i;

	wlist = NULL;
	ext_off = ent->ext_off;
	for (i = 0; i < idr->flistidx; i++) {
		fl = &(idr->flistpool[i]);
		if (fl->noff != noff)
			continue;
		id = fl->isoent->identifier;
		if (noff < 0) {
			cmplen = ent->id_len - nsize;
			if (fl->isoent->id_len < cmplen)
				cmp = 1;
			else
				cmp = memcmp(id, ent->identifier,
				    cmplen);
		} else {
			cmp = memcmp(id, ent->identifier, noff);
			if (cmp == 0) {
				cmplen = fl->isoent->ext_len;
				if (cmplen != ent->ext_len)
					cmp = 1;
				else
					cmp = memcmp(id+ext_off,
					    &ent->identifier[ext_off],
					    cmplen);
			}
		}
		if (cmp == 0) {
			wlist = idr_flist_add(
			    idr, fl, ent, weight, noff);
			break;
		}
	}
	if (wlist == NULL)
		wlist = idr_flist_add(idr, NULL, ent, weight, noff);
	if (ext_off >= nsize && noff + nsize != ext_off)
		idr_llist_insert(&idr->llist[ext_off-nsize], wlist);
}

static struct llist *
idr_extend_identifier(struct llist *llist, struct wlist *wnp, int numsize,
    int nullsize)
{
	unsigned char *p;
	int wnp_ext_off;

	wnp_ext_off = wnp->isoent->ext_off;
	if (wnp->noff + numsize != wnp_ext_off) {
		p = (unsigned char *)wnp->isoent->identifier;
		/* Extend the filename; foo.c --> foo___.c */
		if (wnp_ext_off >= numsize)
			idr_llist_remove(&llist[wnp_ext_off - numsize], wnp);
		memmove(p + wnp->noff + numsize, p + wnp_ext_off,
		    wnp->isoent->ext_len + nullsize);
		wnp->isoent->ext_off = wnp_ext_off = wnp->noff + numsize;
		wnp->isoent->id_len = wnp_ext_off + wnp->isoent->ext_len;
	}
	return (&llist[wnp_ext_off - numsize]);
}

static int
idr_has_same_identifier(struct llist *llist, struct wlist *wnp)
{
	unsigned char *p;
	struct wlist *wnptmp, *w2;
	int p_len;

	p = (unsigned char *)wnp->isoent->identifier;
	p_len = wnp->isoent->id_len;

	for (wnptmp = wnp->next; wnptmp != NULL; wnptmp = wnptmp->next) {
		if (p_len == wnptmp->isoent->id_len &&
		    memcmp(p, wnptmp->isoent->identifier, p_len) == 0)
			return (1); /* Found in following entries */
	}
	if (llist == NULL)
		return (0);/* Not Found */

	for (w2 = llist->first; w2 != NULL; w2 = w2->elqueue.next) {
		if (p_len == w2->isoent->id_len &&
		    memcmp(p, w2->isoent->identifier, p_len) == 0)
			return (1); /* Found */
	}
	idr_llist_insert(llist, wnp);

	return (0);
}

static void
idr_resolve(struct idr *idr, int numsize, int nullsize,
    void (*fsetnum)(unsigned char *p, int num))
{
	struct flist *fl;
	unsigned char *p;
	int i;

	/* Find out the same identifier. */
	for (i = 0; i < idr->flistidx; i++) {
		struct wlist *wnp;
		int num = 0;

		fl = &(idr->flistpool[i]);
		if (fl->pri.first != NULL) {
			*fl->low.last = fl->pri.first;
			fl->low.last = fl->pri.last;
		}
		if (fl->low.first == NULL)
			break;

		for (wnp = fl->low.first; wnp->next != NULL;
		    wnp = wnp->next) {
			struct llist *ep;

			if (!idr_has_same_identifier(NULL, wnp))
				continue;
			ep = idr_extend_identifier(
			    idr->llist, wnp, numsize, nullsize);
			p = (unsigned char *)wnp->isoent->identifier
			    + wnp->noff;
			do {
				fsetnum(p, num++);
			} while (idr_has_same_identifier(ep, wnp));
		}
	}
}

static void
idr_set_num(unsigned char *p, int num)
{
	static const char xdig[] = {
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
		'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
		'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
		'U', 'V', 'W', 'X', 'Y', 'Z'
	};

	num %= sizeof(xdig) * sizeof(xdig) * sizeof(xdig);
	p[0] = xdig[(num / (sizeof(xdig) * sizeof(xdig)))];
	num %= sizeof(xdig) * sizeof(xdig);
	p[1] = xdig[ (num / sizeof(xdig))];
	num %= sizeof(xdig);
	p[2] = xdig[num];
}

static void
idr_set_num_beutf16(unsigned char *p, int num)
{
	static const uint16_t xdig[] = {
		0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035,
		0x0036, 0x0037, 0x0038, 0x0039,
		0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046,
		0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C,
		0x004D, 0x004E, 0x004F, 0x0050, 0x0051, 0x0052,
		0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058,
		0x0059, 0x005A
	};
#define XDIG_CNT	(sizeof(xdig)/sizeof(xdig[0]))

	num %= XDIG_CNT * XDIG_CNT * XDIG_CNT;
	archive_be16enc(p, xdig[(num / (XDIG_CNT * XDIG_CNT))]);
	num %= XDIG_CNT * XDIG_CNT;
	archive_be16enc(p+2, xdig[ (num / XDIG_CNT)]);
	num %= XDIG_CNT;
	archive_be16enc(p+4, xdig[num]);
}

/*
 * Generate ISO9660 Identifier.
 */
static int
isoent_gen_iso9660_identifier(struct archive_write *a, struct isoent *isoent,
    struct idr *idr)
{
	struct iso9660 *iso9660;
	struct isoent *np;
	char *p;
	int l, r;
	const char *char_map;
	char allow_ldots, allow_multidot, allow_period, allow_vernum;
	int fnmax, ffmax, dnmax;

	if (isoent->children.cnt == 0)
		return (0);

	iso9660 = a->format_data;
	char_map = idr->char_map;
	if (iso9660->opt.iso_level <= 3) {
		allow_ldots = iso9660->opt.allow_ldots;
		allow_multidot = iso9660->opt.allow_multidot;
		allow_period = iso9660->opt.allow_period;
		allow_vernum = iso9660->opt.allow_vernum;
		if (iso9660->opt.iso_level == 1) {
			fnmax = 8;
			ffmax = 12;/* fnmax + '.' + 3 */
			dnmax = 8;
		} else {
			fnmax = 30;
			ffmax = 31;
			dnmax = 31;
		}
	} else {
		allow_ldots = allow_multidot = 1;
		allow_period = allow_vernum = 0;
		if (iso9660->opt.rr)
			/*
			 * MDR : The maximum size of Directory Record(254).
			 * DRL : A Directory Record Length(33).
			 * CE  : A size of SUSP CE System Use Entry(28).
			 * MDR - DRL - CE = 254 - 33 - 28 = 193.
			 */
			fnmax = ffmax = dnmax = 193;
		else
			/*
			 * XA  : CD-ROM XA System Use Extension
			 *       Information(14).
			 * MDR - DRL - XA = 254 - 33 -14 = 207.
			 */
			fnmax = ffmax = dnmax = 207;
	}

	r = idr_start(a, idr, isoent->children.cnt, ffmax);
	if (r < 0)
		return (r);

	for (np = isoent->children.first; np != NULL; np = np->chnext) {
		char *dot, *xdot;
		int ext_off, noff, weight;

		l = np->file->basename.length;
		p = malloc(l+31+2+1);
		if (p == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate memory");
			return (ARCHIVE_FATAL);
		}
		memcpy(p, np->file->basename.s, l);
		p[l] = '\0';
		np->identifier = p;

		dot = xdot = NULL;
		if (!allow_ldots) {
			/*
			 * If there is a '.' character at the first byte,
			 * it has to be replaced by '_' character.
			 */
			if (*p == '.')
				*p++ = '_';
		}
		for (;*p; p++) {
			if (*p & 0x80) {
				*p = '_';
				continue;
			}
			if (char_map[(unsigned char)*p]) {
				/* if relaxed-filenames option specified or
				 * iso-level is '4', a character '.' is
				 * allowed by char_map. */
				if (*p == '.') {
					xdot = dot;
					dot = p;
				}
				continue;
			}
			if (*p >= 'a' && *p <= 'z') {
				*p -= 'a' - 'A';
				continue;
			}
			if (*p == '.') {
				xdot = dot;
				dot = p;
				if (allow_multidot)
					continue;
			}
			*p = '_';
		}
		p = np->identifier;
		weight = -1;
		if (dot == NULL) {
			int nammax;

			if (np->dir)
				nammax = dnmax;
			else
				nammax = fnmax;

			if (l > nammax) {
				p[nammax] = '\0';
				weight = nammax;
				ext_off = nammax;
			} else
				ext_off = l;
		} else {
			*dot = '.';
			ext_off = dot - p;

			if (iso9660->opt.iso_level == 1) {
				if (dot - p <= 8) {
					if (strlen(dot) > 4) {
						/* A length of a file extension
						 * must be less than 4 */
						dot[4] = '\0';
						weight = 0;
					}
				} else {
					p[8] = dot[0];
					p[9] = dot[1];
					p[10] = dot[2];
					p[11] = dot[3];
					p[12] = '\0';
					weight = 8;
					ext_off = 8;
				}
			} else if (np->dir) {
				if (l > dnmax) {
					p[dnmax] = '\0';
					weight = dnmax;
					if (ext_off > dnmax)
						ext_off = dnmax;
				}
			} else if (l > ffmax) {
				int extlen = strlen(dot);
				int xdoff;

				if (xdot != NULL)
					xdoff = xdot - p;
				else
					xdoff = 0;

				if (extlen > 1 && xdoff < fnmax-1) {
					int off;

					if (extlen > ffmax)
						extlen = ffmax;
					off = ffmax - extlen;
					if (off == 0) {
						/* A dot('.')  character
						 * does't place to the first
						 * byte of identifier. */
						off ++;
						extlen --;
					}
					memcpy(p+off, dot, extlen);
					p[ffmax] = '\0';
					ext_off = off;
					weight = off;
#ifdef COMPAT_MKISOFS
				} else if (xdoff >= fnmax-1) {
					/* Simulate a bug(?) of mkisofs. */
					p[fnmax-1] = '\0';
					ext_off = fnmax-1;
					weight = fnmax-1;
#endif
				} else {
					p[fnmax] = '\0';
					ext_off = fnmax;
					weight = fnmax;
				}
			}
		}
		/* Save a offset of a file name extension to sort files. */
		np->ext_off = ext_off;
		np->ext_len = strlen(&p[ext_off]);
		np->id_len = l = ext_off + np->ext_len;

		/* Make a offset of the number which is used to be set
		 * hexadecimal number to avoid duplicate identififier. */
		if (iso9660->opt.iso_level == 1) {
			if (ext_off >= 5)
				noff = 5;
			else
				noff = ext_off;
		} else {
			if (l == ffmax)
				noff = ext_off - 3;
			else if (l == ffmax-1)
				noff = ext_off - 2;
			else if (l == ffmax-2)
				noff = ext_off - 1;
			else
				noff = ext_off;
		}
		/* Register entry to the identifier resolver. */
		idr_register(idr, np, weight, noff, 3);
	}

	/* Resolve duplicate identifier. */ 
	idr_resolve(idr, 3, 1, idr_set_num);

	/* Add a period and a version number to identifiers. */
	for (np = isoent->children.first; np != NULL; np = np->chnext) {
		if (!np->dir && np->rr_child == NULL) {
			p = np->identifier + np->ext_off + np->ext_len;
			if (np->ext_len == 0 && allow_period) {
				*p++ = '.';
				np->ext_len = 1;
			}
			if (np->ext_len == 1 && !allow_period) {
				*--p = '\0';
				np->ext_len = 0;
			}
			np->id_len = np->ext_off + np->ext_len;
			if (allow_vernum) {
				*p++ = ';';
				*p++ = '1';
				np->id_len += 2;
			}
			*p = '\0';
		} else
			np->id_len = np->ext_off + np->ext_len;
		np->mb_len = np->id_len;
	}

	return (ARCHIVE_OK);
}

/*
 * Generate Joliet Identifier.
 */
static int
isoent_gen_joliet_identifier(struct archive_write *a, struct isoent *isoent,
    struct idr *idr)
{
	struct iso9660 *iso9660;
	struct isoent *np;
	unsigned char *p;
	size_t l;
	int r;
	int ffmax, parent_len;

	if (isoent->children.cnt == 0)
		return (0);

	iso9660 = a->format_data;
	if (iso9660->opt.joliet == OPT_JOLIET_LONGNAME)
		ffmax = 206;
	else
		ffmax = 128;

	r = idr_start(a, idr, isoent->children.cnt, ffmax);
	if (r < 0)
		return (r);

	parent_len = 1;
	for (np = isoent; np->parent != np; np = np->parent)
		parent_len += np->mb_len + 1;

	for (np = isoent->children.first; np != NULL; np = np->chnext) {
		unsigned char *dot;
		int ext_off, noff, weight;
		size_t lt;

		l = mbstoutf16s(NULL, 0, np->file->basename.s,
		    np->file->basename.length);
		p = malloc(l+6+2);
		if (p == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate memory");
			return (ARCHIVE_FATAL);
		}
		l = mbstoutf16s(p, l, np->file->basename.s,
		    np->file->basename.length);
		if ((int)l > ffmax)
			l = ffmax;
		np->identifier = (char *)p;
		lt = l;
		dot = p + l;
		weight = 0;
		while (lt > 0) {
			if (!joliet_allowed_char(*(uint16_t *)p))
				*(uint16_t *)p = 0x005F; /* '_' */
			else if (*(uint16_t*)p == 0x002E) /* '.' */
				dot = p;
			p += 2;
			lt -= 2;
		}
		ext_off = dot - (unsigned char *)np->identifier;
		np->ext_off = ext_off;
		np->ext_len = l - ext_off;
		np->id_len = l;

		/*
		 * Check full-path name length.
		 */
		if ((int)l == ffmax) {
			np->mb_len = mblen_of_utf16(
			    (const unsigned char *)np->identifier, l);
			if (np->mb_len != (int)np->file->basename.length)
				weight = np->mb_len;
		} else
			np->mb_len = np->file->basename.length;
		/* Convert UTF16 to big endian UTF16. */
		utf16stobeutf16s((unsigned char *)np->identifier, l);

		/* If length of full path name is longer than 240,
		 * it violates Joliet extensions regulation. */
		if (parent_len + np->mb_len > 240) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "A lenght of a full-path name of `%s' is "
			    "longer than 240 bytes.",
			    archive_entry_pathname(np->file->entry));
			return (ARCHIVE_FATAL);
		}

		/* Make a offset of the number which is used to be set
		 * hexadecimal number to avoid duplicate identififier. */
		if ((int)l == ffmax)
			noff = ext_off - 6;
		else if ((int)l == ffmax-2)
			noff = ext_off - 4;
		else if ((int)l == ffmax-4)
			noff = ext_off - 2;
		else
			noff = ext_off;
		/* Register entry to the identifier resolver. */
		idr_register(idr, np, weight, noff, 6);
	}

	/* Resolve duplicate identifier with Joliet Volume. */ 
	idr_resolve(idr, 6, 2, idr_set_num_beutf16);

	return (ARCHIVE_OK);
}

/*
 * This comparing rule is acording to ISO9660 Standard 9.3
 */
static int
_compare_iso9660_identifier(const void *v1, const void *v2)
{
	const struct isoent *p1, *p2;
	const char *s1, *s2;
	int cmp;
	int l;

	p1 = *((const struct isoent **)(uintptr_t)v1);
	p2 = *((const struct isoent **)(uintptr_t)v2);
	s1 = p1->identifier;
	s2 = p2->identifier;

	/* Compare File Name */
	l = p1->ext_off;
	if (l > p2->ext_off)
		l = p2->ext_off;
	cmp = memcmp(s1, s2, l);
	if (cmp != 0)
		return (cmp);
	if (p1->ext_off < p2->ext_off) {
		s2 += l;
		l = p2->ext_off - p1->ext_off;
		while (l--)
			if (0x20 != *s2++)
				return (0x20
				    - *(const unsigned char *)(s2 - 1));
	} else if (p1->ext_off > p2->ext_off) {
		s1 += l;
		l = p1->ext_off - p2->ext_off;
		while (l--)
			if (0x20 != *s1++)
				return (*(const unsigned char *)(s1 - 1)
				    - 0x20);
	}
	/* Compare File Name Extension */
	if (p1->ext_len <= 1)
		return (-1);
	if (p2->ext_len <= 1)
		return (1);
	l = p1->ext_len;
	if (l > p2->ext_len)
		l = p2->ext_len;
	s1 = p1->identifier + p1->ext_off;
	s2 = p2->identifier + p2->ext_off;
	if (l > 1) {
		cmp = memcmp(s1, s2, l);
		if (cmp != 0)
			return (cmp);
	}
	if (p1->ext_len < p2->ext_len) {
		s2 += l;
		l = p2->ext_len - p1->ext_len;
		while (l--)
			if (0x20 != *s2++)
				return (0x20
				    - *(const unsigned char *)(s2 - 1));
	} else if (p1->ext_len < p2->ext_len) {
		s1 += l;
		l = p1->ext_len - p2->ext_len;
		while (l--)
			if (0x20 != *s1++)
				return (*(const unsigned char *)(s1 - 1)
				    - 0x20);
	}
	/* Compare File Version Number */
	/* No operation. The File Version Number is always one. */

	return (cmp);
}

static int
_compare_joliet_identifier(const void *v1, const void *v2)
{
	const struct isoent *p1, *p2;
	const unsigned char *s1, *s2;
	int cmp;
	int l;

	p1 = *((const struct isoent **)(uintptr_t)v1);
	p2 = *((const struct isoent **)(uintptr_t)v2);
	s1 = (const unsigned char *)p1->identifier;
	s2 = (const unsigned char *)p2->identifier;

	/* Compare File Name */
	l = p1->ext_off;
	if (l > p2->ext_off)
		l = p2->ext_off;
	cmp = memcmp(s1, s2, l);
	if (cmp != 0)
		return (cmp);
	if (p1->ext_off < p2->ext_off) {
		s2 += l;
		l = p2->ext_off - p1->ext_off;
		while (l--)
			if (0 != *s2++)
				return (- *(const unsigned char *)(s2 - 1));
	} else if (p1->ext_off > p2->ext_off) {
		s1 += l;
		l = p1->ext_off - p2->ext_off;
		while (l--)
			if (0 != *s1++)
				return (*(const unsigned char *)(s1 - 1));
	}
	/* Compare File Name Extension */
	if (p1->ext_len <= 2)
		return (-1);
	if (p2->ext_len <= 2)
		return (1);
	l = p1->ext_len;
	if (l > p2->ext_len)
		l = p2->ext_len;
	s1 = (unsigned char *)(p1->identifier + p1->ext_off);
	s2 = (unsigned char *)(p2->identifier + p2->ext_off);
	if (l > 1) {
		cmp = memcmp(s1, s2, l);
		if (cmp != 0)
			return (cmp);
	}
	if (p1->ext_len < p2->ext_len) {
		s2 += l;
		l = p2->ext_len - p1->ext_len;
		while (l--)
			if (0 != *s2++)
				return (- *(const unsigned char *)(s2 - 1));
	} else if (p1->ext_len < p2->ext_len) {
		s1 += l;
		l = p1->ext_len - p2->ext_len;
		while (l--)
			if (0 != *s1++)
				return (*(const unsigned char *)(s1 - 1));
	}
	/* Compare File Version Number */
	/* No operation. The File Version Number is always one. */

	return (cmp);
}

static int
isoent_make_sorted_files(struct archive_write *a, struct vdd *vdd,
    struct isoent *isoent)
{
	struct isoent **children;
	struct isoent *np;
	int (*compare)(const void *v1, const void *v2);

	if (isoent->children.cnt == 0)
		return (ARCHIVE_OK);

	children = malloc(isoent->children.cnt * sizeof(struct isoent *));
	if (children == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	compare = NULL;/* gcc complain it's uninitialized. */
	isoent->children_sorted = children;
	switch (vdd->vdd_type) {
 	case VDD_PRIMARY:
 	case VDD_ENHANCED:
		compare = _compare_iso9660_identifier;
		break;
	case VDD_JOLIET:
		compare = _compare_joliet_identifier;
		break;
	}

	for (np = isoent->children.first; np != NULL; np = np->chnext)
		*children ++ = np;
	children = isoent->children_sorted;
	qsort(children, isoent->children.cnt,
	    sizeof(struct isoent *), compare);

	return (ARCHIVE_OK);
}

/*
 * - Generate ISO9660 and Joliet identifiers from basenames.
 * - Sort files by each directory.
 */
static int
isoent_traverse_tree(struct archive_write *a, struct vdd* vdd)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isoent *np;
	struct idr idr;
	int depth;
	int r;
	int (*genid)(struct archive_write *a, struct isoent *isoent,
	    struct idr *idr);

	idr_init(iso9660, vdd, &idr);
	np = vdd->rootent;
	depth = 0;
	if (vdd->vdd_type == VDD_JOLIET)
		genid = isoent_gen_joliet_identifier;
	else
		genid = isoent_gen_iso9660_identifier;
	do {
		if (np->dir) {
			if (np->virtual &&
			    !archive_entry_mtime_is_set(np->file->entry)) {
				/* Set properly times to virtual directory */
				archive_entry_set_mtime(np->file->entry,
				    iso9660->birth_time, 0);
				archive_entry_set_atime(np->file->entry,
				    iso9660->birth_time, 0);
				archive_entry_set_ctime(np->file->entry,
				    iso9660->birth_time, 0);
			}
			if (np->children.first == NULL)
				;/* Next entry.
				  * Only to reduce nesting this source. */
			else if (vdd->vdd_type != VDD_JOLIET &&
			    !iso9660->opt.rr && depth + 1 >= vdd->max_depth) {
				if (np->children.cnt > 0)
					iso9660->directories_too_deep = np;
			} else {
				/* Generate Identifier */
				r = genid(a, np, &idr);
				if (r < 0)
					goto exit_traverse_tree;
				r = isoent_make_sorted_files(a, vdd, np);
				if (r < 0)
					goto exit_traverse_tree;

				if (depth + 1 < vdd->max_depth) {
					/* Enter to sub directories. */
					np = np->children.first;
					depth++;
					continue;
				}
			}
		}
		while (np != np->parent) {
			if (np->chnext == NULL) {
				/* Return to the parent directory. */
				np = np->parent;
				depth--;
			} else {
				np = np->chnext;
				break;
			}
		}
	} while (np != np->parent);

	r = ARCHIVE_OK;
exit_traverse_tree:
	idr_cleanup(&idr);

	return (r);
}

/*
 * Collect directory entries into path_table by a directory depth.
 */
static int
isoent_collect_dirs(struct vdd *vdd, struct isoent *rootent, int depth)
{
	struct isoent *np;

	if (rootent == NULL)
		rootent = vdd->rootent;
	np = rootent;
	do {
		if (np->dir) {
			/* Register current directory to pathtable. */
			path_table_add_entry(&(vdd->pathtbl[depth]), np);

			if (np->children.first != NULL &&
			    depth + 1 < vdd->max_depth) {
				/* Enter to sub directories. */
				np = np->children.first;
				depth++;
				continue;
			}
		}
		while (np != rootent) {
			if (np->chnext == NULL) {
				/* Return to the parent directory. */
				np = np->parent;
				depth--;
			} else {
				np = np->chnext;
				break;
			}
		}
	} while (np != rootent);

	return (ARCHIVE_OK);
}

/*
 * The entry whose number of levels in a directory hierarchy is
 * large than eight relocate to rr_move directory.
 */
static int
isoent_rr_move_dir(struct archive_write *a, struct isoent **rr_moved,
    struct isoent *isoent, struct isoent **newent)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isoent *rrmoved, *mvent, *np;

	if ((rrmoved = *rr_moved) == NULL) {
		struct isoent *rootent = iso9660->primary.rootent;
		/* There isn't rr_move entry.
		 * Create rr_move entry and insert it into the root entry.
		 */
		rrmoved = isoent_create_virtual_dir(iso9660, "rr_moved");
		if (rrmoved == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate memory");
			return (ARCHIVE_FATAL);
		}
		/* Add "rr_moved" entry to the root entry. */
		isoent_add_child_head(rootent, rrmoved);
		archive_entry_set_nlink(rootent->file->entry,
		    archive_entry_nlink(rootent->file->entry) + 1);
		/* Register "rr_moved" entry to second level pathtable. */
		path_table_add_entry(&(iso9660->primary.pathtbl[1]), rrmoved);
		/* Save rr_moved. */
		*rr_moved = rrmoved;
	}
	/*
	 * Make a clone of isoent which is going to be relocated
	 * to rr_moved.
	 */
	mvent = isoent_clone(isoent);
	if (mvent == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	/* linking..  and use for creating "CL", "PL" and "RE" */
	mvent->rr_parent = isoent->parent;
	isoent->rr_child = mvent;
	/*
	 * Move subdirectories from the isoent to mvent
	 */
	if (isoent->children.first != NULL) {
		*mvent->children.last = isoent->children.first;
		mvent->children.last = isoent->children.last;
	}
	for (np = mvent->children.first; np != NULL; np = np->chnext)
		np->parent = mvent;
	mvent->children.cnt = isoent->children.cnt;
	isoent->children.cnt = 0;
	isoent->children.first = NULL;
	isoent->children.last = &isoent->children.first;
	/*
	 * The mvent becomes a child of the rr_moved entry.
	 */
	isoent_add_child_tail(rrmoved, mvent);
	archive_entry_set_nlink(rrmoved->file->entry,
	    archive_entry_nlink(rrmoved->file->entry) + 1);
	/*
	 * This entry which relocated to the rr_moved directory
	 * has to set the flag as a file.
	 * See also RRIP 4.1.5.1 Description of the "CL" System Use Entry.
	 */
	isoent->dir = 0;

	*newent = mvent;

	return (ARCHIVE_OK);
}

static int
isoent_rr_move(struct archive_write *a)
{
	struct iso9660 *iso9660 = a->format_data;
	struct path_table *pt;
	struct isoent *rootent, *rr_moved;
	struct isoent *np, *last;
	int r;

	pt = &(iso9660->primary.pathtbl[MAX_DEPTH-1]);
	/* Theare aren't level 8 directories reaching a deepr level. */
	if (pt->cnt == 0)
		return (ARCHIVE_OK);

	rootent = iso9660->primary.rootent;
	/* If "rr_moved" directory is already existing, 
	 * we have to use it. */
	rr_moved = isoent_find_entry(rootent, "rr_moved");
	if (rr_moved != NULL &&
	    rr_moved != rootent->children.first) {
		/*
		 * It's necessary that rr_move is the first entry
		 * of the root.
		 */
		struct isoent *ent = rootent->children.first;
		while (ent->chnext != rr_moved)
			ent = ent->chnext;
		if ((ent->chnext = ent->chnext->chnext) == NULL)
			rootent->children.last = &(ent->chnext);
		rootent->children.cnt--;
		isoent_add_child_head(rootent, rr_moved);
	}

	/*
	 * Check level 8 path_table.
	 * If find out sub directory entries, that entries move to rr_move.
	 */
	np = pt->first;
	while (np != NULL) {
		last = path_table_last_entry(pt);
		for (; np != NULL; np = np->ptnext) {
			struct isoent *mvent;
			struct isoent *newent;

			if (!np->dir)
				continue;
			for (mvent = np->children.first;
			    mvent != NULL; mvent = mvent->chnext) {
				if (!mvent->dir)
					continue;
				r = isoent_rr_move_dir(a, &rr_moved,
				    mvent, &newent);
				if (r < 0)
					return (r);
				isoent_collect_dirs(&(iso9660->primary),
				    newent, 2);
			}
		}
		/* If new entries are added to level 8 path_talbe,
		 * its sub directory entries move to rr_move too.
		 */
		np = last->ptnext;
	}

	return (ARCHIVE_OK);
}

/*
 * This comparing rule is according to ISO9660 Standard 6.9.1
 */
static int
_compare_path_table(const void *v1, const void *v2)
{
	const struct isoent *p1, *p2;
	const char *s1, *s2;
	int cmp, l;

	p1 = *((const struct isoent **)(uintptr_t)v1);
	p2 = *((const struct isoent **)(uintptr_t)v2);

	/* Compare parent directory number */
	cmp = p1->parent->dir_number - p2->parent->dir_number;
	if (cmp != 0)
		return (cmp);

	/* Compare indetifier */
	s1 = p1->identifier;
	s2 = p2->identifier;
	l = p1->ext_off;
	if (l > p2->ext_off)
		l = p2->ext_off;
	cmp = strncmp(s1, s2, l);
	if (cmp != 0)
		return (cmp);
	if (p1->ext_off < p2->ext_off) {
		s2 += l;
		l = p2->ext_off - p1->ext_off;
		while (l--)
			if (0x20 != *s2++)
				return (0x20
				    - *(const unsigned char *)(s2 - 1));
	} else if (p1->ext_off > p2->ext_off) {
		s1 += l;
		l = p1->ext_off - p2->ext_off;
		while (l--)
			if (0x20 != *s1++)
				return (*(const unsigned char *)(s1 - 1)
				    - 0x20);
	}
	return (0);
}

static int
_compare_path_table_joliet(const void *v1, const void *v2)
{
	const struct isoent *p1, *p2;
	const unsigned char *s1, *s2;
	int cmp, l;

	p1 = *((const struct isoent **)(uintptr_t)v1);
	p2 = *((const struct isoent **)(uintptr_t)v2);

	/* Compare parent directory number */
	cmp = p1->parent->dir_number - p2->parent->dir_number;
	if (cmp != 0)
		return (cmp);

	/* Compare indetifier */
	s1 = (const unsigned char *)p1->identifier;
	s2 = (const unsigned char *)p2->identifier;
	l = p1->ext_off;
	if (l > p2->ext_off)
		l = p2->ext_off;
	cmp = memcmp(s1, s2, l);
	if (cmp != 0)
		return (cmp);
	if (p1->ext_off < p2->ext_off) {
		s2 += l;
		l = p2->ext_off - p1->ext_off;
		while (l--)
			if (0 != *s2++)
				return (- *(const unsigned char *)(s2 - 1));
	} else if (p1->ext_off > p2->ext_off) {
		s1 += l;
		l = p1->ext_off - p2->ext_off;
		while (l--)
			if (0 != *s1++)
				return (*(const unsigned char *)(s1 - 1));
	}
	return (0);
}

static inline void
path_table_add_entry(struct path_table *pathtbl, struct isoent *ent)
{
	ent->ptnext = NULL;
	*pathtbl->last = ent;
	pathtbl->last = &(ent->ptnext);
	pathtbl->cnt ++;
}

static inline struct isoent *
path_table_last_entry(struct path_table *pathtbl)
{
	if (pathtbl->first == NULL)
		return (NULL);
	return (((struct isoent *)(void *)
		((char *)(pathtbl->last) - offsetof(struct isoent, ptnext))));
}

/*
 * Sort directory entries in path_table
 * and assign directory number to each entries.
 */
static int
isoent_make_path_table_2(struct archive_write *a, struct vdd *vdd,
    int depth, int *dir_number)
{
	struct isoent *np;
	struct isoent **enttbl;
	struct path_table *pt;
	int i;

	pt = &vdd->pathtbl[depth];
	if (pt->cnt == 0) {
		pt->sorted = NULL;
		return (ARCHIVE_OK);
	}
	enttbl = malloc(pt->cnt * sizeof(struct isoent *));
	if (enttbl == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	pt->sorted = enttbl;
	for (np = pt->first; np != NULL; np = np->ptnext)
		*enttbl ++ = np;
	enttbl = pt->sorted;

	switch (vdd->vdd_type) {
	case VDD_PRIMARY:
	case VDD_ENHANCED:
		qsort(enttbl, pt->cnt, sizeof(struct isoent *),
		    _compare_path_table);
		break;
	case VDD_JOLIET:
		qsort(enttbl, pt->cnt, sizeof(struct isoent *),
		    _compare_path_table_joliet);
		break;
	}
	for (i = 0; i < pt->cnt; i++)
		enttbl[i]->dir_number = (*dir_number)++;

	return (ARCHIVE_OK);
}

static int
isoent_alloc_path_table(struct archive_write *a, struct vdd *vdd,
    int max_depth)
{
	int i;

	vdd->max_depth = max_depth;
	vdd->pathtbl = malloc(sizeof(*vdd->pathtbl) * vdd->max_depth);
	if (vdd->pathtbl == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	for (i = 0; i < vdd->max_depth; i++) {
		vdd->pathtbl[i].first = NULL;
		vdd->pathtbl[i].last = &(vdd->pathtbl[i].first);
		vdd->pathtbl[i].sorted = NULL;
		vdd->pathtbl[i].cnt = 0;
	}
	return (ARCHIVE_OK);
}

/*
 * Make Path Tables
 */
static int
isoent_make_path_table(struct archive_write *a)
{
	struct iso9660 *iso9660 = a->format_data;
	int depth, r;
	int dir_number;

	/*
	 * Init Path Table.
	 */
	if (iso9660->dircnt_max >= MAX_DEPTH &&
	    (!iso9660->opt.limit_depth || iso9660->opt.iso_level == 4))
		r = isoent_alloc_path_table(a, &(iso9660->primary),
		    iso9660->dircnt_max + 1);
	else
		/* The number of levels in the hierarchy cannot exceed
		 * eight. */
		r = isoent_alloc_path_table(a, &(iso9660->primary),
		    MAX_DEPTH);
	if (r < 0)
		return (r);
	if (iso9660->opt.joliet) {
		r = isoent_alloc_path_table(a, &(iso9660->joliet),
		    iso9660->dircnt_max + 1);
		if (r < 0)
			return (r);
	}

	/* Step 0.
	 * - Collect directories for primary and joliet.
	 */
	isoent_collect_dirs(&(iso9660->primary), NULL, 0);
	if (iso9660->opt.joliet)
		isoent_collect_dirs(&(iso9660->joliet), NULL, 0);
	/*
	 * Rockridge; move deeper depth directories to rr_moved.
	 */
	if (iso9660->opt.rr) {
		r = isoent_rr_move(a);
		if (r < 0)
			return (r);
	}

 	/* Update nlink. */
	isofile_connect_hardlink_files(iso9660);

	/* Step 1.
	 * - Renew a value of the depth of that directories.
	 * - Resolve hardlinks.
 	 * - Convert pathnames to ISO9660 name or UCS2(joliet).
	 * - Sort files by each directory.
	 */
	r = isoent_traverse_tree(a, &(iso9660->primary));
	if (r < 0)
		return (r);
	if (iso9660->opt.joliet) {
		r = isoent_traverse_tree(a, &(iso9660->joliet));
		if (r < 0)
			return (r);
	}

	/* Step 2.
	 * - Sort directories.
	 * - Assign all directory number.
	 */
	dir_number = 1;
	for (depth = 0; depth < iso9660->primary.max_depth; depth++) {
		r = isoent_make_path_table_2(a, &(iso9660->primary),
		    depth, &dir_number);
		if (r < 0)
			return (r);
	}
	if (iso9660->opt.joliet) {
		dir_number = 1;
		for (depth = 0; depth < iso9660->joliet.max_depth; depth++) {
			r = isoent_make_path_table_2(a, &(iso9660->joliet),
			    depth, &dir_number);
			if (r < 0)
				return (r);
		}
	}
	if (iso9660->opt.limit_dirs && dir_number > 0xffff) {
		/*
		 * Maximum number of directories is 65535(0xffff) 
		 * doe to size(16bit) of Parent Directory Number of
		 * the Path Table.
		 * See also ISO9660 Standard 9.4.
		 */
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Too many directories(%d) over 65535.", dir_number);
		return (ARCHIVE_FATAL);
	}

	/* Get the size of the Path Table. */
	calculate_path_table_size(&(iso9660->primary));
	if (iso9660->opt.joliet)
		calculate_path_table_size(&(iso9660->joliet));

	return (ARCHIVE_OK);
}

/*
 * Find a entry full-path of which is specified by `fn' parameter,
 * in the tree.
 */
static struct isoent *
isoent_find_entry(struct isoent *rootent, const char *fn)
{
#if defined(_WIN32) && !defined(__CYGWIN__)
	char name[_MAX_FNAME];/* Included null terminator size. */
#else
	char name[NAME_MAX+1];
#endif
	struct isoent *isoent, *np;
	int l;

	isoent = rootent;
	l = get_path_component(name, sizeof(name), fn);
	if (l == 0)
		return (NULL);
	fn += l;
	if (fn[0] == '/')
		fn++;

	np = isoent->children.first;
	while (np != NULL) {
		if (fn[0] != '\0') {
			/* Now we are finding a parent directory. */
			if (np->dir &&
			    strcmp(name, np->file->basename.s) == 0) {
				/* Try sub directory. */
				l = get_path_component(name, sizeof(name),
				    fn);
				if (l == 0)
					return (NULL);
				fn += l;
				if (fn[0] == '/')
					fn++;
				isoent = np;
				np = isoent->children.first;
				continue;
			}
		} else {
			/* Find a directory/file name. */
			if (strcmp(name, np->file->basename.s) == 0)
				return (np);
		}
		np = np->chnext;
	}

	/* Not found. */
	return (NULL);
}

static int
isoent_create_boot_catalog(struct archive_write *a, struct isoent *rootent)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isofile *file;
	struct isoent *isoent;
	struct archive_entry *entry;

	/* Find a isoent of the boot file. */
	iso9660->el_torito.boot = isoent_find_entry(rootent,
	    iso9660->el_torito.boot_filename.s);
	if (iso9660->el_torito.boot == NULL) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Can't find the boot image file ``%s''",
		    iso9660->el_torito.boot_filename.s);
		return (ARCHIVE_FATAL);
	}
	iso9660->el_torito.boot->file->boot = BOOT_IMAGE;

	/*
	 * Create the entry which is the "boot.catalog" file.
	 */
	file = isofile_new(NULL);
	if (file == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	archive_entry_set_pathname(file->entry,
	    iso9660->el_torito.catalog_filename.s);
	archive_entry_set_size(file->entry, LOGICAL_BLOCK_SIZE);
	archive_entry_set_mtime(file->entry, iso9660->birth_time, 0);
	archive_entry_set_atime(file->entry, iso9660->birth_time, 0);
	archive_entry_set_ctime(file->entry, iso9660->birth_time, 0);
	archive_entry_set_uid(file->entry, getuid());
	archive_entry_set_gid(file->entry, getgid());
	archive_entry_set_mode(file->entry, AE_IFREG | 0444);
	archive_entry_set_nlink(file->entry, 1);

	isofile_gen_utility_names(file);
	file->boot = BOOT_CATALOG;
	file->content.size = LOGICAL_BLOCK_SIZE;
	isofile_add_entry(iso9660, file);

	isoent = isoent_new(file);
	if (isoent == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	isoent->virtual = 1;

	/* Add the "boot.catalog" entry into tree */
	isoent = isoent_tree(a, isoent);
	if (isoent == NULL)
		return (ARCHIVE_FATAL);

	iso9660->el_torito.catalog = isoent;
	/*
	 * Get a boot medai type.
	 */
	switch (iso9660->opt.boot_type) {
	default:
	case OPT_BOOT_TYPE_AUTO:
		/* Try detecting a media type of the boot image. */
		entry = iso9660->el_torito.boot->file->entry;
		if (archive_entry_size(entry) == FD_1_2M_SIZE)
			iso9660->el_torito.media_type = 
			    BOOT_MEDIA_1_2M_DISKETTE;
		else if (archive_entry_size(entry) == FD_1_44M_SIZE)
			iso9660->el_torito.media_type = 
			    BOOT_MEDIA_1_44M_DISKETTE;
		else if (archive_entry_size(entry) == FD_2_88M_SIZE)
			iso9660->el_torito.media_type = 
			    BOOT_MEDIA_2_88M_DISKETTE;
		else
			/* We cannot decide whether the boot image is
			 * hard-disk. */
			iso9660->el_torito.media_type =
			    BOOT_MEDIA_NO_EMULATION;
		break;
	case OPT_BOOT_TYPE_NO_EMU:
		iso9660->el_torito.media_type = BOOT_MEDIA_NO_EMULATION;
		break;
	case OPT_BOOT_TYPE_HARD_DISK:
		iso9660->el_torito.media_type = BOOT_MEDIA_HARD_DISK;
		break;
	case OPT_BOOT_TYPE_FD:
		entry = iso9660->el_torito.boot->file->entry;
		if (archive_entry_size(entry) <= FD_1_2M_SIZE)
			iso9660->el_torito.media_type = 
			    BOOT_MEDIA_1_2M_DISKETTE;
		else if (archive_entry_size(entry) <= FD_1_44M_SIZE)
			iso9660->el_torito.media_type = 
			    BOOT_MEDIA_1_44M_DISKETTE;
		else if (archive_entry_size(entry) <= FD_2_88M_SIZE)
			iso9660->el_torito.media_type = 
			    BOOT_MEDIA_2_88M_DISKETTE;
		else {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Boot image file(``%s'') size is too big "
			    "for fd type.",
			    iso9660->el_torito.boot_filename.s);
			return (ARCHIVE_FATAL);
		}
		break;
	}

	/*
	 * Get a system type.
	 * TODO: `El Torito' specification says "A copy of byte 5 from the
	 *       Partition Table found in the boot image".
	 */
	iso9660->el_torito.system_type = 0;

	/*
	 * Get an ID.
	 */
	if (iso9660->opt.publisher)
		archive_string_copy(&(iso9660->el_torito.id),
		    &(iso9660->publisher_identifier));


	return (ARCHIVE_OK);
}

/*
 * If a media type is floppy, return its image size.
 * otherwise return 0.
 */
static size_t
fd_boot_image_size(int media_type)
{
	switch (media_type) {
	case BOOT_MEDIA_1_2M_DISKETTE:
		return (FD_1_2M_SIZE);
	case BOOT_MEDIA_1_44M_DISKETTE:
		return (FD_1_44M_SIZE);
	case BOOT_MEDIA_2_88M_DISKETTE:
		return (FD_2_88M_SIZE);
	default:
		return (0);
	}
}

/*
 * Make a boot catalog image data.
 */
static void
make_boot_catalog(struct iso9660 *iso9660, unsigned char *block)
{
	unsigned char *p;
	uint16_t sum, *wp;

	memset(block, 0, LOGICAL_BLOCK_SIZE);
	p = block;
	/*
	 * Validation Entry
	 */
	/* Header ID */
	p[0] = 1;
	/* Platform ID */
	p[1] = iso9660->el_torito.platform_id;
	/* Reserved */
	p[2] = p[3] = 0;
	/* ID */
	if (archive_strlen(&(iso9660->el_torito.id)) > 0)
		strncpy((char *)p+4, iso9660->el_torito.id.s, 23);
	p[27] = 0;
	/* Checksum */
	p[28] = p[29] = 0;
	/* Key */
	p[30] = 0x55;
	p[31] = 0xAA;

	sum = 0;
	wp = (uint16_t *)block;
	while (wp < (uint16_t *)&block[32])
		sum += archive_le16dec(wp++);
	set_num_721(&block[28], (~sum) + 1);

	/*
	 * Initial/Default Entry
	 */
	p = &block[32];
	/* Boot Indicator */
	p[0] = 0x88;
	/* Boot media type */
	p[1] = iso9660->el_torito.media_type;
	/* Load Segment */
	if (iso9660->el_torito.media_type == BOOT_MEDIA_NO_EMULATION)
		set_num_721(&p[2], iso9660->el_torito.boot_load_seg);
	else
		set_num_721(&p[2], 0);
	/* System Type */
	p[4] = iso9660->el_torito.system_type;
	/* Unused */
	p[5] = 0;
	/* Sector Count */
	if (iso9660->el_torito.media_type == BOOT_MEDIA_NO_EMULATION)
		set_num_721(&p[6], iso9660->el_torito.boot_load_size);
	else
		set_num_721(&p[6], 1);
	/* Load RBA */
	set_num_731(&p[8],
	    iso9660->el_torito.boot->file->content.location);
	/* Unused */
	memset(&p[12], 0, 20);
}

static int
setup_boot_information(struct archive_write *a)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isoent *np;
	int64_t size;
	uint32_t sum;

	if (iso9660->wbuff_remaining != 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Internal Program error: "
		    "iso9660:setup_boot_information()");
		return (ARCHIVE_FATAL);
	}
	np = iso9660->el_torito.boot;
	lseek(np->file->temp_fd,
	    np->file->content.offset_of_temp + 64, SEEK_SET);
	size = archive_entry_size(np->file->entry) - 64;
	if (size <= 0) {
		archive_set_error(&a->archive, errno,
		    "Boot file(%jd) is too small", size + 64);
		return (ARCHIVE_FATAL);
	}
	sum = 0;
	while (size > 0) {
		size_t rsize;
		ssize_t i, rs;

		if (size > sizeof(iso9660->wbuff))
			rsize = sizeof(iso9660->wbuff);
		else
			rsize = (size_t)size;

		rs = read(np->file->temp_fd, iso9660->wbuff, rsize);
		if (rs <= 0) {
			archive_set_error(&a->archive, errno,
			    "Can't read temporary file(%jd)",
			    (intmax_t)rs);
			return (ARCHIVE_FATAL);
		}
		for (i = 0; i < rs; i += 4)
			sum += archive_le32dec(iso9660->wbuff + i);
		size -= rs;
	}
	/* Set the location of Primary Volume Descriptor. */
	set_num_731(iso9660->wbuff, SYSTEM_AREA_BLOCK);
	/* Set the location of the boot file. */
	set_num_731(iso9660->wbuff+4, np->file->content.location);
	/* Set the size of the boot file. */
	size = fd_boot_image_size(iso9660->el_torito.media_type);
	if (size == 0)
		size = archive_entry_size(np->file->entry);
	set_num_731(iso9660->wbuff+8, (uint32_t)size);
	/* Set the sum of the boot file. */
	set_num_731(iso9660->wbuff+12, sum);
	/* Clear reserved bytes. */
	memset(iso9660->wbuff+16, 0, 40);

	/* Overwrite the boot file. */
	lseek(np->file->temp_fd,
	    np->file->content.offset_of_temp + 8, SEEK_SET);
	return (write_to_temp(a, iso9660->temp_fd, iso9660->wbuff, 56));
}

static void
utf16stobeutf16s(unsigned char *utf16, size_t utf16_size)
{
	uint16_t *wp = (uint16_t *)utf16;

	utf16_size >>= 1;
	while (utf16_size--) {
		uint16_t wc;
		wc = *wp;
		archive_be16enc(wp, wc);
		wp++;
	}
}

#if defined(_WIN32) && !defined(__CYGWIN__)
static size_t
mbstoutf16s(unsigned char *utf16, size_t utf16_size,
    const char *s, int len)
{
	size_t count;

	if (utf16 != NULL && utf16_size > 1) {
		count = MultiByteToWideChar(CP_ACP,
		    MB_PRECOMPOSED | MB_ERR_INVALID_CHARS,
		    s, len, (LPWSTR)utf16, (int)utf16_size/2);
		if (count == 0 &&
		    GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
			unsigned char *tp;
			size_t tl;

			tl = MultiByteToWideChar(CP_ACP,
			    MB_PRECOMPOSED | MB_ERR_INVALID_CHARS,
			    s, len, NULL, 0);
			if (tl) {
				tp = malloc(tl*2);
				if (tp != NULL) {
					count = MultiByteToWideChar(CP_ACP,
					    MB_PRECOMPOSED | MB_ERR_INVALID_CHARS,
					    s, len, (LPWSTR)tp, (int)tl);
					if (count) {
						memcpy(utf16, tp, utf16_size);
						count = utf16_size / 2;
					}
					free(tp);
				}
			}
		}
	} else
		count = MultiByteToWideChar(CP_ACP,
		    MB_PRECOMPOSED | MB_ERR_INVALID_CHARS,
		    s, len, NULL, 0);
	return (count * 2);
}

static size_t
mblen_of_utf16(const unsigned char *utf16, size_t utf16_size)
{
	int count;
	BOOL defchar;

	count = WideCharToMultiByte(CP_ACP, 0,
	    (LPCWSTR)utf16, (int)utf16_size/2,
	    NULL, 0, NULL, &defchar);
	if (count == 0 || defchar)
		return (0);
	return ((size_t)count);
}

#else

/*
 * NOTE: if wchar_t is not UCS4, mbstoutf16s() and mblen_of_utf16()
 * couldn't work as we have expected.
 */
static size_t
mbstoutf16s(unsigned char *utf16, size_t utf16_size,
    const char *s, int len)
{
	size_t utf16_avail;
	wchar_t wc;
#if HAVE_MBRTOWC
	mbstate_t ps;

	memset(&ps, 0, sizeof(ps));
#else
	mbtowc(NULL, NULL, 0);
#endif
	utf16_size &= ~1;
	if (utf16_size == 0) {
		utf16 = NULL;
		utf16_avail = utf16_size = SIZE_MAX;
	} else
		utf16_avail = utf16_size;
	while (*s && len > 0 && utf16_avail > 0) {
		int mlen;

#if HAVE_MBRTOWC
		mlen = mbrtowc(&wc, s, len, &ps);
#else
		mlen = mbtowc(&wc, s, len);
#endif
		if (mlen < 0)
			return (0);
		if (mlen == 0)
			break;
		if (utf16 != NULL) {
			*(uint16_t *)utf16 = (uint16_t)(wc & 0xFFFF);
			utf16 += 2;
		}
		utf16_avail -= 2;
		s += mlen;
		len -= mlen;
	}
	return (utf16_size - utf16_avail);
}

static size_t
mblen_of_utf16(const unsigned char *utf16, size_t utf16_size)
{
	size_t mlen;
	char mbchars[MB_LEN_MAX];
#if HAVE_WCRTOMB
	mbstate_t ps;

	memset(&ps, 0, sizeof(ps));
#else
	wctomb(NULL, L'\0');
#endif
	mlen = 0;
	utf16_size &= ~1;
	while (utf16_size) {
		int len;
		wchar_t wc;

		wc = *(const uint16_t *)utf16;
#if HAVE_WCRTOMB
		len = wcrtomb(mbchars, wc, &ps);
#else
		len = wctomb(mbchars, wc);
#endif
		if (len < 0)
			return (0);
		if (len == 0)
			break;
		mlen += len;
		utf16 += 2;
		utf16_size -= 2;
	}
	return (mlen);
}
#endif

#ifdef HAVE_ZLIB_H

static int
zisofs_init_zstream(struct archive_write *a)
{
	struct iso9660 *iso9660 = a->format_data;
	int r;

	iso9660->zisofs.stream.next_in = NULL;
	iso9660->zisofs.stream.avail_in = 0;
	iso9660->zisofs.stream.total_in = 0;
	iso9660->zisofs.stream.next_out = iso9660->wbuff;
	iso9660->zisofs.stream.avail_out = sizeof(iso9660->wbuff);
	iso9660->zisofs.stream.total_out = 0;
	if (iso9660->zisofs.stream_valid)
		r = deflateReset(&(iso9660->zisofs.stream));
	else {
		r = deflateInit(&(iso9660->zisofs.stream),
		    iso9660->zisofs.compression_level);
		iso9660->zisofs.stream_valid = 1;
	}
	switch (r) {
	case Z_OK:
		break;
	default:
	case Z_STREAM_ERROR:
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Internal error initializing "
		    "compression library: invalid setup parameter");
		return (ARCHIVE_FATAL);
	case Z_MEM_ERROR:
		archive_set_error(&a->archive, ENOMEM,
		    "Internal error initializing "
		    "compression library");
		return (ARCHIVE_FATAL);
	case Z_VERSION_ERROR:
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Internal error initializing "
		    "compression library: invalid library version");
		return (ARCHIVE_FATAL);
	}
	return (ARCHIVE_OK);
}

static int
zisofs_add_nozf(struct archive_write *a, const char *s)
{
	struct iso9660 *iso9660 = a->format_data;
	struct nozf_file *nzf;

	if (iso9660->zisofs.nozf_list == NULL) {
		iso9660->zisofs.nozf_cnt = 1;
		nzf = malloc(sizeof(*nzf));
	} else {
		iso9660->zisofs.nozf_cnt++;
		nzf = realloc(iso9660->zisofs.nozf_list,
		    sizeof(*nzf) * iso9660->zisofs.nozf_cnt);
	}
	if (nzf == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate memory");
		return (ARCHIVE_FATAL);
	}
	iso9660->zisofs.nozf_list = nzf;
	nzf += iso9660->zisofs.nozf_cnt -1;
	archive_string_init(&nzf->parentdir);
	archive_string_init(&nzf->basename);
	get_parent_and_base(&nzf->parentdir, &nzf->basename, s);
	return (ARCHIVE_OK);
}

static void
zisofs_check_nozf(struct archive_write *a, struct isofile *file)
{
	struct iso9660 *iso9660 = a->format_data;
	const char *np, *cp;
	struct nozf_file *nzf;
	int i, len, match;

	nzf = iso9660->zisofs.nozf_list;
	for (i = 0; i < iso9660->zisofs.nozf_cnt; i++) {
		if (nzf[i].basename.length != file->basename.length)
			continue;
		if (strcmp(nzf[i].basename.s, file->basename.s) != 0)
			continue;
		if (nzf[i].parentdir.length > file->parentdir.length)
			continue;
		len = nzf[i].parentdir.length;
		np = nzf[i].parentdir.s + nzf[i].parentdir.length;
		cp = file->parentdir.s + file->parentdir.length;
		match = 1;
		while (len--) {
			if (*--np != *--cp) {
				match = 0;
				break;
			}
		}
		if (match) {
			--cp;
			if ((cp >= file->parentdir.s && *cp == '/') ||
			    cp < file->parentdir.s) {
				file->zisofs.keep_original = 1;
				break;
			}
		}
	}
}

static int
zisofs_fix_nozf(struct archive_write *a)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isoent *isoent;
	struct isofile *file;
	struct nozf_file *nzf;
	struct archive_string str;
	int i;

	if (iso9660->zisofs.nozf_list == NULL)
		return (ARCHIVE_OK);
	
	archive_string_init(&str);
	nzf = iso9660->zisofs.nozf_list;
	for (i = 0; i < iso9660->zisofs.nozf_cnt; i++) {
		archive_string_empty(&str);
		if (nzf[i].parentdir.length) {
			archive_string_copy(&str, &nzf[i].parentdir);
			archive_strappend_char(&str, '/');
		}
		archive_string_concat(&str, &nzf[i].basename);
		isoent = isoent_find_entry(iso9660->primary.rootent,
		    str.s);
		if (isoent == NULL) {
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_MISC,
			    "Specified file ``%s'' which disable to "
			    " be zisofs is not found.",
			    str.s);
			return (ARCHIVE_FATAL);
		}
		if (isoent->file->zisofs.keep_original) {
			file = isoent->file;
			file->temp_fd = iso9660->temp_fd;
			file->content.offset_of_temp =
			    file->zisofs.original_offset_of_temp;
			archive_entry_set_size(file->entry,
			    file->zisofs.uncompressed_size);
			file->content.size = file->zisofs.uncompressed_size;
			/* Remark file->zisofs not to create
			 * RRIP 'ZF' Use Entry. */
			file->zisofs.header_size = 0;
			file->zisofs.log2_bs = 0;
			file->zisofs.uncompressed_size = 0;
		}
	}
	archive_string_free(&str);

	return (ARCHIVE_OK);
}

#endif /* HAVE_ZLIB_H */

static int
zisofs_init(struct archive_write *a,  struct isofile *file)
{
	struct iso9660 *iso9660 = a->format_data;
#ifdef HAVE_ZLIB_H
	size_t ceil, bpsize;
	int r;
#endif

	iso9660->zisofs.detect_magic = 0;
#ifdef HAVE_ZLIB_H
	iso9660->zisofs.making = 0;
#endif
	if (!iso9660->opt.rr || !iso9660->opt.zisofs)
		return (ARCHIVE_OK);

	if (archive_entry_size(file->entry) >= 24 &&
	    archive_entry_size(file->entry) < MULTI_EXTENT_SIZE) {
		/* Acceptable file size for zisofs. */
		iso9660->zisofs.detect_magic = 1;
		iso9660->zisofs.magic_cnt = 0;
	}
	if (iso9660->opt.zisofs == OPT_ZISOFS_INDIRECT ||
	    !iso9660->zisofs.detect_magic)
		return (ARCHIVE_OK);

#ifdef HAVE_ZLIB_H
	/* The number of Logical Blocks which uncompressed data
	 * will use in iso-image file is the same as the number of
	 * Logical Blocks which zisofs(compressed) data will use
	 * in ISO-image file. It won't reduce iso-image file size. */
	if (iso9660->opt.zisofs == OPT_ZISOFS_DIRECT &&
	    archive_entry_size(file->entry) <= LOGICAL_BLOCK_SIZE)
		return (ARCHIVE_OK);

	/*
	 * Create a temporary file, which will be used for saving
	 * compressed data.
	 */
	if (iso9660->zisofs.temp_fd < 0) {
		iso9660->zisofs.temp_fd = __archive_mktemp(NULL);
		if (iso9660->zisofs.temp_fd < 0) {
			archive_set_error( &a->archive, errno,
			    "Can't create temporary file");
			return (ARCHIVE_FATAL);
		}
	}

	/* Initialize compression library */
	r = zisofs_init_zstream(a);
	if (r != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Mark file->zisofs to create RRIP 'ZF' Use Entry. */
	file->zisofs.header_size = ZF_HEADER_SIZE >> 2;
	file->zisofs.log2_bs = ZF_LOG2_BS;
	file->zisofs.uncompressed_size = archive_entry_size(file->entry);

	/* Calculate a size of Block Pointers of zisofs. */
	ceil = (file->zisofs.uncompressed_size + ZF_BLOCK_SIZE -1)
		>> file->zisofs.log2_bs;
	iso9660->zisofs.block_pointers_cnt = ceil + 1;
	iso9660->zisofs.block_pointers_idx = 0;

	/* Ensure a buffer size used for Block Pointers */
	bpsize = iso9660->zisofs.block_pointers_cnt *
	    sizeof(iso9660->zisofs.block_pointers[0]);
	if (iso9660->zisofs.block_pointers_allocated < bpsize) {
		free(iso9660->zisofs.block_pointers);
		iso9660->zisofs.block_pointers = malloc(bpsize);
		if (iso9660->zisofs.block_pointers == NULL) {
			archive_set_error(&a->archive, ENOMEM,
			    "Can't allocate data");
			return (ARCHIVE_FATAL);
		}
		iso9660->zisofs.block_pointers_allocated = bpsize;
	}

	/* Save a offset of current file in zisofs temporary file. */
	iso9660->zisofs.offset_of_temp =
	    lseek(iso9660->zisofs.temp_fd, 0, SEEK_CUR);
	iso9660->zisofs.total_size = ZF_HEADER_SIZE + bpsize;

	/*
	 * Skip zisofs header and Block Pointers, which we will write
	 * after all compressed data of a file written to the temporary
	 * file.
	 */
	lseek(iso9660->zisofs.temp_fd, iso9660->zisofs.total_size, SEEK_CUR);

	/*
	 * Initialize some variables to make zisofs.
	 */
	archive_le32enc(&(iso9660->zisofs.block_pointers[0]),
	    iso9660->zisofs.total_size);
	iso9660->zisofs.remaining = file->zisofs.uncompressed_size;
	iso9660->zisofs.making = 1;
	iso9660->zisofs.allzero = 1;
	iso9660->zisofs.offset_of_block = iso9660->zisofs.total_size;

	/* Check if this file does not need to be zisofs. */
	zisofs_check_nozf(a, file);
#endif

	return (ARCHIVE_OK);
}

static void
zisofs_detect_magic(struct archive_write *a, const void *buff, size_t s)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isofile *file = iso9660->cur_file;
	const unsigned char *p, *endp;
	const unsigned char *magic_buff;
	uint32_t uncompressed_size;
	unsigned char header_size;
	unsigned char log2_bs;
	size_t ceil, doff;
	uint32_t bst, bed;
	int magic_max;
	int64_t entry_size;

	entry_size = archive_entry_size(file->entry);
	if (sizeof(iso9660->zisofs.magic_buffer) > entry_size)
		magic_max = entry_size;
	else
		magic_max = sizeof(iso9660->zisofs.magic_buffer);

	if (iso9660->zisofs.magic_cnt == 0 && s >= (size_t)magic_max)
		/* It's unnecessary we copy buffer. */
		magic_buff = buff;
	else {
		if (iso9660->zisofs.magic_cnt < magic_max) {
			size_t l;

			l = sizeof(iso9660->zisofs.magic_buffer)
			    - iso9660->zisofs.magic_cnt;
			if (l > s)
				l = s;
			memcpy(iso9660->zisofs.magic_buffer
			    + iso9660->zisofs.magic_cnt, buff, l);
			iso9660->zisofs.magic_cnt += l;
			if (iso9660->zisofs.magic_cnt < magic_max)
				return;
		}
		magic_buff = iso9660->zisofs.magic_buffer;
	}
	iso9660->zisofs.detect_magic = 0;
	p = magic_buff;

	/* Check magic code of zisofs. */
	if (memcmp(p, zisofs_magic, sizeof(zisofs_magic)) != 0)
		/* This is not zisofs file which made by mkzftree. */
		return;
	p += sizeof(zisofs_magic);

	/* Read a zisofs header. */
	uncompressed_size = archive_le32dec(p);
	header_size = p[4];
	log2_bs = p[5];
	if (uncompressed_size < 24 || header_size != 4 ||
	    log2_bs > 30 || log2_bs < 7)
		return;/* Invalid or not supported header. */

	/* Calculate a size of Block Pointers of zisofs. */
	ceil = (uncompressed_size +
	        (ARCHIVE_LITERAL_LL(1) << log2_bs) -1) >> log2_bs;
	doff = (ceil + 1) * 4 + 16;
	if (entry_size < doff)
		return;/* Invalid data. */

	/* Check every Block Pointer has valid value. */
	p = magic_buff + 16;
	endp = magic_buff + magic_max;
	while (ceil && p + 8 <= endp) {
		bst = archive_le32dec(p);
		if (bst != doff)
			return;/* Invalid data. */
		p += 4;
		bed = archive_le32dec(p);
		if (bed < bst || bed > entry_size)
			return;/* Invalid data. */
		doff += bed - bst;
		ceil--;
	}
#ifdef HAVE_ZLIB_H
	if (file->zisofs.header_size != 0)
		zisofs_cancel(iso9660, file);
#endif
	file->zisofs.uncompressed_size = uncompressed_size;
	file->zisofs.header_size = header_size;
	file->zisofs.log2_bs = log2_bs;
}

#ifdef HAVE_ZLIB_H

static void
zisofs_cancel(struct iso9660 *iso9660, struct isofile *file)
{

	/* Remark file->zisofs not to create RRIP 'ZF' Use Entry. */
	file->zisofs.header_size = 0;
	file->zisofs.log2_bs = 0;
	file->zisofs.uncompressed_size = 0;
	lseek(iso9660->zisofs.temp_fd,
	    iso9660->zisofs.offset_of_temp, SEEK_SET);
	iso9660->zisofs.making = 0;
}

/*
 * Compress data and write it to a temporary file.
 */
static int
zisofs_write_to_temp(struct archive_write *a, const void *buff, size_t s)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isofile *file = iso9660->cur_file;
	const unsigned char *b;
	z_stream *zstrm;
	size_t avail, csize;
	int flush, r;

	zstrm = &(iso9660->zisofs.stream);
	b = (const unsigned char *)buff;
	do {
		avail = ZF_BLOCK_SIZE - zstrm->total_in;
		if (s < avail) {
			avail = s;
			flush = Z_NO_FLUSH;
		} else
			flush = Z_FINISH;
		iso9660->zisofs.remaining -= avail;
		if (iso9660->zisofs.remaining <= 0)
			flush = Z_FINISH;

		zstrm->next_in = (Bytef *)(uintptr_t)(const void *)b;
		zstrm->avail_in = avail;

		/*
		 * Check if current data block are all zero.
		 */
		if (iso9660->zisofs.allzero) {
			const unsigned char *nonzero = b;
			const unsigned char *nonzeroend = b + avail;

			while (nonzero < nonzeroend)
				if (*nonzero++) {
					iso9660->zisofs.allzero = 0;
					break;
				}
		}
		b += avail;
		s -= avail;

		/*
		 * If current data block are all zero, we do not use
		 * compressed data.
		 */
		if (flush == Z_FINISH && iso9660->zisofs.allzero &&
		    avail + zstrm->total_in == ZF_BLOCK_SIZE) {
			if (iso9660->zisofs.offset_of_block !=
			    iso9660->zisofs.total_size) {
				lseek(iso9660->zisofs.temp_fd,
				    iso9660->zisofs.offset_of_temp +
				        iso9660->zisofs.offset_of_block,
				    SEEK_SET);
				iso9660->zisofs.total_size =
				    iso9660->zisofs.offset_of_block;
			}
			zstrm->avail_in = 0;
		}

		/*
		 * Compress file data.
		 */
		while (zstrm->avail_in > 0) {
			csize = zstrm->total_out;
			r = deflate(zstrm, flush);
			switch (r) {
			case Z_OK:
			case Z_STREAM_END:
				csize = zstrm->total_out - csize;
				if (write_to_temp(a, iso9660->zisofs.temp_fd,
				    iso9660->wbuff, csize) != ARCHIVE_OK)
					return (ARCHIVE_FATAL);
				iso9660->zisofs.total_size += csize;
				zstrm->next_out = iso9660->wbuff;
				zstrm->avail_out = sizeof(iso9660->wbuff);
				break;
			default:
				archive_set_error(&a->archive,
				    ARCHIVE_ERRNO_MISC,
				    "Compression failed:"
				    " deflate() call returned status %d",
				    r);
				return (ARCHIVE_FATAL);
			}
		}

		/*
		 * If a size of zisofs data is more than uncompressed size,
		 * we do not need to make a zisofs extension.
		 */
		if (iso9660->zisofs.total_size >
		    file->zisofs.uncompressed_size) {
			zisofs_cancel(iso9660, file);
			return (ARCHIVE_OK);
		}

		if (flush == Z_FINISH) {
			/*
			 * Save the information of one zisofs block.
			 */
			iso9660->zisofs.block_pointers_idx ++;
			archive_le32enc(&(iso9660->zisofs.block_pointers[
			    iso9660->zisofs.block_pointers_idx]),
				iso9660->zisofs.total_size);
			r = zisofs_init_zstream(a);
			if (r != ARCHIVE_OK)
				return (ARCHIVE_FATAL);
			iso9660->zisofs.allzero = 1;
			iso9660->zisofs.offset_of_block =
			    iso9660->zisofs.total_size;
		}
	} while (s);

	return (ARCHIVE_OK);
}

static int
zisofs_finish_entry(struct archive_write *a)
{
	struct iso9660 *iso9660 = a->format_data;
	struct isofile *file = iso9660->cur_file;
	unsigned char buff[16];
	size_t s;
	int64_t tail;

	if (iso9660->opt.zisofs == OPT_ZISOFS_DIRECT) {
		int bk1, bk2;

		bk1 = (file->zisofs.uncompressed_size +
		    LOGICAL_BLOCK_SIZE -1) / LOGICAL_BLOCK_SIZE;
		bk2 = (iso9660->zisofs.total_size +
		    LOGICAL_BLOCK_SIZE -1) / LOGICAL_BLOCK_SIZE;

		/* The number of Logical Blocks which uncompressed data
		 * will use in iso-image file is the same as the number of
		 * Logical Blocks which zisofs(compressed) data will use
		 * in ISO-image file. It won't reduce iso-image file size. */
		if (bk1 == bk2) {
			zisofs_cancel(iso9660, file);
			return (ARCHIVE_OK);
		}
	}
	if (file->zisofs.keep_original)
		/* Maybe we will use the original data, which is
		 * uncompressed but we cannot decide it now. */
		file->zisofs.original_offset_of_temp =
		    file->content.offset_of_temp;
	else
		/* Remove the original data, which is uncompressed. */
		lseek(iso9660->temp_fd, file->content.offset_of_temp,
		    SEEK_SET);

	/* Direct temp file stream to zisofs temp file stream. */
	file->temp_fd = iso9660->zisofs.temp_fd;
	file->content.offset_of_temp = iso9660->zisofs.offset_of_temp;
	archive_entry_set_size(file->entry, iso9660->zisofs.total_size);
	file->content.size = iso9660->zisofs.total_size;

	/*
	 * Save a file pointer which points the end of current zisofs data.
	 */
	tail = lseek(file->temp_fd, 0, SEEK_CUR);

	/*
	 * Make a header.
	 *
	 * +-----------------+----------------+-----------------+
	 * | Header 16 bytes | Block Pointers | Compressed data |
	 * +-----------------+----------------+-----------------+
	 * 0                16               +X
	 * Block Pointers :
	 *   4 * (((Uncompressed file size + block_size -1) / block_size) + 1)
	 *
	 * Write zisofs header. 
	 *    Magic number
	 * +----+----+----+----+----+----+----+----+
	 * | 37 | E4 | 53 | 96 | C9 | DB | D6 | 07 |
	 * +----+----+----+----+----+----+----+----+
	 * 0    1    2    3    4    5    6    7    8
	 *
	 * +------------------------+------------------+
	 * | Uncompressed file size | header_size >> 2 |
	 * +------------------------+------------------+
	 * 8                       12                 13
	 *
	 * +-----------------+----------------+
	 * | log2 block_size | Reserved(0000) |
	 * +-----------------+----------------+
	 * 13               14               16
	 */
	memcpy(buff, zisofs_magic, 8);
	set_num_731(buff+8, file->zisofs.uncompressed_size);
	buff[12] = file->zisofs.header_size;
	buff[13] = file->zisofs.log2_bs;
	buff[14] = buff[15] = 0;/* Reserved */

	/* Get the right position to write the header. */
	lseek(file->temp_fd, file->content.offset_of_temp, SEEK_SET);
	/* Write the header. */
	if (write_to_temp(a, file->temp_fd, buff, 16) != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/*
	 * Write zisofs Block Pointers.
	 */
	s = iso9660->zisofs.block_pointers_cnt *
	    sizeof(iso9660->zisofs.block_pointers[0]);
	if (write_to_temp(a, file->temp_fd, iso9660->zisofs.block_pointers, s)
	    != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Set the file pointer to the end of current zisofs data. */
	lseek(file->temp_fd, tail, SEEK_SET);

	return (ARCHIVE_OK);
}

#endif /* HAVE_ZLIB_H */
