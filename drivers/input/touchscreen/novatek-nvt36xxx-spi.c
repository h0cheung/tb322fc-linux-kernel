// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal SPI input driver for Novatek NVT36xxx touch controllers.
 *
 * The event packet format and SPI framing are based on the Novatek NVT36xxx
 * Android driver. NT36536 no-flash firmware upload uses the memory map and
 * auto-copy mode in the shipped Lenovo driver, and Lenovo-provided firmware.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#include <drm/drm_panel.h>

#define NVT_POINT_DATA_LEN	108
#define NVT_POINT_CHECKSUM_LEN	65
#define NVT_MAX_TOUCHES		10
#define NVT_TOUCH_RECORD_LEN	6

#define NVT_TOUCH_ENTER		1
#define NVT_TOUCH_MOVING	2

#define NVT_FW_XFER_LEN		15360
#define NVT_FW_MAX_PARTITIONS	32
#define NVT_FW_END_FLAG_LEN	3
#define NVT_EXPECTED_PID	0x6097

/* Shipped NT36536 profile: trim 15,*,*,23,65,03; three-byte CRC lengths. */
#define NVT_EVENT_BUF_ADDR	0x130900
#define NVT_EVENT_RESET_COMPLETE 0x60
#define NVT_EVENT_FWINFO	0x78
#define NVT_EVENT_HOST_CMD	0x50
#define NVT_RESET_STATE_INIT	0xa0
#define NVT_RESET_STATE_MAX	0xaf

#define NVT_ENG_RST_ADDR		0x7fff80
#define NVT_CHIP_VER_TRIM_ADDR	0x1fb104
#define NVT_SWRST_N8_ADDR	0x1fb43e
#define NVT_BOOT_RDY_ADDR	0x1fb50d
#define NVT_TX_AUTO_COPY_EN	0x1fc825
#define NVT_ENB_CASC_ADDR	0x1fb12c
#define NVT_SPI_DMA_TX_INFO	0x1fc814
#define NVT_ILM_LENGTH_ADDR	0x1fcc34
#define NVT_DLM_LENGTH_ADDR	0x1fcc44
#define NVT_ILM_DES_ADDR	0x1fcc30
#define NVT_DLM_DES_ADDR	0x1fcc40
#define NVT_G_ILM_CHECKSUM_ADDR	0x1fcc38
#define NVT_G_DLM_CHECKSUM_ADDR	0x1fcc48

struct nvt36xxx_partition {
	u32 bin_addr;
	u32 sram_addr;
	u32 size;
	u32 crc;
};

struct nvt36xxx {
	struct spi_device *spi;
	struct input_dev *input;
	struct touchscreen_properties prop;
	const char *firmware_name;
	/* Serializes SPI transfers and the shared transfer buffers. */
	struct mutex lock;
	/* Serializes panel power transitions against input open/close. */
	struct mutex state_lock;
	struct drm_panel_follower panel_follower;
	struct work_struct firmware_work;
	bool firmware_loaded;
	bool chip_identified;
	bool is_cascade;
	bool input_open;
	bool irq_enabled;
	int firmware_error;
	u8 *fwbuf;
	/* Separate allocations keep streaming DMA buffers off shared cache lines. */
	u8 *tx;
	u8 *rx;
	u8 point_data[NVT_POINT_DATA_LEN + 1];
};

static int nvt36xxx_spi_read(struct nvt36xxx *nvt, u8 *buf, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = nvt->tx,
		.rx_buf = nvt->rx,
		.len = len + 1,
	};
	int error;

	if (len < 2 || len > NVT_POINT_DATA_LEN + 1)
		return -EINVAL;

	guard(mutex)(&nvt->lock);

	memset(nvt->tx, 0, len + 1);
	memset(nvt->rx, 0, len + 1);
	memcpy(nvt->tx, buf, len);
	nvt->tx[0] &= 0x7f;

	/* Match Lenovo's single full-duplex transfer with one dummy byte. */
	error = spi_sync_transfer(nvt->spi, &xfer, 1);
	if (error)
		return error;

	memcpy(buf + 1, nvt->rx + 2, len - 1);
	return 0;
}

