// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 - 2023, The OctopOS Authors, All rights reserved.
 */

// Some macros and functions are adapted from https://github.com/Xilinx/embeddedsw
/******************************************************************************
* Copyright (c) 2014 - 2021 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 */

#include <config.h>
#include <errno.h>
#include <common.h>
#include <env.h>
#include <mapmem.h>
#include <part.h>
#include <ext4fs.h>
#include <fat.h>
#include <fs.h>
#include <sandboxfs.h>
#include <ubifs_uboot.h>
#include <btrfs.h>
#include <asm/io.h>
#include <div64.h>
#include <linux/math64.h>
#include <efi_loader.h>
#include <linux/delay.h>

DECLARE_GLOBAL_DATA_PTR;

static struct blk_desc *fs_dev_desc;
static int fs_dev_part;
static disk_partition_t fs_partition;
static int fs_type = FS_TYPE_ANY;

static inline int fs_probe_unsupported(struct blk_desc *fs_dev_desc,
				      disk_partition_t *fs_partition)
{
	printf("** Unrecognized filesystem type **\n");
	return -1;
}

static inline int fs_ls_unsupported(const char *dirname)
{
	return -1;
}

/* generic implementation of ls in terms of opendir/readdir/closedir */
__maybe_unused
static int fs_ls_generic(const char *dirname)
{
	struct fs_dir_stream *dirs;
	struct fs_dirent *dent;
	int nfiles = 0, ndirs = 0;

	dirs = fs_opendir(dirname);
	if (!dirs)
		return -errno;

	while ((dent = fs_readdir(dirs))) {
		if (dent->type == FS_DT_DIR) {
			printf("            %s/\n", dent->name);
			ndirs++;
		} else {
			printf(" %8lld   %s\n", dent->size, dent->name);
			nfiles++;
		}
	}

	fs_closedir(dirs);

	printf("\n%d file(s), %d dir(s)\n\n", nfiles, ndirs);

	return 0;
}

static inline int fs_exists_unsupported(const char *filename)
{
	return 0;
}

static inline int fs_size_unsupported(const char *filename, loff_t *size)
{
	return -1;
}

static inline int fs_read_unsupported(const char *filename, void *buf,
				      loff_t offset, loff_t len,
				      loff_t *actread)
{
	return -1;
}

static inline int fs_write_unsupported(const char *filename, void *buf,
				      loff_t offset, loff_t len,
				      loff_t *actwrite)
{
	return -1;
}

static inline int fs_ln_unsupported(const char *filename, const char *target)
{
	return -1;
}

static inline void fs_close_unsupported(void)
{
}

static inline int fs_uuid_unsupported(char *uuid_str)
{
	return -1;
}

static inline int fs_opendir_unsupported(const char *filename,
					 struct fs_dir_stream **dirs)
{
	return -EACCES;
}

static inline int fs_unlink_unsupported(const char *filename)
{
	return -1;
}

static inline int fs_mkdir_unsupported(const char *dirname)
{
	return -1;
}

struct fstype_info {
	int fstype;
	char *name;
	/*
	 * Is it legal to pass NULL as .probe()'s  fs_dev_desc parameter? This
	 * should be false in most cases. For "virtual" filesystems which
	 * aren't based on a U-Boot block device (e.g. sandbox), this can be
	 * set to true. This should also be true for the dummy entry at the end
	 * of fstypes[], since that is essentially a "virtual" (non-existent)
	 * filesystem.
	 */
	bool null_dev_desc_ok;
	int (*probe)(struct blk_desc *fs_dev_desc,
		     disk_partition_t *fs_partition);
	int (*ls)(const char *dirname);
	int (*exists)(const char *filename);
	int (*size)(const char *filename, loff_t *size);
	int (*read)(const char *filename, void *buf, loff_t offset,
		    loff_t len, loff_t *actread);
	int (*write)(const char *filename, void *buf, loff_t offset,
		     loff_t len, loff_t *actwrite);
	void (*close)(void);
	int (*uuid)(char *uuid_str);
	/*
	 * Open a directory stream.  On success return 0 and directory
	 * stream pointer via 'dirsp'.  On error, return -errno.  See
	 * fs_opendir().
	 */
	int (*opendir)(const char *filename, struct fs_dir_stream **dirsp);
	/*
	 * Read next entry from directory stream.  On success return 0
	 * and directory entry pointer via 'dentp'.  On error return
	 * -errno.  See fs_readdir().
	 */
	int (*readdir)(struct fs_dir_stream *dirs, struct fs_dirent **dentp);
	/* see fs_closedir() */
	void (*closedir)(struct fs_dir_stream *dirs);
	int (*unlink)(const char *filename);
	int (*mkdir)(const char *dirname);
	int (*ln)(const char *filename, const char *target);
};

