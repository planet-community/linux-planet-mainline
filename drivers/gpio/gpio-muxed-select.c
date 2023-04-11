// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO multiplexed select line driver
 *
 * Copyright (C) 2023 Otto Pflüger <otto.pflueger@abscue.de>
 * Based on the GPIO latch driver, which is:
 * Copyright (C) 2022 Sascha Hauer <s.hauer@pengutronix.de>
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/delay.h>

#include "gpiolib.h"

struct gpio_muxed_select_priv {
	struct gpio_chip gc;
	struct gpio_desc *enable_gpio;
	struct gpio_descs *select_gpios;
	unsigned int enable_duration_ns;
	unsigned int disable_duration_ns;
	bool enabled;
	bool invert;
	/*
	 * Depending on whether any of the underlying GPIOs may sleep we either
	 * use a mutex or a spinlock to protect our state.
	 */
	union {
		struct mutex mutex; /* protects @enabled */
		spinlock_t spinlock; /* protects @enabled */
	};
};

static int gpio_muxed_select_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static void gpio_muxed_select_set_unlocked(
		struct gpio_muxed_select_priv *priv,
		void (*set)(struct gpio_desc *desc, int value),
		unsigned int offset, bool val)
{
	int i;

	if (priv->invert)
		val = !val;

	if (!val) {
		set(priv->enable_gpio, 0);
		priv->enabled = false;
		return;
	}

	if (priv->enabled) {
		set(priv->enable_gpio, 0);
		ndelay(priv->disable_duration_ns);
	}

	for (i = 0; i < priv->select_gpios->ndescs; i++)
		set(priv->select_gpios->desc[i], (offset >> i) & 1);

	ndelay(priv->enable_duration_ns);
	set(priv->enable_gpio, 1);
	priv->enabled = true;
}

static void gpio_muxed_select_set(struct gpio_chip *gc, unsigned int offset, int val)
{
	struct gpio_muxed_select_priv *priv = gpiochip_get_data(gc);
	unsigned long flags;

	spin_lock_irqsave(&priv->spinlock, flags);

	gpio_muxed_select_set_unlocked(priv, gpiod_set_value, offset, val);

	spin_unlock_irqrestore(&priv->spinlock, flags);
}

static void gpio_muxed_select_set_can_sleep(struct gpio_chip *gc, unsigned int offset, int val)
{
	struct gpio_muxed_select_priv *priv = gpiochip_get_data(gc);

	mutex_lock(&priv->mutex);

	gpio_muxed_select_set_unlocked(priv, gpiod_set_value_cansleep, offset, val);

	mutex_unlock(&priv->mutex);
}

static bool gpio_muxed_select_can_sleep(struct gpio_muxed_select_priv *priv)
{
	int i;

	if (gpiod_cansleep(priv->enable_gpio))
		return true;

	for (i = 0; i < priv->select_gpios->ndescs; i++)
		if (gpiod_cansleep(priv->select_gpios->desc[i]))
			return true;

	return false;
}

/*
 * Some value which is still acceptable to delay in atomic context.
 * If we need to go higher we might have to switch to usleep_range(),
 * but that cannot ne used in atomic context and the driver would have
 * to be adjusted to support that.
 */
#define DURATION_NS_MAX 5000

static int gpio_muxed_select_probe(struct platform_device *pdev)
{
	struct gpio_muxed_select_priv *priv;
	struct device_node *np = pdev->dev.of_node;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->enable_gpio = devm_gpiod_get_optional(&pdev->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(priv->enable_gpio))
		return PTR_ERR(priv->enable_gpio);

	priv->select_gpios = devm_gpiod_get_array(&pdev->dev, "select", GPIOD_OUT_LOW);
	if (IS_ERR(priv->select_gpios))
		return PTR_ERR(priv->select_gpios);

	if (gpio_muxed_select_can_sleep(priv)) {
		priv->gc.can_sleep = true;
		priv->gc.set = gpio_muxed_select_set_can_sleep;
		mutex_init(&priv->mutex);
	} else {
		priv->gc.can_sleep = false;
		priv->gc.set = gpio_muxed_select_set;
		spin_lock_init(&priv->spinlock);
	}

	of_property_read_u32(np, "enable-duration-ns", &priv->enable_duration_ns);
	if (priv->enable_duration_ns > DURATION_NS_MAX) {
		dev_warn(&pdev->dev, "enable-duration-ns too high, limit to %d\n",
			 DURATION_NS_MAX);
		priv->enable_duration_ns = DURATION_NS_MAX;
	}

	of_property_read_u32(np, "disable-duration-ns", &priv->disable_duration_ns);
	if (priv->disable_duration_ns > DURATION_NS_MAX) {
		dev_warn(&pdev->dev, "disable-duration-ns too high, limit to %d\n",
			 DURATION_NS_MAX);
		priv->disable_duration_ns = DURATION_NS_MAX;
	}

	priv->invert = of_property_read_bool(np, "input-active-low");

	priv->gc.get_direction = gpio_muxed_select_get_direction;
	priv->gc.ngpio = 1 << priv->select_gpios->ndescs;
	priv->gc.owner = THIS_MODULE;
	priv->gc.base = -1;
	priv->gc.parent = &pdev->dev;

	platform_set_drvdata(pdev, priv);

	return devm_gpiochip_add_data(&pdev->dev, &priv->gc, priv);
}

static const struct of_device_id gpio_muxed_select_ids[] = {
	{
		.compatible = "gpio-muxed-select",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gpio_muxed_select_ids);

static struct platform_driver gpio_muxed_select_driver = {
	.driver	= {
		.name		= "gpio-muxed-select",
		.of_match_table	= gpio_muxed_select_ids,
	},
	.probe	= gpio_muxed_select_probe,
};
module_platform_driver(gpio_muxed_select_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Otto Pflüger <otto.pflueger@abscue.de>");
MODULE_DESCRIPTION("GPIO multiplexed select line driver");
