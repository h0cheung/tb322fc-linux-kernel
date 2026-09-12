// SPDX-License-Identifier: GPL-2.0-only
/* Samsung S5KJNS, 19.2 MHz input, four-lane RAW10. */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define S5KJNS_WIDTH 4080
#define S5KJNS_HEIGHT 3072
#define S5KJNS_HTS 5840
#define S5KJNS_VTS 3184
#define S5KJNS_EXPOSURE_MAX (S5KJNS_VTS - 10)
#define S5KJNS_CODE MEDIA_BUS_FMT_SGRBG10_1X10
/* 19.2 MHz / 3 * 131 * 4 / 6, rounded to whole pixels per second. */
#define S5KJNS_PIXEL_RATE 558933333

static const s64 s5kjns_link_freq[] = { 672000000 };
static const char * const s5kjns_supply_names[] = { "dovdd", "dvdd", "avdd" };

struct s5kjns {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[ARRAY_SIZE(s5kjns_supply_names)];
	bool streaming;
};

static const char * const s5kjns_test_pattern_menu[] = {
	"Disabled",
	"Solid color",
	"Color bars",
	"Fade to grey color bars",
	"PN9",
};

static struct s5kjns *to_s5kjns(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5kjns, sd);
}

static const struct cci_reg_sequence s5kjns_reset_regs[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0000), 0x0001 },
	{ CCI_REG16(0x0000), 0x38ee },
	{ CCI_REG16(0x001e), 0x000b },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x6010), 0x0001 },
};

static const struct cci_reg_sequence s5kjns_init_regs[] = {
	{ CCI_REG16(0x6226), 0x0001 },
	{ CCI_REG16(0x6028), 0x2400 },
	{ CCI_REG16(0x602a), 0x35cc },
	{ CCI_REG16(0x6f12), 0x1c80 },
	{ CCI_REG16(0x6f12), 0x0024 },
	{ CCI_REG16(0x6f12), 0xa88c },
	{ CCI_REG16(0x6f12), 0x0024 },
	{ CCI_REG16(0x602a), 0x1354 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x7017 },
	{ CCI_REG16(0x602a), 0x13b2 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1236 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1a0a },
	{ CCI_REG16(0x6f12), 0x4c0a },
	{ CCI_REG16(0x602a), 0x2210 },
	{ CCI_REG16(0x6f12), 0x3401 },
	{ CCI_REG16(0x602a), 0x2176 },
	{ CCI_REG16(0x6f12), 0x6400 },
	{ CCI_REG16(0x602a), 0x222e },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x06b6 },
	{ CCI_REG16(0x6f12), 0x0a00 },
	{ CCI_REG16(0x602a), 0x06bc },
	{ CCI_REG16(0x6f12), 0x1001 },
	{ CCI_REG16(0x602a), 0x2140 },
	{ CCI_REG16(0x6f12), 0x0101 },
	{ CCI_REG16(0x602a), 0x1a0e },
	{ CCI_REG16(0x6f12), 0x9600 },
	{ CCI_REG16(0x602a), 0x19fc },
	{ CCI_REG16(0x6f12), 0x0b00 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0xf44e), 0x0011 },
	{ CCI_REG16(0xf44c), 0x0b0b },
	{ CCI_REG16(0xf44a), 0x0006 },
	{ CCI_REG16(0x0118), 0x0002 },
	{ CCI_REG16(0x011a), 0x0001 },
	{ CCI_REG16(0x0fea), 0x1940 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0xb13a), 0x4000 },
	{ CCI_REG16(0xb134), 0x0040 },
};