static int nvt36xxx_spi_write(struct nvt36xxx *nvt, const u8 *buf,
			      size_t len)
{
	int error;

	if (!len || len > NVT_FW_XFER_LEN + 1)
		return -EINVAL;

	guard(mutex)(&nvt->lock);

	memcpy(nvt->fwbuf, buf, len);
	nvt->fwbuf[0] |= BIT(7);
	error = spi_write(nvt->spi, nvt->fwbuf, len);

	return error;
}

static int nvt36xxx_spi_write_chunk(struct nvt36xxx *nvt, u8 addr,
				    const u8 *data, size_t len)
{
	if (!len || len > NVT_FW_XFER_LEN)
		return -EINVAL;

	guard(mutex)(&nvt->lock);

	nvt->fwbuf[0] = addr | BIT(7);
	memcpy(nvt->fwbuf + 1, data, len);

	return spi_write(nvt->spi, nvt->fwbuf, len + 1);
}

static int nvt36xxx_set_page(struct nvt36xxx *nvt, u32 addr)
{
	u8 buf[] = {
		0xff,
		(addr >> 15) & 0xff,
		(addr >> 7) & 0xff,
	};

	return nvt36xxx_spi_write(nvt, buf, sizeof(buf));
}

static int nvt36xxx_write_addr(struct nvt36xxx *nvt, u32 addr, u8 value)
{
	u8 buf[] = { addr & 0x7f, value };
	int error;

	error = nvt36xxx_set_page(nvt, addr);
	if (error)
		return error;

	return nvt36xxx_spi_write(nvt, buf, sizeof(buf));
}

static int nvt36xxx_read_addr(struct nvt36xxx *nvt, u32 addr, u8 *value)
{
	u8 buf[] = { addr & 0x7f, 0 };
	int error;

	error = nvt36xxx_set_page(nvt, addr);
	if (error)
		return error;

	error = nvt36xxx_spi_read(nvt, buf, sizeof(buf));
	if (!error)
		*value = buf[1];

	return error;
}

static int nvt36xxx_log_trim_id(struct nvt36xxx *nvt, u32 addr)
{
	u8 buf[7] = { addr & 0x7f };
	int error;

	error = nvt36xxx_set_page(nvt, addr);
	if (error)
		return error;

	error = nvt36xxx_spi_write(nvt, buf, sizeof(buf));
	if (error)
		return error;

	buf[0] = addr & 0x7f;
	error = nvt36xxx_spi_read(nvt, buf, sizeof(buf));
	if (error)
		return error;

	dev_info(&nvt->spi->dev,
		 "trim ID %02x %02x %02x %02x %02x %02x\n",
		 buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);
	/* Match the first stock table entry for the observed NT36536 silicon. */
	if (buf[1] != 0x15 || buf[4] != 0x23 ||
	    buf[5] != 0x65 || buf[6] != 0x03)
		return -ENODEV;

	return 0;
}

static int nvt36xxx_bootloader_reset(struct nvt36xxx *nvt)
{
	int error;

	error = nvt36xxx_write_addr(nvt, NVT_SWRST_N8_ADDR, 0x69);
	if (error)
		return error;
	usleep_range(5000, 5500);

	return 0;
}

static int nvt36xxx_identify(struct nvt36xxx *nvt)
{
	u8 value;
	int error;

	dev_info(&nvt->spi->dev, "NT36536 identification: engineering reset\n");
	error = nvt36xxx_write_addr(nvt, NVT_ENG_RST_ADDR, 0x5a);
	if (error)
		return error;
	usleep_range(1000, 1500);
	/* Stock probe waits another 10 ms before its first bootloader reset. */
	usleep_range(10000, 11000);

	error = nvt36xxx_bootloader_reset(nvt);
	if (error)
		return error;
	error = nvt36xxx_log_trim_id(nvt, NVT_CHIP_VER_TRIM_ADDR);
	if (error)
		return error;

	error = nvt36xxx_read_addr(nvt, NVT_ENB_CASC_ADDR, &value);
	if (error)
		return error;
	nvt->is_cascade = !(value & BIT(0));
	dev_info(&nvt->spi->dev, "NT36536 hardware cascade=%u strap=%02x\n",
		 nvt->is_cascade, value);
	return 0;
}

