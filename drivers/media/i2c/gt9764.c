// SPDX-License-Identifier: GPL-2.0-only
/* Giantec GT9764 camera lens actuator. */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define GT9764_CONTROL		0x02
#define GT9764_POSITION		0x03
#define GT9764_AAC_MODE		0x06
#define GT9764_AAC_TIMING		0x07
#define GT9764_POSITION_MAX	1023
#define GT9764_POSITION_INIT	512

struct gt9764 {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	struct regulator_bulk_data supplies[2];
	u32 aac_mode;
	u32 aac_timing;
	u32 settling_time;
};

static struct gt9764 *to_gt9764(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gt9764, sd);
}

static int gt9764_set_position(struct gt9764 *lens, u16 position)
{
	struct i2c_client *client = v4l2_get_subdevdata(&lens->sd);

	int ret;

	ret = i2c_smbus_write_word_swapped(client, GT9764_POSITION, position);
	if (!ret)
		fsleep(lens->settling_time);
	return ret;
}

static int gt9764_power_off(struct device *dev)
{
	struct gt9764 *lens = to_gt9764(dev_get_drvdata(dev));
	int ret;

	ret = regulator_bulk_disable(ARRAY_SIZE(lens->supplies), lens->supplies);
	/* AN971 requires at least 100 ms off before the next power-up. */
	fsleep(100000);
	return ret;
}

static int gt9764_power_on(struct device *dev)
{
	struct gt9764 *lens = to_gt9764(dev_get_drvdata(dev));
	struct i2c_client *client = v4l2_get_subdevdata(&lens->sd);
	const struct {
		u8 reg;
		u8 value;
	} init[] = {
		{ GT9764_CONTROL, 0x02 },
		{ GT9764_POSITION, GT9764_POSITION_INIT >> 8 },
		{ GT9764_POSITION + 1, GT9764_POSITION_INIT & 0xff },
		{ GT9764_AAC_MODE, lens->aac_mode },
		{ GT9764_AAC_TIMING, lens->aac_timing },
	};
	unsigned int i;
	int ret;

	ret = regulator_enable(lens->supplies[0].consumer);
	if (ret)
		return ret;
	ret = regulator_enable(lens->supplies[1].consumer);
	if (ret) {
		regulator_disable(lens->supplies[0].consumer);
		return ret;
	}
	fsleep(10000);

	for (i = 0; i < ARRAY_SIZE(init); i++) {
		ret = i2c_smbus_write_byte_data(client, init[i].reg, init[i].value);
		if (ret)
			goto power_off;
		fsleep(100);
	}

	fsleep(lens->settling_time);
	if (lens->focus->cur.val == GT9764_POSITION_INIT)
		return 0;
	ret = gt9764_set_position(lens, lens->focus->cur.val);
	if (!ret)
		return 0;

power_off:
	gt9764_power_off(dev);
	return ret;
}

static int gt9764_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gt9764 *lens = container_of(ctrl->handler, struct gt9764, ctrls);
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;
	ret = pm_runtime_get_if_in_use(lens->sd.dev);
	if (ret <= 0)
		return ret;

	ret = gt9764_set_position(lens, ctrl->val);
	pm_runtime_put(lens->sd.dev);
	return ret;
}

static const struct v4l2_ctrl_ops gt9764_ctrl_ops = {
	.s_ctrl = gt9764_set_ctrl,
};

static int gt9764_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct gt9764 *lens = to_gt9764(sd);
	int ret;

	mutex_lock(lens->ctrls.lock);
	ret = pm_runtime_resume_and_get(sd->dev);
	mutex_unlock(lens->ctrls.lock);
	return ret;
}

static int gt9764_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct gt9764 *lens = to_gt9764(sd);
	int ret;

	mutex_lock(lens->ctrls.lock);
	ret = pm_runtime_put_sync(sd->dev);
	mutex_unlock(lens->ctrls.lock);
	return ret < 0 ? ret : 0;
}

static const struct v4l2_subdev_internal_ops gt9764_internal_ops = {
	.open = gt9764_open,
	.close = gt9764_close,
};

static const struct v4l2_subdev_ops gt9764_subdev_ops = { };

static int gt9764_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gt9764 *lens;
	int ret;

	lens = devm_kzalloc(dev, sizeof(*lens), GFP_KERNEL);
	if (!lens)
		return -ENOMEM;

	ret = device_property_read_u32(dev, "giantec,aac-mode", &lens->aac_mode);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "giantec,aac-timing", &lens->aac_timing);
	if (ret)
		return ret;
	if (lens->aac_mode > 0xff || lens->aac_timing > 0xff)
		return -EINVAL;

	ret = device_property_read_u32(dev, "settling-time-us", &lens->settling_time);
	if (ret)
		return ret;
	if (!lens->settling_time)
		return -EINVAL;

	lens->supplies[0].supply = "vddio";
	lens->supplies[1].supply = "vdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(lens->supplies), lens->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");

	v4l2_i2c_subdev_init(&lens->sd, client, &gt9764_subdev_ops);
	lens->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	lens->sd.internal_ops = &gt9764_internal_ops;
	lens->sd.entity.function = MEDIA_ENT_F_LENS;
	v4l2_ctrl_handler_init(&lens->ctrls, 1);
	lens->focus = v4l2_ctrl_new_std(&lens->ctrls, &gt9764_ctrl_ops,
					V4L2_CID_FOCUS_ABSOLUTE, 0,
					GT9764_POSITION_MAX, 1, GT9764_POSITION_INIT);
	ret = lens->ctrls.error;
	if (ret)
		goto free_ctrls;
	lens->sd.ctrl_handler = &lens->ctrls;

	ret = media_entity_pads_init(&lens->sd.entity, 0, NULL);
	if (ret)
		goto free_ctrls;

	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev(&lens->sd);
	if (ret) {
		pm_runtime_disable(dev);
		media_entity_cleanup(&lens->sd.entity);
		goto free_ctrls;
	}
	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(&lens->ctrls);
	return ret;
}

static void gt9764_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gt9764 *lens = to_gt9764(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(sd->dev);
	if (!pm_runtime_status_suspended(sd->dev))
		gt9764_power_off(sd->dev);
	pm_runtime_set_suspended(sd->dev);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&lens->ctrls);
}

static const struct dev_pm_ops gt9764_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(gt9764_power_off, gt9764_power_on, NULL)
};

static const struct of_device_id gt9764_of_match[] = {
	{ .compatible = "giantec,gt9764" },
	{ }
};
MODULE_DEVICE_TABLE(of, gt9764_of_match);

static struct i2c_driver gt9764_driver = {
	.driver = {
		.name = "gt9764",
		.of_match_table = gt9764_of_match,
		.pm = pm_ptr(&gt9764_pm_ops),
	},
	.probe = gt9764_probe,
	.remove = gt9764_remove,
};
module_i2c_driver(gt9764_driver);

MODULE_DESCRIPTION("Giantec GT9764 lens actuator driver");
MODULE_LICENSE("GPL");
