/*
 * This daemon is the SCSI SSC target (Sequential device - tape drive)
 * portion of the vtl package.
 *
 * The vtl package consists of:
 *   a kernel module (vlt.ko) - Currently on 2.6.x Linux kernel support.
 *   SCSI target daemons for both SMC and SSC devices.
 *
 * Copyright (C) 2005 - 2009 Mark Harvey       markh794@gmail.com
 *                                          mark_harvey@symantec.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *

 * v0.1 -> Proof (proof of concept) that this may actually work (just)
 * v0.2 -> Get queueCommand() callback working -
 *         (Note to self: Sleeping in kernel is bad!)
 * v0.3 -> Message queues + make into daemon
 *	   changed lseek to lseek64
 * v0.4 -> First copy given to anybody,
 * v0.10 -> First start of a Solaris x86 port.. Still underway.
 * v0.11 -> First start of a Linux 2.4 kernel port.. Still underway.
 *	    However I'm scrapping this kfifo stuff and passing a pointer
 *	    and using copy{to|from}_user routines instead.
 * v0.12 -> Forked into 'stable' (0.12) and 'devel' (0.13).
 *          My current thinking : This is a dead end anyway.
 *          An iSCSI target done in user-space is now my perferred solution.
 *          This means I don't have to do any kernel level drivers
 *          and leaverage the hosts native iSCSI initiator.
 * 0.14 13 Feb 2008
 *	Since ability to define device serial number, increased ver from
 *	0.12 to 0.14
 *
 * 0.16 Jun 2009
 *	Moved SCSI Inquiry into user-space.
 *	SCSI lu are created/destroyed as the daemon is started/shutdown
 */

#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 500

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <strings.h>
#include <syslog.h>
#include <inttypes.h>
#include <pwd.h>
#include <signal.h>
#include "list.h"
#include "be_byteshift.h"
#include "vtl_common.h"
#include "scsi.h"
#include "q.h"
#include "vtllib.h"
#include "vtltape.h"
#include "spc.h"
#include "ssc.h"
#include "log.h"

char vtl_driver_name[] = "vtltape";

/* Variables for simple, logical only SCSI Encryption system */

static struct encryption encryption;

#define	UKAD_LENGTH	encryption.ukad_length
#define	AKAD_LENGTH	encryption.akad_length
#define	KEY_LENGTH	encryption.key_length
#define	UKAD		encryption.ukad
#define	AKAD		encryption.akad
#define	KEY		encryption.key

#include <zlib.h>

extern uint8_t last_cmd;

extern int current_state;	/* scope, Global -> Last status sent to fifo */

/* Suppress Incorrect Length Indicator */
#define SILI  0x2
/* Fixed block format */
#define FIXED 0x1

#ifndef Solaris
  /* I'm sure there must be a header where lseek64() is defined */
  loff_t lseek64(int, loff_t, int);
  int ioctl(int, int, void *);
#endif

int verbose = 0;
int debug = 0;
long my_id;


#define MEDIA_WRITABLE 0
#define MEDIA_READONLY 1

struct priv_lu_ssc lu_ssc;

struct lu_phy_attr lunit;

struct MAM_Attributes_table {
	int attribute;
	int length;
	int read_only;
	int format;
	void *value;
} MAM_Attributes[] = {
	{0x000, 8, 1, 0, &mam.remaining_capacity },
	{0x001, 8, 1, 0, &mam.max_capacity },
	{0x002, 8, 1, 0, &mam.TapeAlert },
	{0x003, 8, 1, 0, &mam.LoadCount },
	{0x004, 8, 1, 0, &mam.MAMSpaceRemaining },
	{0x005, 8, 1, 1, &mam.AssigningOrganization_1 },
	{0x006, 1, 1, 0, &mam.FormattedDensityCode },
	{0x007, 2, 1, 0, &mam.InitializationCount },
	{0x20a, 40, 1, 1, &mam.DevMakeSerialLastLoad },
	{0x20b, 40, 1, 1, &mam.DevMakeSerialLastLoad1 },
	{0x20c, 40, 1, 1, &mam.DevMakeSerialLastLoad2 },
	{0x20d, 40, 1, 1, &mam.DevMakeSerialLastLoad3 },
	{0x220, 8, 1, 0, &mam.WrittenInMediumLife },
	{0x221, 8, 1, 0, &mam.ReadInMediumLife },
	{0x222, 8, 1, 0, &mam.WrittenInLastLoad },
	{0x223, 8, 1, 0, &mam.ReadInLastLoad },
	{0x400, 8, 1, 1, &mam.MediumManufacturer },
	{0x401, 32, 1, 1, &mam.MediumSerialNumber },
	{0x402, 4, 1, 0, &mam.MediumLength },
	{0x403, 4, 1, 0, &mam.MediumWidth },
	{0x404, 8, 1, 1, &mam.AssigningOrganization_2 },
	{0x405, 1, 1, 0, &mam.MediumDensityCode },
	{0x406, 8, 1, 1, &mam.MediumManufactureDate },
	{0x407, 8, 1, 0, &mam.MAMCapacity },
	{0x408, 1, 0, 0, &mam.MediumType },
	{0x409, 2, 1, 0, &mam.MediumTypeInformation },
	{0x800, 8, 0, 1, &mam.ApplicationVendor },
	{0x801, 32, 0, 1, &mam.ApplicationName },
	{0x802, 8, 0, 1, &mam.ApplicationVersion },
	{0x803, 160, 0, 2, &mam.UserMediumTextLabel },
	{0x804, 12, 0, 1, &mam.DateTimeLastWritten },
	{0x805, 1, 0, 0, &mam.LocalizationIdentifier },
	{0x806, 32, 0, 1, &mam.Barcode },
	{0x807, 80, 0, 2, &mam.OwningHostTextualName },
	{0x808, 160, 0, 2, &mam.MediaPool },
	{0xbff, 0, 1, 0, NULL }
};

static struct tape_drives_table {
	char *name;
	void (*init)(struct lu_phy_attr *);
} tape_drives[] = {
	{ "ULT3580-TD1     ", init_ult3580_td1 },
	{ "ULT3580-TD2     ", init_ult3580_td2 },
	{ "ULT3580-TD3     ", init_ult3580_td3 },
	{ "ULT3580-TD4     ", init_ult3580_td4 },
	{ "ULT3580-TD5     ", init_ult3580_td5 },
	{ "ULTRIUM-TD1     ", init_ult3580_td1 },
	{ "ULTRIUM-TD2     ", init_ult3580_td2 },
	{ "ULTRIUM-TD3     ", init_ult3580_td3 },
	{ "ULTRIUM-TD4     ", init_ult3580_td4 },
	{ "ULTRIUM-TD5     ", init_ult3580_td5 },
	{ "Ultrium 1-SCSI  ", init_hp_ult_1 },
	{ "Ultrium 2-SCSI  ", init_hp_ult_2 },
	{ "Ultrium 3-SCSI  ", init_hp_ult_3 },
	{ "Ultrium 4-SCSI  ", init_hp_ult_4 },
	{ "Ultrium 5-SCSI  ", init_hp_ult_5 },
	{ "SDX-300C        ", init_ait1_ssc },
	{ "SDX-500C        ", init_ait2_ssc },
	{ "SDX-500V        ", init_ait2_ssc },
	{ "SDX-700C        ", init_ait3_ssc },
	{ "SDX-700V        ", init_ait3_ssc },
	{ "SDX-900V        ", init_ait4_ssc },
	{ "03592J1A        ", init_3592_j1a },
	{ "03592E05        ", init_3592_E05 },
	{ "03592E06        ", init_3592_E06 },
	{ "T10000C         ", init_t10kC_ssc },
	{ "T10000B         ", init_t10kB_ssc },
	{ "T10000          ", init_t10kA_ssc },
	{ "T9840D          ", init_9840D_ssc },
	{ "T9840C          ", init_9840C_ssc },
	{ "T9840B          ", init_9840B_ssc },
	{ "T9840A          ", init_9840A_ssc },
	{ "T9940B          ", init_9940B_ssc },
	{ "T9940A          ", init_9940A_ssc },
	{ NULL, NULL},
};

