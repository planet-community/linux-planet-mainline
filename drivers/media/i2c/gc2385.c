#define DEBUG
// SPDX-License-Identifier: GPL-2.0
/*
 * gc2385 driver
 *
 * Copyright (C) 2017 Fuzhou Rockchip Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#define GC2385_XVCLK_FREQ		24000000
#define GC2385_LANES			1
#define GC2385_BITS_PER_SAMPLE		10

#define CHIP_ID				0x2385
#define GC2385_REG_CHIP_ID_H		0xf0
#define GC2385_REG_CHIP_ID_L		0xf1

#define GC2385_REG_SET_PAGE		0xfe
#define GC2385_SET_PAGE_ONE		0x00

#define GC2385_REG_CTRL_MODE		0xed
#define GC2385_MODE_SW_STANDBY		0x00
#define GC2385_MODE_STREAMING		0x90

#define GC2385_REG_EXPOSURE_H		0x03
#define GC2385_REG_EXPOSURE_L		0x04
#define	GC2385_EXPOSURE_MIN		4
#define	GC2385_EXPOSURE_STEP		1
#define GC2385_VTS_MAX			0x1fff

#define GC2385_REG_AGAIN		0xb6
#define GC2385_REG_DGAIN_INT		0xb1
#define GC2385_REG_DGAIN_FRAC		0xb2
#define GC2385_AGAIN_MIN		0
#define GC2385_AGAIN_MAX		8
#define GC2385_AGAIN_STEP		1
#define GC2385_AGAIN_DEFAULT		0

#define GC2385_REG_VTS_H		0x07
#define GC2385_REG_VTS_L		0x08

#define REG_NULL			0xFF

static const char * const gc2385_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define GC2385_NUM_SUPPLIES ARRAY_SIZE(gc2385_supply_names)

struct regval {
	u8 addr;
	u8 val;
};

struct gc2385_mode {
	u32 width;
	u32 height;
	u32 exp_def;
	u32 hts_def;
	u32 vts_def;
	const struct regval *reg_list;
};

struct gc2385 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*pwdn_gpio;
	struct gpio_desc	*reset_gpio;
	struct regulator_bulk_data supplies[GC2385_NUM_SUPPLIES];

	bool			streaming;
	struct mutex		mutex;
	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl	*gain;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl_handler ctrl_handler;

	const struct gc2385_mode *cur_mode;
};

#define to_gc2385(sd) container_of(sd, struct gc2385, subdev)

/* PLL settings bases on 24M xvclk */
static const struct regval gc2385_1600x1200_regs[] = {
	{0xfe, 0x00},
	{0xfe, 0x00},
	{0xfe, 0x00},
	{0xf2, 0x02},
	{0xf4, 0x03},
	{0xf7, 0x01},
	{0xf8, 0x28},
	{0xf9, 0x02},
	{0xfa, 0x08},
	{0xfc, 0x8e},
	{0xe7, 0xcc},
	{0x88, 0x03},
	{0x03, 0x04},
	{0x04, 0x80},
	{0x05, 0x02},
	{0x06, 0x86},
	{0x07, 0x00},
	{0x08, 0x10},
	{0x09, 0x00},
	{0x0a, 0x04},
	{0x0b, 0x00},
	{0x0c, 0x02},
	{0x17, 0xd4},
	{0x18, 0x02},
	{0x19, 0x17},
	{0x1c, 0x18},
	{0x20, 0x73},
	{0x21, 0x38},
	{0x22, 0xa2},
	{0x29, 0x20},
	{0x2f, 0x14},
	{0x3f, 0x40},
	{0xcd, 0x94},
	{0xce, 0x45},
	{0xd1, 0x0c},
	{0xd7, 0x9b},
	{0xd8, 0x99},
	{0xda, 0x3b},
	{0xd9, 0xb5},
	{0xdb, 0x75},
	{0xe3, 0x1b},
	{0xe4, 0xf8},
	{0x40, 0x22},
	{0x43, 0x07},
	{0x4e, 0x3c},
	{0x4f, 0x00},
	{0x68, 0x00},
	{0xb0, 0x46},
	{0xb1, 0x01},
	{0xb2, 0x00},
	{0xb6, 0x00},
	{0x90, 0x01},
	{0x92, 0x03},
	{0x94, 0x05},
	{0x95, 0x04},
	{0x96, 0xb0},
	{0x97, 0x06},
	{0x98, 0x40},
	{0xfe, 0x00},
	{0xed, 0x00},
	{0xfe, 0x03},
	{0x01, 0x03},
	{0x02, 0x82},
	{0x03, 0xd0},
	{0x04, 0x04},
	{0x05, 0x00},
	{0x06, 0x80},
	{0x11, 0x2b},
	{0x12, 0xd0},
	{0x13, 0x07},
	{0x15, 0x00},
	{0x1b, 0x10},
	{0x1c, 0x10},
	{0x21, 0x08},
	{0x22, 0x05},
	{0x23, 0x13},
	{0x24, 0x02},
	{0x25, 0x13},
	{0x26, 0x06},
	{0x29, 0x06},
	{0x2a, 0x08},
	{0x2b, 0x06},
	{0xfe, 0x00},
	{REG_NULL, 0x00}
};

