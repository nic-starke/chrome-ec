/* Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "atomic.h"
#include "clock.h"
#include "common.h"
#include "config.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "hwtimer.h"
#include "keyboard_config.h"
#include "keyboard_protocol.h"
#include "link_defs.h"
#include "queue.h"
#include "registers.h"
#include "task.h"
#include "timer.h"
#include "util.h"
#include "usb_api.h"
#include "usb_descriptor.h"
#include "usb_hw.h"
#include "usb_hid.h"
#include "usb_hid_hw.h"

/* Console output macro */
#define CPRINTF(format, args...) cprintf(CC_USB, format, ## args)

static const int keyboard_debug;

struct key_event {
	uint32_t time;
	uint8_t keycode;
	uint8_t pressed;
};

static struct queue const key_queue = QUEUE_NULL(16, struct key_event);
static struct mutex key_queue_mutex;

enum hid_protocol {
	HID_BOOT_PROTOCOL = 0,
	HID_REPORT_PROTOCOL = 1,
	HID_PROTOCOL_COUNT = 2,
};

/* Current protocol, behaviour is identical in both modes. */
static enum hid_protocol protocol = HID_REPORT_PROTOCOL;

/*
 * Note: This report format cannot be changed without breaking HID Boot protocol
 * compatibility (see HID 1.11 "Appendix B: Boot Interface Descriptors").
 *
 * If this needs to be extended, we need to use this report in boot protocol
 * mode, and an alternate one in report protocol mode.
 */
struct usb_hid_keyboard_report {
	uint8_t modifiers; /* bitmap of modifiers 224-231 */
	uint8_t reserved; /* 0x0 */
	uint8_t keys[6];
#ifdef CONFIG_KEYBOARD_NEW_KEY
	uint8_t new_key:1;
	uint8_t reserved2:7;
#endif
} __packed;

#define HID_KEYBOARD_REPORT_SIZE sizeof(struct usb_hid_keyboard_report)

#define HID_KEYBOARD_EP_INTERVAL_MS 16 /* ms */

/*
 * Coalesce events happening within some interval. The value must be greater
 * than EP interval to ensure we cannot have a backlog of keys.
 * It must also be short enough to ensure that the intended order of key presses
 * is passed to AP, and that we do not coalesce press and release events (which
 * would result in lost keys).
 */
#define COALESCE_INTERVAL (18 * MSEC)

/*
 * Discard key events in the FIFO buffer that are older than this amount of
 * time. Note that we do not fully drop them, we still update the report,
 * but we do not send the events individually anymore (so an old key press
 * and release will be dropped altogether, but a single press/release will
 * still be reported correctly).
 */
#define KEY_DISCARD_MAX_TIME (1 * SECOND)

/* Modifiers keycode range */
#define HID_KEYBOARD_MODIFIER_LOW 0xe0
#define HID_KEYBOARD_MODIFIER_HIGH 0xe7

#define HID_KEYBOARD_NEW_KEY 0xf0

/* The standard Chrome OS keyboard matrix table. See HUT 1.12v2 Table 12 and
 * https://www.w3.org/TR/DOM-Level-3-Events-code .
 *
 * NEW_KEY is mapped as 0xf0, but this key code is never actually send.
 */
const uint8_t keycodes[KEYBOARD_ROWS][KEYBOARD_COLS] = {
	{ 0x00, 0xe3, 0x3a, 0x05, 0x43, 0x87, 0x11, 0x00, 0x2e,
	  0x00, 0xe6, 0x00, 0x00 },
	{ 0x00, 0x29, 0x3d, 0x0a, 0x40, 0x00, 0x0b, 0x00, 0x34,
	  0x42, 0x00, 0x2a, 0x90 },
	{ 0xe0, 0x2b, 0x3c, 0x17, 0x3f, 0x30, 0x1c, 0x64, 0x2F,
	  0x41, 0x89, 0x00, 0x00 },
	{ 0xe3, 0x35, 0x3b, 0x22, 0x3e, 0x00, 0x23, 0x00, 0x2d,
	  0x68, 0x00, 0x31, 0x91 },
	{ 0xe4, 0x04, 0x07, 0x09, 0x16, 0x0e, 0x0d, 0x00, 0x33,
	  0x0f, 0x31, 0x28, 0x00 },
	{ HID_KEYBOARD_NEW_KEY,
	        0x1d, 0x06, 0x19, 0x1b, 0x36, 0x10, 0xe1, 0x38,
	  0x37, 0x00, 0x2c, 0x00 },
	{ 0x00, 0x1e, 0x20, 0x21, 0x1f, 0x25, 0x24, 0x00, 0x27,
	  0x26, 0xe2, 0x51, 0x4f },
	{ 0x00, 0x14, 0x08, 0x15, 0x1a, 0x0c, 0x18, 0xe5, 0x13,
	  0x12, 0x00, 0x52, 0x50 }
};