static struct media_name_index_table {
	char *name;
	int media_type;
	int mode_media_type;
	int media_density;
} media_info[] = {
	{"Undefined", Media_undefined,
			media_type_unknown, medium_density_code_unknown},

	/* Ultrium media */
	{"LTO1", Media_LTO1,
			media_type_lto1_data, medium_density_code_lto1},
	{"LTO1 Clean", Media_LTO1_CLEAN,
			media_type_lto1_data, medium_density_code_lto1},
	{"LTO2", Media_LTO2,
			media_type_lto2_data, medium_density_code_lto2},
	{"LTO2 Clean", Media_LTO2_CLEAN,
			media_type_lto2_data, medium_density_code_lto2},
	{"LTO3", Media_LTO3,
			media_type_lto3_data, medium_density_code_lto3},
	{"LTO3 Clean", Media_LTO3_CLEAN,
			media_type_lto3_data, medium_density_code_lto3},
	{"LTO3 WORM", Media_LTO3_WORM,
			media_type_lto3_worm, medium_density_code_lto3_WORM},
	{"LTO4", Media_LTO4,
			media_type_lto4_data, medium_density_code_lto4},
	{"LTO4 Clean", Media_LTO4_CLEAN,
			media_type_lto4_data, medium_density_code_lto4},
	{"LTO4 WORM", Media_LTO4_WORM,
			media_type_lto4_worm, medium_density_code_lto4_WORM},
	{"LTO5", Media_LTO5,
			media_type_lto5_data, medium_density_code_lto5},
	{"LTO5 Clean", Media_LTO5_CLEAN,
			media_type_lto5_data, medium_density_code_lto5},
	{"LTO5 WORM", Media_LTO5_WORM,
			media_type_lto5_worm, medium_density_code_lto5_WORM},

	/* IBM 03592 media */
	{"03592 JA", Media_3592_JA,
			media_type_unknown, medium_density_code_j1a},
	{"03592 JA Clean", Media_3592_JA_CLEAN,
			media_type_unknown, medium_density_code_j1a},
	{"03592 JA WORM", Media_3592_JW,
			media_type_unknown, medium_density_code_j1a},

	{"03592 JB", Media_3592_JB,
			media_type_unknown, medium_density_code_e05},
	{"03592 JB Clean", Media_3592_JB_CLEAN,
			media_type_unknown, medium_density_code_e05},
	{"03592 JB ENCR", Media_3592_JB,
			media_type_unknown, medium_density_code_e05_ENCR},

	{"03592 JC", Media_3592_JX,
			media_type_unknown, medium_density_code_e06},
	{"03592 JC Clean", Media_3592_JX_CLEAN,
			media_type_unknown, medium_density_code_e06},
	{"03592 JC ENCR", Media_3592_JX,
			media_type_unknown, medium_density_code_e06_ENCR},

	/* AIT media */
	{"AIT1", Media_AIT1,
			media_type_unknown, medium_density_code_ait1},
	{"AIT1 Clean", Media_AIT1_CLEAN,
			media_type_unknown, medium_density_code_ait1},
	{"AIT2", Media_AIT2,
			media_type_unknown, medium_density_code_ait2},
	{"AIT2 Clean", Media_AIT2_CLEAN,
			media_type_unknown, medium_density_code_ait2},
	{"AIT3", Media_AIT3,
			media_type_unknown, medium_density_code_ait3},
	{"AIT3 Clean", Media_AIT3_CLEAN,
			media_type_unknown, medium_density_code_ait3},
	{"AIT4",  Media_AIT4,
			media_type_unknown, medium_density_code_ait4},
	{"AIT4 Clean", Media_AIT4_CLEAN,
			media_type_unknown, medium_density_code_ait4},
	{"AIT4 WORM", Media_AIT4_WORM,
			media_type_unknown, medium_density_code_ait4},

	/* STK 9x40 media */
	{"9840A", Media_9840A,
			media_type_unknown, medium_density_code_9840A},
	{"9840A Clean", Media_9840A_CLEAN,
			media_type_unknown, medium_density_code_9840A},
	{"9840B", Media_9840B,
			media_type_unknown, medium_density_code_9840B},
	{"9840B Clean", Media_9840B_CLEAN,
			media_type_unknown, medium_density_code_9840B},
	{"9840C", Media_9840C,
			media_type_unknown, medium_density_code_9840C},
	{"9840C Clean", Media_9840C_CLEAN,
			media_type_unknown, medium_density_code_9840C},
	{"9840D", Media_9840D,
			media_type_unknown, medium_density_code_9840D},
	{"9840D Clean", Media_9840D_CLEAN,
			media_type_unknown, medium_density_code_9840D},

	{"9940A", Media_9940A,
			media_type_unknown, medium_density_code_9940A},
	{"9940A Clean", Media_9940A_CLEAN,
			media_type_unknown, medium_density_code_9940A},
	{"9940B", Media_9940B,
			media_type_unknown, medium_density_code_9940B},
	{"9940B Clean", Media_9940B_CLEAN,
			media_type_unknown, medium_density_code_9940B},

	/* STK T10000 media */
	{"T10KA", Media_T10KA,
			media_type_unknown, medium_density_code_10kA},
	{"T10KA WORM", Media_T10KA_WORM,
			media_type_unknown, medium_density_code_10kA},
	{"T10KA Clean", Media_T10KA_CLEAN,
			media_type_unknown, medium_density_code_10kA},
	{"T10KB", Media_T10KB,
			media_type_unknown, medium_density_code_10kB},
	{"T10KB WORM", Media_T10KB_WORM,
			media_type_unknown, medium_density_code_10kB},
	{"T10KB Clean", Media_T10KB_CLEAN,
			media_type_unknown, medium_density_code_10kB},
	{"T10KC", Media_T10KC,
			media_type_unknown, medium_density_code_10kC},
	{"T10KC WORM", Media_T10KC_WORM,
			media_type_unknown, medium_density_code_10kC},
	{"T10KC Clean", Media_T10KC_CLEAN,
			media_type_unknown, medium_density_code_10kC},

	/* Quantum DLT / SDLT media */
	{"DLT2", Media_DLT2,
			media_type_unknown, medium_density_code_dlt2},
	{"DLT3", Media_DLT3,
			media_type_unknown, medium_density_code_dlt3},
	{"DLT4", Media_DLT4,
			media_type_unknown, medium_density_code_dlt4},
	{"SDLT", Media_SDLT,
			media_type_unknown, medium_density_code_sdlt},
	{"SDLT 220", Media_SDLT220,
			media_type_unknown, medium_density_code_220},
	{"SDLT 320", Media_SDLT320,
			media_type_unknown, medium_density_code_320},
	{"SDLT 320 Clean", Media_SDLT320_CLEAN,
			media_type_unknown, medium_density_code_320},
	{"SDLT 600", Media_SDLT600,
			media_type_unknown, medium_density_code_600},
	{"SDLT 600 Clean", Media_SDLT600_CLEAN,
			media_type_unknown, medium_density_code_600},
	{"SDLT 600 WORM", Media_SDLT600_WORM,
			media_type_unknown, medium_density_code_600},

	/* 4MM DAT media */
	{"DDS1", Media_DDS1,
			media_type_unknown, medium_density_code_DDS1},
	{"DDS2", Media_DDS2,
			media_type_unknown, medium_density_code_DDS2},
	{"DDS3", Media_DDS3,
			media_type_unknown, medium_density_code_DDS3},
	{"DDS4", Media_DDS4,
			media_type_unknown, medium_density_code_DDS4},
	{"DDS5", Media_DDS5,
			media_type_unknown, medium_density_code_DDS5},
};

static void (*drive_init)(struct lu_phy_attr *) = init_default_ssc;

static void usage(char *progname) {
	printf("Usage: %s -q <Q number> [-d] [-v]\n", progname);
	printf("       Where:\n");
	printf("              'q number' is the queue priority number\n");
	printf("              'd' == debug\n");
	printf("              'v' == verbose\n");
}


static void
mk_sense_short_block(uint32_t requested, uint32_t processed, uint8_t *sense_valid)
{
	int difference = (int)requested - (int)processed;

	/* No sense, ILI bit set */
	mkSenseBuf(SD_ILI, NO_ADDITIONAL_SENSE, sense_valid);

	MHVTL_DBG(2, "Short block read: Requested: %d, Read: %d,"
			" short by %d bytes",
					requested, processed, difference);

	/* Now fill in the datablock with number of bytes not read/written */
	put_unaligned_be32(difference, &sense[3]);
}

static int lookup_media_int(char *s)
{
	int i;

	MHVTL_DBG(2, "looking for media type %s", s);

	for (i = 0; i < ARRAY_SIZE(media_info); i++)
		if (!strcmp(media_info[i].name, s))
			return media_info[i].media_type;

	return Media_undefined;
}

static const char *lookup_density_name(int den)
{
	int i;

	MHVTL_DBG(2, "looking for density type 0x%02x", den);

	for (i = 0; i < ARRAY_SIZE(media_info); i++)
		if (media_info[i].media_density == den)
			return media_info[i].name;

	return "(UNKNOWN density)";
}

static const char *lookup_media_type(int med)
{
	int i;

	MHVTL_DBG(2, "looking for media type 0x%02x", med);

	for (i = 0; i < ARRAY_SIZE(media_info); i++)
		if (media_info[i].media_type == med)
			return media_info[i].name;

	return "(UNKNOWN media type)";
}

int lookup_mode_media_type(int med)
{
	int i;

	MHVTL_DBG(2, "looking for mode media type for 0x%02x", med);

	for (i = 0; i < ARRAY_SIZE(media_info); i++) {
		MHVTL_DBG(3, "%s : 0x%02x mode media type 0x%02x",
			media_info[i].name,
			media_info[i].media_type,
			media_info[i].mode_media_type);
		if (media_info[i].media_type == med)
			return media_info[i].mode_media_type;
	}

	return media_type_unknown;
}

/***********************************************************************/

/*
 * Report supported densities
 */

#define REPORT_DENSITY_LEN 52
int resp_report_density(struct priv_lu_ssc *lu_priv, uint8_t media,
						struct vtl_ds *dbuf_p)
{
	uint8_t *buf = dbuf_p->data;
	struct list_head *l_head;
	struct density_info *di;
	struct supported_density_list *den;
	int len = dbuf_p->sz;
	int count;
	uint32_t a;
	uint8_t *ds;	/* Density Support Data Block Descriptor */

	l_head = &lu_priv->pm->lu->den_list;

	/* Zero out buf */
	memset(buf, 0, len);
	ds = &buf[4];
	count = 0;

	buf[2] = 0;	/* Reserved */
	buf[3] = 0;	/* Reserved */

	/* Assigning Oranization (8 chars long) */
	if (media) { /* Report supported density by this media */
		count = 1;

		ds[0] = mam.MediumDensityCode;
		ds[1] = mam.MediumDensityCode;
		ds[2] = (OK_to_write) ? 0xa0 : 0x20; /* Set write OK flg */
		put_unaligned_be16(REPORT_DENSITY_LEN, &ds[3]);
		memcpy(&ds[5], &mam.media_info.bits_per_mm, 3);
		memcpy(&ds[8], &mam.MediumWidth, 2);
		memcpy(&ds[10], &mam.MediumLength, 2);
		memcpy(&ds[12], &mam.max_capacity, 4);

		snprintf((char *)&ds[16], 9, "%-8s",
					mam.AssigningOrganization_1);
		snprintf((char *)&ds[24], 9, "%-8s",
					mam.media_info.density_name);
		snprintf((char *)&ds[32], 20, "%-20s",
					mam.media_info.description);
		/* Fudge.. Now 'fix' up the spaces. */
		for (a = 16; a < REPORT_DENSITY_LEN; a++)
			if (!ds[a])
				ds[a] = 0x20; /* replace 0 with ' ' */
	} else { /* Report supported density by this drive */
		list_for_each_entry(den, l_head, siblings) {
			di = den->density_info;
			count++;

			MHVTL_DBG(2, "%s -> %s", di->description,
					(den->rw) ? "RW" : "RO");

			ds[0] = di->density;
			ds[1] = di->density;
			ds[2] = (den->rw) ? 0xa0 : 0x20; /* Set write OK flg */
			put_unaligned_be16(REPORT_DENSITY_LEN, &ds[3]);
			put_unaligned_be24(di->bits_per_mm, &ds[5]);
			put_unaligned_be16(di->media_width, &ds[8]);
			put_unaligned_be16(di->tracks, &ds[10]);
			put_unaligned_be32(di->capacity, &ds[12]);
			snprintf((char *)&ds[16], 9, "%-8s", di->assigning_org);
			snprintf((char *)&ds[24], 9, "%-8s", di->density_name);
			snprintf((char *)&ds[32], 20, "%-20s",
					di->description);
			/* Fudge.. Now 'fix' up the spaces. */
			for (a = 16; a < REPORT_DENSITY_LEN; a++)
				if (!ds[a])
					ds[a] = 0x20; /* replace 0 with ' ' */
			ds += REPORT_DENSITY_LEN;
		}
	}
	put_unaligned_be16(REPORT_DENSITY_LEN * count, &buf[0]);
	return REPORT_DENSITY_LEN * count + 4;
}

/*
 * Read Attribute
 *
 * Fill in 'buf' with data and return number of bytes
 */
int resp_read_attribute(struct scsi_cmd *cmd)
{
	uint16_t attrib;
	uint32_t alloc_len;
	int ret_val = 0;
	int byte_index = 4;
	int indx, found_attribute;
	uint8_t *cdb = cmd->scb;
	uint8_t *buf = cmd->dbuf_p->data;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	attrib = get_unaligned_be16(&cdb[8]);
	alloc_len = get_unaligned_be32(&cdb[10]);
	MHVTL_DBG(2, "Read Attribute: 0x%x, allocation len: %d",
							attrib, alloc_len);

	memset(buf, 0, alloc_len);	/* Clear memory */

	if (cdb[1] == 0) {
		/* Attribute Values */
		for (indx = found_attribute = 0; MAM_Attributes[indx].length; indx++) {
			if (attrib == MAM_Attributes[indx].attribute)
				found_attribute = 1;

			if (found_attribute) {
				/* calculate available data length */
				ret_val += MAM_Attributes[indx].length + 5;
				if (ret_val < alloc_len) {
					/* add it to output */
					buf[byte_index++] = MAM_Attributes[indx].attribute >> 8;
					buf[byte_index++] = MAM_Attributes[indx].attribute;
					buf[byte_index++] = (MAM_Attributes[indx].read_only << 7) | MAM_Attributes[indx].format;
					buf[byte_index++] = MAM_Attributes[indx].length >> 8;
					buf[byte_index++] = MAM_Attributes[indx].length;
					memcpy(&buf[byte_index], MAM_Attributes[indx].value, MAM_Attributes[indx].length);
					byte_index += MAM_Attributes[indx].length;
				}
			}
		}
		if (!found_attribute) {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
			return 0;
		}
	} else {
		/* Attribute List */
		for (indx = found_attribute = 0; MAM_Attributes[indx].length; indx++) {
			/* calculate available data length */
			ret_val += 2;
			if (ret_val <= alloc_len) {
				/* add it to output */
				buf[byte_index++] = MAM_Attributes[indx].attribute >> 8;
				buf[byte_index++] = MAM_Attributes[indx].attribute;
			}
		}
	}

	put_unaligned_be32(ret_val, &buf[0]);

	if (ret_val > alloc_len)
		ret_val = alloc_len;

	return ret_val;
}

