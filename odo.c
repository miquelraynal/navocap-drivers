/*
 * Offers a sysfs interface to deal with a timer
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
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>
#include <linux/delay.h>
#include <asm/io.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miquèl Raynal <miquel.raynal@navocap.com>");
MODULE_DESCRIPTION("Reads the pulses from an odometer on a timer");

// Pointer to the physical memory region
#define MEM_BASE       0x10000000
#define MEM_GPT_OFFSET {0x3000,0x4000,0x5000,0x19000,0x1A000,0x1F000}
#define GPT_TIN        {79, 79, 79, 91, 89, 78}
#define MEM_LENGTH     0x18

#define TCTL_REG   0x0
#define TPRER_REG  0x4
#define TCMP_REG   0x8
#define TCN_REG    0x10
#define TSTAT_REG  0x14

// Position of the field, size of the field
#define TCTL_TEN        0,  1
#define TCTL_CLKSOURCE  1,  3
#define TCTL_COMP_EN    4,  1
#define TCTL_FRR        8,  1
#define TCTL_CC         10, 1
#define TCMP_CMP        0, 32
#define TPRER_PRESCALER 0, 10
#define TSTAT_COMP      0x1

struct _odo {
	struct kobject *kobj;
	unsigned long gpt_base;
	void __iomem *vmem;
	unsigned long counter_ms;
	unsigned long counter_ls;
	unsigned long nb_access;
	unsigned long first_access;
	unsigned long last_access;
};

static struct _odo *odo;
int gpt_id = 2;
module_param(gpt_id, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(gpt_id, "General purpose timer ID (default 2)");

/* Actions on the GPT */

static void odo_set_gpt_field(int reg, int pos, int sz, int val)
{
	int bit, v, mask, tmp;

	v = ioread32(odo->vmem + reg); /* Read register */
	tmp=v;
	for(bit = pos; bit < pos + sz; ++bit)
		v &= ~(1 << bit); /* Reset the field */

	mask = val << pos; /* Derive the value to write */
	v |= mask; /* Merge the value with the register */

	iowrite32(v, odo->vmem + reg); /* Push to the register */
}

static int odo_timer_setup(void)
{
	/* Disable the counter */
	odo_set_gpt_field(TCTL_REG, TCTL_TEN, 0x0);
	/* Enable reset of the counter when timer disabled */
	odo_set_gpt_field(TCTL_REG, TCTL_CC, 0x1);
	/* Choose TIN as input clock */
	odo_set_gpt_field(TCTL_REG, TCTL_CLKSOURCE, 0x3);
	/* Divide by 1 */
	odo_set_gpt_field(TPRER_REG, TPRER_PRESCALER, 0x0);
	/* Enable compare action */
	odo_set_gpt_field(TCTL_REG, TCTL_COMP_EN, 0x1);
	/* Restart after compare trig (here, free run would have been similar) */
	odo_set_gpt_field(TCTL_REG, TCTL_FRR, 0x0);
	/* Set compare register to 0xFFFFFFFF */
	odo_set_gpt_field(TCMP_REG, TCMP_CMP, 0xFFFFFFFF);
	/* Start counting */
	odo_set_gpt_field(TCTL_REG, TCTL_TEN, 0x1);

	return 0;
}

static int odo_read_count(void)
{
	unsigned int count;

	count = ioread32(odo->vmem + TCN_REG);

	return count;
}

static int odo_reset_count(void)
{
	odo->counter_ms = 0;
	odo->counter_ls = 0;

	/* Disable the timer (resets the counter because CC bit is set) */
	odo_set_gpt_field(TCTL_REG, TCTL_TEN, 0x0);

	msleep(10);

	/* Start counting */
	odo_set_gpt_field(TCTL_REG, TCTL_TEN, 0x1);

	return 0;
}


static const struct of_device_id odo_dt_ids[] = {
	{ .compatible = "nvp,odo", },
	{ }
};

MODULE_DEVICE_TABLE(of, odo_dt_ids);

/* Sysfs management */

static ssize_t counter_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	unsigned long long counter_64;
	unsigned int status = ioread32(odo->vmem + TSTAT_REG);
	if (status & TSTAT_COMP) { /* Evaluate carry */
		odo->counter_ms++;
		iowrite32(TSTAT_COMP, odo->vmem + TSTAT_REG);
	}
	odo->counter_ls = odo_read_count();
	counter_64 = ((u64)odo->counter_ms << 32) + odo->counter_ls;
	odo->nb_access++;
	odo->last_access = jiffies;
	if (odo->first_access == 0)
		odo->first_access = odo->last_access;

	return snprintf(buf, PAGE_SIZE, "%llu\n", counter_64);
}

