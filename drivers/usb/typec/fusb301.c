#define DEBUG
// SPDX-License-Identifier: GPL-2.0
/*
 * Fairchild FUSB301 Type-C port controller driver
 *
 * Copyright (C) 2023 Otto Pflüger
 *
 * Based on wusb3801.c, Copyright (C) 2022 Samuel Holland <samuel@sholland.org>
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/typec.h>

#define FUSB301_REG_DEVICE_ID		0x01

#define FUSB301_REG_MODES		0x02
#define FUSB301_MODES_DRP_ACC		BIT(5)
#define FUSB301_MODES_DRP		BIT(4)
#define FUSB301_MODES_SINK_ACC		BIT(3)
#define FUSB301_MODES_SINK		BIT(2)
#define FUSB301_MODES_SOURCE_ACC	BIT(1)
#define FUSB301_MODES_SOURCE		BIT(0)

#define FUSB301_REG_CONTROL		0x03
#define FUSB301_CONTROL_HOST_CURRENT	GENMASK(2, 1)
#define FUSB301_HOST_CURRENT_3_A	(0x3 << 1)
#define FUSB301_HOST_CURRENT_1_5_A	(0x2 << 1)
#define FUSB301_HOST_CURRENT_DEFAULT	(0x1 << 1)
#define FUSB301_HOST_CURRENT_NONE	(0x0 << 1)
#define FUSB301_CONTROL_INT_MASK	BIT(0)
#define FUSB301_ENABLE_INTERRUPTS	0
#define FUSB301_DISABLE_INTERRUPTS	1

#define FUSB301_REG_MANUAL		0x04
#define FUSB301_MANUAL_UNATT_SINK	BIT(3)
#define FUSB301_MANUAL_UNATT_SOURCE	BIT(2)
#define FUSB301_MANUAL_DISABLED		BIT(1)
#define FUSB301_MANUAL_ERROR_RECOVERY	BIT(0)

#define FUSB301_REG_RESET		0x05
#define FUSB301_RESET_SW		BIT(0)

#define FUSB301_REG_MASK		0x10
#define FUSB301_MASK_ACC_CHANGE		BIT(3)
#define FUSB301_MASK_BC_LVL		BIT(2)
#define FUSB301_MASK_DETACH		BIT(1)
#define FUSB301_MASK_ATTACH		BIT(0)

#define FUSB301_REG_STATUS		0x11
#define FUSB301_STATUS_ORIENTATION	GENMASK(5, 4)
#define FUSB301_ORIENTATION_ERROR	(0x3 << 4)
#define FUSB301_ORIENTATION_CC2		(0x2 << 4)
#define FUSB301_ORIENTATION_CC1		(0x1 << 4)
#define FUSB301_ORIENTATION_NONE	(0x0 << 4)
#define FUSB301_STATUS_VBUSOK		BIT(3)
#define FUSB301_STATUS_BC_LVL		GENMASK(2, 1)
#define FUSB301_BC_LVL_3_A		(0x3 << 1)
#define FUSB301_BC_LVL_1_5_A		(0x2 << 1)
#define FUSB301_BC_LVL_DEFAULT		(0x1 << 1)
#define FUSB301_BC_LVL_NONE		(0x0 << 1)
#define FUSB301_STATUS_ATTACH		BIT(0)

#define FUSB301_REG_TYPE		0x12
#define FUSB301_TYPE_SINK		BIT(4)
#define FUSB301_TYPE_SOURCE		BIT(3)
#define FUSB301_TYPE_DEBUGACC		BIT(1)
#define FUSB301_TYPE_AUDIOACC		BIT(0)

#define FUSB301_REG_INTERRUPT		0x13
#define FUSB301_INTERRUPT_ACC_CHANGE	BIT(3)
#define FUSB301_INTERRUPT_BC_LVL	BIT(2)
#define FUSB301_INTERRUPT_DETACH	BIT(1)
#define FUSB301_INTERRUPT_ATTACH	BIT(0)

struct fusb301 {
	struct typec_capability	cap;
	struct device		*dev;
	struct typec_partner	*partner;
	struct typec_port	*port;
	struct regmap		*regmap;
	struct regulator	*vbus_supply;
	unsigned int		partner_type;
	enum typec_port_type	port_type;
	enum typec_pwr_opmode	pwr_opmode;
	bool			vbus_on;
};

static enum typec_role fusb301_get_default_role(struct fusb301 *fusb301)
{
	switch (fusb301->port_type) {
	case TYPEC_PORT_SRC:
		return TYPEC_SOURCE;
	case TYPEC_PORT_SNK:
		return TYPEC_SINK;
	case TYPEC_PORT_DRP:
	default:
		if (fusb301->cap.prefer_role == TYPEC_SOURCE)
			return TYPEC_SOURCE;
		return TYPEC_SINK;
	}
}

static int fusb301_map_port_type(enum typec_port_type type)
{
	switch (type) {
	case TYPEC_PORT_SRC:
		return FUSB301_MODES_SOURCE_ACC;
	case TYPEC_PORT_SNK:
		return FUSB301_MODES_SINK_ACC;
	case TYPEC_PORT_DRP:
	default:
		return FUSB301_MODES_DRP_ACC;
	}
}

static int fusb301_map_pwr_opmode(enum typec_pwr_opmode mode)
{
	switch (mode) {
	case TYPEC_PWR_MODE_USB:
	default:
		return FUSB301_HOST_CURRENT_DEFAULT;
	case TYPEC_PWR_MODE_1_5A:
		return FUSB301_HOST_CURRENT_1_5_A;
	case TYPEC_PWR_MODE_3_0A:
		return FUSB301_HOST_CURRENT_3_A;
	}
}

static unsigned int fusb301_map_try_role(int role)
{
	switch (role) {
	case TYPEC_NO_PREFERRED_ROLE:
	default:
		return 0;
	case TYPEC_SINK:
		return FUSB301_MANUAL_UNATT_SINK;
	case TYPEC_SOURCE:
		return FUSB301_MANUAL_UNATT_SOURCE;
	}
}

static enum typec_orientation fusb301_unmap_orientation(unsigned int status)
{
	switch (status & FUSB301_STATUS_ORIENTATION) {
	case FUSB301_ORIENTATION_NONE:
	case FUSB301_ORIENTATION_ERROR:
	default:
		return TYPEC_ORIENTATION_NONE;
	case FUSB301_ORIENTATION_CC1:
		return TYPEC_ORIENTATION_NORMAL;
	case FUSB301_ORIENTATION_CC2:
		return TYPEC_ORIENTATION_REVERSE;
	}
}

static enum typec_pwr_opmode fusb301_unmap_pwr_opmode(unsigned int status)
{
	switch (status & FUSB301_STATUS_BC_LVL) {
	case FUSB301_BC_LVL_NONE:
	case FUSB301_BC_LVL_DEFAULT:
	default:
		return TYPEC_PWR_MODE_USB;
	case FUSB301_BC_LVL_1_5_A:
		return TYPEC_PWR_MODE_1_5A;
	case FUSB301_BC_LVL_3_A:
		return TYPEC_PWR_MODE_3_0A;
	}
}

static int fusb301_try_role(struct typec_port *port, int role)
{
	struct fusb301 *fusb301 = typec_get_drvdata(port);

	return regmap_write(fusb301->regmap, FUSB301_REG_MANUAL,
			    fusb301_map_try_role(role));
}

static int fusb301_port_type_set(struct typec_port *port,
				  enum typec_port_type type)
{
	struct fusb301 *fusb301 = typec_get_drvdata(port);
	int ret;

	ret = regmap_write(fusb301->regmap, FUSB301_REG_MODES,
			   fusb301_map_port_type(type));
	if (ret)
		return ret;

	fusb301->port_type = type;

	return 0;
}

static const struct typec_operations fusb301_typec_ops = {
	.try_role	= fusb301_try_role,
	.port_type_set	= fusb301_port_type_set,
};

static int fusb301_hw_init(struct fusb301 *fusb301)
{
	int ret;
	ret = regmap_write(fusb301->regmap, FUSB301_REG_RESET,
			   FUSB301_RESET_SW);
	if (ret < 0)
		return ret;
	ret = regmap_write_bits(fusb301->regmap, FUSB301_REG_CONTROL,
				FUSB301_CONTROL_HOST_CURRENT |
				FUSB301_CONTROL_INT_MASK,
				fusb301_map_pwr_opmode(fusb301->pwr_opmode) |
				FUSB301_ENABLE_INTERRUPTS);
	if (ret < 0)
		return ret;
	ret = regmap_write(fusb301->regmap, FUSB301_REG_MODES,
			   fusb301_map_port_type(fusb301->port_type));
	if (ret < 0)
		return ret;
	ret = regmap_write(fusb301->regmap, FUSB301_REG_MANUAL,
			   fusb301_map_try_role(fusb301->cap.prefer_role));
	if (ret < 0)
		return ret;
	return 0;
}

static void fusb301_hw_update(struct fusb301 *fusb301)
{
	struct typec_port *port = fusb301->port;
	struct device *dev = fusb301->dev;
	unsigned int partner_type, status;
	int ret;

	ret = regmap_read(fusb301->regmap, FUSB301_REG_STATUS, &status);
	if (ret) {
		dev_warn(dev, "Failed to read port status: %d\n", ret);
		status = 0;
	}
	dev_dbg(dev, "status = 0x%02x\n", status);

	ret = regmap_read(fusb301->regmap, FUSB301_REG_TYPE, &partner_type);
	if (ret) {
		dev_warn(dev, "Failed to read partner type: %d\n", ret);
		status = 0;
	}
	dev_dbg(dev, "partner_type = 0x%02x\n", partner_type);

	/* ignore undefined bits */
	partner_type &= FUSB301_TYPE_SINK |
			FUSB301_TYPE_SOURCE |
			FUSB301_TYPE_AUDIOACC |
			FUSB301_TYPE_DEBUGACC;

	if (partner_type == FUSB301_TYPE_SINK) {
		if (!fusb301->vbus_on) {
			ret = regulator_enable(fusb301->vbus_supply);
			if (ret)
				dev_warn(dev, "Failed to enable VBUS: %d\n", ret);
			fusb301->vbus_on = true;
		}
	} else {
		if (fusb301->vbus_on) {
			regulator_disable(fusb301->vbus_supply);
			fusb301->vbus_on = false;
		}
	}

	if (partner_type != fusb301->partner_type) {
		struct typec_partner_desc desc = {};
		enum typec_data_role data_role;
		enum typec_role pwr_role = fusb301_get_default_role(fusb301);

		switch (partner_type) {
		case FUSB301_TYPE_SINK:
			pwr_role = TYPEC_SOURCE;
			break;
		case FUSB301_TYPE_SOURCE:
			pwr_role = TYPEC_SINK;
			break;
		case FUSB301_TYPE_AUDIOACC:
			desc.accessory = TYPEC_ACCESSORY_AUDIO;
			break;
		case FUSB301_TYPE_DEBUGACC:
			desc.accessory = TYPEC_ACCESSORY_DEBUG;
			break;
		}

		if (fusb301->partner) {
			typec_unregister_partner(fusb301->partner);
			fusb301->partner = NULL;
		}

		if (partner_type != 0) {
			fusb301->partner = typec_register_partner(port, &desc);
			if (IS_ERR(fusb301->partner))
				dev_err(dev, "Failed to register partner: %ld\n",
					PTR_ERR(fusb301->partner));
		}

		data_role = pwr_role == TYPEC_SOURCE ? TYPEC_HOST : TYPEC_DEVICE;
		typec_set_data_role(port, data_role);
		typec_set_pwr_role(port, pwr_role);
		typec_set_vconn_role(port, pwr_role);
	}

	typec_set_pwr_opmode(fusb301->port,
			     partner_type == FUSB301_TYPE_SOURCE
				? fusb301_unmap_pwr_opmode(status)
				: fusb301->pwr_opmode);
	typec_set_orientation(fusb301->port,
			      fusb301_unmap_orientation(status));

	fusb301->partner_type = partner_type;
}