static struct fstype_info fstypes[] = {
#ifdef CONFIG_FS_FAT
	{
		.fstype = FS_TYPE_FAT,
		.name = "fat",
		.null_dev_desc_ok = false,
		.probe = fat_set_blk_dev,
		.close = fat_close,
		.ls = fs_ls_generic,
		.exists = fat_exists,
		.size = fat_size,
		.read = fat_read_file,
#if CONFIG_IS_ENABLED(FAT_WRITE)
		.write = file_fat_write,
		.unlink = fat_unlink,
		.mkdir = fat_mkdir,
#else
		.write = fs_write_unsupported,
		.unlink = fs_unlink_unsupported,
		.mkdir = fs_mkdir_unsupported,
#endif
		.uuid = fs_uuid_unsupported,
		.opendir = fat_opendir,
		.readdir = fat_readdir,
		.closedir = fat_closedir,
		.ln = fs_ln_unsupported,
	},
#endif

#if CONFIG_IS_ENABLED(FS_EXT4)
	{
		.fstype = FS_TYPE_EXT,
		.name = "ext4",
		.null_dev_desc_ok = false,
		.probe = ext4fs_probe,
		.close = ext4fs_close,
		.ls = ext4fs_ls,
		.exists = ext4fs_exists,
		.size = ext4fs_size,
		.read = ext4_read_file,
#ifdef CONFIG_CMD_EXT4_WRITE
		.write = ext4_write_file,
		.ln = ext4fs_create_link,
#else
		.write = fs_write_unsupported,
		.ln = fs_ln_unsupported,
#endif
		.uuid = ext4fs_uuid,
		.opendir = fs_opendir_unsupported,
		.unlink = fs_unlink_unsupported,
		.mkdir = fs_mkdir_unsupported,
	},
#endif
#ifdef CONFIG_SANDBOX
	{
		.fstype = FS_TYPE_SANDBOX,
		.name = "sandbox",
		.null_dev_desc_ok = true,
		.probe = sandbox_fs_set_blk_dev,
		.close = sandbox_fs_close,
		.ls = sandbox_fs_ls,
		.exists = sandbox_fs_exists,
		.size = sandbox_fs_size,
		.read = fs_read_sandbox,
		.write = fs_write_sandbox,
		.uuid = fs_uuid_unsupported,
		.opendir = fs_opendir_unsupported,
		.unlink = fs_unlink_unsupported,
		.mkdir = fs_mkdir_unsupported,
		.ln = fs_ln_unsupported,
	},
#endif
#ifdef CONFIG_CMD_UBIFS
	{
		.fstype = FS_TYPE_UBIFS,
		.name = "ubifs",
		.null_dev_desc_ok = true,
		.probe = ubifs_set_blk_dev,
		.close = ubifs_close,
		.ls = ubifs_ls,
		.exists = ubifs_exists,
		.size = ubifs_size,
		.read = ubifs_read,
		.write = fs_write_unsupported,
		.uuid = fs_uuid_unsupported,
		.opendir = fs_opendir_unsupported,
		.unlink = fs_unlink_unsupported,
		.mkdir = fs_mkdir_unsupported,
		.ln = fs_ln_unsupported,
	},
#endif
#ifdef CONFIG_FS_BTRFS
	{
		.fstype = FS_TYPE_BTRFS,
		.name = "btrfs",
		.null_dev_desc_ok = false,
		.probe = btrfs_probe,
		.close = btrfs_close,
		.ls = btrfs_ls,
		.exists = btrfs_exists,
		.size = btrfs_size,
		.read = btrfs_read,
		.write = fs_write_unsupported,
		.uuid = btrfs_uuid,
		.opendir = fs_opendir_unsupported,
		.unlink = fs_unlink_unsupported,
		.mkdir = fs_mkdir_unsupported,
		.ln = fs_ln_unsupported,
	},
#endif
	{
		.fstype = FS_TYPE_ANY,
		.name = "unsupported",
		.null_dev_desc_ok = true,
		.probe = fs_probe_unsupported,
		.close = fs_close_unsupported,
		.ls = fs_ls_unsupported,
		.exists = fs_exists_unsupported,
		.size = fs_size_unsupported,
		.read = fs_read_unsupported,
		.write = fs_write_unsupported,
		.uuid = fs_uuid_unsupported,
		.opendir = fs_opendir_unsupported,
		.unlink = fs_unlink_unsupported,
		.mkdir = fs_mkdir_unsupported,
		.ln = fs_ln_unsupported,
	},
};

