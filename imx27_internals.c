/*
 * Retrieves registers values like MAC address from i.MX27 memory
 * Copyright (C) 2016  Miquèl Raynal
 *
 *  This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/ioport.h>
#include <linux/seq_file.h>
#include <asm/io.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miquèl Raynal <miquel.raynal@navocap.com>");
MODULE_DESCRIPTION("i.MX27 specific module retrieving internal values");

/* Pointer to the physical memory region */
#define PCCR0         0x10027020
#define SYSCTRL_BASE  0x10027800
#define IIM_BASE      0x10028000
#define SYSCTRL_ID    (SYSCTRL_BASE + 0x0)
#define IIM_PREV      (IIM_BASE + 0x0020)
#define IIM_SREV      (IIM_BASE + 0x0024)
#define IIM_MAC       (IIM_BASE + 0x0814)
#define IIM_SUID      (IIM_BASE + 0x0C04)
#define PROC_BUF_SIZE 200

struct phys_reg {
	char name[20];
	int address;
	int length; /* in bytes */
	u64 value;
};

enum reg {
	CHIP_ID = 0,
	PREV,
	SREV,
	SUID,
	MAC,
	/* Count */
	ENUM_REG_COUNT,
};

struct phys_reg registers[ENUM_REG_COUNT] = {
	{ "chip_id",     SYSCTRL_ID, 4, 0 },
	{ "product_rev", IIM_PREV,   4, 0 },
	{ "silicon_rev", IIM_SREV,   4, 0 },
	{ "suid",        IIM_SUID,   6, 0 },
	{ "mac_address", IIM_MAC,    6, 0 },
};

/*
 * Reads the value pointed to by the address field of reg
 * This function is designed to work for address out of IIM
 * Registers are 32 bits wide
 */
static int read_reg_mem(struct phys_reg *reg)
{
	void __iomem *vmem;
	int rc = 0;
	int i;
	u64 tmp_value = 0;
	u32 read_value = 0;

	if ((reg->length % 4 != 0) || (reg->length > 8)) {
		pr_err("Memory access sould be aligned to 32 bits\n");
		return -EINVAL;
	}

	/* Read each 32 bits registers normally */
	if (!request_mem_region(reg->address, reg->length, "Mem register")) {
		pr_err("Unable to request region for reg: %s\n", reg->name);
		return -EBUSY;
	}
	vmem = ioremap(reg->address, reg->length);
	if (!vmem) {
		pr_err("Unable to map region for reg: %s\n", reg->name);
		rc = -ENOMEM;
		goto release_reg;
	}

	for (i = 0; i < reg->length; i += 4) {
		read_value = ioread32(vmem + i);
		tmp_value += (u64)(read_value) << i * 32;
	}

	reg->value = tmp_value;

	iounmap(vmem);
release_reg:
	release_mem_region(reg->address, reg->length);

	return rc;
}

/*
 * Reads the value pointed to by the address field of reg
 * This function is designed to work for address related to IIM
 * Values are 8 bits sectors aligned on 32 bits
 */
static int read_reg_iim(struct phys_reg *reg)
{
	void __iomem *vmem, *clocks;
	int clocks_reg;
	int rc = 0;
	int i;
	u64 tmp_value = 0;
	u8 read_value = 0;

	if (reg->length > 8) {
		pr_err("Register size should be at most 64 bits\n");
		return -EINVAL;
	}

	/* Enable IIM clock (needed if module loaded after boot time) */
	if (!request_mem_region(PCCR0, 4, "Peripheral clock control")) {
		pr_err("Unable to request region for PCCR0\n");
		return -EBUSY;
	}
	clocks = ioremap(PCCR0, 4);
	if (!clocks) {
		pr_err("Unable to map registers for PCCR0\n");
		rc = -ENOMEM;
		goto release_pccr;
	}
	clocks_reg = ioread32(clocks);
	iowrite32(clocks_reg | (1<<16), clocks);

	/* Read each 32 bits registers as if they were 8 bits values */
	if (!request_mem_region(reg->address, reg->length * 4, "IIM register")) {
		pr_err("Unable to request region for IIM reg: %s\n", reg->name);
		rc = -EBUSY;
		goto unmap_pccr;
	}
	vmem = ioremap(reg->address, reg->length * 4);
	if (!vmem) {
		pr_err("Unable to map region for IIM reg: %s\n", reg->name);
		rc = -ENOMEM;
		goto release_reg;
	}

	for (i = 0; i < reg->length; ++i) {
		int id = i * 4;
		read_value = ioread8(vmem + id);
		tmp_value += (u64)(read_value) << i * 8;
	}

	reg->value = tmp_value;

	iounmap(vmem);
release_reg:
	release_mem_region(reg->address, reg->length * 4);
unmap_pccr:
	iounmap(clocks);
release_pccr:
	release_mem_region(PCCR0, 4);

	return rc;
}

static int read_reg(struct phys_reg *reg)
{
	if ((reg->address >= IIM_BASE) && (reg->address < IIM_BASE + 0x1000))
		return read_reg_iim(reg);
	else
		return read_reg_mem(reg);
}

static int registers_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < ENUM_REG_COUNT; ++i) {
		seq_printf(m, "%s: 0x%llX\n",
			registers[i].name, registers[i].value);
	}

	return 0;
}

static int registers_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, registers_show, NULL);
}

struct proc_dir_entry *proc_file_entry;

static const struct file_operations proc_file_fops = {
	.owner      = THIS_MODULE,
	.open       = registers_proc_open,
	.read       = seq_read,
	.llseek     = seq_lseek,
	.release    = single_release,
};

static int __init internals_init(void)
{
	int i, ret;

	for (i = 0; i < ENUM_REG_COUNT; ++i) {
		ret = read_reg(&registers[i]);
		if (ret < 0)
			continue;
		pr_info("%s: 0x%llX\n", registers[i].name, registers[i].value);
	}

	if (!proc_create("internal_registers", 0444, NULL, &proc_file_fops))
		return -ENOMEM;

	return 0;
}

static void __exit internals_exit(void)
{
	remove_proc_entry("internal_registers", NULL);
	return;
}

module_init(internals_init);
module_exit(internals_exit);
