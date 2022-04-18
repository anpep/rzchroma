/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/dmi.h>
#include <linux/hiddev.h>
#include <linux/usb.h>
#include <linux/random.h>

MODULE_AUTHOR("Angel Perez <angel@ttm.sh>");
MODULE_DESCRIPTION("Razer DeathAdder Chroma Control Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.0");

enum {
	ATTR_WHEEL_COLOR = 0x01, /* Scroll wheel LED ID */
	ATTR_LOGO_COLOR = 0x04 /* Razer logo LED ID */
};

/* See <https://github.com/anpep/rzctl> */
struct rzchroma_report {
	u8 status;
	u8 transaction_id;
	u8 remaining_packets;
	u8 protocol_type;
	u8 args_len;
	u8 cmd_class;
	u8 cmd_id;
	u8 args[80];
	u8 crc;
	u8 _reserved;
};

/**
 * report_crc - Razer device CRC implementation. See <https://github.com/anpep/rzctl>
 *
 * @data: report data to be sent to the device
 * @count: number of bytes of the report
 *
 * This function returns a 8-bit checksum that Razer devices use for transfer
 * error-checking
 */
static inline u8 report_crc(const char *data, size_t count)
{
	u8 i, crc = 0;
	for (i = 2; i < count - 2; i++)
		crc ^= data[i];
	return crc;
}

/**
 * write_attr - set a value on a specific device attribute
 *
 * @attr_id: attribute to be changed
 * @dev: device
 * @buf: value to be written
 * @count: number of bytes to be written
 *
 * This function returns the number of written bytes on success or a
 * negative value on failure
 */
static ssize_t write_attr(int attr_id, struct device *dev, const char *buf,
			  size_t count)
{
	struct usb_interface *usb_if = to_usb_interface(dev->parent);
	struct usb_device *usb_dev = interface_to_usbdev(usb_if);
	struct rzchroma_report *report;
	int rc;

	/* Make sure we receive exactly 3 bytes (R, G, B) */
	if (count != 3)
		return -EINVAL;
	pr_info("sending %zu-byte report for attribute %d\n",
		sizeof(struct rzchroma_report), attr_id);

	/* Build output feature report. We need to initialize a heap object
	 * because in some scenarios where DMA could be used, using a buffer
	 * allocated in the stack could lead to data corruption if, for
	 * example, the stack lays on a virtually-mapped page. */
	report = kzalloc(sizeof(struct rzchroma_report), GFP_KERNEL);
	if (!report)
		return -ENOMEM;

	/* Fill in feature report. See <https://github.com/anpep/rzctl> */
	report->cmd_class = 0x03; /* ??? */
	report->cmd_id = 0x01; /* ??? */

	get_random_bytes(&(report->transaction_id),
			 sizeof(report->transaction_id));

	report->args_len = 5;
	report->args[0] = 1; /* Persist LED configuration */
	report->args[1] = attr_id; /* LED ID */
	report->args[2] = buf[0]; /* R */
	report->args[3] = buf[1]; /* G */
	report->args[4] = buf[2]; /* B */

	report->crc = report_crc((u8 *)report, sizeof(struct rzchroma_report));

	/* Send report. */
	rc = usb_control_msg(
		usb_dev, /* Send to our USB device */
		usb_sndctrlpipe(usb_dev, 0), /* Send to endpoint 0 */
		HID_REQ_SET_REPORT, /* HID report */
		USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT,
		(HID_REPORT_TYPE_FEATURE << 8) | 0x00, /* HID feature report */
		0, /* Interface index 0 */
		report, /* Report data */
		sizeof(struct rzchroma_report), /* Report size */
		USB_CTRL_SET_TIMEOUT /* Default timeout */
	);

	/* Release heap object */
	kfree(report);

	if (rc != sizeof(struct rzchroma_report)) {
		/* URB send failed */
		return rc < 0 ? rc : -EIO;
	}

	return count;
}

/**
 * rzchroma_attr_write_logo_color - handle writes to the logo_color attribute
 *
 * @dev: device
 * @attr: device attribute
 * @buf: buffer supplied by the user
 * @count: number of bytes copied from the userspace buffer
 *
 * This function returns the number of written bytes or a negative value on
 * failure
 */
static ssize_t rzchroma_attr_write_logo_color(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	return write_attr(ATTR_LOGO_COLOR, dev, buf, count);
}

/**
 * rzchroma_attr_write_wheel_color - handle writes to the wheel_color attribute
 *
 * @dev: device
 * @attr: device attribute
 * @buf: buffer supplied by the user
 * @count: number of bytes copied from the userspace buffer
 *
 * This function returns the number of written bytes or a negative value on
 * failure
 */
static ssize_t rzchroma_attr_write_wheel_color(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	return write_attr(ATTR_WHEEL_COLOR, dev, buf, count);
}

/* Razer logo LED color attribute */
static DEVICE_ATTR(logo_color, 0220, NULL, rzchroma_attr_write_logo_color);
/* Scroll wheel LED color attribute */
static DEVICE_ATTR(wheel_color, 0220, NULL, rzchroma_attr_write_wheel_color);

/**
 * rzchroma_probe - called by the HID subsystem when a device matching the list
 * of vendor/product IDs is connected an bound to this driver
 *
 * @hdev: hid device
 * @id: device ID
 *
 * This function returns 0 on success or a nonzero value on error
 */
static int rzchroma_probe(struct hid_device *hdev,
			  const struct hid_device_id *id)
{
	struct device *dev = &hdev->dev;
	struct usb_interface *usb_if = to_usb_interface(dev->parent);
	struct usb_device *usb_dev = interface_to_usbdev(usb_if);
	int rc;

	/* Create files associated to device attributes. */
	device_create_file(dev, &dev_attr_logo_color);
	device_create_file(dev, &dev_attr_wheel_color);

	/* Parse HW reports. */
	if ((rc = hid_parse(hdev))) {
		hid_err(hdev, "hid_parse() failed\n");
		return rc;
	}

	/* Allocate HW buffers and start device. */
	if ((rc = hid_hw_start(hdev, HID_CONNECT_DEFAULT)) != 0) {
		hid_err(hdev, "hid_hw_start() failed\n");
		return rc;
	}

	/* Let HW send event reports. */
	if ((rc = hid_hw_open(hdev)) != 0) {
		hid_err(hdev, "hid_hw_open() failed\n");
		return rc;
	}

	usb_disable_autosuspend(usb_dev);
	return 0;
}

/**
 * rzchroma_remove - called by the HID subsystem when a matching connected
 * device is detached or unbound from this driver
 *
 * @hdev: hid device
 */
static void rzchroma_remove(struct hid_device *hdev)
{
	/* Remove attribute files from sysfs */
	device_remove_file(&hdev->dev, &dev_attr_logo_color);
	device_remove_file(&hdev->dev, &dev_attr_wheel_color);

	/* Close and stop underlying HW. */
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

/* USB product/vendor IDs handled by this driver */
static const struct hid_device_id rzchroma_devices[] = {
	/* Razer DeathAdder Chroma */
	{ HID_USB_DEVICE(0x1532, 0x0043) },
	{}
};
MODULE_DEVICE_TABLE(hid, rzchroma_devices);

/* HID driver specification. */
static struct hid_driver rzchroma_driver = { .name = "hid-rzchroma",
					     .id_table = rzchroma_devices,
					     .probe = rzchroma_probe,
					     .remove = rzchroma_remove };
module_hid_driver(rzchroma_driver);