static struct fstype_info *fs_get_info(int fstype)
{
	struct fstype_info *info;
	int i;

	for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes) - 1; i++, info++) {
		if (fstype == info->fstype)
			return info;
	}

	/* Return the 'unsupported' sentinel */
	return info;
}

/**
 * fs_get_type() - Get type of current filesystem
 *
 * Return: filesystem type
 *
 * Returns filesystem type representing the current filesystem, or
 * FS_TYPE_ANY for any unrecognised filesystem.
 */
int fs_get_type(void)
{
	return fs_type;
}

/**
 * fs_get_type_name() - Get type of current filesystem
 *
 * Return: Pointer to filesystem name
 *
 * Returns a string describing the current filesystem, or the sentinel
 * "unsupported" for any unrecognised filesystem.
 */
const char *fs_get_type_name(void)
{
	return fs_get_info(fs_type)->name;
}

int fs_set_blk_dev(const char *ifname, const char *dev_part_str, int fstype)
{
	struct fstype_info *info;
	int part, i;
#ifdef CONFIG_NEEDS_MANUAL_RELOC
	static int relocated;

	if (!relocated) {
		for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes);
				i++, info++) {
			info->name += gd->reloc_off;
			info->probe += gd->reloc_off;
			info->close += gd->reloc_off;
			info->ls += gd->reloc_off;
			info->read += gd->reloc_off;
			info->write += gd->reloc_off;
		}
		relocated = 1;
	}
#endif

	part = blk_get_device_part_str(ifname, dev_part_str, &fs_dev_desc,
					&fs_partition, 1);
	if (part < 0)
		return -1;

	for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes); i++, info++) {
		if (fstype != FS_TYPE_ANY && info->fstype != FS_TYPE_ANY &&
				fstype != info->fstype)
			continue;

		if (!fs_dev_desc && !info->null_dev_desc_ok)
			continue;

		if (!info->probe(fs_dev_desc, &fs_partition)) {
			fs_type = info->fstype;
			fs_dev_part = part;
			return 0;
		}
	}

	return -1;
}

/* set current blk device w/ blk_desc + partition # */
int fs_set_blk_dev_with_part(struct blk_desc *desc, int part)
{
	struct fstype_info *info;
	int ret, i;

	if (part >= 1)
		ret = part_get_info(desc, part, &fs_partition);
	else
		ret = part_get_info_whole_disk(desc, &fs_partition);
	if (ret)
		return ret;
	fs_dev_desc = desc;

	for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes); i++, info++) {
		if (!info->probe(fs_dev_desc, &fs_partition)) {
			fs_type = info->fstype;
			fs_dev_part = part;
			return 0;
		}
	}

	return -1;
}

void fs_close(void)
{
	struct fstype_info *info = fs_get_info(fs_type);

	info->close();

	fs_type = FS_TYPE_ANY;
}

int fs_uuid(char *uuid_str)
{
	struct fstype_info *info = fs_get_info(fs_type);

	return info->uuid(uuid_str);
}

int fs_ls(const char *dirname)
{
	int ret;

	struct fstype_info *info = fs_get_info(fs_type);

	ret = info->ls(dirname);

	fs_close();

	return ret;
}

int fs_exists(const char *filename)
{
	int ret;

	struct fstype_info *info = fs_get_info(fs_type);

	ret = info->exists(filename);

	fs_close();

	return ret;
}