static const struct cci_reg_sequence s5kjns_4080x3072_regs[] = {
	{ CCI_REG16(0x6028), 0x2400 },
	{ CCI_REG16(0x602a), 0x1a28 },
	{ CCI_REG16(0x6f12), 0x4c00 },
	{ CCI_REG16(0x602a), 0x065a },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x139e },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x139c },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x13a0 },
	{ CCI_REG16(0x6f12), 0x0a00 },
	{ CCI_REG16(0x6f12), 0x0120 },
	{ CCI_REG16(0x602a), 0x2072 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1a64 },
	{ CCI_REG16(0x6f12), 0x0301 },
	{ CCI_REG16(0x6f12), 0xff00 },
	{ CCI_REG16(0x602a), 0x19e6 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x1a30 },
	{ CCI_REG16(0x6f12), 0x3401 },
	{ CCI_REG16(0x602a), 0x19f4 },
	{ CCI_REG16(0x6f12), 0x0606 },
	{ CCI_REG16(0x602a), 0x19f8 },
	{ CCI_REG16(0x6f12), 0x1010 },
	{ CCI_REG16(0x602a), 0x1b26 },
	{ CCI_REG16(0x6f12), 0x6f80 },
	{ CCI_REG16(0x6f12), 0xa060 },
	{ CCI_REG16(0x602a), 0x1a3c },
	{ CCI_REG16(0x6f12), 0x6207 },
	{ CCI_REG16(0x602a), 0x1a48 },
	{ CCI_REG16(0x6f12), 0x6207 },
	{ CCI_REG16(0x602a), 0x1444 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x602a), 0x144c },
	{ CCI_REG16(0x6f12), 0x3f00 },
	{ CCI_REG16(0x6f12), 0x3f00 },
	{ CCI_REG16(0x602a), 0x7f6c },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x2f00 },
	{ CCI_REG16(0x6f12), 0xfa00 },
	{ CCI_REG16(0x6f12), 0x2400 },
	{ CCI_REG16(0x6f12), 0xe500 },
	{ CCI_REG16(0x602a), 0x0650 },
	{ CCI_REG16(0x6f12), 0x0600 },
	{ CCI_REG16(0x602a), 0x0654 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1a46 },
	{ CCI_REG16(0x6f12), 0x8a00 },
	{ CCI_REG16(0x602a), 0x1a52 },
	{ CCI_REG16(0x6f12), 0xbf00 },
	{ CCI_REG16(0x602a), 0x0674 },
	{ CCI_REG16(0x6f12), 0x0500 },
	{ CCI_REG16(0x6f12), 0x0500 },
	{ CCI_REG16(0x6f12), 0x0500 },
	{ CCI_REG16(0x6f12), 0x0500 },
	{ CCI_REG16(0x602a), 0x0668 },
	{ CCI_REG16(0x6f12), 0x0800 },
	{ CCI_REG16(0x6f12), 0x0800 },
	{ CCI_REG16(0x6f12), 0x0800 },
	{ CCI_REG16(0x6f12), 0x0800 },
	{ CCI_REG16(0x602a), 0x0684 },
	{ CCI_REG16(0x6f12), 0x4001 },
	{ CCI_REG16(0x602a), 0x0688 },
	{ CCI_REG16(0x6f12), 0x4001 },
	{ CCI_REG16(0x602a), 0x147c },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x1480 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x19f6 },
	{ CCI_REG16(0x6f12), 0x0904 },
	{ CCI_REG16(0x602a), 0x0812 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1a02 },
	{ CCI_REG16(0x6f12), 0x1800 },
	{ CCI_REG16(0x602a), 0x2104 },
	{ CCI_REG16(0x6f12), 0x0018 },
	{ CCI_REG16(0x602a), 0x2148 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x2042 },
	{ CCI_REG16(0x6f12), 0x1a00 },
	{ CCI_REG16(0x602a), 0x0874 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x09c0 },
	{ CCI_REG16(0x6f12), 0x2008 },
	{ CCI_REG16(0x602a), 0x09c4 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x602a), 0x19fe },
	{ CCI_REG16(0x6f12), 0x0e1c },
	{ CCI_REG16(0x602a), 0x4d92 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x8a88 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x4d94 },
	{ CCI_REG16(0x6f12), 0x0005 },
	{ CCI_REG16(0x6f12), 0x000a },
	{ CCI_REG16(0x6f12), 0x0010 },
	{ CCI_REG16(0x6f12), 0x0810 },
	{ CCI_REG16(0x6f12), 0x000a },
	{ CCI_REG16(0x6f12), 0x0040 },
	{ CCI_REG16(0x6f12), 0x0810 },
	{ CCI_REG16(0x6f12), 0x0810 },
	{ CCI_REG16(0x6f12), 0x8002 },
	{ CCI_REG16(0x6f12), 0xfd03 },
	{ CCI_REG16(0x6f12), 0x0010 },
	{ CCI_REG16(0x6f12), 0x1510 },
	{ CCI_REG16(0x602a), 0x3570 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x3574 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x21e4 },
	{ CCI_REG16(0x6f12), 0x0400 },
	{ CCI_REG16(0x602a), 0x21ec },
	{ CCI_REG16(0x6f12), 0x5000 },
	{ CCI_REG16(0x602a), 0x2080 },
	{ CCI_REG16(0x6f12), 0x0101 },
	{ CCI_REG16(0x6f12), 0xff00 },
	{ CCI_REG16(0x6f12), 0x7f01 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x8001 },
	{ CCI_REG16(0x6f12), 0xd244 },
	{ CCI_REG16(0x6f12), 0xd244 },
	{ CCI_REG16(0x6f12), 0x14f4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x20ba },
	{ CCI_REG16(0x6f12), 0x121c },
	{ CCI_REG16(0x6f12), 0x111c },
	{ CCI_REG16(0x6f12), 0x54f4 },
	{ CCI_REG16(0x602a), 0x120e },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x212e },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x13ae },
	{ CCI_REG16(0x6f12), 0x0101 },
	{ CCI_REG16(0x602a), 0x0718 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x0710 },
	{ CCI_REG16(0x6f12), 0x0002 },
	{ CCI_REG16(0x6f12), 0x0804 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x1b5c },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x0786 },
	{ CCI_REG16(0x6f12), 0x7701 },
	{ CCI_REG16(0x602a), 0x2022 },
	{ CCI_REG16(0x6f12), 0x0500 },
	{ CCI_REG16(0x6f12), 0x0500 },
	{ CCI_REG16(0x602a), 0x1360 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x1376 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x6f12), 0x6038 },
	{ CCI_REG16(0x6f12), 0x7038 },
	{ CCI_REG16(0x6f12), 0x8038 },
	{ CCI_REG16(0x602a), 0x1386 },
	{ CCI_REG16(0x6f12), 0x0b00 },
	{ CCI_REG16(0x602a), 0x06fa },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x4a94 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x0a76 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x0aee },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x0b66 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x0bde },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x0be8 },
	{ CCI_REG16(0x6f12), 0x3000 },
	{ CCI_REG16(0x6f12), 0x3000 },
	{ CCI_REG16(0x602a), 0x0c56 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x602a), 0x0c60 },
	{ CCI_REG16(0x6f12), 0x3000 },
	{ CCI_REG16(0x6f12), 0x3000 },
	{ CCI_REG16(0x602a), 0x0cb6 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x0cf2 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x0cf0 },
	{ CCI_REG16(0x6f12), 0x0101 },
	{ CCI_REG16(0x602a), 0x11b8 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x11f6 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x4a74 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x218e },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x2268 },
	{ CCI_REG16(0x6f12), 0xf279 },
	{ CCI_REG16(0x602a), 0x5006 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x500e },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x4e70 },
	{ CCI_REG16(0x6f12), 0x2062 },
	{ CCI_REG16(0x6f12), 0x5501 },
	{ CCI_REG16(0x602a), 0x06dc },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0xf46a), 0xae80 },
	{ CCI_REG16(0x0344), 0x0000 },
	{ CCI_REG16(0x0346), 0x0000 },
	{ CCI_REG16(0x0348), 0x1fff },
	{ CCI_REG16(0x034a), 0x181f },
	{ CCI_REG16(0x034c), 0x0ff0 },
	{ CCI_REG16(0x034e), 0x0c00 },
	{ CCI_REG16(0x0350), 0x0008 },
	{ CCI_REG16(0x0352), 0x0008 },
	{ CCI_REG16(0x0900), 0x0122 },
	{ CCI_REG16(0x0380), 0x0002 },
	{ CCI_REG16(0x0382), 0x0002 },
	{ CCI_REG16(0x0384), 0x0002 },
	{ CCI_REG16(0x0386), 0x0002 },
	{ CCI_REG16(0x0110), 0x1002 },
	{ CCI_REG16(0x0114), 0x0301 },
	{ CCI_REG16(0x0116), 0x3000 },
	{ CCI_REG16(0x0136), 0x1300 },
	{ CCI_REG16(0x013e), 0x00c8 },
	{ CCI_REG16(0x0300), 0x0006 },
	{ CCI_REG16(0x0302), 0x0001 },
	{ CCI_REG16(0x0304), 0x0003 },
	{ CCI_REG16(0x0306), 0x0083 },
	{ CCI_REG16(0x0308), 0x0008 },
	{ CCI_REG16(0x030a), 0x0001 },
	{ CCI_REG16(0x030c), 0x0000 },
	{ CCI_REG16(0x030e), 0x0003 },
	{ CCI_REG16(0x0310), 0x0069 },
	{ CCI_REG16(0x0312), 0x0000 },
	{ CCI_REG16(0x080e), 0x0000 },
	{ CCI_REG16(0x0340), 0x0c70 },
	{ CCI_REG16(0x0342), 0x16d0 },
	{ CCI_REG16(0x0702), 0x0000 },
	{ CCI_REG16(0x0202), 0x0100 },
	{ CCI_REG16(0x0200), 0x0100 },
	{ CCI_REG16(0x0d00), 0x0101 },
	{ CCI_REG16(0x0d02), 0x0101 },
	{ CCI_REG16(0x0d04), 0x0102 },
	{ CCI_REG16(0x6226), 0x0000 },
};