/* HID descriptors */
const struct usb_interface_descriptor USB_IFACE_DESC(USB_IFACE_HID_KEYBOARD) = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = USB_IFACE_HID_KEYBOARD,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_HID,
	.bInterfaceSubClass = USB_HID_SUBCLASS_BOOT,
	.bInterfaceProtocol = USB_HID_PROTOCOL_KEYBOARD,
	.iInterface = 0,
};
const struct usb_endpoint_descriptor USB_EP_DESC(USB_IFACE_HID_KEYBOARD, 81) = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x80 | USB_EP_HID_KEYBOARD,
	.bmAttributes = 0x03 /* Interrupt endpoint */,
	.wMaxPacketSize = HID_KEYBOARD_REPORT_SIZE,
	.bInterval = HID_KEYBOARD_EP_INTERVAL_MS /* ms polling interval */
};

/* HID : Report Descriptor */
static const uint8_t report_desc[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x06, /* Usage (Keyboard) */
	0xA1, 0x01, /* Collection (Application) */

	/* Modifiers */
	0x05, 0x07, /* Usage Page (Key Codes) */
	0x19, HID_KEYBOARD_MODIFIER_LOW, /* Usage Minimum */
	0x29, HID_KEYBOARD_MODIFIER_HIGH, /* Usage Maximum */
	0x15, 0x00, /* Logical Minimum (0) */
	0x25, 0x01, /* Logical Maximum (1) */
	0x75, 0x01, /* Report Size (1) */
	0x95, 0x08, /* Report Count (8) */
	0x81, 0x02, /* Input (Data, Variable, Absolute), ;Modifier byte */

	0x95, 0x01, /* Report Count (1) */
	0x75, 0x08, /* Report Size (8) */
	0x81, 0x01, /* Input (Constant), ;Reserved byte */

	/* Normal keys */
	0x95, 0x06, /* Report Count (6) */
	0x75, 0x08, /* Report Size (8) */
	0x15, 0x00, /* Logical Minimum (0) */
	0x25, 0xa4, /* Logical Maximum (164) */
	0x05, 0x07, /* Usage Page (Key Codes) */
	0x19, 0x00, /* Usage Minimum (0) */
	0x29, 0xa4, /* Usage Maximum (164) */
	0x81, 0x00, /* Input (Data, Array), ;Key arrays (6 bytes) */

#ifdef CONFIG_KEYBOARD_NEW_KEY
	0x06, 0xd1, 0xff, /* Usage Page (Vendor-defined 0xffd1) */
	0x19, 0x18, /* Usage Minimum */
	0x29, 0x18, /* Usage Maximum */
	0x15, 0x00, /* Logical Minimum (0) */
	0x25, 0x01, /* Logical Maximum (1) */
	0x75, 0x01, /* Report Size (1) */
	0x95, 0x01, /* Report Count (1) */
	0x81, 0x02, /* Input (Data, Variable, Absolute), ;Modifier byte */

	0x95, 0x01, /* Report Count (1) */
	0x75, 0x07, /* Report Size (7) */
	0x81, 0x01, /* Input (Constant), ;7-bit padding */
#endif

	0xC0        /* End Collection */
};

/* HID: HID Descriptor */
const struct usb_hid_descriptor USB_CUSTOM_DESC_VAR(USB_IFACE_HID_KEYBOARD,
						hid, hid_desc_kb) = {
	.bLength = 9,
	.bDescriptorType = USB_HID_DT_HID,
	.bcdHID = 0x0100,
	.bCountryCode = 0x00, /* Hardware target country */
	.bNumDescriptors = 1,
	.desc = {{
		.bDescriptorType = USB_HID_DT_REPORT,
		.wDescriptorLength = sizeof(report_desc)
	}}
};