/*
 * Process WRITE ATTRIBUTE scsi command
 * Returns 0 if OK
 *         or 1 if MAM needs to be written.
 *         or -1 on failure.
 */
int resp_write_attribute(struct scsi_cmd *cmd)
{
	uint32_t alloc_len;
	int byte_index;
	int indx, attrib, attribute_length, found_attribute = 0;
	struct MAM *mamp;
	struct MAM mam_backup;
	uint8_t *buf = cmd->dbuf_p->data;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t *cdb = cmd->scb;
	struct priv_lu_ssc *lu_priv;

	alloc_len = get_unaligned_be32(&cdb[10]);
	lu_priv = cmd->lu->lu_private;
	mamp = lu_priv->mamp;

	memcpy(&mam_backup, mamp, sizeof(struct MAM));
	for (byte_index = 4; byte_index < alloc_len; ) {
		attrib = ((uint16_t)buf[byte_index++] << 8);
		attrib += buf[byte_index++];
		for (indx = found_attribute = 0; MAM_Attributes[indx].length; indx++) {
			if (attrib == MAM_Attributes[indx].attribute) {
				found_attribute = 1;
				byte_index += 1;
				attribute_length = ((uint16_t)buf[byte_index++] << 8);
				attribute_length += buf[byte_index++];
				if ((attrib == 0x408) &&
					(attribute_length == 1) &&
						(buf[byte_index] == 0x80)) {
					/* set media to worm */
					MHVTL_LOG("Converted media to WORM");
					mamp->MediumType = MEDIA_TYPE_WORM;
				} else {
					memcpy(MAM_Attributes[indx].value,
						&buf[byte_index],
						MAM_Attributes[indx].length);
				}
				byte_index += attribute_length;
				break;
			} else {
				found_attribute = 0;
			}
		}
		if (!found_attribute) {
			memcpy(&mamp, &mam_backup, sizeof(mamp));
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_PARMS, sam_stat);
			return 0;
		}
	}
	return found_attribute;
}

/*
 *
 * Return number of bytes read.
 *        0 on error with sense[] filled in...
 */
