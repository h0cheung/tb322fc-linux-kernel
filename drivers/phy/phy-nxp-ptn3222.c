// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024, Linaro Limited
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define NUM_SUPPLIES 2

struct ptn3222 {
	struct i2c_client *client;
	struct phy *phy;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data *supplies;
	struct regmap *regmap;
	const u32 *override_sequence;
	unsigned int override_sequence_count;
};

static const struct regmap_config ptn3222_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static int ptn3222_init(struct phy *phy)
{
	struct ptn3222 *ptn3222 = phy_get_drvdata(phy);
	unsigned int i;
	int ret;

	ret = regulator_bulk_enable(NUM_SUPPLIES, ptn3222->supplies);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(ptn3222->reset_gpio, 0);
	usleep_range(1000, 2000);

	for (i = 0; i < ptn3222->override_sequence_count; i += 2) {
		ret = regmap_write(ptn3222->regmap,
				   ptn3222->override_sequence[i + 1],
				   ptn3222->override_sequence[i]);
		if (ret) {
			gpiod_set_value_cansleep(ptn3222->reset_gpio, 1);
			regulator_bulk_disable(NUM_SUPPLIES, ptn3222->supplies);
			return ret;
		}
	}

	return 0;
}

static int ptn3222_exit(struct phy *phy)
{
	struct ptn3222 *ptn3222 = phy_get_drvdata(phy);

	gpiod_set_value_cansleep(ptn3222->reset_gpio, 1);

	return regulator_bulk_disable(NUM_SUPPLIES, ptn3222->supplies);
}

static const struct phy_ops ptn3222_ops = {
	.init		= ptn3222_init,
	.exit		= ptn3222_exit,
	.owner		= THIS_MODULE,
};

static const struct regulator_bulk_data ptn3222_supplies[NUM_SUPPLIES] = {
	{
		.supply = "vdd3v3",
		.init_load_uA = 11000,
	}, {
		.supply = "vdd1v8",
		.init_load_uA = 55000,
	}
};

static int ptn3222_parse_override_sequence(struct device *dev,
					   struct ptn3222 *ptn3222)
{
	const char *property = "qcom,param-override-seq-c1";
	u32 *values;
	int count;
	int ret;

	if (!device_property_present(dev, property))
		return 0;

	count = device_property_count_u32(dev, property);
	if (count < 0)
		return dev_err_probe(dev, count,
				     "failed to count override sequence\n");
	if (!count || count % 2)
		return dev_err_probe(dev, -EINVAL,
				     "invalid override sequence length %d\n", count);

	values = devm_kcalloc(dev, count, sizeof(*values), GFP_KERNEL);
	if (!values)
		return -ENOMEM;

	ret = device_property_read_u32_array(dev, property, values, count);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read override sequence\n");

	/* Qualcomm's downstream property stores value/register pairs. */
	ptn3222->override_sequence = values;
	ptn3222->override_sequence_count = count;

	return 0;
}

static int ptn3222_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct phy_provider *phy_provider;
	struct ptn3222 *ptn3222;
	int ret;

	ptn3222 = devm_kzalloc(dev, sizeof(*ptn3222), GFP_KERNEL);
	if (!ptn3222)
		return -ENOMEM;

	ptn3222->client = client;

	ptn3222->regmap = devm_regmap_init_i2c(client, &ptn3222_regmap_config);
	if (IS_ERR(ptn3222->regmap))
		return dev_err_probe(dev, PTR_ERR(ptn3222->regmap),
				     "failed to initialize regmap\n");

	ret = ptn3222_parse_override_sequence(dev, ptn3222);
	if (ret)
		return ret;

	ptn3222->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(ptn3222->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ptn3222->reset_gpio),
				     "unable to acquire reset gpio\n");

	ret = devm_regulator_bulk_get_const(dev, NUM_SUPPLIES, ptn3222_supplies,
					    &ptn3222->supplies);
	if (ret)
		return ret;

	ptn3222->phy = devm_phy_create(dev, dev->of_node, &ptn3222_ops);
	if (IS_ERR(ptn3222->phy)) {
		dev_err(dev, "failed to create PHY: %d\n", ret);
		return PTR_ERR(ptn3222->phy);
	}

	phy_set_drvdata(ptn3222->phy, ptn3222);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct i2c_device_id ptn3222_table[] = {
	{ .name = "ptn3222" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ptn3222_table);

static const struct of_device_id ptn3222_of_table[] = {
	{ .compatible = "nxp,ptn3222" },
	{ }
};
MODULE_DEVICE_TABLE(of, ptn3222_of_table);

static struct i2c_driver ptn3222_driver = {
	.driver = {
		.name = "ptn3222",
		.of_match_table = ptn3222_of_table,
	},
	.probe = ptn3222_probe,
	.id_table = ptn3222_table,
};

module_i2c_driver(ptn3222_driver);

MODULE_DESCRIPTION("NXP PTN3222 eUSB2 Redriver driver");
MODULE_LICENSE("GPL");