int fs_size(const char *filename, loff_t *size)
{
	int ret;

	struct fstype_info *info = fs_get_info(fs_type);

	ret = info->size(filename, size);

	fs_close();

	return ret;
}

#ifdef CONFIG_LMB
/* Check if a file may be read to the given address */
static int fs_read_lmb_check(const char *filename, ulong addr, loff_t offset,
			     loff_t len, struct fstype_info *info)
{
	struct lmb lmb;
	int ret;
	loff_t size;
	loff_t read_len;

	/* get the actual size of the file */
	ret = info->size(filename, &size);
	if (ret)
		return ret;
	if (offset >= size) {
		/* offset >= EOF, no bytes will be written */
		return 0;
	}
	read_len = size - offset;

	/* limit to 'len' if it is smaller */
	if (len && len < read_len)
		read_len = len;

	lmb_init_and_reserve(&lmb, gd->bd, (void *)gd->fdt_blob);
	lmb_dump_all(&lmb);

	if (lmb_alloc_addr(&lmb, addr, read_len) == addr)
		return 0;

	printf("** Reading file would overwrite reserved memory **\n");
	return -ENOSPC;
}
#endif

static int _fs_read(const char *filename, ulong addr, loff_t offset, loff_t len,
		    int do_lmb_check, loff_t *actread)
{
	struct fstype_info *info = fs_get_info(fs_type);
	void *buf;
	int ret;

#ifdef CONFIG_LMB
	if (do_lmb_check) {
		ret = fs_read_lmb_check(filename, addr, offset, len, info);
		if (ret)
			return ret;
	}
#endif

	/*
	 * We don't actually know how many bytes are being read, since len==0
	 * means read the whole file.
	 */
	buf = map_sysmem(addr, len);
	ret = info->read(filename, buf, offset, len, actread);
	unmap_sysmem(buf);

	/* If we requested a specific number of bytes, check we got it */
	if (ret == 0 && len && *actread != len)
		debug("** %s shorter than offset + len **\n", filename);
	fs_close();

	return ret;
}

int fs_read(const char *filename, ulong addr, loff_t offset, loff_t len,
	    loff_t *actread)
{
	return _fs_read(filename, addr, offset, len, 0, actread);
}

int fs_write(const char *filename, ulong addr, loff_t offset, loff_t len,
	     loff_t *actwrite)
{
	struct fstype_info *info = fs_get_info(fs_type);
	void *buf;
	int ret;

	buf = map_sysmem(addr, len);
	ret = info->write(filename, buf, offset, len, actwrite);
	unmap_sysmem(buf);

	if (ret < 0 && len != *actwrite) {
		printf("** Unable to write file %s **\n", filename);
		ret = -1;
	}
	fs_close();

	return ret;
}

struct fs_dir_stream *fs_opendir(const char *filename)
{
	struct fstype_info *info = fs_get_info(fs_type);
	struct fs_dir_stream *dirs = NULL;
	int ret;

	ret = info->opendir(filename, &dirs);
	fs_close();
	if (ret) {
		errno = -ret;
		return NULL;
	}

	dirs->desc = fs_dev_desc;
	dirs->part = fs_dev_part;

	return dirs;
}

struct fs_dirent *fs_readdir(struct fs_dir_stream *dirs)
{
	struct fstype_info *info;
	struct fs_dirent *dirent;
	int ret;

	fs_set_blk_dev_with_part(dirs->desc, dirs->part);
	info = fs_get_info(fs_type);

	ret = info->readdir(dirs, &dirent);
	fs_close();
	if (ret) {
		errno = -ret;
		return NULL;
	}

	return dirent;
}

void fs_closedir(struct fs_dir_stream *dirs)
{
	struct fstype_info *info;

	if (!dirs)
		return;

	fs_set_blk_dev_with_part(dirs->desc, dirs->part);
	info = fs_get_info(fs_type);

	info->closedir(dirs);
	fs_close();
}

int fs_unlink(const char *filename)
{
	int ret;

	struct fstype_info *info = fs_get_info(fs_type);

	ret = info->unlink(filename);

	fs_close();

	return ret;
}

int fs_mkdir(const char *dirname)
{
	int ret;

	struct fstype_info *info = fs_get_info(fs_type);

	ret = info->mkdir(dirname);

	fs_close();

	return ret;
}

