/*
 *  pca953x.c - 4/8/16 bit I/O ports
 *
 *  Copyright (C) 2005 Ben Gardner <bgardner@wabtec.com>
 *  Copyright (C) 2007 Marvell International Ltd.
 *
 *  Derived from drivers/i2c/chips/pca9539.c
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/i2c/pca953x.h>

#include <asm/gpio.h>

#define PCA953X_INPUT          0
#define PCA953X_OUTPUT         1
#define PCA953X_INVERT         2
#define PCA953X_DIRECTION      3

/* This is temporary - in 2.6.26 i2c_driver_data should replace it. */
struct pca953x_desc {
	char		name[I2C_NAME_SIZE];
	unsigned long	driver_data;
};

static const struct pca953x_desc pca953x_descs[] = {
	{ "pca9534", 8, },
	{ "pca9535", 16, },
	{ "pca9536", 4, },
	{ "pca9537", 4, },
	{ "pca9538", 8, },
	{ "pca9539", 16, },
	/* REVISIT several pca955x parts should work here too */
};

struct pca953x_chip {
	unsigned gpio_start;
	uint16_t reg_output;
	uint16_t reg_direction;

	struct i2c_client *client;
	struct gpio_chip gpio_chip;
};

/* NOTE:  we can't currently rely on fault codes to come from SMBus
 * calls, so we map all errors to EIO here and return zero otherwise.
 */
static int pca953x_write_reg(struct pca953x_chip *chip, int reg, uint16_t val)
{
	int ret;

	if (chip->gpio_chip.ngpio <= 8)
		ret = i2c_smbus_write_byte_data(chip->client, reg, val);
	else
		ret = i2c_smbus_write_word_data(chip->client, reg << 1, val);

	if (ret < 0) {
		dev_err(&chip->client->dev, "failed writing register\n");
		return -EIO;
	}

	return 0;
}

static int pca953x_read_reg(struct pca953x_chip *chip, int reg, uint16_t *val)
{
	int ret;

	if (chip->gpio_chip.ngpio <= 8)
		ret = i2c_smbus_read_byte_data(chip->client, reg);
	else
		ret = i2c_smbus_read_word_data(chip->client, reg << 1);

	if (ret < 0) {
		dev_err(&chip->client->dev, "failed reading register\n");
		return -EIO;
	}

	*val = (uint16_t)ret;
	return 0;
}

static int pca953x_gpio_direction_input(struct gpio_chip *gc, unsigned off)
{
	struct pca953x_chip *chip;
	uint16_t reg_val;
	int ret;

	chip = container_of(gc, struct pca953x_chip, gpio_chip);

	reg_val = chip->reg_direction | (1u << off);
	ret = pca953x_write_reg(chip, PCA953X_DIRECTION, reg_val);
	if (ret)
		return ret;

	chip->reg_direction = reg_val;
	return 0;
}

static int pca953x_gpio_direction_output(struct gpio_chip *gc,
		unsigned off, int val)
{
	struct pca953x_chip *chip;
	uint16_t reg_val;
	int ret;

	chip = container_of(gc, struct pca953x_chip, gpio_chip);

	/* set output level */
	if (val)
		reg_val = chip->reg_output | (1u << off);
	else
		reg_val = chip->reg_output & ~(1u << off);

	ret = pca953x_write_reg(chip, PCA953X_OUTPUT, reg_val);
	if (ret)
		return ret;

	chip->reg_output = reg_val;

	/* then direction */
	reg_val = chip->reg_direction & ~(1u << off);
	ret = pca953x_write_reg(chip, PCA953X_DIRECTION, reg_val);
	if (ret)
		return ret;

	chip->reg_direction = reg_val;
	return 0;
}

static int pca953x_gpio_get_value(struct gpio_chip *gc, unsigned off)
{
	struct pca953x_chip *chip;
	uint16_t reg_val;
	int ret;

	chip = container_of(gc, struct pca953x_chip, gpio_chip);

	ret = pca953x_read_reg(chip, PCA953X_INPUT, &reg_val);
	if (ret < 0) {
		/* NOTE:  diagnostic already emitted; that's all we should
		 * do unless gpio_*_value_cansleep() calls become different
		 * from their nonsleeping siblings (and report faults).
		 */
		return 0;
	}

	return (reg_val & (1u << off)) ? 1 : 0;
}