static int s5kjns_power_on(struct device *dev)
{
	struct s5kjns *sensor = to_s5kjns(dev_get_drvdata(dev));
	int i, ret;

	gpiod_set_value_cansleep(sensor->reset, 1);
	fsleep(1000);
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++) {
		ret = regulator_enable(sensor->supplies[i].consumer);
		if (ret)
			goto disable_supplies;
		fsleep(1000);
	}

	gpiod_set_value_cansleep(sensor->reset, 0);
	fsleep(1000);
	ret = clk_prepare_enable(sensor->xclk);
	if (ret) {
		gpiod_set_value_cansleep(sensor->reset, 1);
		fsleep(1000);
		goto disable_supplies;
	}
	fsleep(15000);
	return 0;

disable_supplies:
	while (i--) {
		regulator_disable(sensor->supplies[i].consumer);
		fsleep(1000);
	}
	return ret;
}

static int s5kjns_power_off(struct device *dev)
{
	struct s5kjns *sensor = to_s5kjns(dev_get_drvdata(dev));
	int i;

	clk_disable_unprepare(sensor->xclk);
	fsleep(10000);
	gpiod_set_value_cansleep(sensor->reset, 1);
	fsleep(1000);
	for (i = ARRAY_SIZE(sensor->supplies) - 1; i >= 0; i--) {
		regulator_disable(sensor->supplies[i].consumer);
		fsleep(1000);
	}
	return 0;
}