int fs_ln(const char *fname, const char *target)
{
	struct fstype_info *info = fs_get_info(fs_type);
	int ret;

	ret = info->ln(fname, target);

	if (ret < 0) {
		printf("** Unable to create link %s -> %s **\n", fname, target);
		ret = -1;
	}
	fs_close();

	return ret;
}

int do_size(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
	loff_t size;

	if (argc != 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	if (fs_size(argv[3], &size) < 0)
		return CMD_RET_FAILURE;

	env_set_hex("filesize", size);

	return 0;
}

#define OCTOPOS_MAILBOX_INTR_OFFSET 4
#define STORAGE_BLOCK_SIZE 512
#define MAILBOX_QUEUE_MSG_SIZE_LARGE 512
#define P_PREVIOUS 0xff

// from octopos_mbox.h
#define OWNER_MASK (u32) 0x00FFFFFF
#define QUOTA_MASK (u32) 0xFF000FFF
#define TIME_MASK  (u32) 0xFFFFF000

// adapted from
// https://github.com/Xilinx/embeddedsw/blob/master/XilinxProcessorIPLib/drivers/mbox/src/xmbox_hw.h
#define XMB_WRITE_REG_OFFSET	0x00	/**< Mbox write register */
#define XMB_READ_REG_OFFSET	0x08	/**< Mbox read register */
#define XMB_STATUS_REG_OFFSET	0x10	/**< Mbox status reg  */
#define XMB_ERROR_REG_OFFSET	0x14	/**< Mbox Error reg  */
#define XMB_SIT_REG_OFFSET	0x18	/**< Mbox send interrupt threshold register */
#define XMB_RIT_REG_OFFSET	0x1C	/**< Mbox receive interrupt threshold register */
#define XMB_IS_REG_OFFSET	0x20	/**< Mbox interrupt status register */
#define XMB_IE_REG_OFFSET	0x24	/**< Mbox interrupt enable register */
#define XMB_IP_REG_OFFSET	0x28	/**< Mbox interrupt pending register */
#define XMB_CTRL_REG_OFFSET	0x2C	/**< Mbox control register */
#define XMB_STATUS_FIFO_EMPTY	0x00000001 /**< Receive FIFO is Empty */
#define XMB_STATUS_FIFO_FULL	0x00000002 /**< Send FIFO is Full */
#define XMB_STATUS_STA		0x00000004 /**< Send FIFO Threshold Status */
#define XMB_STATUS_RTA		0x00000008 /**< Receive FIFO Threshold Status */

// from octopos/mailbox.h and octopos/runtime.h
#define MAILBOX_NO_LIMIT_VAL			0xFFF
#define MAILBOX_NO_TIMEOUT_VAL			0xFFF
#define MAILBOX_MAX_LIMIT_VAL			0xFFE
#define MAILBOX_MAX_TIMEOUT_VAL			0xFFE
#define MAILBOX_MIN_PRACTICAL_TIMEOUT_VAL	20
#define MAILBOX_DEFAULT_TIMEOUT_VAL		60

// adapted from 
// https://github.com/Xilinx/embeddedsw/blob/master/lib/bsp/standalone/src/common/xil_io.h
u32 Xil_In32(u32 Addr)
{
	return *(volatile u32 *) Addr;
}

// adapted from 
// https://github.com/Xilinx/embeddedsw/blob/master/lib/bsp/standalone/src/common/xil_io.h
void Xil_Out32(u32 Addr, u32 Value)
{
	volatile u32 *LocalAddr = (volatile u32 *)Addr;
	*LocalAddr = Value;
}

// adapted from
// https://github.com/Xilinx/embeddedsw/blob/master/XilinxProcessorIPLib/drivers/mbox/src/xmbox_hw.h
#define XMbox_ReadMBox(BaseAddress)				\
	Xil_In32(BaseAddress + XMB_READ_REG_OFFSET)

#define XMbox_IsEmptyHw(BaseAddress)				 \
((Xil_In32(BaseAddress + XMB_STATUS_REG_OFFSET) & XMB_STATUS_FIFO_EMPTY))

// adapted from 
// https://github.com/Xilinx/embeddedsw/blob/master/XilinxProcessorIPLib/drivers/mbox/src/xmbox.c
void XMbox_ReadBlocking(u32 Addr, u32 *BufferPtr,
			u32 RequestedBytes)
{
	u32 NumBytes = 0;

	do {
		while(XMbox_IsEmptyHw(Addr));

		*BufferPtr++ = XMbox_ReadMBox(Addr);
		NumBytes += 4;
	} while (NumBytes != RequestedBytes);

}

u16 octopos_mailbox_get_quota_limit(u32 base)
{
	return (u16) (Xil_In32(base) >> 12 & 0xfff);
}

void octopos_mailbox_deduct_and_set_owner(u32 base, u8 owner)
{
	Xil_Out32(base, 0xFF000000);
}

int do_load_octopos(ulong addr, loff_t offset, loff_t len, loff_t *actread)
{
	/* FIXME: hard coded address */
	u32 q_storage_data_out = 0xa0007000;
	u32 q_storage_control = 0xa0080000;

	int need_repeat = 0;
	int total = 0;
	void* buf;

	buf = map_sysmem(addr, len);

repeat:
	/* wait for os to delegate data queue access */
	while (0xdeadbeef == Xil_In32(q_storage_control));

	/* clear octopos control interrupt */
	Xil_Out32(q_storage_control + OCTOPOS_MAILBOX_INTR_OFFSET, 1);

#ifdef FINITE_DELEGATION
	/* repeatedly read from mailbox until OS stops delegating the queue */
	u32 count = octopos_mailbox_get_quota_limit(q_storage_control);
	count = count / 128;
#endif

#ifdef FINITE_DELEGATION

	if (count == MAILBOX_MAX_LIMIT_VAL / 128)
		need_repeat = 1;
	else
		need_repeat = 0;
#endif

#ifdef FINITE_DELEGATION
	for (int i = 0; i < (int) count; i++) {
#else
	int block_size = 12623;
	for (int i = 0; i < block_size; i++) {	
#endif
		XMbox_ReadBlocking(
			q_storage_data_out,
			(u8*) (buf + (total + i) * MAILBOX_QUEUE_MSG_SIZE_LARGE),
			MAILBOX_QUEUE_MSG_SIZE_LARGE);
	}

#ifdef FINITE_DELEGATION
	total += count;
#else
	total = block_size;
#endif

	octopos_mailbox_deduct_and_set_owner(q_storage_control, P_PREVIOUS);

#ifdef FINITE_DELEGATION
	if (need_repeat)
		goto repeat;
#endif

	*actread = total * MAILBOX_QUEUE_MSG_SIZE_LARGE;


	unmap_sysmem(buf);

	return CMD_RET_SUCCESS;
}

/* FIXME: this comes from octopos/storage.h and storage/storage.c */
#define NUM_PARTITIONS		6
#define STORAGE_KEY_SIZE	32
#define STORAGE_METADATA_SIZE	64
#define STORAGE_BLOCK_SIZE	512  /* bytes */
#define STORAGE_BOOT_PARTITION_SIZE			100000
#define STORAGE_UNTRUSTED_ROOT_FS_PARTITION_SIZE	300000
#define RAM_ROOT_PARTITION_METADATA_BASE 0x25000000
#define RAM_ROOT_PARTITION_BASE 0x30000000
#define RAM_UNTRUSTED_PARTITION_BASE (RAM_ROOT_PARTITION_BASE + STORAGE_BOOT_PARTITION_SIZE * STORAGE_BLOCK_SIZE)
#define RAM_ENCLAVE_PARTITION_1_BASE (RAM_UNTRUSTED_PARTITION_BASE + STORAGE_UNTRUSTED_ROOT_FS_PARTITION_SIZE * STORAGE_BLOCK_SIZE)
#define RAM_ENCLAVE_PARTITION_2_BASE (RAM_ENCLAVE_PARTITION_1_BASE + 100 * STORAGE_BLOCK_SIZE)
#define RAM_ENCLAVE_PARTITION_3_BASE (RAM_ENCLAVE_PARTITION_2_BASE + 100 * STORAGE_BLOCK_SIZE)
#define RAM_ENCLAVE_PARTITION_4_BASE (RAM_ENCLAVE_PARTITION_3_BASE + 100 * STORAGE_BLOCK_SIZE)

uint32_t partition_sizes[NUM_PARTITIONS] = {STORAGE_BOOT_PARTITION_SIZE,
	STORAGE_UNTRUSTED_ROOT_FS_PARTITION_SIZE, 100, 100, 100, 100};

uint32_t partition_base[NUM_PARTITIONS] = {
	RAM_ROOT_PARTITION_BASE,
	RAM_UNTRUSTED_PARTITION_BASE,
	RAM_ENCLAVE_PARTITION_1_BASE,
	RAM_ENCLAVE_PARTITION_2_BASE,
	RAM_ENCLAVE_PARTITION_3_BASE,
	RAM_ENCLAVE_PARTITION_4_BASE
};

int do_load(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
	unsigned long addr;
	const char *addr_str;
	const char *filename;
	loff_t bytes;
	loff_t pos;
	loff_t len_read;
	loff_t len_read_bootimg;
	int ret, ret_bootimg;
	unsigned long time;
	char *ep;
	uint32_t size, base, metabase;
	uint32_t creation_tag;
	char data_name[256];
	char create_name[256];
	char keys_name[256];

	if (argc < 2)
		return CMD_RET_USAGE;
	if (argc > 7)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], (argc >= 3) ? argv[2] : NULL, fstype))
		return 1;

	if (argc >= 4) {
		addr = simple_strtoul(argv[3], &ep, 16);
		if (ep == argv[3] || *ep != '\0')
			return CMD_RET_USAGE;
	} else {
		addr_str = env_get("loadaddr");
		if (addr_str != NULL)
			addr = simple_strtoul(addr_str, NULL, 16);
		else
			addr = CONFIG_SYS_LOAD_ADDR;
	}
	if (argc >= 5) {
		filename = argv[4];
	} else {
		filename = env_get("bootfile");
		if (!filename) {
			puts("** No boot file defined **\n");
			return 1;
		}
	}
	if (argc >= 6)
		bytes = simple_strtoul(argv[5], NULL, 16);
	else
		bytes = 0;
	if (argc >= 7)
		pos = simple_strtoul(argv[6], NULL, 16);
	else
		pos = 0;

