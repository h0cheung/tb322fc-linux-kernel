// SPDX-License-Identifier: GPL-2.0-only
/* Will Semiconductor WL2868C seven-channel camera regulator. */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

#define WL2868C_DEVICE_ID		0x00
#define WL2868C_LDO_ENABLE	0x0e
#define WL2868C_SYS_ENABLE	BIT(7)

static int wl2868c_enable(struct regulator_dev *rdev)
{
	return regmap_update_bits(rdev->regmap, WL2868C_LDO_ENABLE,
				  WL2868C_SYS_ENABLE | rdev->desc->enable_mask,
				  WL2868C_SYS_ENABLE | rdev->desc->enable_mask);
}

static int wl2868c_is_enabled(struct regulator_dev *rdev)
{
	unsigned int mask = WL2868C_SYS_ENABLE | rdev->desc->enable_mask;
	unsigned int value;
	int ret;

	ret = regmap_read(rdev->regmap, WL2868C_LDO_ENABLE, &value);
	if (ret)
		return ret;
	return (value & mask) == mask;
}

static const struct regulator_ops wl2868c_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.enable = wl2868c_enable,
	.disable = regulator_disable_regmap,
	.is_enabled = wl2868c_is_enabled,
};

#define WL2868C_LDO(_num, _supply, _min, _mask) {		\
	.name = "ldo" #_num,				\
	.of_match = "ldo" #_num,				\
	.regulators_node = "regulators",			\
	.id = (_num) - 1,				\
	.type = REGULATOR_VOLTAGE,			\
	.owner = THIS_MODULE,				\
	.ops = &wl2868c_ops,				\
	.supply_name = _supply,				\
	.min_uV = _min,					\
	.uV_step = 8000,					\
	.n_voltages = (_mask) + 1,			\
	.vsel_reg = 0x02 + (_num),			\
	.vsel_mask = _mask,				\
	.enable_reg = WL2868C_LDO_ENABLE,			\
	.enable_mask = BIT((_num) - 1),			\
	.enable_time = 150,				\
}

static const struct regulator_desc wl2868c_regulators[] = {
	WL2868C_LDO(1, "vin12", 496000, 0x7f),
	WL2868C_LDO(2, "vin12", 496000, 0x7f),
	WL2868C_LDO(3, "vin34", 1504000, 0xff),
	WL2868C_LDO(4, "vin34", 1504000, 0xff),
	WL2868C_LDO(5, "vin5", 1504000, 0xff),
	WL2868C_LDO(6, "vin6", 1504000, 0xff),
	WL2868C_LDO(7, "vin7", 1504000, 0xff),
};

static const struct regmap_config wl2868c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x25,
};

static void wl2868c_reset(void *data)
{
	gpiod_set_value_cansleep(data, 1);
}

static int wl2868c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	struct gpio_desc *reset;
	unsigned int id;
	int ret, i;

	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "Failed to get reset GPIO\n");

	if (reset) {
		ret = devm_add_action_or_reset(dev, wl2868c_reset, reset);
		if (ret)
			return ret;
		fsleep(1000);
	}

	config.regmap = devm_regmap_init_i2c(client, &wl2868c_regmap_config);
	if (IS_ERR(config.regmap))
		return dev_err_probe(dev, PTR_ERR(config.regmap), "Failed to create regmap\n");

	ret = regmap_read(config.regmap, WL2868C_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read device ID\n");
	if (id != 0x82)
		return dev_err_probe(dev, -ENODEV, "Unexpected device ID: %#x\n", id);

	for (i = 0; i < ARRAY_SIZE(wl2868c_regulators); i++) {
		rdev = devm_regulator_register(dev, &wl2868c_regulators[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev),
					     "Failed to register LDO%d\n", i + 1);
	}

	return 0;
}

static const struct of_device_id wl2868c_of_match[] = {
	{ .compatible = "willsemi,wl2868c" },
	{ }
};
MODULE_DEVICE_TABLE(of, wl2868c_of_match);

static struct i2c_driver wl2868c_driver = {
	.driver = {
		.name = "wl2868c",
		.of_match_table = wl2868c_of_match,
	},
	.probe = wl2868c_probe,
};
module_i2c_driver(wl2868c_driver);

MODULE_DESCRIPTION("Will Semiconductor WL2868C regulator driver");
MODULE_LICENSE("GPL");