static void pca953x_gpio_set_value(struct gpio_chip *gc, unsigned off, int val)
{
	struct pca953x_chip *chip;
	uint16_t reg_val;
	int ret;

	chip = container_of(gc, struct pca953x_chip, gpio_chip);

	if (val)
		reg_val = chip->reg_output | (1u << off);
	else
		reg_val = chip->reg_output & ~(1u << off);

	ret = pca953x_write_reg(chip, PCA953X_OUTPUT, reg_val);
	if (ret)
		return;

	chip->reg_output = reg_val;
}

static void pca953x_setup_gpio(struct pca953x_chip *chip, int gpios)
{
	struct gpio_chip *gc;

	gc = &chip->gpio_chip;

	gc->direction_input  = pca953x_gpio_direction_input;
	gc->direction_output = pca953x_gpio_direction_output;
	gc->get = pca953x_gpio_get_value;
	gc->set = pca953x_gpio_set_value;

	gc->base = chip->gpio_start;
	gc->ngpio = gpios;
	gc->label = chip->client->name;
}

static int __devinit pca953x_probe(struct i2c_client *client)
{
	struct pca953x_platform_data *pdata;
	struct pca953x_chip *chip;
	int ret, i;
	const struct pca953x_desc *id = NULL;

	pdata = client->dev.platform_data;
	if (pdata == NULL)
		return -ENODEV;

	/* this loop vanishes when we get i2c_device_id */
	for (i = 0; i < ARRAY_SIZE(pca953x_descs); i++)
		if (!strcmp(pca953x_descs[i].name, client->name)) {
			id = pca953x_descs + i;
			break;
		}
	if (!id)
		return -ENODEV;

	chip = kzalloc(sizeof(struct pca953x_chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	chip->client = client;

	chip->gpio_start = pdata->gpio_base;

	/* initialize cached registers from their original values.
	 * we can't share this chip with another i2c master.
	 */
	pca953x_setup_gpio(chip, id->driver_data);

	ret = pca953x_read_reg(chip, PCA953X_OUTPUT, &chip->reg_output);
	if (ret)
		goto out_failed;

	ret = pca953x_read_reg(chip, PCA953X_DIRECTION, &chip->reg_direction);
	if (ret)
		goto out_failed;

	/* set platform specific polarity inversion */
	ret = pca953x_write_reg(chip, PCA953X_INVERT, pdata->invert);
	if (ret)
		goto out_failed;


	ret = gpiochip_add(&chip->gpio_chip);
	if (ret)
		goto out_failed;

	if (pdata->setup) {
		ret = pdata->setup(client, chip->gpio_chip.base,
				chip->gpio_chip.ngpio, pdata->context);
		if (ret < 0)
			dev_warn(&client->dev, "setup failed, %d\n", ret);
	}

	i2c_set_clientdata(client, chip);
	return 0;

out_failed:
	kfree(chip);
	return ret;
}

static int pca953x_remove(struct i2c_client *client)
{
	struct pca953x_platform_data *pdata = client->dev.platform_data;
	struct pca953x_chip *chip = i2c_get_clientdata(client);
	int ret = 0;

	if (pdata->teardown) {
		ret = pdata->teardown(client, chip->gpio_chip.base,
				chip->gpio_chip.ngpio, pdata->context);
		if (ret < 0) {
			dev_err(&client->dev, "%s failed, %d\n",
					"teardown", ret);
			return ret;
		}
	}

	ret = gpiochip_remove(&chip->gpio_chip);
	if (ret) {
		dev_err(&client->dev, "%s failed, %d\n",
				"gpiochip_remove()", ret);
		return ret;
	}

	kfree(chip);
	return 0;
}

static struct i2c_driver pca953x_driver = {
	.driver = {
		.name	= "pca953x",
	},
	.probe		= pca953x_probe,
	.remove		= pca953x_remove,
};

static int __init pca953x_init(void)
{
	return i2c_add_driver(&pca953x_driver);
}
module_init(pca953x_init);

static void __exit pca953x_exit(void)
{
	i2c_del_driver(&pca953x_driver);
}
module_exit(pca953x_exit);

MODULE_AUTHOR("eric miao <eric.miao@marvell.com>");
MODULE_DESCRIPTION("GPIO expander driver for PCA953x");
MODULE_LICENSE("GPL");