#ifdef CONFIG_CMD_BOOTEFI
	efi_set_bootdev(argv[1], (argc > 2) ? argv[2] : "",
			(argc > 4) ? argv[4] : "");
#endif
	time = get_timer(0);

	if (strcmp(filename, "/boot.scr") == 0) {
		ret = fs_read(filename, addr, pos, bytes, &len_read);
		/* at time of loading boot.scr, load all sec_hw images */
		for (uint32_t i = 0; i < NUM_PARTITIONS; i++) {
			size = partition_sizes[i];
			base = partition_base[i];
			metabase = RAM_ROOT_PARTITION_METADATA_BASE + i * STORAGE_METADATA_SIZE;

			memset(data_name, 0x0, 256);
			sprintf(data_name, "/octopos_partition_%d_data", i);

			memset(create_name, 0x0, 256);
			sprintf(create_name, "/octopos_partition_%d_create", i);

			memset(keys_name, 0x0, 256);
			sprintf(keys_name, "/octopos_partition_%d_keys", i);

			if (fs_set_blk_dev(argv[1], (argc >= 3) ? argv[2] : NULL, fstype)) {
				printf("FATAL: fs_set_blk_dev failure\r\n");
				return 1;
			}

			ret_bootimg = fs_read(data_name, base, 0, 0, &len_read_bootimg);
			printf("%s: %d data(%d %d)\r\n", __FUNCTION__, i, ret_bootimg, len_read_bootimg);
			if (ret_bootimg != 0 || len_read_bootimg == 0) {
				/* no file by data_name */
				memset(base, 0, size);
				printf("%s: partition %d data does not exist\r\n", __FUNCTION__, i);
			}
			
			if (fs_set_blk_dev(argv[1], (argc >= 3) ? argv[2] : NULL, fstype)) {
				printf("FATAL: fs_set_blk_dev failure\r\n");
				return 1;
			}

			printf("[1] %08x\r\n", metabase);
			memset(metabase, 0, STORAGE_METADATA_SIZE);
			printf("[2] %08x\r\n", metabase);
			ret_bootimg = fs_read(create_name, metabase, 0, 4, &len_read_bootimg);
			printf("%s: %d create(%08x %d %d)\r\n", __FUNCTION__, i, 
					metabase, ret_bootimg, len_read_bootimg);
			if (ret_bootimg != 0 || len_read_bootimg == 0) {
				/* no file by create_name */
				memset(metabase, 0, STORAGE_METADATA_SIZE);
				printf("%s: partition %d create does not exist\r\n", __FUNCTION__, i);
			} else {
				if (*((uint32_t*) metabase) != 1) {
					memset(metabase, 0, STORAGE_METADATA_SIZE);
					printf("%s: bad create tag(%u)\r\n", 
							__FUNCTION__, 
							*((uint32_t*) metabase));
				}	
			}

		}
		flush_cache(0x30000000, 0xfffffff);
		flush_cache(RAM_ROOT_PARTITION_METADATA_BASE, 0xffffff);
	} else {
		ret = do_load_octopos(addr, pos, bytes, &len_read);
	}

	time = get_timer(time);
	if (ret < 0)
		return 1;

	printf("%llu bytes read in %lu ms", len_read, time);
	if (time > 0) {
		puts(" (");
		print_size(div_u64(len_read, time) * 1000, "/s");
		puts(")");
	}
	puts("\n");

	env_set_hex("fileaddr", addr);
	env_set_hex("filesize", len_read);

	return 0;
}