static irqreturn_t fusb301_irq(int irq, void *data)
{
	struct fusb301 *fusb301 = data;
	unsigned int interrupt;

	/*
	 * The interrupt register must be read in order to clear the IRQ,
	 * but all of the useful information is in the status register.
	 */
	regmap_read(fusb301->regmap, FUSB301_REG_INTERRUPT, &interrupt);
	dev_dbg(fusb301->dev, "IRQ (interrupt = 0x%02x)\n", interrupt);

	fusb301_hw_update(fusb301);

	return IRQ_HANDLED;
}

static const struct regmap_config config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= FUSB301_REG_INTERRUPT,
};

static int fusb301_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *connector;
	struct fusb301 *fusb301;
	const char *cap_str;
	int ret;

	fusb301 = devm_kzalloc(dev, sizeof(*fusb301), GFP_KERNEL);
	if (!fusb301)
		return -ENOMEM;

	i2c_set_clientdata(client, fusb301);

	fusb301->dev = dev;

	fusb301->regmap = devm_regmap_init_i2c(client, &config);
	if (IS_ERR(fusb301->regmap))
		return PTR_ERR(fusb301->regmap);

	fusb301->vbus_supply = devm_regulator_get(dev, "vbus");
	if (IS_ERR(fusb301->vbus_supply))
		return PTR_ERR(fusb301->vbus_supply);

	connector = device_get_named_child_node(dev, "connector");
	if (!connector) {
		dev_err(dev, "Failed to get connector node\n");
		return -ENODEV;
	}

	ret = typec_get_fw_cap(&fusb301->cap, connector);
	if (ret) {
		dev_err(dev, "Failed to read capabilities from node\n");
		goto err_put_connector;
	}
	fusb301->port_type = fusb301->cap.type;

	ret = fwnode_property_read_string(connector, "typec-power-opmode", &cap_str);
	if (ret) {
		dev_err(dev, "Failed to read typec-power-opmode property\n");
		goto err_put_connector;
	}

	ret = typec_find_pwr_opmode(cap_str);
	if (ret < 0 || ret == TYPEC_PWR_MODE_PD) {
		dev_err(dev, "Invalid typec-power-opmode specified\n");
		goto err_put_connector;
	}
	fusb301->pwr_opmode = ret;

	/* Initialize the hardware with the devicetree settings. */
	ret = fusb301_hw_init(fusb301);
	if (ret) {
		dev_err(dev, "Failed to initialize hardware\n");
		goto err_put_connector;
	}

	fusb301->cap.revision		= USB_TYPEC_REV_1_2;
	fusb301->cap.accessory[0]	= TYPEC_ACCESSORY_AUDIO;
	fusb301->cap.accessory[1]	= TYPEC_ACCESSORY_DEBUG;
	fusb301->cap.orientation_aware	= true;
	fusb301->cap.driver_data	= fusb301;
	fusb301->cap.ops		= &fusb301_typec_ops;

	fusb301->port = typec_register_port(dev, &fusb301->cap);
	if (IS_ERR(fusb301->port)) {
		ret = PTR_ERR(fusb301->port);
		goto err_put_connector;
	}

	/* Initialize the port attributes from the hardware state. */
	fusb301_hw_update(fusb301);

	ret = request_threaded_irq(client->irq, NULL, fusb301_irq,
				   IRQF_ONESHOT, dev_name(dev), fusb301);
	if (ret)
		goto err_unregister_port;

	fwnode_handle_put(connector);

	return 0;

err_unregister_port:
	typec_unregister_port(fusb301->port);
err_put_connector:
	fwnode_handle_put(connector);

	return ret;
}

static void fusb301_remove(struct i2c_client *client)
{
	struct fusb301 *fusb301 = i2c_get_clientdata(client);

	free_irq(client->irq, fusb301);

	if (fusb301->partner)
		typec_unregister_partner(fusb301->partner);
	typec_unregister_port(fusb301->port);

	if (fusb301->vbus_on)
		regulator_disable(fusb301->vbus_supply);
}

static const struct of_device_id fusb301_of_match[] = {
	{ .compatible = "fcs,fusb301" },
	{}
};
MODULE_DEVICE_TABLE(of, fusb301_of_match);

static struct i2c_driver fusb301_driver = {
	.probe_new	= fusb301_probe,
	.remove		= fusb301_remove,
	.driver		= {
		.name		= "fusb301",
		.of_match_table	= fusb301_of_match,
	},
};

module_i2c_driver(fusb301_driver);

MODULE_AUTHOR("Otto Pflüger <otto.pflueger@abscue.de>");
MODULE_DESCRIPTION("Fairchild FUSB301 Type-C port controller driver");
MODULE_LICENSE("GPL");