static ssize_t nb_access_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%lu\n", odo->nb_access);
}

static ssize_t mean_period_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	unsigned long diff = odo->last_access - odo->first_access;
	unsigned long period;
	if (odo->nb_access <= 0)
		period = 0;
	else
		period = diff * 1000 / HZ / odo->nb_access;

	return snprintf(buf, PAGE_SIZE, "%lu ms\n", period);
}

static ssize_t reset_store(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	int ret;
	int reset;

	ret = kstrtoint(buf, 10, &reset);
	if (ret < 0)
		return ret;

	if ((reset == 1) || (reset == '1')) {
		odo_reset_count();
		odo->nb_access = 0;
		odo->first_access = 0;
		odo->last_access = 0;
	}

	return count;
}

static struct kobj_attribute odo_counter_attr     = __ATTR_RO(counter);
static struct kobj_attribute odo_nb_access_attr   = __ATTR_RO(nb_access);
static struct kobj_attribute odo_mean_period_attr = __ATTR_RO(mean_period);
static struct kobj_attribute odo_reset_attr       = __ATTR_WO(reset);

static struct attribute *odo_attrs[] =
{
	&odo_counter_attr.attr,
	&odo_nb_access_attr.attr,
	&odo_mean_period_attr.attr,
	&odo_reset_attr.attr,
	NULL,
};

static struct attribute_group odo_attr_group =
{
	.name = NULL,
	.attrs = odo_attrs,
};

static int __init odo_init(void)
{
	int offset[] = MEM_GPT_OFFSET;
	int gpio[] = GPT_TIN;
	struct device_node *node;
	int rc = 0;

	node = of_find_node_by_path("/odo@0");
	if (!node) {
		pr_err("odo: No node in DT, using timer %d (GPIO %d)\n",
			gpt_id, gpio[gpt_id]);
		if ((gpt_id < 2) || (gpt_id > 6))
			return -EINVAL;
	} else {
		gpt_id = be32_to_cpup(of_get_property(node, "odo,timer", NULL));
	}

	odo = kzalloc(sizeof(struct _odo), GFP_KERNEL);
	if (!odo)
		return -ENOMEM;

	odo->gpt_base = MEM_BASE | offset[gpt_id - 1];
	odo->counter_ms = 0;
	odo->counter_ls = 0;
	odo->nb_access = 0;
	odo->first_access = 0;
	odo->last_access = 0;

	if (!request_mem_region(odo->gpt_base, MEM_LENGTH, "Odometer GPT")) {
		pr_err("odo: Impossible to reserve memory region\n");
		rc = -ENOMEM;
		goto free_alloc;
		kfree(odo);
	}

	odo->vmem = (u32 *)ioremap_nocache(odo->gpt_base, MEM_LENGTH);
	if (!odo->vmem)	{
		pr_err("odo: Ioremap failed\n");
		rc = -ENOMEM;
		goto release_region;
	}

	rc = gpio_request_one(gpio[gpt_id], GPIOF_IN, "Odometer clock input");
	if (rc < 0) {
		pr_err("Cannot use GPIO %d for odometer pulses\n", gpio[gpt_id]);
		goto unmap;
	}

	odo_timer_setup();

	odo->kobj = kobject_create_and_add("odo", kernel_kobj->parent);
	if (!odo->kobj)	{
		pr_err("odo: Kobject creation failed\n");
		rc = -ENOMEM;
		goto free_gpio;
	}

	if (sysfs_create_group(odo->kobj, &odo_attr_group)) {
		pr_err("odo: Sysfs group creation failed\n");
		rc = -ENOMEM;
		goto put_kobj;
	}

	return 0;

put_kobj:
	kobject_put(odo->kobj);
free_gpio:
	gpio_free(gpio[gpt_id]);
unmap:
	iounmap(odo->vmem);
release_region:
	release_mem_region(odo->gpt_base, MEM_LENGTH);
free_alloc:
	kfree(odo);

	return rc;
}

static void __exit odo_exit(void)
{
	int gpio[] = GPT_TIN;

	sysfs_remove_group(odo->kobj, &odo_attr_group);
	kobject_put(odo->kobj);
	gpio_free(gpio[gpt_id]);
	iounmap(odo->vmem);
	release_mem_region(odo->gpt_base, MEM_LENGTH);
	kfree(odo);
	return;
}

module_init(odo_init);
module_exit(odo_exit);