#define EP_BUF_SIZE DIV_ROUND_UP(HID_KEYBOARD_REPORT_SIZE, 2)

static usb_uint hid_ep_buf[EP_BUF_SIZE] __usb_ram;
static volatile int hid_ep_data_ready;

static struct usb_hid_keyboard_report report;

static void keyboard_process_queue(void);
DECLARE_DEFERRED(keyboard_process_queue);

static void write_keyboard_report(void)
{
	/* Tell the interrupt handler to send the next buffer. */
	hid_ep_data_ready = 1;
	if ((STM32_USB_EP(USB_EP_HID_KEYBOARD) & EP_TX_MASK) == EP_TX_VALID) {
		/* Endpoint is busy */
		return;
	}

	if (atomic_read_clear(&hid_ep_data_ready)) {
		/*
		 * Endpoint is not busy, and interrupt handler did not just
		 * send the buffer: enable TX.
		 */

		memcpy_to_usbram((void *) usb_sram_addr(hid_ep_buf),
				&report, sizeof(report));
		STM32_TOGGLE_EP(USB_EP_HID_KEYBOARD, EP_TX_MASK,
				EP_TX_VALID, 0);
	}

	/*
	 * Wake the host. This is required to prevent a race between EP getting
	 * reloaded and host suspending the device, as, ideally, we never want
	 * to have EP loaded during suspend, to avoid reporting stale data.
	 */
	usb_wake();
}

static void hid_keyboard_tx(void)
{
	hid_tx(USB_EP_HID_KEYBOARD);
	if (hid_ep_data_ready) {
		memcpy_to_usbram((void *) usb_sram_addr(hid_ep_buf),
				&report, sizeof(report));
		STM32_TOGGLE_EP(USB_EP_HID_KEYBOARD, EP_TX_MASK,
				EP_TX_VALID, 0);
		hid_ep_data_ready = 0;
	}

	if (queue_count(&key_queue) > 0)
		hook_call_deferred(&keyboard_process_queue_data, 0);
}

static void hid_keyboard_event(enum usb_ep_event evt)
{
	if (evt == USB_EVENT_RESET)
		hid_reset(USB_EP_HID_KEYBOARD, hid_ep_buf,
			HID_KEYBOARD_REPORT_SIZE);
	else if (evt == USB_EVENT_DEVICE_RESUME && queue_count(&key_queue) > 0)
		hook_call_deferred(&keyboard_process_queue_data, 0);
}

USB_DECLARE_EP(USB_EP_HID_KEYBOARD, hid_keyboard_tx, hid_keyboard_tx,
	       hid_keyboard_event);

static int hid_keyboard_iface_request(usb_uint *ep0_buf_rx,
				      usb_uint *ep0_buf_tx)
{
	int ret = hid_iface_request(ep0_buf_rx, ep0_buf_tx,
				    report_desc, sizeof(report_desc),
				    &hid_desc_kb);
	if (ret >= 0)
		return ret;

	if (ep0_buf_rx[0] == (USB_DIR_OUT | USB_TYPE_CLASS |
		       USB_RECIP_INTERFACE | (USB_HID_REQ_SET_PROTOCOL << 8))) {
		uint16_t value = ep0_buf_rx[1];

		if (value >= HID_PROTOCOL_COUNT)
			return -1;

		protocol = value;

		btable_ep[0].tx_count = 0;
		STM32_TOGGLE_EP(0, EP_TX_RX_MASK, EP_TX_RX_VALID, 0);
		return 0;
	} else if (ep0_buf_rx[0] == (USB_DIR_IN | USB_TYPE_CLASS |
		       USB_RECIP_INTERFACE | (USB_HID_REQ_GET_PROTOCOL << 8))) {
		uint8_t value = protocol;

		memcpy_to_usbram((void *) usb_sram_addr(ep0_buf_tx),
				 &value, sizeof(value));
		btable_ep[0].tx_count = 1;
		STM32_TOGGLE_EP(0, EP_TX_RX_MASK, EP_TX_RX_VALID, 0);
		return 0;
	}

	return -1;
}
USB_DECLARE_IFACE(USB_IFACE_HID_KEYBOARD, hid_keyboard_iface_request)

void keyboard_clear_buffer(void)
{
	mutex_lock(&key_queue_mutex);
	queue_init(&key_queue);
	mutex_unlock(&key_queue_mutex);

	memset(&report, 0, sizeof(report));
	write_keyboard_report();
}

