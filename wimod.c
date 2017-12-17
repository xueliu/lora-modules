// SPDX-License-Identifier: GPL-2.0+
/*
 * IMST WiMOD
 *
 * Copyright (c) 2017 Andreas Färber
 */

#include <linux/crc-ccitt.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/rculist.h>
#include <linux/serdev.h>

#include "af_lora.h"
#include "lora.h"

#define WIMOD_HCI_PAYLOAD_MAX	300
#define WIMOD_HCI_PACKET_MAX	(1 + (2 + WIMOD_HCI_PAYLOAD_MAX + 2) * 2 + 1)

struct wimod_device {
	struct serdev_device *serdev;

	u8 rx_buf[WIMOD_HCI_PACKET_MAX];
	int rx_len;
	bool rx_esc;
	struct list_head packet_dispatchers;
};

#define SLIP_END	0300
#define SLIP_ESC	0333
#define SLIP_ESC_END	0334
#define SLIP_ESC_ESC	0335

static inline void slip_print_bytes(const u8* buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		printk("%02x ", buf[i]);
}

static int slip_send_end(struct serdev_device *sdev, unsigned long timeout)
{
	u8 val = SLIP_END;

	slip_print_bytes(&val, 1);

	return serdev_device_write(sdev, &val, 1, timeout);
}

static int slip_send_data(struct serdev_device *sdev, const u8 *buf, int len,
	unsigned long timeout)
{
	int last_idx = -1;
	int i;
	u8 esc[2] = { SLIP_ESC, 0 };
	int ret;

	for (i = 0; i < len; i++) {
		if (buf[i] != SLIP_END &&
		    buf[i] != SLIP_ESC)
			continue;

		slip_print_bytes(&buf[last_idx + 1], i - (last_idx + 1));

		ret = serdev_device_write(sdev,
			&buf[last_idx + 1], i - (last_idx + 1), timeout);
		if (ret)
			return ret;

		switch (buf[i]) {
		case SLIP_END:
			esc[1] = SLIP_ESC_END;
			break;
		case SLIP_ESC:
			esc[1] = SLIP_ESC_ESC;
			break;
		}
		slip_print_bytes(esc, 2);
		ret = serdev_device_write(sdev, esc, 2, timeout);
		if (ret)
			return ret;

		last_idx = i;
	}

	slip_print_bytes(&buf[last_idx + 1], len - (last_idx + 1));

	ret = serdev_device_write(sdev,
		&buf[last_idx + 1], len - (last_idx + 1), timeout);

	return ret;
}

#define DEVMGMT_ID	0x01

#define DEVMGMT_MSG_PING_REQ		0x01
#define DEVMGMT_MSG_PING_RSP		0x02
#define DEVMGMT_MSG_GET_DEVICE_INFO_REQ	0x03
#define DEVMGMT_MSG_GET_DEVICE_INFO_RSP	0x04

#define DEVMGMT_STATUS_OK	0x00

struct wimod_hci_packet_dispatcher {
	struct list_head list;
	u8 dst_id;
	u8 msg_id;
	void (*dispatchee)(const u8*, int, struct wimod_hci_packet_dispatcher *);
	void *priv;
};

struct wimod_hci_packet_completion {
	struct wimod_hci_packet_dispatcher disp;
	struct completion comp;
	char *payload;
	int payload_len;
};

static void wimod_hci_add_dispatcher(struct wimod_device *wmdev,
	struct wimod_hci_packet_dispatcher *entry)
{
	list_add_tail_rcu(&entry->list, &wmdev->packet_dispatchers);
}

static void wimod_hci_remove_dispatcher(struct wimod_device *wmdev,
	struct wimod_hci_packet_dispatcher *entry)
{
	list_del_rcu(&entry->list);
}

static void wimod_hci_packet_dispatch_completion(const u8 *data, int len,
	struct wimod_hci_packet_dispatcher *d)
{
	struct wimod_hci_packet_completion *disp =
		container_of(d, struct wimod_hci_packet_completion, disp);

	if (completion_done(&disp->comp))
		return;

	disp->payload_len = len - 2;
	disp->payload = kzalloc(disp->payload_len, GFP_KERNEL);
	if (disp->payload)
		memcpy(disp->payload, data + 2, len - 2);

	complete(&disp->comp);
}