static int s5kjns_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5kjns *sensor = container_of(ctrl->handler, struct s5kjns, ctrls);
	struct device *dev = sensor->sd.dev;
	u16 reg;
	int ret = 0, release;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		reg = 0x0202;
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		reg = 0x0204;
		break;
	case V4L2_CID_TEST_PATTERN:
		reg = 0x0601;
		break;
	default:
		return 0;
	}

	ret = pm_runtime_get_if_in_use(dev);
	if (ret <= 0)
		return ret;
	ret = 0;

	cci_write(sensor->regmap, CCI_REG16(0x6028), 0x4000, &ret);
	if (ctrl->id == V4L2_CID_TEST_PATTERN) {
		cci_write(sensor->regmap, CCI_REG8(reg), ctrl->val, &ret);
		pm_runtime_put(dev);
		return ret;
	}

	cci_write(sensor->regmap, CCI_REG8(0x0104), 1, &ret);
	cci_write(sensor->regmap, CCI_REG8(reg), ctrl->val >> 8, &ret);
	cci_write(sensor->regmap, CCI_REG8(reg + 1), ctrl->val & 0xff, &ret);
	/* Release group hold even when a control write failed. */
	release = cci_write(sensor->regmap, CCI_REG8(0x0104), 0, NULL);
	pm_runtime_put(dev);
	return ret ? ret : release;
}

static const struct v4l2_ctrl_ops s5kjns_ctrl_ops = {
	.s_ctrl = s5kjns_set_ctrl,
};

