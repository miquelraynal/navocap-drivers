/*
 * Offers a sysfs interface to deal with a counter available through I2C
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
#include <linux/i2c.h>
#include <asm/io.h>
#include <linux/of_platform.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/sched.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miquèl Raynal <miquel.raynal@navocap.com>");
MODULE_DESCRIPTION("Reads the pulses from an odometer through I2C (PIC counter)");

#define REG_CNT 0X0
#define REG_VER 0x4

struct picodo_chip
{
	struct i2c_client *client;
	struct mutex lock;
	int gpio_reset;
	unsigned int counter;
	int version;
	int nb_access;
	unsigned long first_access;
	unsigned long last_access;
};

static struct picodo_chip *chip;

/* Actions on the chip */

static int picodo_read_reg(struct picodo_chip *chip, int reg, int *storage)
{
	int byte, ret = 0, v = 0;

	for (byte = 0; byte < 4; ++byte) {
		ret = i2c_smbus_read_byte_data(chip->client, reg + byte);
		if (ret < 0) {
			pr_err("error reading byte 0x%X.\n", reg + byte);
			return ret;
		}
		v += ret << (byte * 8);
	}

	*storage = v;

	return ret;
}

static int picodo_reset(struct picodo_chip *chip)
{
	int ret;

	ret = gpio_direction_output(chip->gpio_reset, 0);
	if(ret < 0) {
		pr_err("Cannot change reset gpio direction to output.\n");
		goto end;
	}
	msleep(10);
	ret = gpio_direction_input(chip->gpio_reset);
	msleep(10);
	i2c_smbus_read_byte_data(chip->client, REG_CNT); /* Unlock register read */

end:
	return ret;
}

/* I2C management */

static int picodo_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int rc;

	chip = devm_kzalloc(&client->dev, sizeof(struct picodo_chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	mutex_init(&chip->lock);
	chip->gpio_reset = be32_to_cpup(
		of_get_property(client->dev.of_node, "gpio-reset", NULL)
		);
	rc = gpio_request_one(chip->gpio_reset, GPIOF_IN, "picodo-reset");
	if(rc < 0) {
		pr_err("Cannot reserve reset GPIO %d\n", chip->gpio_reset);
		return rc;
	}

	chip->counter = 0;
	picodo_read_reg(chip, REG_VER, &chip->version);
	chip->nb_access = 0;
	chip->first_access = 0;
	chip->last_access = 0;
	picodo_reset(chip);

	i2c_set_clientdata(client, chip);
	return 0;
}

static int picodo_remove(struct i2c_client *client)
{
	gpio_free(chip->gpio_reset);
	return 0;
}

static const struct of_device_id picodo_dt_ids[] = {
	{ .compatible = "nvp,picodo", },
	{ }
};

MODULE_DEVICE_TABLE(of, picodo_dt_ids);

static struct i2c_device_id picodo_idtable[] = {
	{ "picodo", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, picodo_idtable);

static struct i2c_driver picodo_driver = {
	.driver = {
		.name = "picodo",
		.of_match_table = picodo_dt_ids,
	},
	.id_table = picodo_idtable,
	.probe    = picodo_probe,
	.remove   = picodo_remove,
};

/* Sysfs management */

static ssize_t counter_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	int ret = picodo_read_reg(chip, REG_CNT, &chip->counter);
	if (ret < 0) {
		picodo_reset(chip);
		chip->nb_access = 0;
		return ret;
	}

	chip->nb_access++;
	chip->last_access = jiffies;
	if (chip->first_access == 0)
		chip->first_access = chip->last_access;

	return snprintf(buf, PAGE_SIZE, "%d\n", chip->counter);
}

#define BYTE(c) ((c) & 0xFF)
static ssize_t version_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	int v = chip->version;
	return snprintf(buf, PAGE_SIZE, "%c%c%c%c\n",
			BYTE(v >> 24), BYTE(v >> 16), BYTE(v >> 8), BYTE(v >> 0));
}

static ssize_t nb_access_show(struct kobject *kobj,
			struct kobj_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", chip->nb_access);
}

static ssize_t mean_period_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	unsigned long diff = chip->last_access - chip->first_access;
	unsigned long period;
	if (chip->nb_access <= 0)
		period = 0;
	else
		period = diff * 1000 / HZ / chip->nb_access;

	return snprintf(buf, PAGE_SIZE, "%ld ms\n", period);
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
		picodo_reset(chip);
		chip->nb_access = 0;
		chip->first_access = 0;
		chip->last_access = 0;
	}

	return count;
}

static struct kobj_attribute picodo_counter_attr     = __ATTR_RO(counter);
static struct kobj_attribute picodo_version_attr     = __ATTR_RO(version);
static struct kobj_attribute picodo_nb_access_attr   = __ATTR_RO(nb_access);
static struct kobj_attribute picodo_mean_period_attr = __ATTR_RO(mean_period);
static struct kobj_attribute picodo_reset_attr       = __ATTR_WO(reset);

static struct attribute *picodo_attrs[] =
{
	&picodo_counter_attr.attr,
	&picodo_version_attr.attr,
	&picodo_nb_access_attr.attr,
	&picodo_mean_period_attr.attr,
	&picodo_reset_attr.attr,
	NULL,
};

static struct attribute_group picodo_attr_group =
{
	.name = NULL,
	.attrs = picodo_attrs,
};

static struct kobject *picodo_kobj;

static int __init picodo_init(void)
{
	int rc;

	rc = i2c_add_driver(&picodo_driver);
	if (rc < 0) {
		pr_err("I2C add driver failed\n");
		goto err_add_drv;
	}

	picodo_kobj = kobject_create_and_add("odo", kernel_kobj->parent);
	if (!picodo_kobj) {
		pr_err("Kobject creation failed\n");
		rc = -ENOMEM;
		goto err_kobj;
	}

	if (sysfs_create_group(picodo_kobj, &picodo_attr_group)) {
		pr_err("Sysfs group creation failed\n");
		rc = -ENOMEM;
		goto err_sys;
	}

	return 0;

err_sys:
	kobject_put(picodo_kobj);
err_kobj:
	i2c_del_driver(&picodo_driver);
err_add_drv:
	return rc;
}

static void __exit picodo_exit(void)
{
	sysfs_remove_group(picodo_kobj, &picodo_attr_group);
	kobject_put(picodo_kobj);

	i2c_del_driver(&picodo_driver);
	return;
}

module_init(picodo_init);
module_exit(picodo_exit);