int do_ls(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
	int fstype)
{
	if (argc < 2)
		return CMD_RET_USAGE;
	if (argc > 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], (argc >= 3) ? argv[2] : NULL, fstype))
		return 1;

	if (fs_ls(argc >= 4 ? argv[3] : "/"))
		return 1;

	return 0;
}

int file_exists(const char *dev_type, const char *dev_part, const char *file,
		int fstype)
{
	if (fs_set_blk_dev(dev_type, dev_part, fstype))
		return 0;

	return fs_exists(file);
}

int do_save(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
	unsigned long addr;
	const char *filename;
	loff_t bytes;
	loff_t pos;
	loff_t len;
	int ret;
	unsigned long time;

	if (argc < 6 || argc > 7)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	addr = simple_strtoul(argv[3], NULL, 16);
	filename = argv[4];
	bytes = simple_strtoul(argv[5], NULL, 16);
	if (argc >= 7)
		pos = simple_strtoul(argv[6], NULL, 16);
	else
		pos = 0;

	time = get_timer(0);
	ret = fs_write(filename, addr, pos, bytes, &len);
	time = get_timer(time);
	if (ret < 0)
		return 1;

	printf("%llu bytes written in %lu ms", len, time);
	if (time > 0) {
		puts(" (");
		print_size(div_u64(len, time) * 1000, "/s");
		puts(")");
	}
	puts("\n");

	return 0;
}