static int s5kjns_init_controls(struct s5kjns *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrls;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_fwnode_device_parse(sensor->sd.dev, &props);
	if (ret)
		return ret;
	v4l2_ctrl_handler_init(hdl, 9);
	ctrl = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				      s5kjns_link_freq);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE,
			  S5KJNS_PIXEL_RATE, S5KJNS_PIXEL_RATE, 1, S5KJNS_PIXEL_RATE);
	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK,
				 S5KJNS_HTS - S5KJNS_WIDTH, S5KJNS_HTS - S5KJNS_WIDTH,
				 1, S5KJNS_HTS - S5KJNS_WIDTH);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_VBLANK,
				 S5KJNS_VTS - S5KJNS_HEIGHT, S5KJNS_VTS - S5KJNS_HEIGHT,
				 1, S5KJNS_VTS - S5KJNS_HEIGHT);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(hdl, &s5kjns_ctrl_ops, V4L2_CID_EXPOSURE,
			  2, S5KJNS_EXPOSURE_MAX, 1, 0x0100);
	v4l2_ctrl_new_std(hdl, &s5kjns_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  32, 2048, 1, 32);
	v4l2_ctrl_new_std_menu_items(hdl, &s5kjns_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5kjns_test_pattern_menu) - 1,
				     0, 0, s5kjns_test_pattern_menu);
	v4l2_ctrl_new_fwnode_properties(hdl, &s5kjns_ctrl_ops, &props);
	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}
	sensor->sd.ctrl_handler = hdl;
	return 0;
}

static int s5kjns_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct s5kjns *sensor = to_s5kjns(sd);

	if (format->pad)
		return -EINVAL;
	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE && sensor->streaming)
		return -EBUSY;

	format->format = (struct v4l2_mbus_framefmt) {
		.width = S5KJNS_WIDTH,
		.height = S5KJNS_HEIGHT,
		.code = S5KJNS_CODE,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_RAW,
		.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
		.xfer_func = V4L2_XFER_FUNC_NONE,
	};
	*v4l2_subdev_state_get_format(state, 0) = format->format;
	return 0;
}

static int s5kjns_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format format = { .which = V4L2_SUBDEV_FORMAT_TRY };

	return s5kjns_set_fmt(sd, state, &format);
}

static int s5kjns_enum_code(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = S5KJNS_CODE;
	return 0;
}

static int s5kjns_enum_size(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_frame_size_enum *size)
{
	if (size->pad || size->index || size->code != S5KJNS_CODE)
		return -EINVAL;
	size->min_width = S5KJNS_WIDTH;
	size->max_width = S5KJNS_WIDTH;
	size->min_height = S5KJNS_HEIGHT;
	size->max_height = S5KJNS_HEIGHT;
	return 0;
}

static int s5kjns_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	if (sel->pad)
		return -EINVAL;
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r = (struct v4l2_rect) { .width = 8192, .height = 6176 };
		return 0;
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		/* Two-by-two binning followed by an eight-pixel output crop. */
		sel->r = (struct v4l2_rect) {
			.left = 16,
			.top = 16,
			.width = S5KJNS_WIDTH * 2,
			.height = S5KJNS_HEIGHT * 2,
		};
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5kjns_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct s5kjns *sensor = to_s5kjns(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (sensor->streaming == !!enable)
		goto unlock;
	if (!enable) {
		ret = cci_write(sensor->regmap, CCI_REG8(0x0100), 0, NULL);
		sensor->streaming = false;
		/* Reset and remove power even if the stream-off write failed. */
		pm_runtime_put_sync(sd->dev);
		goto unlock;
	}

	ret = pm_runtime_resume_and_get(sd->dev);
	if (ret < 0)
		goto unlock;
	cci_multi_reg_write(sensor->regmap, s5kjns_reset_regs,
			    ARRAY_SIZE(s5kjns_reset_regs), &ret);
	if (ret)
		goto power_off;
	fsleep(13000);
	cci_multi_reg_write(sensor->regmap, s5kjns_init_regs,
			    ARRAY_SIZE(s5kjns_init_regs), &ret);
	cci_multi_reg_write(sensor->regmap, s5kjns_4080x3072_regs,
			    ARRAY_SIZE(s5kjns_4080x3072_regs), &ret);
	if (!ret)
		ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	cci_write(sensor->regmap, CCI_REG16(0x6028), 0x4000, &ret);
	cci_write(sensor->regmap, CCI_REG16(0x0100), 0x0100, &ret);
power_off:
	if (ret)
		pm_runtime_put_sync(sd->dev);
	else
		sensor->streaming = true;
unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static const struct v4l2_subdev_video_ops s5kjns_video_ops = {
	.s_stream = s5kjns_s_stream,
};

static const struct v4l2_subdev_pad_ops s5kjns_pad_ops = {
	.enum_mbus_code = s5kjns_enum_code,
	.enum_frame_size = s5kjns_enum_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5kjns_set_fmt,
	.get_selection = s5kjns_get_selection,
};

static const struct v4l2_subdev_ops s5kjns_ops = {
	.video = &s5kjns_video_ops,
	.pad = &s5kjns_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5kjns_internal_ops = {
	.init_state = s5kjns_init_state,
};

static int s5kjns_check_endpoint(struct device *dev)
{
	struct v4l2_fwnode_endpoint bus = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	unsigned long bitmap;
	int ret, i;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!ep)
		return -EINVAL;
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus);
	fwnode_handle_put(ep);
	if (ret)
		goto free;
	ret = -EINVAL;
	if (bus.bus.mipi_csi2.num_data_lanes != 4)
		goto free;
	for (i = 0; i < 4; i++)
		if (bus.bus.mipi_csi2.data_lanes[i] != i + 1)
			goto free;
	ret = v4l2_link_freq_to_bitmap(dev, bus.link_frequencies,
				       bus.nr_of_link_frequencies, s5kjns_link_freq,
				       ARRAY_SIZE(s5kjns_link_freq), &bitmap);
free:
	v4l2_fwnode_endpoint_free(&bus);
	return ret;
}