int readBlock(uint8_t *buf, uint32_t request_sz, int sili, uint8_t *sam_stat)
{
	uint32_t disk_blk_size, blk_size;
	uLongf uncompress_sz;
	uint8_t	*cbuf, *c2buf;
	int z;
	uint32_t tgtsize, rc;
	loff_t nread = 0;
	uint32_t save_sense;

	MHVTL_DBG(3, "Request to read: %d bytes, SILI: %d", request_sz, sili);

	/* check for a zero length read
	 * This is not an error, and shouldn't change the tape position */
	if (request_sz == 0)
		return 0;

	switch (c_pos->blk_type) {
	case B_DATA:
		break;
	case B_FILEMARK:
		MHVTL_DBG(1, "Expected to find DATA header, found: FILEMARK");
		position_blocks_forw(1, sam_stat);
		mk_sense_short_block(request_sz, 0, sam_stat);
		save_sense = get_unaligned_be32(&sense[3]);
		mkSenseBuf(NO_SENSE | SD_FILEMARK, E_MARK, sam_stat);
		put_unaligned_be32(save_sense, &sense[3]);
		return 0;
		break;
	case B_EOD:
		MHVTL_DBG(1, "Expected to find DATA header, found: EOD");
		mk_sense_short_block(request_sz, 0, sam_stat);
		save_sense = get_unaligned_be32(&sense[3]);
		mkSenseBuf(BLANK_CHECK, E_END_OF_DATA, sam_stat);
		put_unaligned_be32(save_sense, &sense[3]);
		return 0;
		break;
	default:
		MHVTL_ERR("Unknown blk header at offset %u"
				" - Abort read cmd", c_pos->blk_number);
		mkSenseBuf(MEDIUM_ERROR, E_UNRECOVERED_READ, sam_stat);
		return 0;
		break;
	}

	/* The tape block is compressed.  Save field values we will need after
	   the read causes the tape block to advance.
	*/
	blk_size = c_pos->blk_size;
	disk_blk_size = c_pos->disk_blk_size;

	/* We have a data block to read.
	   Only read upto size of allocated buffer by initiator
	*/
	tgtsize = min(request_sz, blk_size);

	/* If the tape block is uncompressed, we can read the number of bytes
	   we need directly into the scsi read buffer and we are done.
	*/
	if (!(c_pos->blk_flags & BLKHDR_FLG_COMPRESSED)) {
		if (read_tape_block(buf, tgtsize, sam_stat) != tgtsize) {
			MHVTL_ERR("read failed, %s", strerror(errno));
			mkSenseBuf(MEDIUM_ERROR, E_UNRECOVERED_READ, sam_stat);
			return 0;
		}
		if (tgtsize != request_sz)
			mk_sense_short_block(request_sz, tgtsize, sam_stat);
		else if (!sili) {
			if (request_sz < blk_size)
				mk_sense_short_block(request_sz, blk_size, sam_stat);
		}
		lu_ssc.bytesRead_I += tgtsize;
		lu_ssc.bytesRead_M += tgtsize;
		return tgtsize;
	}

	/* Malloc a buffer to hold the compressed data, and read the
	   data into it.
	*/
	cbuf = malloc(disk_blk_size);
	if (!cbuf) {
		MHVTL_ERR("Out of memory: %d", __LINE__);
		mkSenseBuf(MEDIUM_ERROR, E_DECOMPRESSION_CRC, sam_stat);
		return 0;
	}

	nread = read_tape_block(cbuf, disk_blk_size, sam_stat);
	if (nread != disk_blk_size) {
		MHVTL_ERR("read failed, %s", strerror(errno));
		mkSenseBuf(MEDIUM_ERROR, E_UNRECOVERED_READ, sam_stat);
		free(cbuf);
		return 0;
	}

	rc = tgtsize;
	uncompress_sz = blk_size;

	/* If the scsi read buffer is at least as big as the size of
	   the uncompressed data then we can uncompress directly into
	   the read buffer.  If not, then we need an extra buffer to
	   uncompress into, then memcpy the subrange we need to the
	   read buffer.
	*/

	if (tgtsize == blk_size) {
		/* block sizes match, uncompress directly into buf */
		z = uncompress(buf, &uncompress_sz, cbuf, disk_blk_size);
	} else {
		/* Initiator hasn't requested same size as data block */
		if ((c2buf = malloc(uncompress_sz)) == NULL) {
			MHVTL_ERR("Out of memory: %d", __LINE__);
			mkSenseBuf(MEDIUM_ERROR, E_DECOMPRESSION_CRC, sam_stat);
			free(cbuf);
			return 0;
		}
		z = uncompress(c2buf, &uncompress_sz, cbuf, disk_blk_size);
		/* Now copy 'requested size' of data into buffer */
		memcpy(buf, c2buf, tgtsize);
		free(c2buf);
	}

	lu_ssc.bytesRead_I += blk_size;
	lu_ssc.bytesRead_M += disk_blk_size;

	switch (z) {
	case Z_OK:
		MHVTL_DBG(2, "Read %u (%u) bytes of compressed"
			" data, have %u bytes for result",
			(uint32_t)nread, disk_blk_size,
			tgtsize);
		break;
	case Z_MEM_ERROR:
		MHVTL_ERR("Not enough memory to decompress");
		mkSenseBuf(MEDIUM_ERROR, E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
		break;
	case Z_DATA_ERROR:
		MHVTL_ERR("Block corrupt or incomplete");
		mkSenseBuf(MEDIUM_ERROR, E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
		break;
	case Z_BUF_ERROR:
		MHVTL_ERR("Not enough memory in destination buf");
		mkSenseBuf(MEDIUM_ERROR, E_DECOMPRESSION_CRC, sam_stat);
		rc = 0;
		break;
	}
	free(cbuf);

	if (rc != request_sz)
		mk_sense_short_block(request_sz, rc, sam_stat);
	else if (!sili) {
		if (request_sz < blk_size)
			mk_sense_short_block(request_sz, blk_size, sam_stat);
	}

	return rc;
}


/*
 * Return number of bytes written to 'file'
 *
 * Zero on error with sense buffer already filled in
 */
int writeBlock(struct scsi_cmd *cmd, uint32_t src_sz)
{
	Bytef *dest_buf;
	uLong dest_len;
	uLong src_len = src_sz;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t *src_buf = cmd->dbuf_p->data;
	struct priv_lu_ssc *lu_priv;
	uint64_t current_position;
	int rc;
	int z;

	lu_priv = cmd->lu->lu_private;

	/* Determine whether or not to store the crypto info in the tape
	 * blk_header.
	 * We may adjust this decision for the 3592. (See ibm_3592_xx.pm)
	 */
	lu_priv->cryptop = lu_priv->ENCRYPT_MODE == 2 ? &encryption : NULL;

	if (lu_priv->pm->valid_encryption_media)
		lu_priv->pm->valid_encryption_media(cmd);

	if (*lu_priv->compressionFactor) {
		dest_len = compressBound(src_sz);
		dest_buf = malloc(dest_len);
		if (!dest_buf) {
			MHVTL_ERR("malloc(%d) failed", (int)dest_len);
			mkSenseBuf(MEDIUM_ERROR, E_WRITE_ERROR, sam_stat);
			return 0;
		}
		z = compress2(dest_buf, &dest_len, src_buf, src_sz,
						*lu_priv->compressionFactor);
		if (z != Z_OK) {
			switch (z) {
			case Z_MEM_ERROR:
				MHVTL_ERR("Not enough memory to compress "
						"data");
				break;
			case Z_BUF_ERROR:
				MHVTL_ERR("Not enough memory in destination "
						"buf to compress data");
				break;
			case Z_DATA_ERROR:
				MHVTL_ERR("Input data corrupt / incomplete");
				break;
			}
			mkSenseBuf(HARDWARE_ERROR, E_COMPRESSION_CHECK,
							sam_stat);
			return 0;
		}
		MHVTL_DBG(2, "Compression: Orig %d, after comp: %ld"
				", Compression factor: %d",
					src_sz, (unsigned long)dest_len,
					*lu_priv->compressionFactor);
	} else {
		dest_buf = src_buf;
		dest_len = 0;	/* no compression */
	}

	rc = write_tape_block(dest_buf, src_len, dest_len, lu_priv->cryptop, sam_stat);

	if (*lu_priv->compressionFactor != Z_NO_COMPRESSION) {
		free(dest_buf);
		lu_priv->bytesWritten_I += src_len;
		lu_priv->bytesWritten_M += dest_len;
	} else {
		lu_priv->bytesWritten_I += src_len;
		lu_priv->bytesWritten_M += dest_len;
	}

	if (rc < 0)
		return 0;

	current_position = current_tape_offset();

	if (current_position <= lu_priv->prog_early_warning_position) {
		put_unaligned_be64(lu_priv->max_capacity - current_position,
						&mam.remaining_capacity);
	} else if (current_position >= lu_priv->max_capacity) {
		mam.remaining_capacity = 0L;
		MHVTL_DBG(1, "End of Medium - VOLUME_OVERFLOW/EOM");
		mkSenseBuf(VOLUME_OVERFLOW | SD_EOM, E_EOM, sam_stat);
	} else if (current_position >= lu_priv->early_warning_position) {
		mam.remaining_capacity = 0L;
		MHVTL_DBG(1, "End of Medium (early warning) - Setting EOM flag");
		mkSenseBuf(NO_SENSE | SD_EOM, NO_ADDITIONAL_SENSE, sam_stat);
	} else if (current_position >= lu_priv->prog_early_warning_position) {
		mam.remaining_capacity = 0L;
		MHVTL_DBG(1, "End of Medium - Programmable Early Warning");
		mkSenseBuf(NO_SENSE | SD_EOM,
					E_PROGRAMMABLE_EARLY_WARNING, sam_stat);
	}

	return src_len;
}

/*
 * Space over (to) x filemarks. Setmarks not supported as yet.
 */
void resp_space(int32_t count, int code, uint8_t *sam_stat)
{
	switch (code) {
	/* Space 'count' blocks */
	case 0:
		if (count >= 0)
			position_blocks_forw(count, sam_stat);
		else
			position_blocks_back(-count, sam_stat);
		break;
	/* Space 'count' filemarks */
	case 1:
		if (count >= 0)
			position_filemarks_forw(count, sam_stat);
		else
			position_filemarks_back(-count, sam_stat);
		break;
	/* Space to end-of-data - Ignore 'count' */
	case 3:
		position_to_eod(sam_stat);
		break;

	default:
		mkSenseBuf(ILLEGAL_REQUEST,E_INVALID_FIELD_IN_CDB, sam_stat);
		break;
	}
}

#ifdef MHVTL_DEBUG
static char *sps_pg0 = "Tape Data Encyrption in Support page";
static char *sps_pg1 = "Tape Data Encyrption Out Support Page";
static char *sps_pg16 = "Data Encryption Capabilities page";
static char *sps_pg17 = "Supported key formats page";
static char *sps_pg18 = "Data Encryption management capabilities page";
static char *sps_pg32 = "Data Encryption Status page";
static char *sps_pg33 = "Next Block Encryption Status Page";
static char *sps_pg48 = "Random Number Page";
static char *sps_pg49 = "Device Server Key Wrapping Public Key page";
static char *sps_reserved = "Security Protcol Specific : reserved value";

static char *lookup_sp_specific(uint16_t field)
{
	MHVTL_DBG(3, "Lookup %d", field);
	switch (field) {
	case 0:	return sps_pg0;
	case 1: return sps_pg1;
	case 16: return sps_pg16;
	case 17: return sps_pg17;
	case 18: return sps_pg18;
	case 32: return sps_pg32;
	case 33: return sps_pg33;
	case 48: return sps_pg48;
	case 49: return sps_pg49;
	default: return sps_reserved;
	break;
	}
}
#endif

#define SUPPORTED_SECURITY_PROTOCOL_LIST 0
#define CERTIFICATE_DATA		 1
#define SECURITY_PROTOCOL_INFORMATION	0
#define TAPE_DATA_ENCRYPTION		0x20

/* FIXME:
 * Took this certificate from my Ubuntu install
 *          /usr/share/doc/libssl-dev/demos/tunala/CA.pem
 * 		I wonder if RIAA is in NZ ?
 *
 * Need to insert a valid certificate of my own here...
 */
#include "vtltape.pem"

/*
 * Returns number of bytes in struct
 */
static int resp_spin_page_0(uint8_t *buf, uint16_t sps, uint32_t alloc_len, uint8_t *sam_stat)
{
	int ret = 0;

	MHVTL_DBG(2, "%s", lookup_sp_specific(sps));

	memset(buf, 0, alloc_len);
	switch (sps) {
	case SUPPORTED_SECURITY_PROTOCOL_LIST:
		buf[6] = 0;	/* list length (MSB) */
		buf[7] = 2;	/* list length (LSB) */
		buf[8] = SECURITY_PROTOCOL_INFORMATION;
		buf[9] = TAPE_DATA_ENCRYPTION;
		ret = 10;
		break;

	case CERTIFICATE_DATA:
		strncpy((char *)&buf[4], certificate, alloc_len - 4);
		if (strlen(certificate) >= alloc_len - 4) {
			put_unaligned_be16(alloc_len - 4, &buf[2]);
			ret = alloc_len;
		} else {
			put_unaligned_be16(strlen(certificate), &buf[2]);
			ret = strlen(certificate) + 4;
		}
		break;

	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
	}
	return ret;
}

/*
 * Return number of valid bytes in data structure
 */
static int resp_spin_page_20(struct scsi_cmd *cmd)
{
	int ret = 0;
	int indx, count, correct_key;
	uint8_t *buf = cmd->dbuf_p->data;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint16_t sps = get_unaligned_be16(&cmd->scb[2]);
	uint32_t alloc_len = get_unaligned_be32(&cmd->scb[6]);
	struct priv_lu_ssc *lu_priv;
	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(2, "%s", lookup_sp_specific(sps));

	memset(buf, 0, alloc_len);
	switch (sps) {
	case ENCR_IN_SUPPORT_PAGES:
		put_unaligned_be16(ENCR_IN_SUPPORT_PAGES, &buf[0]);
		put_unaligned_be16(14, &buf[2]); /* List length */
		put_unaligned_be16(ENCR_IN_SUPPORT_PAGES, &buf[4]);
		put_unaligned_be16(ENCR_OUT_SUPPORT_PAGES, &buf[6]);
		put_unaligned_be16(ENCR_CAPABILITIES, &buf[8]);
		put_unaligned_be16(ENCR_KEY_FORMATS, &buf[10]);
		put_unaligned_be16(ENCR_KEY_MGT_CAPABILITIES, &buf[12]);
		put_unaligned_be16(ENCR_DATA_ENCR_STATUS, &buf[14]);
		put_unaligned_be16(ENCR_NEXT_BLK_ENCR_STATUS, &buf[16]);
		ret = 18;
		break;

	case ENCR_OUT_SUPPORT_PAGES:
		put_unaligned_be16(ENCR_OUT_SUPPORT_PAGES, &buf[0]);
		put_unaligned_be16(2, &buf[2]); /* List length */
		put_unaligned_be16(ENCR_SET_DATA_ENCRYPTION, &buf[4]);
		ret = 6;
		break;

	case ENCR_CAPABILITIES:
		ret = lu_priv->pm->encryption_capabilities(cmd);
		break;

	case ENCR_KEY_FORMATS:
		put_unaligned_be16(ENCR_KEY_FORMATS, &buf[0]);
		put_unaligned_be16(2, &buf[2]); /* List length */
		put_unaligned_be16(0, &buf[4]);	/* Plain text */
		ret = 6;
		break;

	case ENCR_KEY_MGT_CAPABILITIES:
		put_unaligned_be16(ENCR_KEY_MGT_CAPABILITIES, &buf[0]);
		put_unaligned_be16(0x0c, &buf[2]); /* List length */
		buf[4] = 1;	/* LOCK_C */
		buf[5] = 7;	/* CKOD_C, DKOPR_C, CKORL_C */
		buf[6] = 0;	/* Reserved */
		buf[7] = 7;	/* AITN_C, LOCAL_C, PUBLIC_C */
		/* buf 8 - 15 reserved */
		ret = 16;
		break;

	case ENCR_DATA_ENCR_STATUS:
		put_unaligned_be16(ENCR_DATA_ENCR_STATUS, &buf[0]);
		put_unaligned_be16(0x20, &buf[2]); /* List length */
		buf[4] = 0x21;	/* I_T Nexus scope and Key Scope */
		buf[5] = lu_priv->ENCRYPT_MODE;
		buf[6] = lu_priv->DECRYPT_MODE;
		buf[7] = 0x01;	/* Algorithm Index */
		put_unaligned_be32(lu_priv->KEY_INSTANCE_COUNTER, &buf[8]);
		ret = 24;
		indx = 24;
		if (UKAD_LENGTH) {
			buf[3] += 4 + UKAD_LENGTH;
			buf[indx++] = 0x00;
			buf[indx++] = 0x00;
			buf[indx++] = 0x00;
			buf[indx++] = UKAD_LENGTH;
			for (count = 0; count < UKAD_LENGTH; ++count) {
				buf[indx++] = UKAD[count];
			}
			ret += 4 + UKAD_LENGTH;
		}
		if (AKAD_LENGTH) {
			buf[3] += 4 + AKAD_LENGTH;
			buf[indx++] = 0x01;
			buf[indx++] = 0x00;
			buf[indx++] = 0x00;
			buf[indx++] = AKAD_LENGTH;
			for (count = 0; count < AKAD_LENGTH; ++count) {
				buf[indx++] = AKAD[count];
			}
			ret += 4 + AKAD_LENGTH;
		}
		break;

	case ENCR_NEXT_BLK_ENCR_STATUS:
		if (lu_priv->tapeLoaded != TAPE_LOADED) {
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
			break;
		}
		/* c_pos contains the NEXT block's header info already */
		put_unaligned_be16(ENCR_NEXT_BLK_ENCR_STATUS, &buf[0]);
		buf[2] = 0;	/* List length (MSB) */
		buf[3] = 12;	/* List length (MSB) */
		if (sizeof(loff_t) > 32)
			put_unaligned_be64(c_pos->blk_number, &buf[4]);
		else
			put_unaligned_be32(c_pos->blk_number, &buf[8]);
		if (c_pos->blk_type != B_DATA)
			buf[12] = 0x2; /* not a logical block */
		else
			buf[12] = 0x3; /* not encrypted */
		buf[13] = 0x01; /* Algorithm Index */
		ret = 16;
		if (c_pos->blk_flags & BLKHDR_FLG_ENCRYPTED) {
			correct_key = TRUE;
			indx = 16;
			if (c_pos->encryption.ukad_length) {
				buf[3] += 4 + c_pos->encryption.ukad_length;
				buf[indx++] = 0x00;
				buf[indx++] = 0x01;
				buf[indx++] = 0x00;
				buf[indx++] = c_pos->encryption.ukad_length;
				for (count = 0; count < c_pos->encryption.ukad_length; ++count) {
					buf[indx++] = c_pos->encryption.ukad[count];
				}
				ret += 4 + c_pos->encryption.ukad_length;
			}
			if (c_pos->encryption.akad_length) {
				buf[3] += 4 + c_pos->encryption.akad_length;
				buf[indx++] = 0x01;
				buf[indx++] = 0x03;
				buf[indx++] = 0x00;
				buf[indx++] = c_pos->encryption.akad_length;
				for (count = 0; count < c_pos->encryption.akad_length; ++count) {
					buf[indx++] = c_pos->encryption.akad[count];
				}
				ret += 4 + c_pos->encryption.akad_length;
			}
			/* compare the keys */
			if (correct_key) {
				if (c_pos->encryption.key_length != KEY_LENGTH)
					correct_key = FALSE;
				for (count = 0; count < c_pos->encryption.key_length; ++count) {
					if (c_pos->encryption.key[count] != KEY[count]) {
						correct_key = FALSE;
						break;
					}
				}
			}
			if (correct_key)
				buf[12] = 0x5; /* encrypted, correct key */
			else
				buf[12] = 0x6; /* encrypted, need key */
		}
		break;

	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
	}
	return ret;
}

/*
 * Retrieve Security Protocol Information
 */
uint8_t resp_spin(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t *buf = cmd->dbuf_p->data;
	uint8_t *cdb = cmd->scb;
	uint16_t sps = get_unaligned_be16(&cmd->scb[2]);
	uint32_t alloc_len = get_unaligned_be32(&cdb[6]);
	uint8_t inc_512 = (cdb[4] & 0x80) ? 1 : 0;

	cmd->dbuf_p->sz = 0;

	if (inc_512)
		alloc_len = alloc_len * 512;

	switch (cdb[1]) {
	case SECURITY_PROTOCOL_INFORMATION:
		cmd->dbuf_p->sz = resp_spin_page_0(buf, sps, alloc_len, sam_stat);
		break;
	case TAPE_DATA_ENCRYPTION:
		cmd->dbuf_p->sz = resp_spin_page_20(cmd);
		break;
	default:
		MHVTL_DBG(1, "Security protocol 0x%04x unknown", cdb[1]);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	return SAM_STAT_GOOD;
}

uint8_t resp_spout(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t	*buf = cmd->dbuf_p->data;
	struct lu_phy_attr *lu;
	struct priv_lu_ssc *lu_priv;
	int count;
#ifdef MHVTL_DEBUG
	uint16_t sps = get_unaligned_be16(&cmd->scb[2]);
	uint8_t inc_512 = (cmd->scb[4] & 0x80) ? 1 : 0;
#endif

	lu = cmd->lu;
	lu_priv = cmd->lu->lu_private;

	if (cmd->scb[1] != TAPE_DATA_ENCRYPTION) {
		MHVTL_DBG(1, "Security protocol 0x%02x unknown", cmd->scb[1]);
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	MHVTL_DBG(2, "Tape Data Encryption, %s, "
			" alloc len: 0x%02x, inc_512: %s",
				lookup_sp_specific(sps),
				cmd->dbuf_p->sz, (inc_512) ? "Set" : "Unset");

	/* check for a legal "set data encryption page" */
	if ((buf[0] != 0x00) || (buf[1] != 0x10) ||
	    (buf[2] != 0x00) || (buf[3] < 16) ||
	    (buf[8] != 0x01) || (buf[9] != 0x00)) {
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	lu_ssc.KEY_INSTANCE_COUNTER++;
	lu_ssc.ENCRYPT_MODE = buf[6];
	lu_ssc.DECRYPT_MODE = buf[7];
	UKAD_LENGTH = 0;
	AKAD_LENGTH = 0;
	KEY_LENGTH = get_unaligned_be16(&buf[18]);
	for (count = 0; count < KEY_LENGTH; ++count) {
		KEY[count] = buf[20 + count];
	}

	MHVTL_DBG(2, "Encrypt mode: %d Decrypt mode: %d, "
			"ukad len: %d akad len: %d",
				lu_ssc.ENCRYPT_MODE, lu_ssc.DECRYPT_MODE,
				UKAD_LENGTH, AKAD_LENGTH);

	if (cmd->dbuf_p->sz > (19 + KEY_LENGTH + 4)) {
		if (buf[20 + KEY_LENGTH] == 0x00) {
			UKAD_LENGTH = get_unaligned_be16(&buf[22 + KEY_LENGTH]);
			for (count = 0; count < UKAD_LENGTH; ++count) {
				UKAD[count] = buf[24 + KEY_LENGTH + count];
			}
		} else if (buf[20 + KEY_LENGTH] == 0x01) {
			AKAD_LENGTH = get_unaligned_be16(&buf[22 + KEY_LENGTH]);
			for (count = 0; count < AKAD_LENGTH; ++count) {
				AKAD[count] = buf[24 + KEY_LENGTH + count];
			}
		}
	}

	count = lu_priv->pm->kad_validation(lu_ssc.ENCRYPT_MODE,
						UKAD_LENGTH, AKAD_LENGTH);

	/* For some reason, this command needs to be failed */
	if (count) {
		lu_ssc.KEY_INSTANCE_COUNTER--;
		lu_ssc.ENCRYPT_MODE = 0;
		lu_ssc.DECRYPT_MODE = buf[7];
		UKAD_LENGTH = 0;
	        AKAD_LENGTH = 0;
		KEY_LENGTH = 0;
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (!lu_priv->pm->update_encryption_mode)
		lu_priv->pm->update_encryption_mode(&lu->mode_pg, NULL, lu_ssc.ENCRYPT_MODE);

	return SAM_STAT_GOOD;
}

/*
 * Update MAM contents with current counters
 */
static void updateMAM(uint8_t *sam_stat, int loadCount)
{
	uint64_t bw;		/* Bytes Written */
	uint64_t br;		/* Bytes Read */
	uint64_t load;	/* load count */

	MHVTL_DBG(2, "updateMAM(%d)", loadCount);

	/* Update bytes written this load. */
	put_unaligned_be64(lu_ssc.bytesWritten_I, &mam.WrittenInLastLoad);
	put_unaligned_be64(lu_ssc.bytesRead_I, &mam.ReadInLastLoad);

	/* Update total bytes read/written */
	bw = get_unaligned_be64(&mam.WrittenInMediumLife);
	bw += lu_ssc.bytesWritten_I;
	put_unaligned_be64(bw, &mam.WrittenInMediumLife);

	br = get_unaligned_be64(&mam.ReadInMediumLife);
	br += lu_ssc.bytesRead_I;
	put_unaligned_be64(br, &mam.ReadInMediumLife);

	/* Update load count */
	if (loadCount) {
		load = get_unaligned_be64(&mam.LoadCount);
		load++;
		put_unaligned_be64(load, &mam.LoadCount);
	}

	rewriteMAM(sam_stat);
}

/*
 *
 * Process the SCSI command
 *
 * Called with:
 *	cdev     -> Char dev file handle,
 *	cdb      -> SCSI Command buffer pointer,
 *	dbuf     -> struct vtl_ds *
 */
static void processCommand(int cdev, uint8_t *cdb, struct vtl_ds *dbuf_p)
{
	static int last_count;
	struct scsi_cmd _cmd;
	struct scsi_cmd *cmd;
	cmd = &_cmd;

	cmd->scb = cdb;
	cmd->scb_len = 16;	/* fixme */
	cmd->dbuf_p = dbuf_p;
	cmd->lu = &lunit;
	cmd->cdev = cdev;

	dbuf_p->sz = 0;

	if ((cdb[0] == READ_6 || cdb[0] == WRITE_6) && cdb[0] == last_cmd) {
		MHVTL_DBG_PRT_CDB(2, dbuf_p->serialNo, cdb);
		if ((++last_count % 50) == 0) {
			MHVTL_DBG(1, "%dth contiguous %s request (%ld) ",
				last_count,
				last_cmd == READ_6 ? "READ_6" : "WRITE_6",
				(long)dbuf_p->serialNo);
		}
	} else {
		MHVTL_DBG_PRT_CDB(1, dbuf_p->serialNo, cdb);
		last_count = 0;
	}

	/* Limited subset of commands don't need to check for power-on reset */
	switch (cdb[0]) {
	case REPORT_LUNS:
	case REQUEST_SENSE:
	case MODE_SELECT:
	case INQUIRY:
		dbuf_p->sam_stat = SAM_STAT_GOOD;
		break;
	default:
		if (check_reset(&dbuf_p->sam_stat))
			return;
	}

	if (cmd->lu->scsi_ops->ops[cdb[0]].pre_cmd_perform)
		cmd->lu->scsi_ops->ops[cdb[0]].pre_cmd_perform(cmd, NULL);

	dbuf_p->sam_stat = cmd->lu->scsi_ops->ops[cdb[0]].cmd_perform(cmd);

	last_cmd = cdb[0];

	return;
}

static struct media_details *check_media_can_load(struct list_head *mdl, int mt)
{
	struct media_details *m_detail;

	MHVTL_DBG(2, "Looking for media_type: 0x%02x", mt);

	list_for_each_entry(m_detail, mdl, siblings) {
		MHVTL_DBG(3, "testing against m_detail->media_type (0x%02x)",
						m_detail->media_type);
		if (m_detail->media_type == mt)
			return m_detail;
	}
	return NULL;
}

/*
 * Attempt to load PCL - i.e. Open datafile and read in BOT header & MAM
 *
 * Returns:
 * == 0 -> Load OK
 * == 1 -> Tape already loaded.
 * == 2 -> format corrupt.
 * == 3 -> cartridge does not exist or cannot be opened.
 */

static int loadTape(char *PCL, uint8_t *sam_stat)
{
	int rc;
	uint64_t fg = 0;	/* TapeAlert flags */
	struct media_details *m_detail;
	struct lu_phy_attr *lu;

	lu_ssc.bytesWritten_I = 0;	/* Global - Bytes written this load */
	lu_ssc.bytesWritten_M = 0;	/* Global - Bytes written this load */
	lu_ssc.bytesRead_I = 0;		/* Global - Bytes read this load */
	lu_ssc.bytesRead_M = 0;		/* Global - Bytes read this load */
	lu = lu_ssc.pm->lu;

	rc = load_tape(PCL, sam_stat);
	if (rc) {
		MHVTL_DBG(1, "Media load failed.. Unsupported format");
		lu_ssc.mediaSerialNo[0] = '\0';
		if (rc == 2) {
			/* TapeAlert - Unsupported format */
			fg = 0x800;
			update_TapeAlert(lu, fg);
		}
		return rc;
	}

	lu_ssc.tapeLoaded = TAPE_LOADED;
	lu_ssc.pm->media_load(lu, TAPE_LOADED);

	strncpy((char *)lu_ssc.mediaSerialNo, (char *)mam.MediumSerialNumber,
				sizeof(mam.MediumSerialNumber) - 1);

	MHVTL_DBG(1, "Media type '%s' loaded with S/No. : %s",
		lookup_media_type(mam.MediaType), mam.MediumSerialNumber);

	lu_ssc.max_capacity = 0L;

	switch(mam.MediumType) {
	case MEDIA_TYPE_DATA:
		current_state = MHVTL_STATE_LOADING;
		OK_to_write = 1;	/* Reset flag to OK. */
		if (lu_ssc.pm->clear_WORM)
			lu_ssc.pm->clear_WORM(&lu->mode_pg);
		lu_ssc.max_capacity = get_unaligned_be64(&mam.max_capacity);
		mkSenseBuf(UNIT_ATTENTION, E_NOT_READY_TO_TRANSITION, sam_stat);
		break;
	case MEDIA_TYPE_CLEAN:
		current_state = MHVTL_STATE_LOADING_CLEAN;
		OK_to_write = 0;
		if (lu_ssc.pm->clear_WORM)
			lu_ssc.pm->clear_WORM(&lu->mode_pg);
		if (lu_ssc.pm->cleaning_media)
			lu_ssc.pm->cleaning_media(&lu_ssc);
		fg |= 0x400;
		MHVTL_DBG(1, "Cleaning media loaded");
		mkSenseBuf(UNIT_ATTENTION,E_CLEANING_CART_INSTALLED, sam_stat);
		break;
	case MEDIA_TYPE_WORM:
		current_state = MHVTL_STATE_LOADING_WORM;
		/* Special condition...
		* If we
		* - rewind,
		* - write filemark
		* - EOD
		* We set this as writable media as the tape is blank.
		*/
		if (!lu_ssc.pm->set_WORM) { /* PM doesn't support WORM */
			MHVTL_DBG(1, "load failed - WORM media,"
					" but drive doesn't support WORM");
			goto mismatchmedia;
		}

		if (c_pos->blk_type == B_EOD) {
			OK_to_write = 1;
		} else if (c_pos->blk_type != B_FILEMARK) {
			OK_to_write = 0;

		/* Check that this header is a filemark and
		 * the next header is End of Data.
		 * If it is, we are OK to write
		 */
		} else if (position_to_block(1, sam_stat)) {
			OK_to_write = 0;
		} else {
			if (c_pos->blk_type == B_EOD) {
				OK_to_write = 1;
			} else {
				OK_to_write = 0;
			}
			rewind_tape(sam_stat);
		}
		if (lu_ssc.pm->set_WORM)
			lu_ssc.pm->set_WORM(&lu->mode_pg);
		MHVTL_DBG(1, "Write Once Read Many (WORM) media loaded");
		break;
	}

	/* Set TapeAlert flg 32h => */
	/*	Lost Statics */
	if (mam.record_dirty != 0) {
		fg = 0x02000000000000ull;
		MHVTL_DBG(1, "Previous unload was not clean");
	}

	if (lu_ssc.max_capacity) {
		lu_ssc.early_warning_position =
				lu_ssc.max_capacity -
				lu_ssc.early_warning_sz;

		lu_ssc.prog_early_warning_position =
				lu_ssc.early_warning_position -
				lu_ssc.prog_early_warning_sz;
	}

	MHVTL_DBG(2, "Tape capacity: %" PRId64 ", + Early Warning %" PRId64
			", + Prog Early Warning %" PRId64,
			lu_ssc.max_capacity,
			lu_ssc.early_warning_sz,
			lu_ssc.prog_early_warning_sz);

	mam.record_dirty = 1;
	/* Increment load count */
	updateMAM(sam_stat, 1);

	m_detail = check_media_can_load(&lu_ssc.supported_media_list,
						mam.MediaType);

	if (!m_detail)	/* Media not defined.. Reject */
		goto mismatchmedia;

	MHVTL_DBG(2, "Density Status: 0x%x", m_detail->load_capability);

	/* Now check for WORM support */
	if (mam.MediumType == MEDIA_TYPE_WORM) {
		/* If media is WORM, check drive will allow mount */
		if (m_detail->load_capability & (LOAD_WORM | LOAD_RW)) {
			/* Prev check will correctly set OK_to_write flag */
			MHVTL_DBG(2, "Allow LOAD as R/W WORM");
		} else if (m_detail->load_capability & (LOAD_WORM | LOAD_RO)) {
			MHVTL_DBG(2, "Allow LOAD as R/O WORM");
			OK_to_write = 0;
		} else {
			MHVTL_ERR("Load failed: Unable to load as WORM");
			goto mismatchmedia;
		}
	} else if (mam.MediumType == MEDIA_TYPE_DATA) {
		/* Allow media to be either RO or RW */
		if (m_detail->load_capability & LOAD_RO) {
			MHVTL_DBG(2, "Mounting READ ONLY");
			lu_ssc.MediaWriteProtect = MEDIA_READONLY;
			OK_to_write = 0;
		} else if (m_detail->load_capability & LOAD_RW) {
			MHVTL_DBG(2, "Mounting READ/WRITE");
			lu_ssc.MediaWriteProtect = MEDIA_WRITABLE;
			OK_to_write = 1;
		} else if (m_detail->load_capability & LOAD_FAIL) {
			MHVTL_ERR("Load failed: Data format not suitable for "
					"read/write or read-only");
			goto mismatchmedia;
		}
	} else	/* Can't write to cleaning media */
		OK_to_write = 0;

	/* Update TapeAlert flags */
	update_TapeAlert(lu, fg);

	MHVTL_DBG(1, "Media is %s",
				(OK_to_write) ? "writable" : "not writable");

	blockDescriptorBlock[0] = mam.MediumDensityCode;
	lu->mode_media_type = lookup_mode_media_type(mam.MediaType);

	MHVTL_DBG(1, "Setting MediumDensityCode to %s (0x%02x)"
			" Media type: 0x%02x",
			lookup_density_name(mam.MediumDensityCode),
			mam.MediumDensityCode,
			lu->mode_media_type);

	current_state = MHVTL_STATE_LOADED;
	return TAPE_LOADED;	/* Return successful load */

mismatchmedia:
	unload_tape(sam_stat);
	fg |= 0x800;	/* Unsupported format */
	update_TapeAlert(lu, fg);
	MHVTL_ERR("Tape %s failed to load with type '%s' in drive type '%s'",
			PCL,
			lookup_media_type(mam.MediaType),
			lu_ssc.pm->name);
	lu_ssc.tapeLoaded = TAPE_UNLOADED;
	lu_ssc.pm->media_load(lu, TAPE_UNLOADED);
	current_state = MHVTL_STATE_LOAD_FAILED;
	return TAPE_UNLOADED;
}

static void dump_linked_list(void)
{
	struct media_details *m_detail;
	struct list_head *mdl;

	MHVTL_DBG(3, "Dumping media type support");

	mdl = &lu_ssc.supported_media_list;;

	list_for_each_entry(m_detail, mdl, siblings) {
		MHVTL_DBG(3, "Media type: 0x%02x, status: 0x%02x",
				m_detail->media_type,
				m_detail->load_capability);
	}
}

/* Strip (recover) the 'Physical Cartridge Label'
 *   Well at least the data filename which relates to the same thing
 */
static char * strip_PCL(char *p, int start)
{
	char *q;

	/* p += 4 (skip over 'load' string)
	 * Then keep going until '*p' is a space or NULL
	 */
	for (p += start; *p == ' '; p++)
		if ('\0' == *p)
			break;
	q = p;	/* Set end-of-word marker to start of word. */
	for (q = p; *q != '\0'; q++)
		if (*q == ' ' || *q == '\t')
			break;
	*q = '\0';	/* Set null terminated string */

return p;
}

void unloadTape(uint8_t *sam_stat)
{
	struct lu_phy_attr *lu = lu_ssc.pm->lu;

	switch (lu_ssc.tapeLoaded) {
	case TAPE_LOADED:
		mam.record_dirty = 0;
		/* Don't update load count on unload -done at load time */
		updateMAM(sam_stat, 0);
		unload_tape(sam_stat);
		if (lu_ssc.pm->clear_WORM)
			lu_ssc.pm->clear_WORM(&lu->mode_pg);
		if (lu_ssc.cleaning_media_state)
			lu_ssc.cleaning_media_state = NULL;
		lu_ssc.pm->media_load(lu, TAPE_UNLOADED);
		break;
	default:
		MHVTL_DBG(2, "Tape not mounted");
		break;
	}
	OK_to_write = 0;
	lu_ssc.tapeLoaded = TAPE_UNLOADED;
}

static int processMessageQ(struct q_msg *msg, uint8_t *sam_stat)
{
	char * pcl;
	char s[128];
	struct lu_phy_attr *lu;

	lu = lu_ssc.pm->lu;

	MHVTL_DBG(1, "Sender id: %ld, msg : %s", msg->snd_id, msg->text);

	/* Tape Load message from Library */
	if (!strncmp(msg->text, "lload", 5)) {
		if (!lu_ssc.inLibrary) {
			MHVTL_DBG(2, "lload & drive not in library");
			return 0;
		}

		if (lu_ssc.tapeLoaded != TAPE_UNLOADED) {
			MHVTL_DBG(2, "Tape already mounted");
			send_msg("Load failed", msg->snd_id);
		} else {
			pcl = strip_PCL(msg->text, 6); /* 'lload ' => offset of 6 */
			loadTape(pcl, sam_stat);
			if (lu_ssc.tapeLoaded == TAPE_LOADED)
				sprintf(s, "Loaded OK: %s", pcl);
			else
				sprintf(s, "Load failed: %s", pcl);
			send_msg(s, msg->snd_id);
		}
	}

	/* Tape Load message from User space */
	if (!strncmp(msg->text, "load", 4)) {
		if (lu_ssc.inLibrary)
			MHVTL_DBG(2, "Warn: Tape assigned to library");
		if (lu_ssc.tapeLoaded == TAPE_LOADED) {
			MHVTL_DBG(2, "A tape is already mounted");
		} else {
			pcl = strip_PCL(msg->text, 4);
			loadTape(pcl, sam_stat);
		}
	}

	if (!strncmp(msg->text, "unload", 6)) {
		unloadTape(sam_stat);
		MHVTL_DBG(1, "Library requested tape unload");
	}

	if (!strncmp(msg->text, "exit", 4))
		return 1;

	if (!strncmp(msg->text, "Register", 8)) {
		lu_ssc.inLibrary = 1;
		MHVTL_DBG(1, "Notice from Library controller : %s", msg->text);
	}

	if (!strncmp(msg->text, "verbose", 7)) {
		if (verbose)
			verbose--;
		else
			verbose = 3;
		MHVTL_LOG("Verbose: %s at level %d",
				 verbose ? "enabled" : "disabled", verbose);
	}

	if (!strncmp(msg->text, "TapeAlert", 9)) {
		uint64_t flg = 0L;
		sscanf(msg->text, "TapeAlert %" PRIx64, &flg);
		update_TapeAlert(lu, flg);
	}

	if (!strncmp(msg->text, "debug", 5)) {
		if (debug > 4) {
			debug = 1;
			printf("Debug: %d\n", debug);
		} else if (debug > 1) {
			printf("Debug: %d\n", debug);
			debug++;
		} else {
			printf("Debug: %d\n", debug);
			debug++;
			verbose = 4;
		}
	}

	if (!strncmp(msg->text, "dump", 4)) {
		dump_linked_list();
	}

return 0;
}

/* Set VPD data with device serial number */
static void update_vpd_80(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0x80)];

	memcpy(vpd_pg->data, p, strlen(p));
}

static void update_vpd_83(struct lu_phy_attr *lu, void *p)
{
	struct vpd *vpd_pg = lu->lu_vpd[PCODE_OFFSET(0x83)];
	uint8_t *d;
	char *ptr;
	int num;
	int len, j;

	d = vpd_pg->data;

	d[0] = 2;
	d[1] = 1;
	d[2] = 0;
	num = VENDOR_ID_LEN + PRODUCT_ID_LEN + 10;
	d[3] = num;

	memcpy(&d[4], &lu->vendor_id, VENDOR_ID_LEN);
	memcpy(&d[12], &lu->product_id, PRODUCT_ID_LEN);
	memcpy(&d[28], &lu->lu_serial_no, 10);
	len = (int)strlen(lu->lu_serial_no);
	ptr = &lu->lu_serial_no[len];

	num += 4;
	/* NAA IEEE registered identifier (faked) */
	d[num] = 0x1;	/* Binary */
	d[num + 1] = 0x3;
	d[num + 2] = 0x0;
	d[num + 3] = 0x8;
	d[num + 4] = 0x51;
	d[num + 5] = 0x23;
	d[num + 6] = 0x45;
	d[num + 7] = 0x60;
	d[num + 8] = 0x3;
	d[num + 9] = 0x3;
	d[num + 10] = 0x3;
	d[num + 11] = 0x3;

	if (lu->naa) { /* If defined in config file */
		sscanf((const char *)lu->naa,
			"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			&d[num + 4],
			&d[num + 5],
			&d[num + 6],
			&d[num + 7],
			&d[num + 8],
			&d[num + 9],
			&d[num + 10],
			&d[num + 11]);
	} else { /* Else munge the serial number */
		ptr--;
		for (j = 11; j > 3; ptr--, j--)
			d[num + j] = *ptr;
	}
	/* Bug reported by Stefan Hauser.
	 * [num +4] is always 0x5x
	 */
	d[num + 4] &= 0x0f;
	d[num + 4] |= 0x50;
}

/*
 * A place to setup any customisations (WORM / Security handling)
 */
static void config_lu(struct lu_phy_attr *lu)
{
	int i;

	for (i = 0; tape_drives[i].name; i++) {
		if (!strncmp(tape_drives[i].name, lu->product_id,
				max(strlen(tape_drives[i].name),
					strlen(lu->product_id)))) {

			drive_init = tape_drives[i].init;
			break;
		}
	}

	lu_ssc.early_warning_sz = EARLY_WARNING_SZ;
	lu_ssc.prog_early_warning_sz = 0;

	drive_init(lu);

	if (lu_ssc.configCompressionEnabled)
		lu_ssc.pm->set_compression(&lu->mode_pg, lu_ssc.configCompressionFactor);
	else
		lu_ssc.pm->clear_compression(&lu->mode_pg);
}

int add_drive_media_list(struct lu_phy_attr *lu, int status, char *s)
{
	struct priv_lu_ssc *lu_tape;
	struct media_details *m_detail;
	struct list_head *den_list;
	int media_type;

	lu_tape = lu->lu_private;
	den_list = &lu_tape->supported_media_list;

	MHVTL_DBG(2, "Adding %s, status: 0x%02x", s, status);
	media_type = lookup_media_int(s);
	m_detail = check_media_can_load(den_list, media_type);

	if (m_detail) {
		MHVTL_DBG(2, "Existing status for %s, status: 0x%02x",
					s, m_detail->load_capability);
		m_detail->load_capability |= status;
		MHVTL_DBG(2, "Already have an entry for %s, new status: 0x%02x",
					s, m_detail->load_capability);
	} else {
		MHVTL_DBG(2, "Adding new entry for %s", s);
		m_detail = malloc(sizeof(struct media_details));
		if (!m_detail) {
			MHVTL_DBG(1, "Failed to allocate %d bytes",
						(int)sizeof(m_detail));
			return -ENOMEM;
		}
		m_detail->media_type = media_type;
		m_detail->load_capability = status;
		list_add_tail(&m_detail->siblings, den_list);
	}

	return 0;
}

static struct device_type_template ssc_ops = {
	.ops	= {
		/* 0x00 -> 0x0f */
		{ssc_tur,},
		{ssc_rewind,},
		{spc_illegal_op,},
		{spc_request_sense,},
		{ssc_format_media,},
		{ssc_read_block_limits,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{ssc_read_6,},
		{spc_illegal_op,},
		{ssc_write_6,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_release,},

		/* 0x10 -> 0x1f */
		{ssc_write_filemarks,},
		{ssc_space,},
		{spc_inquiry,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_mode_select,},
		{ssc_reserve,},
		{ssc_release,},

		{spc_illegal_op,},
		{ssc_erase,},
		{spc_mode_sense,},
		{ssc_load_unload,},
		{spc_recv_diagnostics,},
		{spc_send_diagnostics,},
		{ssc_allow_prevent_removal,},
		{spc_illegal_op,},

		/* 0x20 -> 0x2f */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_seek_10,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0x30 -> 0x3ff */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_read_position,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0x40 -> 0x4f */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_report_density_support,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_log_select,},
		{ssc_log_sense,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0x50 -> 0x5f */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_mode_select,},
		{ssc_reserve,},
		{ssc_release,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_mode_sense,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_pr_in,},
		{ssc_pr_out,},

		[0x60 ... 0x7f] = {spc_illegal_op,},

		/* 0x80 -> 0x8f */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_allow_overwrite,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_read_attributes,},
		{ssc_write_attributes,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		[0x90 ... 0x9f] = {spc_illegal_op,},

		/* 0xa0 -> 0xaf */
		{ssc_report_luns,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_a3_service_action,},
		{ssc_a4_service_action,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{ssc_read_media_sn,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		/* 0xb0 -> 0xbf */
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},
		{spc_illegal_op,},

		[0xc0 ... 0xff] = {spc_illegal_op,},
	}
};

/*
 * Update ops[xx] with new/updated/custom function 'f'
 */
void register_ops(struct lu_phy_attr *lu, int op, void *f)
{
	lu->scsi_ops->ops[op].cmd_perform = f;
}

#define MALLOC_SZ 512
static int init_lu(struct lu_phy_attr *lu, int minor, struct vtl_ctl *ctl)
{
	struct vpd **lu_vpd = lu->lu_vpd;
	int pg;

	char *config=MHVTL_CONFIG_PATH"/device.conf";
	FILE *conf;
	char *b;	/* Read from file into this buffer */
	char *s;	/* Somewhere for sscanf to store results */
	int indx;
	struct vtl_ctl tmpctl;
	int found = 0;

	INIT_LIST_HEAD(&lu->den_list);
	INIT_LIST_HEAD(&lu->log_pg);
	INIT_LIST_HEAD(&lu->mode_pg);

	lu->scsi_ops = &ssc_ops;

	lu->fifoname = NULL;
	lu->fifo_fd = NULL;
	lu->fifo_flag = 0;

	conf = fopen(config , "r");
	if (!conf) {
		MHVTL_ERR("Can not open config file %s : %s",
						config, strerror(errno));
		perror("Can not open config file");
		exit(1);
	}
	s = malloc(MALLOC_SZ);
	if (!s) {
		perror("Could not allocate memory");
		exit(1);
	}
	b = malloc(MALLOC_SZ);
	if (!b) {
		perror("Could not allocate memory");
		exit(1);
	}

	/* While read in a line */
	while (readline(b, MALLOC_SZ, conf) != NULL) {
		if (b[0] == '#')	/* Ignore comments */
			continue;
		if (strlen(b) == 1)	/* Reset drive number of blank line */
			indx = 0xff;
		if (sscanf(b, "Drive: %d CHANNEL: %d TARGET: %d LUN: %d",
					&indx, &tmpctl.channel,
					&tmpctl.id, &tmpctl.lun)) {
			MHVTL_DBG(2, "Looking for %d, Found drive %d",
							minor, indx);
			if (indx == minor) {
				char *v;

				found = 1;
				memcpy(ctl, &tmpctl, sizeof(tmpctl));

				/* Default rev with mhvtl release info */
				v = get_version();
				sprintf(lu->product_rev, "%-4s", v);
				free(v);
			}
		}
		if (indx == minor) {
			unsigned int c, d, e, f, g, h, j, k;
			int i;

			memset(s, 0x20, MALLOC_SZ);

			if (sscanf(b, " Unit serial number: %s", s)) {
				checkstrlen(s, SCSI_SN_LEN);
				sprintf(lu->lu_serial_no, "%-10s", s);
			}
			if (sscanf(b, " Product identification: %16c", s)) {
				/* sscanf does not NULL terminate */
				/* 25 is len of ' Product identification: ' */
				s[strlen(b) - 25] = '\0';
				checkstrlen(s, PRODUCT_ID_LEN);
				sprintf(lu->product_id, "%-16s", s);
			}
			if (sscanf(b, " Product revision level: %s", s)) {
				checkstrlen(s, PRODUCT_REV_LEN);
				sprintf(lu->product_rev, "%-4s", s);
			}
			if (sscanf(b, " Vendor identification: %s", s)) {
				checkstrlen(s, VENDOR_ID_LEN);
				sprintf(lu->vendor_id, "%-8s", s);
			}
			if (sscanf(b, " Compression: factor %d enabled %d",
							&i, &j)) {
				lu_ssc.configCompressionFactor = i;
				lu_ssc.configCompressionEnabled = j;
			} else if (sscanf(b, " Compression: %d", &i)) {
				if ((i > Z_NO_COMPRESSION)
						&& (i <= Z_BEST_COMPRESSION))
					lu_ssc.configCompressionFactor = i;
				else
					lu_ssc.configCompressionFactor = 0;
			}
			if (sscanf(b, " fifo: %s", s))
				process_fifoname(lu, s, 0);
			i = sscanf(b,
				" NAA: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
					&c, &d, &e, &f, &g, &h, &j, &k);
			if (i == 8) {
				if (lu->naa)
					free(lu->naa);
				lu->naa = malloc(48);
				if (lu->naa)
					sprintf((char *)lu->naa,
				"%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
					c, d, e, f, g, h, j, k);
				MHVTL_DBG(2, "Setting NAA: to %s", lu->naa);
			} else if (i > 0) {
				int y;

				free(lu->naa);
				lu->naa = NULL;
				for (y = 0; y < MALLOC_SZ; y++)
					if (b[y] == '\n')
						b[y] = 0;
				MHVTL_DBG(1, "NAA: Incorrect params %s"
						" : using defaults", b);
			}
		}
	}
	fclose(conf);
	free(b);
	free(s);

	lu->ptype = TYPE_TAPE;
	lu->removable = 1;	/* Supports removable media */

	lu->version_desc[0] = 0x0300;	/* SPC-3 No version claimed */
	lu->version_desc[1] = 0x0960;	/* iSCSI */
	lu->version_desc[2] = 0x0200;	/* SSC */

	/* Unit Serial Number */
	pg = 0x80 & 0x7f;
	lu_vpd[pg] = alloc_vpd(strlen(lu->lu_serial_no));
	lu_vpd[pg]->vpd_update = update_vpd_80;
	lu_vpd[pg]->vpd_update(lu, lu->lu_serial_no);

	/* Device Identification */
	pg = 0x83 & 0x7f;
	lu_vpd[pg] = alloc_vpd(VPD_83_SZ);
	lu_vpd[pg]->vpd_update = update_vpd_83;
	lu_vpd[pg]->vpd_update(lu, NULL);

	return found;
}

static void process_cmd(int cdev, uint8_t *buf, struct vtl_header *vtl_cmd)
{
	struct vtl_ds dbuf;
	uint8_t *cdb;

	/* Get the SCSI cdb from vtl driver
	 * - Returns SCSI command S/No. */

	cdb = (uint8_t *)&vtl_cmd->cdb;

	/* Interpret the SCSI command & process
	-> Returns no. of bytes to send back to kernel
	 */
	memset(&dbuf, 0, sizeof(struct vtl_ds));
	dbuf.serialNo = vtl_cmd->serialNo;
	dbuf.data = buf;
	dbuf.sam_stat = lu_ssc.sam_status;
	dbuf.sense_buf = &sense;

	processCommand(cdev, cdb, &dbuf);

	/* Complete SCSI cmd processing */
	completeSCSICommand(cdev, &dbuf);

	/* dbuf.sam_stat was zeroed in completeSCSICommand */
	lu_ssc.sam_status = dbuf.sam_stat;
}

static void init_lu_ssc(struct priv_lu_ssc *lu_priv)
{
	lu_priv->bufsize = 2 * 1024 * 1024;
	lu_priv->tapeLoaded = TAPE_UNLOADED;
	lu_priv->inLibrary = 0;
	lu_priv->sam_status = SAM_STAT_GOOD;
	lu_priv->MediaWriteProtect = MEDIA_WRITABLE;
	lu_priv->capacity_unit = 1;
	lu_priv->configCompressionFactor = Z_BEST_SPEED;
	lu_priv->bytesRead_I = 0;
	lu_priv->bytesRead_M = 0;
	lu_priv->bytesWritten_I = 0;
	lu_priv->bytesWritten_M = 0;
	lu_priv->c_pos = c_pos;
	lu_priv->KEY_INSTANCE_COUNTER = 0;
	lu_priv->DECRYPT_MODE = 0;
	lu_priv->ENCRYPT_MODE = 0;
	lu_priv->encr = &encryption;
	lu_priv->OK_2_write = &OK_to_write;
	lu_priv->mamp = &mam;
	INIT_LIST_HEAD(&lu_priv->supported_media_list);
	lu_priv->pm = NULL;
	lu_priv->state_msg = NULL;
}

void personality_module_register(struct ssc_personality_template *pm)
{
	MHVTL_DBG(2, "%s", pm->name);
	lu_ssc.pm = pm;
}

static void caught_signal(int signo)
{
	MHVTL_DBG(1, "(%d)", signo);
	printf("Please use 'vtlcmd <index> exit' to shutdown nicely\n");
	MHVTL_LOG("Please use 'vtlcmd <index> exit' to shutdown nicely,"
			" Received signal: %d", signo);
}

int main(int argc, char *argv[])
{
	int cdev;
	int ret;
	int last_state = MHVTL_STATE_UNKNOWN;
	long pollInterval = 50000L;
	uint8_t *buf;
	pid_t child_cleanup, pid, sid;
	struct sigaction new_action, old_action;
	int fifo_retval;

	char *progname = argv[0];
	char *fifoname = NULL;
	char *name = "mhvtl";
	int minor = 0;
	struct passwd *pw;

	struct vtl_header vtl_cmd;
	struct vtl_header *cmd;
	struct vtl_ctl ctl;

	/* Output file pointer (data file) */
	int ofp = -1;

	/* Message Q */
	int	mlen, r_qid;
	struct q_entry r_entry;

	current_state = MHVTL_STATE_INIT;

	if (argc < 2) {
		usage(argv[0]);
		printf("  -- Not enough parameters --\n");
		exit(1);
	}

	while (argc > 0) {
		if (argv[0][0] == '-') {
			switch (argv[0][1]) {
			case 'd':
				debug = 4;
				verbose = 9;	/* If debug, make verbose... */
				break;
			case 'v':
				verbose++;
				break;
			case 'q':
				if (argc > 1)
					my_id = atoi(argv[1]);
				break;
			case 'f':
				if (argc > 1)
					fifoname = argv[1];
				break;
			default:
				usage(progname);
				printf("    Unknown option %c\n", argv[0][1]);
				exit(1);
				break;
			}
		}
		argv++;
		argc--;
	}

	if (my_id <= 0 || my_id > MAXPRIOR) {
		usage(progname);
		if (my_id == 0)
			puts("    -q must be specified\n");
		else
			printf("    -q value out of range [1 - %d]\n",
				MAXPRIOR);
		exit(1);
	}
	minor = my_id;	/* Minor == Message Queue priority */

	openlog(progname, LOG_PID, LOG_DAEMON|LOG_WARNING);
	MHVTL_LOG("%s: version %s, verbose log: %d",
					progname, MHVTL_VERSION, verbose);

	/* Clear Sense arr */
	memset(sense, 0, sizeof(sense));

	/* Powered on / reset flag */
	reset_device();

	/* Initialise lu_ssc 'global vars' used by this daemon */
	init_lu_ssc(&lu_ssc);

	lunit.lu_private = &lu_ssc;

	/* Parse config file and build up each device */
	if (!init_lu(&lunit, minor, &ctl)) {
		printf("Can not find entry for '%d' in config file\n", minor);
		exit(1);
	}

	MHVTL_DBG(1, "starting...");

	/*
	 * Determine drive type
	 * register personality module
	 * Indirectly, mode page tables are initialised
	 */
	config_lu(&lunit);

	/* Check for user account before creating lu */
	pw = getpwnam(USR);	/* Find UID for user 'vtl' */
	if (!pw) {
		MHVTL_DBG(1, "Unable to find user: %s", USR);
		exit(1);
	}

	if (chrdev_create(minor)) {
		MHVTL_DBG(1, "Unable to create device node mhvtl%d", minor);
		exit(1);
	}

	child_cleanup = add_lu(my_id, &ctl);
	if (! child_cleanup) {
		MHVTL_DBG(1, "Could not create logical unit");
		exit(1);
	}

	chrdev_chown(minor, pw->pw_uid, pw->pw_gid);

	if (setgid(pw->pw_gid)) {
		perror("Unable to change gid");
		exit(1);
	}
	if (setuid(pw->pw_uid)) {
		perror("Unable to change uid");
		exit(1);
	}

	/* Initialise message queue as necessary */
	if ((r_qid = init_queue()) == -1) {
		printf("Could not initialise message queue\n");
		exit(1);
	}

	if (check_for_running_daemons(minor)) {
		MHVTL_LOG("%s: version %s, found another running daemon... exiting\n", progname, MHVTL_VERSION);
		exit(2);
	}

	MHVTL_DBG(2, "Running as %s, uid: %d", pw->pw_name, getuid());

	cdev = chrdev_open(name, minor);
	if (cdev == -1) {
		MHVTL_ERR("Could not open /dev/%s%d: %s", name, minor,
						strerror(errno));
		fflush(NULL);
		exit(1);
	}

	MHVTL_DBG(1, "Size of buffer is %d", lu_ssc.bufsize);
	buf = (uint8_t *)malloc(lu_ssc.bufsize);
	if (NULL == buf) {
		perror("Problems allocating memory");
		exit(1);
	}

	/* If debug, don't fork/run in background */
	if (!debug) {
		switch (pid = fork()) {
		case 0:         /* Child */
			break;
		case -1:
			perror("Failed to fork daemon");
			exit(-1);
			break;
		default:
			MHVTL_DBG(1, "vtltape process PID is %d", (int)pid);
			exit(0);
			break;
		}

		umask(0);	/* Change the file mode mask */

		sid = setsid();
		if (sid < 0)
			exit(-1);

		if ((chdir(MHVTL_HOME_PATH)) < 0) {
			perror("Unable to change directory to " MHVTL_HOME_PATH);
			exit(-1);
		}

		close(STDIN_FILENO);
		close(STDERR_FILENO);
	}

	oom_adjust();

	new_action.sa_handler = caught_signal;
	new_action.sa_flags = 0;
	sigemptyset(&new_action.sa_mask);
	sigaction(SIGALRM, &new_action, &old_action);
	sigaction(SIGHUP, &new_action, &old_action);
	sigaction(SIGINT, &new_action, &old_action);
	sigaction(SIGPIPE, &new_action, &old_action);
	sigaction(SIGTERM, &new_action, &old_action);
	sigaction(SIGUSR1, &new_action, &old_action);
	sigaction(SIGUSR2, &new_action, &old_action);

	/* If fifoname passed as switch */
	if (fifoname)
		process_fifoname(&lunit, fifoname, 1);
	/* fifoname can be defined in device.conf */
	if (lunit.fifoname)
		open_fifo(&lunit.fifo_fd, lunit.fifoname);

	fifo_retval = inc_fifo_count(lunit.fifoname);
	if (fifo_retval == -ENOMEM) {
		MHVTL_ERR("shared memory setup failed - exiting...");
		goto exit;
	} else if (fifo_retval < 0) {
		MHVTL_ERR("Failed to set fifo count()...");
	}

	for (;;) {
		/* Check for anything in the messages Q */
		mlen = msgrcv(r_qid, &r_entry, MAXOBN, my_id, IPC_NOWAIT);
		if (mlen > 0) {
			if (processMessageQ(&r_entry.msg, &lu_ssc.sam_status))
				goto exit;
		} else if (mlen < 0) {
			if ((r_qid = init_queue()) == -1) {
				MHVTL_ERR("Can not open message queue: %s",
							strerror(errno));
			}
		}
		ret = ioctl(cdev, VTL_POLL_AND_GET_HEADER, &vtl_cmd);
		if (ret < 0) {
			MHVTL_DBG(2,
				"ioctl(VTL_POLL_AND_GET_HEADER: %d : %s",
							ret, strerror(errno));
		} else {
			if (debug)
				printf("ioctl(VX_TAPE_POLL_STATUS) "
					"returned: %d, interval: %ld\n",
						ret, pollInterval);
			if (child_cleanup) {
				if (waitpid(child_cleanup, NULL, WNOHANG)) {
					MHVTL_DBG(1,
						"Cleaning up after child %d",
							child_cleanup);
					child_cleanup = 0;
				}
			}
			fflush(NULL);
			switch (ret) {
			case VTL_QUEUE_CMD:	/* A cdb to process */
				cmd = malloc(sizeof(struct vtl_header));
				if (!cmd) {
					MHVTL_ERR("Out of memory");
					pollInterval = 1000000;
				} else {
					memcpy(cmd, &vtl_cmd, sizeof(vtl_cmd));
					process_cmd(cdev, buf, cmd);
					/* Something to do, reduce poll time */
					pollInterval = 10;
					free(cmd);
				}
				break;

			case VTL_IDLE:
				/* While nothing to do, increase
				 * time we sleep before polling again.
				 */
				if (pollInterval < 1000000)
					pollInterval += 1000;

				usleep(pollInterval);

				break;

			default:
				MHVTL_LOG("ioctl(0x%x) returned %d\n",
						VTL_POLL_AND_GET_HEADER, ret);
				sleep(1);
				break;
			}
			if (current_state != last_state) {
				status_change(lunit.fifo_fd,
							current_state,
							my_id,
							&lu_ssc.state_msg);
				last_state = current_state;
			}
			if (pollInterval > 0xf000) {
				if (lu_ssc.tapeLoaded == TAPE_LOADED)
					current_state = MHVTL_STATE_LOADED_IDLE;
				else
					current_state = MHVTL_STATE_IDLE;
			}

		}
	}

exit:
	ioctl(cdev, VTL_REMOVE_LU, &ctl);
	close(cdev);
	close(ofp);
	free(buf);
	if (!dec_fifo_count(lunit.fifoname))
		unlink(lunit.fifoname);
	if (lunit.fifo_fd) {
		fclose(lunit.fifo_fd);
		free(lunit.fifoname);
	}
	exit(0);
}