#define GC2385_LINK_FREQ_328MHZ		328000000
static const s64 link_freq_menu_items[] = {
	GC2385_LINK_FREQ_328MHZ
};

static const struct gc2385_mode supported_modes[] = {
	{
		.width = 1600,
		.height = 1200,
		.exp_def = 0x0480,
		.hts_def = 0x10dc,
		.vts_def = 0x04e0,
		.reg_list = gc2385_1600x1200_regs,
	},
};

/* Write one register */
static int gc2385_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[2];

	buf[0] = reg;
	buf[1] = val;

	if (i2c_master_send(client, buf, 2) != 2)
		return -EIO;

	return 0;
}

static int gc2385_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	int ret = 0;
	u32 i;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
		ret = gc2385_write_reg(client, regs[i].addr, regs[i].val);

	return ret;
}

/* Read registers up to 4 at a time */
static int gc2385_read_reg(struct i2c_client *client, u8 reg, u8 *val)
{
	struct i2c_msg msgs[2];
	int ret;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = (u8 *)&reg;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = val;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

static void gc2385_fill_fmt(const struct gc2385_mode *mode,
			    struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
}

static int gc2385_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct gc2385 *gc2385 = to_gc2385(sd);
	struct v4l2_mbus_framefmt *mbus_fmt = &fmt->format;

	/* only one mode supported for now */
	gc2385_fill_fmt(gc2385->cur_mode, mbus_fmt);

	return 0;
}

static int gc2385_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct gc2385 *gc2385 = to_gc2385(sd);
	struct v4l2_mbus_framefmt *mbus_fmt = &fmt->format;

	gc2385_fill_fmt(gc2385->cur_mode, mbus_fmt);

	return 0;
}

static int gc2385_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	return 0;
}

static int gc2385_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	int index = fse->index;

	if (index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	fse->code = MEDIA_BUS_FMT_SBGGR10_1X10;

	fse->min_width  = supported_modes[index].width;
	fse->max_width  = supported_modes[index].width;
	fse->max_height = supported_modes[index].height;
	fse->min_height = supported_modes[index].height;

	return 0;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 gc2385_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, GC2385_XVCLK_FREQ / 1000 / 1000);
}