static int s5kjns_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5kjns *sensor;
	u64 id;
	int ret, i;

	ret = s5kjns_check_endpoint(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Unsupported CSI-2 endpoint\n");
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap), "Failed to create regmap\n");
	sensor->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk), "Failed to get sensor clock\n");
	if (clk_get_rate(sensor->xclk) != 19200000)
		return dev_err_probe(dev, -EINVAL, "Only a 19.2 MHz input clock is supported\n");
	for (i = 0; i < ARRAY_SIZE(sensor->supplies); i++)
		sensor->supplies[i].supply = s5kjns_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies), sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");
	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset), "Failed to get reset GPIO\n");

	v4l2_i2c_subdev_init(&sensor->sd, client, &s5kjns_ops);
	sensor->sd.internal_ops = &s5kjns_internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = s5kjns_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to power sensor\n");
	ret = cci_read(sensor->regmap, CCI_REG16(0x0000), &id, NULL);
	s5kjns_power_off(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read sensor ID\n");
	if (id != 0x38ee)
		return dev_err_probe(dev, -ENODEV, "Unexpected sensor ID: %#llx\n", id);

	ret = s5kjns_init_controls(sensor);
	if (ret)
		return ret;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto free_controls;
	sensor->sd.state_lock = sensor->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto clean_entity;

	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto disable_pm;
	return 0;

disable_pm:
	pm_runtime_disable(dev);
	v4l2_subdev_cleanup(&sensor->sd);
clean_entity:
	media_entity_cleanup(&sensor->sd.entity);
free_controls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
	return ret;
}

static void s5kjns_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5kjns *sensor = to_s5kjns(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(sd->dev);
	if (!pm_runtime_status_suspended(sd->dev))
		s5kjns_power_off(sd->dev);
	pm_runtime_set_suspended(sd->dev);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);
}

static int s5kjns_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5kjns *sensor = to_s5kjns(sd);
	struct v4l2_subdev_state *state;
	int ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	ret = sensor->streaming ? -EBUSY : pm_runtime_force_suspend(dev);
	v4l2_subdev_unlock_state(state);
	return ret;
}

static const struct dev_pm_ops s5kjns_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(s5kjns_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(s5kjns_power_off, s5kjns_power_on, NULL)
};

static const struct of_device_id s5kjns_of_match[] = {
	{ .compatible = "samsung,s5kjns" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5kjns_of_match);

static struct i2c_driver s5kjns_driver = {
	.driver = {
		.name = "s5kjns",
		.of_match_table = s5kjns_of_match,
		.pm = pm_ptr(&s5kjns_pm_ops),
	},
	.probe = s5kjns_probe,
	.remove = s5kjns_remove,
};
module_i2c_driver(s5kjns_driver);

MODULE_DESCRIPTION("Samsung S5KJNS image sensor driver");
MODULE_LICENSE("GPL");