static void keyboard_process_queue(void)
{
	int i;
	uint8_t mask;
	struct key_event ev;
	int valid = 0;
	int trimming = 0;
	uint32_t now = __hw_clock_source_read();
	uint32_t first_key_time;

	if (keyboard_debug)
		CPRINTF("Q%d (s%d ep%d hw%d)\n", queue_count(&key_queue),
			usb_is_suspended(), hid_ep_data_ready,
			(STM32_USB_EP(USB_EP_HID_KEYBOARD) & EP_TX_MASK)
			== EP_TX_VALID);
	mutex_lock(&key_queue_mutex);

	if (queue_count(&key_queue) == 0) {
		mutex_unlock(&key_queue_mutex);
		return;
	}

	if (usb_is_suspended() || hid_ep_data_ready) {
		usb_wake();

		if (!queue_is_full(&key_queue)) {
			/* Queue still has space, let's keep gathering keys. */
			mutex_unlock(&key_queue_mutex);
			return;
		}

		/*
		 * Queue is full, so we continue, as the code below is
		 * guaranteed to pop at least one key from the queue, but we do
		 * not write the report at the end.
		 */
		CPRINTF("Trimming queue (%d %d %d)\n", queue_count(&key_queue),
			usb_is_suspended(), hid_ep_data_ready);

		trimming = 1;
	}

	/* There is at least one element in the queue. */
	queue_peek_units(&key_queue, &ev, 0, 1);
	first_key_time = ev.time;

	/*
	 * Pick key events from the queue, coalescing events older than events
	 * within EP interval time to make sure the queue cannot grow, and
	 * dropping keys that are too old.
	 */
	while (queue_count(&key_queue) > 0) {
		queue_peek_units(&key_queue, &ev, 0, 1);
		if (keyboard_debug)
			CPRINTF(" =%02x/%d %d %d\n", ev.keycode, ev.pressed,
				ev.time - now);

		if ((now - ev.time) <= KEY_DISCARD_MAX_TIME &&
		    (ev.time - first_key_time) >= COALESCE_INTERVAL)
			break;

		queue_advance_head(&key_queue, 1);

		if (ev.keycode == HID_KEYBOARD_NEW_KEY) {
#ifdef CONFIG_KEYBOARD_NEW_KEY
			report.new_key = ev.pressed ? 1 : 0;
			valid = 1;
#endif
		} else if (ev.keycode >= HID_KEYBOARD_MODIFIER_LOW &&
		    ev.keycode <= HID_KEYBOARD_MODIFIER_HIGH) {
			mask = 0x01 << (ev.keycode - HID_KEYBOARD_MODIFIER_LOW);
			if (ev.pressed)
				report.modifiers |= mask;
			else
				report.modifiers &= ~mask;
			valid = 1;
		} else if (ev.pressed) {
			/*
			 * Add keycode to the list of keys (does nothing if the
			 * array is already full).
			 */
			for (i = 0; i < ARRAY_SIZE(report.keys); i++) {
				/* Is key already pressed? */
				if (report.keys[i] == ev.keycode)
					break;
				if (report.keys[i] == 0) {
					report.keys[i] = ev.keycode;
					valid = 1;
					break;
				}
			}
		} else {
			/*
			 * Remove keycode from the list of keys (does nothing
			 * if the key is not in the array).
			 */
			for (i = 0; i < ARRAY_SIZE(report.keys); i++) {
				if (report.keys[i] == ev.keycode) {
					report.keys[i] = 0;
					valid = 1;
					break;
				}
			}
		}
	}

	mutex_unlock(&key_queue_mutex);

	if (valid && !trimming)
		write_keyboard_report();
}

void keyboard_state_changed(int row, int col, int is_pressed)
{
	uint8_t keycode = keycodes[row][col];
	struct key_event ev = {
		.time = __hw_clock_source_read(),
		.keycode = keycode,
		.pressed = is_pressed,
	};

	if (!keycode) {
		CPRINTF("Unknown key at %d/%d\n", row, col);
		return;
	}

	mutex_lock(&key_queue_mutex);
	queue_add_unit(&key_queue, &ev);
	mutex_unlock(&key_queue_mutex);

	keyboard_process_queue();
}