static int wimod_hci_send(struct serdev_device *sdev,
	u8 dst_id, u8 msg_id, u8 *payload, int payload_len,
	unsigned long timeout)
{
	u16 crc = 0xffff;
	int ret;

	crc = crc_ccitt_byte(crc, dst_id);
	crc = crc_ccitt_byte(crc, msg_id);
	if (payload_len > 0)
		crc = crc_ccitt(crc, payload, payload_len);
	crc = ~crc;

	printk(KERN_INFO "sending: ");

	ret = slip_send_end(sdev, timeout);
	if (ret) {
		dev_err(&sdev->dev, "%s: initial END failed\n", __func__);
		return ret;
	}

	ret = slip_send_data(sdev, &dst_id, 1, timeout);
	if (ret) {
		dev_err(&sdev->dev, "%s: dst_id failed\n", __func__);
		return ret;
	}

	ret = slip_send_data(sdev, &msg_id, 1, timeout);
	if (ret) {
		dev_err(&sdev->dev, "%s: msg_id failed\n", __func__);
		return ret;
	}

	if (payload_len > 0) {
		ret = slip_send_data(sdev, payload, payload_len, timeout);
		if (ret) {
			dev_err(&sdev->dev, "%s: payload failed\n", __func__);
			return ret;
		}
	}

	cpu_to_le16s(crc);
	ret = slip_send_data(sdev, (u8 *)&crc, 2, timeout);
	if (ret) {
		dev_err(&sdev->dev, "%s: FCS failed\n", __func__);
		return ret;
	}

	ret = slip_send_end(sdev, timeout);
	if (ret) {
		dev_err(&sdev->dev, "%s: trailing END failed\n", __func__);
		return ret;
	}

	printk("\n");

	return 0;
}

static int wimod_hci_devmgmt_status(u8 status)
{
	switch (status) {
	case DEVMGMT_STATUS_OK:
		return 0;
	default:
		pr_info("DEVMGMT status %u\n", (int)status);
		return -EINVAL;
	}
}

static int wimod_hci_ping(struct wimod_device *wmdev, unsigned long timeout)
{
	struct wimod_hci_packet_completion packet = {0};
	int ret;

	packet.disp.dst_id = DEVMGMT_ID;
	packet.disp.msg_id = DEVMGMT_MSG_PING_RSP;
	packet.disp.dispatchee = wimod_hci_packet_dispatch_completion;
	init_completion(&packet.comp);

	wimod_hci_add_dispatcher(wmdev, &(packet.disp));

	ret = wimod_hci_send(wmdev->serdev, DEVMGMT_ID, DEVMGMT_MSG_PING_REQ, NULL, 0, timeout);
	if (ret) {
		wimod_hci_remove_dispatcher(wmdev, &(packet.disp));
		dev_err(&wmdev->serdev->dev, "ping: send failed\n");
		return ret;
	}

	timeout = wait_for_completion_timeout(&(packet.comp), timeout);
	wimod_hci_remove_dispatcher(wmdev, &(packet.disp));
	if (!timeout) {
		dev_err(&wmdev->serdev->dev, "ping: response timeout\n");
		return -ETIMEDOUT;
	}

	if (packet.payload_len < 1) {
		dev_err(&wmdev->serdev->dev, "ping: payload length\n");
		kfree(packet.payload);
		return -EINVAL;
	}

	ret = wimod_hci_devmgmt_status(packet.payload[0]);
	kfree(packet.payload);
	return ret;
}

static int wimod_hci_get_device_info(struct wimod_device *wmdev, u8 *buf, unsigned long timeout)
{
	struct wimod_hci_packet_completion packet = {0};
	int ret;

	packet.disp.dst_id = DEVMGMT_ID;
	packet.disp.msg_id = DEVMGMT_MSG_GET_DEVICE_INFO_RSP;
	packet.disp.dispatchee = wimod_hci_packet_dispatch_completion;
	init_completion(&packet.comp);

	wimod_hci_add_dispatcher(wmdev, &(packet.disp));

	ret = wimod_hci_send(wmdev->serdev, DEVMGMT_ID, DEVMGMT_MSG_GET_DEVICE_INFO_REQ, NULL, 0, timeout);
	if (ret) {
		wimod_hci_remove_dispatcher(wmdev, &(packet.disp));
		dev_err(&wmdev->serdev->dev, "get_device_info: send failed\n");
		return ret;
	}

	timeout = wait_for_completion_timeout(&(packet.comp), timeout);
	wimod_hci_remove_dispatcher(wmdev, &(packet.disp));
	if (!timeout) {
		dev_err(&wmdev->serdev->dev, "get_device_info: response timeout\n");
		return -ETIMEDOUT;
	}

	if (packet.payload_len < 1) {
		dev_err(&wmdev->serdev->dev, "get_device_info: payload length (1)\n");
		kfree(packet.payload);
		return -EINVAL;
	}

	ret = wimod_hci_devmgmt_status(packet.payload[0]);
	if (ret) {
		dev_err(&wmdev->serdev->dev, "get_device_info: status\n");
		kfree(packet.payload);
		return ret;
	}

	if (packet.payload_len < 10) {
		dev_err(&wmdev->serdev->dev, "get_device_info: payload length (10)\n");
		kfree(packet.payload);
		return -EINVAL;
	}

	if (buf)
		memcpy(buf, packet.payload + 1, min(packet.payload_len - 1, 9));

	kfree(packet.payload);
	return 0;
}

static void wimod_process_packet(struct serdev_device *sdev, const u8 *data, int len)
{
	struct wimod_device *wmdev = serdev_device_get_drvdata(sdev);
	struct wimod_hci_packet_dispatcher *e;
	u16 crc;

	dev_info(&sdev->dev, "Processing incoming packet (%d)\n", len);

	if (len < 4) {
		dev_dbg(&sdev->dev, "Discarding packet of length %d\n", len);
		return;
	}

	crc = ~crc_ccitt(0xffff, data, len);
	if (crc != 0x0f47) {
		dev_dbg(&sdev->dev, "Discarding packet with wrong checksum\n");
		return;
	}

	list_for_each_entry(e, &wmdev->packet_dispatchers, list) {
		if (e->dst_id == data[0] && e->msg_id == data[1]) {
			e->dispatchee(data, len - 2, e);
			break;
		}
	}
}