static int nvt36xxx_add_partition(const struct firmware *fw,
				  struct nvt36xxx_partition *partitions,
				  unsigned int *num_partitions,
				  u32 bin_addr, u32 sram_addr,
				  u32 size, u32 crc)
{
	struct nvt36xxx_partition *partition;

	if (!size)
		return 0;
	if (*num_partitions >= NVT_FW_MAX_PARTITIONS)
		return -E2BIG;
	if (bin_addr >= fw->size || size >= fw->size - bin_addr)
		return -EINVAL;
	if (sram_addr > 0x7fffff || size > 0x7fffff - sram_addr)
		return -EINVAL;

	partition = &partitions[(*num_partitions)++];
	partition->bin_addr = bin_addr;
	partition->sram_addr = sram_addr;
	partition->size = size;
	partition->crc = crc;

	return 0;
}

static int nvt36xxx_parse_firmware(const struct firmware *fw,
				   struct nvt36xxx_partition *partitions,
				   unsigned int *num_partitions,
				   bool *cascade)
{
	const u8 *data = fw->data;
	u32 header_end, limit, pos;
	u32 bin_addr, sram_addr, size, crc;
	unsigned int i;
	int error;

	if (fw->size < 4096 ||
	    memcmp(data + fw->size - NVT_FW_END_FLAG_LEN,
		   "NVT", NVT_FW_END_FLAG_LEN))
		return -EINVAL;
	if (data[fw->size - 4096] + data[fw->size - 4095] != 0xff)
		return -EINVAL;

	header_end = get_unaligned_le32(data);
	if (header_end < 0x30 || header_end > fw->size || header_end % 16)
		return -EINVAL;
	/* Overlay images need a separate DLM header parser. */
	if (data[0x28] & BIT(4))
		return -EOPNOTSUPP;

	*cascade = !!(data[0x20] & BIT(1));
	/* This uploader does not implement the optional cascade DMA setup. */
	if (*cascade && (data[0x29] & BIT(0)))
		return -EOPNOTSUPP;
	*num_partitions = 0;

	/*
	 * The ILM file offset can include more than the two cascade headers.
	 * Stock finds the first header's end from its self-copy descriptor,
	 * then locates the second header relative to that boundary.
	 */
	limit = header_end;
	for (pos = 0x30; pos <= header_end - 0x10; pos += 0x10) {
		if (get_unaligned_le32(data + pos + 4) &&
		    !get_unaligned_le32(data + pos + 8)) {
			limit = pos + 0x10;
			break;
		}
	}
	if (*cascade && limit > header_end / 2)
		return -EINVAL;

	for (i = 0; i < 2; i++) {
		pos = i * 12;
		bin_addr = get_unaligned_le32(data + pos);
		sram_addr = get_unaligned_le32(data + pos + 4);
		size = get_unaligned_le32(data + pos + 8);
		crc = get_unaligned_le32(data + 0x18 + i * 4);
		if (!size)
			return -EINVAL;

		error = nvt36xxx_add_partition(fw, partitions,
					       num_partitions, bin_addr,
					       sram_addr, size, crc);
		if (error)
			return error;
	}

	for (pos = 0x30; pos <= limit - 0x10; pos += 0x10) {
		sram_addr = get_unaligned_le32(data + pos);
		size = get_unaligned_le32(data + pos + 4);
		bin_addr = get_unaligned_le32(data + pos + 8);
		crc = get_unaligned_le32(data + pos + 12);

		error = nvt36xxx_add_partition(fw, partitions,
					       num_partitions, bin_addr,
					       sram_addr, size, crc);
		if (error)
			return error;
	}

	if (*cascade) {
		pos = 2 * limit - 0x10;
		sram_addr = get_unaligned_le32(data + pos);
		size = get_unaligned_le32(data + pos + 4);
		bin_addr = get_unaligned_le32(data + pos + 8);
		crc = get_unaligned_le32(data + pos + 12);
		if (!size || bin_addr != limit)
			return -EINVAL;
		error = nvt36xxx_add_partition(fw, partitions, num_partitions,
					       bin_addr, sram_addr, size, crc);
		if (error)
			return error;
	}

	return *num_partitions >= 2 ? 0 : -EINVAL;
}

static int nvt36xxx_write_partition(struct nvt36xxx *nvt,
				    const struct firmware *fw,
				    const struct nvt36xxx_partition *partition)
{
	size_t remaining = partition->size + 1;
	u32 bin_addr = partition->bin_addr;
	u32 sram_addr = partition->sram_addr;
	int error;

	while (remaining) {
		size_t len = min_t(size_t, remaining, NVT_FW_XFER_LEN);

		error = nvt36xxx_set_page(nvt, sram_addr);
		if (error)
			return error;

		error = nvt36xxx_spi_write_chunk(nvt, sram_addr & 0x7f,
						 fw->data + bin_addr, len);
		if (error)
			return error;

		sram_addr += len;
		bin_addr += len;
		remaining -= len;
	}

	return 0;
}