int do_fs_uuid(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
	int ret;
	char uuid[37];
	memset(uuid, 0, sizeof(uuid));

	if (argc < 3 || argc > 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	ret = fs_uuid(uuid);
	if (ret)
		return CMD_RET_FAILURE;

	if (argc == 4)
		env_set(argv[3], uuid);
	else
		printf("%s\n", uuid);

	return CMD_RET_SUCCESS;
}

int do_fs_type(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	struct fstype_info *info;

	if (argc < 3 || argc > 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], FS_TYPE_ANY))
		return 1;

	info = fs_get_info(fs_type);

	if (argc == 4)
		env_set(argv[3], info->name);
	else
		printf("%s\n", info->name);

	fs_close();

	return CMD_RET_SUCCESS;
}

int do_rm(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
	  int fstype)
{
	if (argc != 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	if (fs_unlink(argv[3]))
		return 1;

	return 0;
}

int do_mkdir(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
	     int fstype)
{
	int ret;

	if (argc != 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	ret = fs_mkdir(argv[3]);
	if (ret) {
		printf("** Unable to create a directory \"%s\" **\n", argv[3]);
		return 1;
	}

	return 0;
}

int do_ln(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
	  int fstype)
{
	if (argc != 5)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	if (fs_ln(argv[3], argv[4]))
		return 1;

	return 0;
}