static int __gc2385_power_on(struct gc2385 *gc2385)
{
	int ret;
	u32 delay_us;
	struct device *dev = &gc2385->client->dev;

	dev_dbg(dev, "%s\n", __func__);

	ret = clk_prepare_enable(gc2385->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}

	gpiod_set_value_cansleep(gc2385->reset_gpio, 1);

	ret = regulator_bulk_enable(GC2385_NUM_SUPPLIES, gc2385->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	/* The minimum delay between power supplies and reset rising can be 0 */
	gpiod_set_value_cansleep(gc2385->reset_gpio, 0);
	usleep_range(500, 1000);
	gpiod_set_value_cansleep(gc2385->pwdn_gpio, 0);
	/* 8192 xvclk cycles prior to the first SCCB transaction */
	delay_us = gc2385_cal_delay(8192);
	usleep_range(delay_us, delay_us * 2);

	return 0;

disable_clk:
	clk_disable_unprepare(gc2385->xvclk);

	return ret;
}

static void __gc2385_power_off(struct gc2385 *gc2385)
{
	/* 512 xvclk cycles after the last SCCB transaction or MIPI frame end */
	u32 delay_us = gc2385_cal_delay(512);

	dev_dbg(&gc2385->client->dev, "%s\n", __func__);

	usleep_range(delay_us, delay_us * 2);
	gpiod_set_value_cansleep(gc2385->pwdn_gpio, 1);
	clk_disable_unprepare(gc2385->xvclk);
	gpiod_set_value_cansleep(gc2385->reset_gpio, 1);
	regulator_bulk_disable(GC2385_NUM_SUPPLIES, gc2385->supplies);
}

static int __gc2385_start_stream(struct gc2385 *gc2385)
{
	int ret;

	ret = gc2385_write_array(gc2385->client, gc2385->cur_mode->reg_list);
	if (ret)
		return ret;

	ret = gc2385_write_reg(gc2385->client,
			       GC2385_REG_SET_PAGE,
			       GC2385_SET_PAGE_ONE);
	if (ret)
		return ret;

	ret = gc2385_write_reg(gc2385->client,
			       GC2385_REG_CTRL_MODE,
			       GC2385_MODE_STREAMING);
	return ret;
}

static int __gc2385_stop_stream(struct gc2385 *gc2385)
{
	int ret;

	ret = gc2385_write_reg(gc2385->client,
			       GC2385_REG_SET_PAGE,
			       GC2385_SET_PAGE_ONE);
	if (ret)
		return ret;

	ret = gc2385_write_reg(gc2385->client,
			       GC2385_REG_CTRL_MODE,
			       GC2385_MODE_SW_STANDBY);
	return ret;
}

static int gc2385_s_stream(struct v4l2_subdev *sd, int on)
{
	struct gc2385 *gc2385 = to_gc2385(sd);
	struct i2c_client *client = gc2385->client;
	int ret = 0;

	mutex_lock(&gc2385->mutex);

	on = !!on;
	if (on == gc2385->streaming)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_resume_and_get(&gc2385->client->dev);
		if (ret < 0)
			goto unlock_and_return;

		ret = __v4l2_ctrl_handler_setup(&gc2385->ctrl_handler);
		if (ret) {
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
		ret = __gc2385_start_stream(gc2385);
		if (ret) {
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		ret = __gc2385_stop_stream(gc2385);
		pm_runtime_put(&gc2385->client->dev);
	}

	gc2385->streaming = on;

unlock_and_return:
	mutex_unlock(&gc2385->mutex);

	return ret;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int gc2385_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct gc2385 *gc2385 = to_gc2385(sd);
	struct v4l2_mbus_framefmt *try_fmt;

	mutex_lock(&gc2385->mutex);

	try_fmt = v4l2_subdev_get_try_format(sd, fh->state, 0);
	/* Initialize try_fmt */
	gc2385_fill_fmt(&supported_modes[0], try_fmt);

	mutex_unlock(&gc2385->mutex);

	return 0;
}
#endif

static int __maybe_unused gc2385_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gc2385 *gc2385 = to_gc2385(sd);

	return __gc2385_power_on(gc2385);
}

static int __maybe_unused gc2385_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gc2385 *gc2385 = to_gc2385(sd);

	__gc2385_power_off(gc2385);

	return 0;
}

static const struct dev_pm_ops gc2385_pm_ops = {
	SET_RUNTIME_PM_OPS(gc2385_runtime_suspend,
			   gc2385_runtime_resume, NULL)
};