static int nvt36xxx_write_le24(struct nvt36xxx *nvt, u32 addr, u32 value)
{
	u8 buf[] = {
		addr & 0x7f,
		value & 0xff,
		(value >> 8) & 0xff,
		(value >> 16) & 0xff,
	};
	int error;

	error = nvt36xxx_set_page(nvt, addr);
	if (error)
		return error;

	return nvt36xxx_spi_write(nvt, buf, sizeof(buf));
}

static int nvt36xxx_write_le32(struct nvt36xxx *nvt, u32 addr, u32 value)
{
	u8 buf[] = {
		addr & 0x7f,
		value & 0xff,
		(value >> 8) & 0xff,
		(value >> 16) & 0xff,
		(value >> 24) & 0xff,
	};
	int error;

	error = nvt36xxx_set_page(nvt, addr);
	if (error)
		return error;

	return nvt36xxx_spi_write(nvt, buf, sizeof(buf));
}

static int nvt36xxx_set_crc_banks(struct nvt36xxx *nvt,
				  const struct nvt36xxx_partition *partitions)
{
	int error;

	error = nvt36xxx_write_le24(nvt, NVT_ILM_DES_ADDR,
				    partitions[0].sram_addr);
	if (error)
		return error;
	error = nvt36xxx_write_le24(nvt, NVT_ILM_LENGTH_ADDR,
				    partitions[0].size);
	if (error)
		return error;
	error = nvt36xxx_write_le32(nvt, NVT_G_ILM_CHECKSUM_ADDR,
				    partitions[0].crc);
	if (error)
		return error;

	error = nvt36xxx_write_le24(nvt, NVT_DLM_DES_ADDR,
				    partitions[1].sram_addr);
	if (error)
		return error;
	error = nvt36xxx_write_le24(nvt, NVT_DLM_LENGTH_ADDR,
				    partitions[1].size);
	if (error)
		return error;

	return nvt36xxx_write_le32(nvt, NVT_G_DLM_CHECKSUM_ADDR,
				   partitions[1].crc);
}

static int nvt36xxx_wait_dma(struct nvt36xxx *nvt)
{
	u8 value;
	int error, retry;

	for (retry = 0; retry < 200; retry++) {
		error = nvt36xxx_read_addr(nvt, NVT_TX_AUTO_COPY_EN, &value);
		if (error)
			return error;
		if (!value)
			return 0;
		usleep_range(1000, 1500);
	}

	dev_err(&nvt->spi->dev, "NT36536 auto-copy timed out, status=%02x\n", value);
	return -ETIMEDOUT;
}

static int nvt36xxx_wait_reset(struct nvt36xxx *nvt)
{
	u8 buf[] = { NVT_EVENT_RESET_COMPLETE, 0, 0, 0, 0, 0 };
	int error, retry;

	error = nvt36xxx_set_page(nvt, NVT_EVENT_BUF_ADDR);
	if (error)
		return error;

	for (retry = 0; retry < 100; retry++) {
		usleep_range(10000, 11000);
		buf[0] = NVT_EVENT_RESET_COMPLETE;
		error = nvt36xxx_spi_read(nvt, buf, sizeof(buf));
		if (error)
			return error;
		if (buf[1] >= NVT_RESET_STATE_INIT &&
		    buf[1] <= NVT_RESET_STATE_MAX)
			return 0;
	}

	dev_err(&nvt->spi->dev,
		"firmware reset-state timeout: %02x %02x %02x %02x %02x\n",
		buf[1], buf[2], buf[3], buf[4], buf[5]);
	return -ETIMEDOUT;
}

static int nvt36xxx_check_firmware_info(struct nvt36xxx *nvt)
{
	u8 buf[39] = { NVT_EVENT_FWINFO };
	u16 pid;
	int error;

	error = nvt36xxx_set_page(nvt, NVT_EVENT_BUF_ADDR | NVT_EVENT_FWINFO);
	if (error)
		return error;

	error = nvt36xxx_spi_read(nvt, buf, sizeof(buf));
	if (error)
		return error;
	if (buf[1] + buf[2] != 0xff)
		return -EIO;

	pid = (buf[36] << 8) | buf[35];
	dev_info(&nvt->spi->dev,
		 "firmware version %02x.%02x event protocol %02x PID %04x\n",
		 buf[1], buf[14], buf[13], pid);

	return pid == NVT_EXPECTED_PID ? 0 : -ENODEV;
}