static int wimod_receive_buf(struct serdev_device *sdev, const u8 *data, size_t count)
{
	struct wimod_device *wmdev = serdev_device_get_drvdata(sdev);
	size_t i = 0;
	int len = 0;

	dev_dbg(&sdev->dev, "Receive (%d)\n", (int)count);

	while (i < min(count, sizeof(wmdev->rx_buf) - wmdev->rx_len)) {
		if (wmdev->rx_esc) {
			wmdev->rx_esc = false;
			switch (data[i]) {
			case SLIP_ESC_END:
				wmdev->rx_buf[wmdev->rx_len++] = SLIP_END;
				break;
			case SLIP_ESC_ESC:
				wmdev->rx_buf[wmdev->rx_len++] = SLIP_ESC;
				break;
			default:
				dev_warn(&sdev->dev, "Ignoring unknown escape sequence 0300 0%o\n", data[i]);
				break;
			}
			len += i + 1;
			data += i + 1;
			count -= i + 1;
			i = 0;
			continue;
		}
		if (data[i] != SLIP_END &&
		    data[i] != SLIP_ESC) {
			i++;
			continue;
		}
		if (i > 0) {
			memcpy(&wmdev->rx_buf[wmdev->rx_len], data, i);
			wmdev->rx_len += i;
		}
		if (data[i] == SLIP_END && wmdev->rx_len > 0) {
			wimod_process_packet(sdev, wmdev->rx_buf, wmdev->rx_len);
			wmdev->rx_len = 0;
		} else if (data[i] == SLIP_ESC) {
			wmdev->rx_esc = true;
		}
		len += i + 1;
		data += i + 1;
		count -= i + 1;
		i = 0;
	}

	dev_dbg(&sdev->dev, "Receive: processed %d\n", len);

	return len;
}

static const struct serdev_device_ops wimod_serdev_client_ops = {
	.receive_buf = wimod_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

static int wimod_probe(struct serdev_device *sdev)
{
	struct wimod_device *wmdev;
	u8 buf[9];
	int ret;

	dev_info(&sdev->dev, "Probing");

	wmdev = devm_kzalloc(&sdev->dev, sizeof(struct wimod_device), GFP_KERNEL);
	if (!wmdev)
		return -ENOMEM;

	wmdev->serdev = sdev;
	INIT_LIST_HEAD(&wmdev->packet_dispatchers);
	serdev_device_set_drvdata(sdev, wmdev);

	ret = serdev_device_open(sdev);
	if (ret) {
		dev_err(&sdev->dev, "Failed to open (%d)\n", ret);
		return ret;
	}

	serdev_device_set_baudrate(sdev, 115200);
	serdev_device_set_flow_control(sdev, false);
	serdev_device_set_client_ops(sdev, &wimod_serdev_client_ops);

	/*ret = wimod_hci_ping(wmdev, 3 * HZ);
	if (ret) {
		dev_err(&sdev->dev, "Ping failed (%d)\n", ret);
		goto err;
	}*/

	//mdelay(500);
	/*ret = wimod_hci_ping(wmdev, 3 * HZ);
	if (ret) {
		dev_err(&sdev->dev, "Ping 2 failed (%d)\n", ret);
		goto err;
	}*/

	ret = wimod_hci_get_device_info(wmdev, buf, 3 * HZ);
	if (ret) {
		dev_err(&sdev->dev, "Failed to obtain device info (%d)\n", ret);
		goto err;
	}
	dev_info(&sdev->dev, "Module type: 0x%02x\n", (int)buf[0]);

	dev_info(&sdev->dev, "Done.\n");

	return 0;
err:
	serdev_device_close(sdev);
	return ret;
}

static void wimod_remove(struct serdev_device *sdev)
{
	serdev_device_close(sdev);

	dev_info(&sdev->dev, "Removed\n");
}

static const struct of_device_id wimod_of_match[] = {
	{ .compatible = "imst,wimod-hci" },
	{}
};
MODULE_DEVICE_TABLE(of, wimod_of_match);

static struct serdev_device_driver wimod_serdev_driver = {
	.probe = wimod_probe,
	.remove = wimod_remove,
	.driver = {
		.name = "wimod",
		.of_match_table = wimod_of_match,
	},
};

static int __init wimod_init(void)
{
	int ret;

	ret = serdev_device_driver_register(&wimod_serdev_driver);
	if (ret)
		return ret;

	return 0;
}

static void __exit wimod_exit(void)
{
	serdev_device_driver_unregister(&wimod_serdev_driver);
}

module_init(wimod_init);
module_exit(wimod_exit);

MODULE_DESCRIPTION("WiMOD serdev driver");
MODULE_AUTHOR("Andreas Färber <afaerber@suse.de>");
MODULE_LICENSE("GPL");
