/*
 * Interface for managing a proprietary hardware watchdog
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
#include <asm/io.h>
#include <linux/of_platform.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/interrupt.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miquèl Raynal <miquel.raynal@navocap.com>");
MODULE_DESCRIPTION("Manages the hardware watchdog on Navocap Thelma7 baseboard");

struct _wd {
	struct kobject *kobj;
	int gpio_clock;
	int gpio_inhib;
	int gpio_trig;
	unsigned int period_s;
	unsigned int last_trig_s;
	int stopped;
};

static struct _wd *wd;

/* Actions on the watchdog */

static void wd_trig(void)
{
	gpio_set_value_cansleep(wd->gpio_trig, 1);
	msleep(50);
	gpio_set_value_cansleep(wd->gpio_trig, 0);

	wd->last_trig_s = jiffies / HZ;
}

static int wd_has_inhib(void)
{
	return gpio_get_value_cansleep(wd->gpio_inhib);
}

static int wd_has_clock(void)
{
	int clk[3];
	int has_clk = false;

	clk[0] = gpio_get_value_cansleep(wd->gpio_clock);
	msleep(400);
	clk[1] = gpio_get_value_cansleep(wd->gpio_clock);
	msleep(400);
	clk[2] = gpio_get_value_cansleep(wd->gpio_clock);

	if((clk[0] != clk[1]) || (clk[1] != clk[2]))
		has_clk = true;

	return has_clk;
}

static int wd_remaining_time(void)
{
	unsigned int remaining, now;

	now = jiffies / HZ;
	remaining = wd->period_s - (now - wd->last_trig_s);

	return remaining > 0 ? remaining : 0;
}

static const struct of_device_id wd_dt_ids[] = {
	{ .compatible = "nvp,watchdog_hw", },
	{ }
};

MODULE_DEVICE_TABLE(of, wd_dt_ids);

/* Sysfs management */

static ssize_t inhib_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	int inhibited = wd_has_inhib();
	if (inhibited)
		wd->stopped = true;
	return snprintf(buf, PAGE_SIZE, "%d\n", inhibited);
}

static ssize_t clock_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	int clocking = wd_has_clock();
	if (!clocking)
		wd->stopped = true;
	return snprintf(buf, PAGE_SIZE, "%d\n", clocking);
}

static ssize_t remaining_time_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	int rest = wd_remaining_time();
	if (wd_has_inhib())
		wd->stopped = true;
	if (wd->stopped)
		rest = -1;
	return snprintf(buf, PAGE_SIZE, "%d\n", rest);
}

static ssize_t trig_store(struct kobject *kobj,
			struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	int ret;
	int reset;

	ret = kstrtoint(buf, 10, &reset);
	if (ret < 0)
		return ret;

	if ((reset == 1) || (reset == '1')) {
		wd->stopped = false;
		wd_trig();
	}

	return count;
}

static struct kobj_attribute wd_inhib_attr = __ATTR_RO(inhib);
static struct kobj_attribute wd_clock_attr = __ATTR_RO(clock);
static struct kobj_attribute wd_remaining_time_attr = __ATTR_RO(remaining_time);
static struct kobj_attribute wd_trig_attr = __ATTR_WO(trig);

static struct attribute *wd_attrs[] = {
	&wd_inhib_attr.attr,
	&wd_clock_attr.attr,
	&wd_remaining_time_attr.attr,
	&wd_trig_attr.attr,
	NULL,
};

static struct attribute_group wd_attr_group = {
	.name = NULL,
	.attrs = wd_attrs,
};

static int __init wd_init(void)
{
	struct device_node *node;
	int rc;

	node = of_find_node_by_path("/wd@0");
	if (!node) {
		pr_err("Find node by path failed.\n");
		return -ENOENT;
	}

	wd = kzalloc(sizeof(struct _wd), GFP_KERNEL);
	if (!wd)
		return -ENOMEM;
	wd->gpio_clock = be32_to_cpup(of_get_property(node, "wd,gpio_clock", NULL));
	wd->gpio_inhib = be32_to_cpup(of_get_property(node, "wd,gpio_inhib", NULL));
	wd->gpio_trig = be32_to_cpup(of_get_property(node, "wd,gpio_trig", NULL));
	wd->period_s = be32_to_cpup(of_get_property(node, "wd,period_s", NULL));
	wd->last_trig_s = jiffies / HZ;

	if (!(gpio_is_valid(wd->gpio_inhib))
		|| !(gpio_is_valid(wd->gpio_clock))
		|| !(gpio_is_valid(wd->gpio_trig))) {
		pr_err("GPIO are not valid (problem with device tree ?)\n");
		rc = -EACCES;
		goto free_mem;
	}

	rc = gpio_request_one(wd->gpio_clock, GPIOF_IN, "wd-clock");
	if (rc < 0) {
		pr_err("wd-clock GPIO not available\n");
		goto free_mem;
	}

	rc = gpio_request_one(wd->gpio_inhib ,GPIOF_IN, "wd-inhib");
	if (rc < 0) {
		pr_err("wd-inhib GPIO not available\n");
		goto free_gpio_clock;
	}

	rc = gpio_request_one(wd->gpio_trig, GPIOF_OUT_INIT_LOW, "wd-trig");
	if (rc < 0) {
		pr_err("wd-trig GPIO not available\n");
		goto free_gpio_inhib;
	}

	wd_trig();

	wd->kobj = kobject_create_and_add("watchdog", kernel_kobj->parent);
	if (!wd->kobj) {
		pr_err("Kobject creation failed\n");
		rc = -ENOMEM;
		goto free_gpio_trig;
	}

	if (sysfs_create_group(wd->kobj, &wd_attr_group)) {
		pr_err("Sysfs group creation failed\n");
		rc = -ENOMEM;
		goto put_kobj;
	}

	return 0;

put_kobj:
	kobject_put(wd->kobj);
free_gpio_trig:
	gpio_free(wd->gpio_trig);
free_gpio_inhib:
	gpio_free(wd->gpio_inhib);
free_gpio_clock:
	gpio_free(wd->gpio_clock);
free_mem:
	kfree(wd);

	return rc;
}

static void __exit wd_exit(void)
{
	sysfs_remove_group(wd->kobj, &wd_attr_group);
	kobject_put(wd->kobj);
	gpio_free(wd->gpio_clock);
	gpio_free(wd->gpio_inhib);
	gpio_free(wd->gpio_trig);
	kfree(wd);
	return;
}

module_init(wd_init);
module_exit(wd_exit);