static int nvt36xxx_upload_firmware_once(struct nvt36xxx *nvt,
					 const struct firmware *fw)
{
	struct nvt36xxx_partition partitions[NVT_FW_MAX_PARTITIONS] = {};
	unsigned int num_partitions, i;
	bool cascade;
	u8 reset_status[] = { NVT_EVENT_RESET_COMPLETE, 0, 0, 0, 0, 0, 0 };
	u8 crc_command[] = { NVT_EVENT_HOST_CMD, 0xae, 0 };
	int error;

	error = nvt36xxx_parse_firmware(fw, partitions, &num_partitions, &cascade);
	if (error)
		return error;

	dev_info(&nvt->spi->dev,
		 "host-downloading %u partitions%s\n", num_partitions,
		 cascade ? " with cascade auto-copy" : "");

	if (!nvt->chip_identified) {
		error = nvt36xxx_identify(nvt);
		if (error)
			return error;
		nvt->chip_identified = true;
	}

	/* Stock selects auto-copy from the image header, not the chip strap. */
	/* Stock starts firmware download with a new bootloader reset. */
	dev_info(&nvt->spi->dev, "chip identified: starting firmware download\n");
	error = nvt36xxx_bootloader_reset(nvt);
	if (error)
		return error;

	error = nvt36xxx_set_crc_banks(nvt, partitions);
	if (error)
		return error;

	if (cascade) {
		error = nvt36xxx_write_addr(nvt, NVT_TX_AUTO_COPY_EN, 0x56);
		if (error)
			return error;
	}

	for (i = 0; i < num_partitions; i++) {
		error = nvt36xxx_write_partition(nvt, fw, &partitions[i]);
		if (error)
			return error;
	}
	dev_info(&nvt->spi->dev, "firmware SRAM transfer complete\n");

	if (cascade) {
		error = nvt36xxx_wait_dma(nvt);
		if (error)
			return error;
		dev_info(&nvt->spi->dev, "cascade auto-copy complete\n");
	}

	/* Match the stock reset-status clear and firmware CRC command. */
	error = nvt36xxx_set_page(nvt, NVT_EVENT_BUF_ADDR);
	if (error)
		return error;
	error = nvt36xxx_spi_write(nvt, reset_status, sizeof(reset_status));
	if (error)
		return error;
	error = nvt36xxx_spi_write(nvt, crc_command, sizeof(crc_command));
	if (error)
		return error;
	error = nvt36xxx_write_addr(nvt, NVT_BOOT_RDY_ADDR, 1);
	if (error)
		return error;
	dev_info(&nvt->spi->dev, "firmware boot-ready asserted\n");

	error = nvt36xxx_wait_reset(nvt);
	if (error)
		return error;

	return nvt36xxx_check_firmware_info(nvt);
}

static int nvt36xxx_upload_firmware(struct nvt36xxx *nvt)
{
	const struct firmware *fw;
	int error;

	error = request_firmware(&fw, nvt->firmware_name, &nvt->spi->dev);
	if (error)
		return error;

	/* Leave a failed transport or firmware state for diagnosis until reset. */
	error = nvt36xxx_upload_firmware_once(nvt, fw);

	release_firmware(fw);
	return error;
}

static bool nvt36xxx_valid_checksum(const u8 *data)
{
	u8 checksum = 0;
	int i;

	for (i = 0; i < NVT_POINT_CHECKSUM_LEN - 1; i++)
		checksum += data[i + 1];

	checksum = ~checksum + 1;
	return checksum == data[NVT_POINT_CHECKSUM_LEN];
}