static int gc2385_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc2385 *gc2385 = container_of(ctrl->handler,
					     struct gc2385, ctrl_handler);
	struct i2c_client *client = gc2385->client;
	s64 max_expo;
	int ret;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max_expo = gc2385->cur_mode->height + ctrl->val - 4;
		__v4l2_ctrl_modify_range(gc2385->exposure,
					 gc2385->exposure->minimum, max_expo,
					 gc2385->exposure->step,
					 gc2385->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	ret = gc2385_write_reg(gc2385->client,
			       GC2385_REG_SET_PAGE,
			       GC2385_SET_PAGE_ONE);
	if (ret) {
		pm_runtime_put(&client->dev);
		return ret;
	}

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = gc2385_write_reg(gc2385->client,
				       GC2385_REG_EXPOSURE_H,
				       (ctrl->val >> 8) & 0x3f);
		if (ret) break;
		ret = gc2385_write_reg(gc2385->client,
				       GC2385_REG_EXPOSURE_L,
				       ctrl->val & 0xff);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = gc2385_write_reg(gc2385->client,
				       GC2385_REG_AGAIN,
				       ((ctrl->val - 32) >> 8) & 0xff);
		break;
	case V4L2_CID_VBLANK:
		ret = gc2385_write_reg(gc2385->client,
				       GC2385_REG_VTS_H,
				       ((ctrl->val - 32) >> 8) & 0xff);
		if (ret) break;
		ret = gc2385_write_reg(gc2385->client,
				       GC2385_REG_VTS_L,
				       (ctrl->val - 32) & 0xff);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_subdev_video_ops gc2385_video_ops = {
	.s_stream = gc2385_s_stream,
};

static const struct v4l2_subdev_pad_ops gc2385_pad_ops = {
	.enum_mbus_code = gc2385_enum_mbus_code,
	.enum_frame_size = gc2385_enum_frame_sizes,
	.get_fmt = gc2385_get_fmt,
	.set_fmt = gc2385_set_fmt,
};

static const struct v4l2_subdev_ops gc2385_subdev_ops = {
	.video	= &gc2385_video_ops,
	.pad	= &gc2385_pad_ops,
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops gc2385_internal_ops = {
	.open = gc2385_open,
};
#endif

static const struct v4l2_ctrl_ops gc2385_ctrl_ops = {
	.s_ctrl = gc2385_set_ctrl,
};

static int gc2385_initialize_controls(struct gc2385 *gc2385)
{
	const struct gc2385_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	u64 exposure_max;
	u32 pixel_rate, h_blank;
	int ret;

	handler = &gc2385->ctrl_handler;
	mode = gc2385->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &gc2385->mutex;

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = (link_freq_menu_items[0] * 2 * GC2385_LANES) /
		     GC2385_BITS_PER_SAMPLE;
	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, pixel_rate, 1, pixel_rate);

	h_blank = mode->hts_def - mode->width;
	gc2385->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (gc2385->hblank)
		gc2385->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc2385->vblank = v4l2_ctrl_new_std(handler, &gc2385_ctrl_ops,
				V4L2_CID_VBLANK, mode->vts_def - mode->height,
				GC2385_VTS_MAX - mode->height, 1,
				mode->vts_def - mode->height);

	exposure_max = mode->vts_def - 4;
	gc2385->exposure = v4l2_ctrl_new_std(handler, &gc2385_ctrl_ops,
				V4L2_CID_EXPOSURE, GC2385_EXPOSURE_MIN,
				exposure_max, GC2385_EXPOSURE_STEP,
				mode->exp_def);

	gc2385->gain = v4l2_ctrl_new_std(handler, &gc2385_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, GC2385_AGAIN_MIN,
				GC2385_AGAIN_MAX, GC2385_AGAIN_STEP,
				GC2385_AGAIN_DEFAULT);

	if (handler->error) {
		ret = handler->error;
		dev_err(&gc2385->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	gc2385->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int gc2385_check_sensor_id(struct gc2385 *gc2385,
				  struct i2c_client *client)
{
	struct device *dev = &gc2385->client->dev;
	int ret;
	u8 id_l = 0, id_h = 0;
	u16 id;

	dev_dbg(dev, "%s\n", __func__);

	ret = gc2385_write_reg(gc2385->client,
			       GC2385_REG_SET_PAGE,
			       GC2385_SET_PAGE_ONE);
	ret |= gc2385_read_reg(client, GC2385_REG_CHIP_ID_H, &id_h);
	ret |= gc2385_read_reg(client, GC2385_REG_CHIP_ID_L, &id_l);
	id = (id_h << 8) | id_l;
	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%04x), ret(%d)\n", id, ret);
		return ret;
	}

	dev_info(dev, "Detected GC%04x sensor\n", CHIP_ID);

	return 0;
}

static int gc2385_configure_regulators(struct gc2385 *gc2385)
{
	int i;

	for (i = 0; i < GC2385_NUM_SUPPLIES; i++)
		gc2385->supplies[i].supply = gc2385_supply_names[i];

	return devm_regulator_bulk_get(&gc2385->client->dev,
				       GC2385_NUM_SUPPLIES,
				       gc2385->supplies);
}

static int gc2385_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc2385 *gc2385;
	int ret;

	gc2385 = devm_kzalloc(dev, sizeof(*gc2385), GFP_KERNEL);
	if (!gc2385)
		return -ENOMEM;

	gc2385->client = client;
	gc2385->cur_mode = &supported_modes[0];

	gc2385->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(gc2385->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}
	ret = clk_set_rate(gc2385->xvclk, GC2385_XVCLK_FREQ);
	if (ret < 0) {
		dev_err(dev, "Failed to set xvclk rate (24MHz)\n");
		return ret;
	}
	if (clk_get_rate(gc2385->xvclk) != GC2385_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched, modes are based on 24MHz\n");

	gc2385->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(gc2385->pwdn_gpio)) {
		dev_err(dev, "Failed to get pwdn-gpios\n");
		return -EINVAL;
	}

	gc2385->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(gc2385->reset_gpio)) {
		dev_err(dev, "Failed to get reset-gpios\n");
		return -EINVAL;
	}

	ret = gc2385_configure_regulators(gc2385);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	dev_dbg(dev, "Setting up gc2385 subdev\n");

	mutex_init(&gc2385->mutex);
	v4l2_i2c_subdev_init(&gc2385->subdev, client, &gc2385_subdev_ops);
	ret = gc2385_initialize_controls(gc2385);
	if (ret)
		goto err_destroy_mutex;

	ret = __gc2385_power_on(gc2385);
	if (ret)
		goto err_free_handler;

	ret = gc2385_check_sensor_id(gc2385, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	gc2385->subdev.internal_ops = &gc2385_internal_ops;
	gc2385->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	gc2385->pad.flags = MEDIA_PAD_FL_SOURCE;
	gc2385->subdev.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&gc2385->subdev.entity, 1, &gc2385->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	dev_dbg(dev, "Registering up gc2385 subdev\n");

	ret = v4l2_async_register_subdev(&gc2385->subdev);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&gc2385->subdev.entity);
#endif
err_power_off:
	__gc2385_power_off(gc2385);
err_free_handler:
	v4l2_ctrl_handler_free(&gc2385->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&gc2385->mutex);

	return ret;
}

static void gc2385_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc2385 *gc2385 = to_gc2385(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&gc2385->ctrl_handler);
	mutex_destroy(&gc2385->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__gc2385_power_off(gc2385);
	pm_runtime_set_suspended(&client->dev);
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id gc2385_of_match[] = {
	{ .compatible = "galaxycore,gc2385" },
	{},
};
MODULE_DEVICE_TABLE(of, gc2385_of_match);
#endif

static struct i2c_driver gc2385_i2c_driver = {
	.driver = {
		.name = "gc2385",
		.pm = &gc2385_pm_ops,
		.of_match_table = of_match_ptr(gc2385_of_match),
	},
	.probe		= &gc2385_probe,
	.remove		= &gc2385_remove,
};

module_i2c_driver(gc2385_i2c_driver);

MODULE_DESCRIPTION("GalaxyCore gc2385 sensor driver");
MODULE_LICENSE("GPL v2");