static irqreturn_t nvt36xxx_irq(int irq, void *dev_id)
{
	struct nvt36xxx *nvt = dev_id;
	int error, i;

	memset(nvt->point_data, 0, sizeof(nvt->point_data));
	error = nvt36xxx_spi_read(nvt, nvt->point_data,
				  sizeof(nvt->point_data));
	if (error)
		return IRQ_HANDLED;

	if (!nvt36xxx_valid_checksum(nvt->point_data))
		return IRQ_HANDLED;

	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		unsigned int offset = 1 + NVT_TOUCH_RECORD_LEN * i;
		u8 status = nvt->point_data[offset] & 0x7;
		u8 id = (nvt->point_data[offset] >> 3) - 1;
		unsigned int x, y, width;

		if (id >= NVT_MAX_TOUCHES)
			continue;
		if (status != NVT_TOUCH_ENTER && status != NVT_TOUCH_MOVING)
			continue;

		x = (nvt->point_data[offset + 1] << 8) |
		    nvt->point_data[offset + 2];
		y = (nvt->point_data[offset + 3] << 8) |
		    nvt->point_data[offset + 4];
		if (x > nvt->prop.max_x || y > nvt->prop.max_y)
			continue;
		width = nvt->point_data[offset + 5];
		if (!width)
			width = 1;

		input_mt_slot(nvt->input, id);
		input_mt_report_slot_state(nvt->input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(nvt->input, &nvt->prop, x, y, true);
		input_report_abs(nvt->input, ABS_MT_TOUCH_MAJOR, width);
		input_report_abs(nvt->input, ABS_MT_PRESSURE, 1);
	}

	input_mt_sync_frame(nvt->input);
	input_sync(nvt->input);

	return IRQ_HANDLED;
}

static int nvt36xxx_open(struct input_dev *input)
{
	struct nvt36xxx *nvt = input_get_drvdata(input);

	guard(mutex)(&nvt->state_lock);

	nvt->input_open = true;
	if (nvt->firmware_loaded && !nvt->irq_enabled) {
		enable_irq(nvt->spi->irq);
		nvt->irq_enabled = true;
	}

	return 0;
}

static void nvt36xxx_close(struct input_dev *input)
{
	struct nvt36xxx *nvt = input_get_drvdata(input);

	guard(mutex)(&nvt->state_lock);

	if (nvt->irq_enabled) {
		disable_irq(nvt->spi->irq);
		nvt->irq_enabled = false;
	}
	nvt->input_open = false;
}

static void nvt36xxx_firmware_work(struct work_struct *work)
{
	struct nvt36xxx *nvt = container_of(work, struct nvt36xxx,
					     firmware_work);
	int error;

	guard(mutex)(&nvt->state_lock);

	if (nvt->firmware_loaded)
		return;

	error = pinctrl_pm_select_default_state(&nvt->spi->dev);
	if (!error)
		error = nvt36xxx_upload_firmware(nvt);
	nvt->firmware_error = error;
	if (error) {
		dev_err(&nvt->spi->dev, "panel-time firmware upload failed: %d\n",
			error);
		return;
	}

	nvt->firmware_loaded = true;
	if (nvt->input_open && !nvt->irq_enabled) {
		enable_irq(nvt->spi->irq);
		nvt->irq_enabled = true;
	}
}

static int nvt36xxx_panel_prepared(struct drm_panel_follower *follower)
{
	struct nvt36xxx *nvt = container_of(follower, struct nvt36xxx,
					     panel_follower);

	dev_info(&nvt->spi->dev, "panel prepared: scheduling firmware upload\n");
	schedule_work(&nvt->firmware_work);
	return 0;
}

static int nvt36xxx_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct nvt36xxx *nvt = container_of(follower, struct nvt36xxx,
					     panel_follower);
	int i;

	cancel_work_sync(&nvt->firmware_work);

	guard(mutex)(&nvt->state_lock);

	if (nvt->irq_enabled) {
		disable_irq(nvt->spi->irq);
		nvt->irq_enabled = false;
	}
	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		input_mt_slot(nvt->input, i);
		input_mt_report_slot_state(nvt->input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(nvt->input);
	input_sync(nvt->input);
	nvt->firmware_loaded = false;
	nvt->firmware_error = -EAGAIN;

	return pinctrl_pm_select_sleep_state(&nvt->spi->dev);
}

static const struct drm_panel_follower_funcs nvt36xxx_panel_follower_funcs = {
	.panel_prepared = nvt36xxx_panel_prepared,
	.panel_unpreparing = nvt36xxx_panel_unpreparing,
};

static void nvt36xxx_cancel_firmware_work(void *data)
{
	struct nvt36xxx *nvt = data;

	cancel_work_sync(&nvt->firmware_work);
}

static int nvt36xxx_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct nvt36xxx *nvt;
	struct input_dev *input;
	int error;

	nvt = devm_kzalloc(dev, sizeof(*nvt), GFP_KERNEL);
	if (!nvt)
		return -ENOMEM;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	nvt->spi = spi;
	nvt->input = input;
	mutex_init(&nvt->lock);
	mutex_init(&nvt->state_lock);
	INIT_WORK(&nvt->firmware_work, nvt36xxx_firmware_work);
	nvt->firmware_error = -EAGAIN;
	spi_set_drvdata(spi, nvt);

	nvt->fwbuf = devm_kmalloc(dev, NVT_FW_XFER_LEN + 1, GFP_KERNEL);
	if (!nvt->fwbuf)
		return -ENOMEM;
	nvt->tx = devm_kmalloc(dev, NVT_POINT_DATA_LEN + 2, GFP_KERNEL);
	if (!nvt->tx)
		return -ENOMEM;
	nvt->rx = devm_kmalloc(dev, NVT_POINT_DATA_LEN + 2, GFP_KERNEL);
	if (!nvt->rx)
		return -ENOMEM;

	if (device_property_read_string(dev, "firmware-name",
					&nvt->firmware_name))
		nvt->firmware_name = "novatek_ts_csot_3k_fw.bin";

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	/* The shipped driver sets these before spi_setup(). */
	spi->cs_setup.value = 300;
	spi->cs_setup.unit = SPI_DELAY_UNIT_NSECS;
	spi->cs_hold.value = 300;
	spi->cs_hold.unit = SPI_DELAY_UNIT_NSECS;
	error = spi_setup(spi);
	if (error)
		return dev_err_probe(dev, error, "failed to configure SPI\n");
	dev_info(dev, "stock SPI CS setup/hold delays: 300 ns\n");

	input->name = "NVTCapacitiveTouchScreen";
	input->id.bustype = BUS_SPI;
	input->dev.parent = dev;
	input->open = nvt36xxx_open;
	input->close = nvt36xxx_close;
	input_set_drvdata(input, nvt);

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 19040, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 30400, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 1, 0, 0);
	touchscreen_parse_properties(input, true, &nvt->prop);

	error = input_mt_init_slots(input, NVT_MAX_TOUCHES,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	error = devm_request_threaded_irq(dev, spi->irq, NULL, nvt36xxx_irq,
					  IRQF_ONESHOT | IRQF_NO_AUTOEN |
					  IRQF_TRIGGER_FALLING,
					  dev_name(dev), nvt);
	if (error)
		return dev_err_probe(dev, error, "failed to request IRQ\n");

	if (drm_is_panel_follower(dev)) {
		error = devm_add_action_or_reset(dev,
						 nvt36xxx_cancel_firmware_work, nvt);
		if (error)
			return error;

		nvt->panel_follower.funcs = &nvt36xxx_panel_follower_funcs;
		error = devm_drm_panel_add_follower(dev, &nvt->panel_follower);
		if (error)
			return dev_err_probe(dev, error,
					     "failed to follow display panel\n");

	} else {
		error = nvt36xxx_upload_firmware(nvt);
		nvt->firmware_error = error;
		if (error)
			return dev_err_probe(dev, error, "failed to upload %s\n",
					     nvt->firmware_name);
		nvt->firmware_loaded = true;
	}

	error = input_register_device(input);
	if (error)
		return error;

	dev_info(dev, "registered NVT36xxx touchscreen%s\n",
		 drm_is_panel_follower(dev) ? " as panel follower" : "");
	return 0;
}

static const struct of_device_id nvt36xxx_of_match[] = {
	{ .compatible = "novatek,nt36536-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, nvt36xxx_of_match);

static const struct spi_device_id nvt36xxx_spi_ids[] = {
	{ "nt36536-ts" },
	{ }
};
MODULE_DEVICE_TABLE(spi, nvt36xxx_spi_ids);

static struct spi_driver nvt36xxx_driver = {
	.driver = {
		.name = "novatek-nvt36xxx-spi",
		.of_match_table = nvt36xxx_of_match,
	},
	.probe = nvt36xxx_probe,
	.id_table = nvt36xxx_spi_ids,
};
module_spi_driver(nvt36xxx_driver);

MODULE_DESCRIPTION("Novatek NVT36xxx SPI touchscreen driver");
MODULE_LICENSE("GPL");
