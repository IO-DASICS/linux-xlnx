// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Inter Processor Interrupt(IPI) Mailbox Driver
 *
 * Copyright (C) 2018 Xilinx, Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/cpuhotplug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/zynqmp-ipi-message.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

/* IPI agent ID any */
#define IPI_ID_ANY 0xFFUL

/* indicate if ZynqMP IPI mailbox driver uses SMC calls or HVC calls */
#define USE_SMC 0
#define USE_HVC 1

/* Default IPI SMC function IDs */
#define SMC_IPI_MAILBOX_OPEN		0x82001000U
#define SMC_IPI_MAILBOX_RELEASE		0x82001001U
#define SMC_IPI_MAILBOX_STATUS_ENQUIRY	0x82001002U
#define SMC_IPI_MAILBOX_NOTIFY		0x82001003U
#define SMC_IPI_MAILBOX_ACK		0x82001004U
#define SMC_IPI_MAILBOX_ENABLE_IRQ	0x82001005U
#define SMC_IPI_MAILBOX_DISABLE_IRQ	0x82001006U

/* IPI SMC Macros */
#define IPI_SMC_ENQUIRY_DIRQ_MASK	0x00000001UL /* Flag to indicate if
						      * notification interrupt
						      * to be disabled.
						      */
#define IPI_SMC_ACK_EIRQ_MASK		0x00000001UL /* Flag to indicate if
						      * notification interrupt
						      * to be enabled.
						      */

/* IPI mailbox status */
#define IPI_MB_STATUS_IDLE		0
#define IPI_MB_STATUS_SEND_PENDING	1
#define IPI_MB_STATUS_RECV_PENDING	2

#define IPI_MB_CHNL_TX	0 /* IPI mailbox TX channel */
#define IPI_MB_CHNL_RX	1 /* IPI mailbox RX channel */

/* IPI Message Buffer Information */
#define RESP_OFFSET	0x20U
#define DEST_OFFSET	0x40U
#define IPI_BUF_SIZE	0x20U
#define DST_BIT_POS	9U
#define SRC_BITMASK	GENMASK(11, 8)

#define MAX_SGI 16

/*
 * Module parameters
 */
static int tx_poll_period = 5;
module_param_named(tx_poll_period, tx_poll_period, int, 0644);
MODULE_PARM_DESC(tx_poll_period, "Poll period waiting for ack after send.");

/**
 * struct zynqmp_ipi_mchan - Description of a Xilinx ZynqMP IPI mailbox channel
 * @is_opened: indicate if the IPI channel is opened
 * @req_buf: local to remote request buffer start address
 * @resp_buf: local to remote response buffer start address
 * @req_buf_size: request buffer size
 * @resp_buf_size: response buffer size
 * @rx_buf: receive buffer to pass received message to client
 * @chan_type: channel type
 */
struct zynqmp_ipi_mchan {
	int is_opened;
	void __iomem *req_buf;
	void __iomem *resp_buf;
	void *rx_buf;
	size_t req_buf_size;
	size_t resp_buf_size;
	unsigned int chan_type;
};

struct zynqmp_ipi_mbox;

typedef int (*setup_ipi_fn)(struct zynqmp_ipi_mbox *ipi_mbox, struct device_node *node);

/**
 * struct zynqmp_ipi_mbox - Description of a ZynqMP IPI mailbox
 *                          platform data.
 * @pdata:		  pointer to the IPI private data
 * @dev:                  device pointer corresponding to the Xilinx ZynqMP
 *                        IPI mailbox
 * @remote_id:            remote IPI agent ID
 * @mbox:                 mailbox Controller
 * @mchans:               array for channels, tx channel and rx channel.
 * @setup_ipi_fn:         Function Pointer to set up IPI Channels
 */
struct zynqmp_ipi_mbox {
	struct zynqmp_ipi_pdata *pdata;
	struct device dev;
	u32 remote_id;
	struct mbox_controller mbox;
	struct zynqmp_ipi_mchan mchans[2];
	setup_ipi_fn setup_ipi_fn;
};

/**
 * struct zynqmp_ipi_pdata - Description of z ZynqMP IPI agent platform data.
 *
 * @dev:                  device pointer corresponding to the Xilinx ZynqMP
 *                        IPI agent
 * @irq:                  IPI agent interrupt ID
 * @method:               IPI SMC or HVC is going to be used
 * @local_id:             local IPI agent ID
 * @virq_sgi:             IRQ number mapped to SGI
 * @num_mboxes:           number of mailboxes of this IPI agent
 * @ipi_mboxes:           IPI mailboxes of this IPI agent
 */
struct zynqmp_ipi_pdata {
	struct device *dev;
	int irq;
	unsigned int method;
	u32 local_id;
	int virq_sgi;
	int num_mboxes;
	struct zynqmp_ipi_mbox ipi_mboxes[] __counted_by(num_mboxes);
};

static DEFINE_PER_CPU(struct zynqmp_ipi_pdata *, per_cpu_pdata);

static struct device_driver zynqmp_ipi_mbox_driver = {
	.owner = THIS_MODULE,
	.name = "zynqmp-ipi-mbox",
};

static void zynqmp_ipi_fw_call(struct zynqmp_ipi_mbox *ipi_mbox,
			       unsigned long a0, unsigned long a3,
			       struct arm_smccc_res *res)
{
	struct zynqmp_ipi_pdata *pdata = ipi_mbox->pdata;
	unsigned long a1, a2;

	a1 = pdata->local_id;
	a2 = ipi_mbox->remote_id;
	if (pdata->method == USE_SMC)
		arm_smccc_smc(a0, a1, a2, a3, 0, 0, 0, 0, res);
	else
		arm_smccc_hvc(a0, a1, a2, a3, 0, 0, 0, 0, res);
}

/**
 * zynqmp_ipi_interrupt - Interrupt handler for IPI notification
 *
 * @irq:  Interrupt number
 * @data: ZynqMP IPI mailbox platform data.
 *
 * Return: -EINVAL if there is no instance
 * IRQ_NONE if the interrupt is not ours.
 * IRQ_HANDLED if the rx interrupt was successfully handled.
 */
static irqreturn_t zynqmp_ipi_interrupt(int irq, void *data)
{
	struct zynqmp_ipi_pdata *pdata = data;
	struct mbox_chan *chan;
	struct zynqmp_ipi_mbox *ipi_mbox;
	struct zynqmp_ipi_mchan *mchan;
	struct zynqmp_ipi_message *msg;
	u64 arg0, arg3;
	struct arm_smccc_res res;
	int ret, i, status = IRQ_NONE;

	(void)irq;
	arg0 = SMC_IPI_MAILBOX_STATUS_ENQUIRY;
	arg3 = IPI_SMC_ENQUIRY_DIRQ_MASK;
	for (i = 0; i < pdata->num_mboxes; i++) {
		ipi_mbox = &pdata->ipi_mboxes[i];
		mchan = &ipi_mbox->mchans[IPI_MB_CHNL_RX];
		chan = &ipi_mbox->mbox.chans[IPI_MB_CHNL_RX];
		zynqmp_ipi_fw_call(ipi_mbox, arg0, arg3, &res);
		ret = (int)(res.a0 & 0xFFFFFFFF);
		if (ret > 0 && ret & IPI_MB_STATUS_RECV_PENDING) {
			if (mchan->is_opened) {
				msg = mchan->rx_buf;
				if (msg) {
					msg->len = mchan->req_buf_size;
					memcpy_fromio(msg->data, mchan->req_buf,
						      msg->len);
				}
				mbox_chan_received_data(chan, (void *)msg);
				status = IRQ_HANDLED;
			}
		}
	}
	return status;
}

static irqreturn_t zynqmp_sgi_interrupt(int irq, void *data)
{
	struct zynqmp_ipi_pdata **pdata_ptr = data;
	struct zynqmp_ipi_pdata *pdata = *pdata_ptr;

	return zynqmp_ipi_interrupt(irq, pdata);
}

/**
 * zynqmp_ipi_peek_data - Peek to see if there are any rx messages.
 *
 * @chan: Channel Pointer
 *
 * Return: 'true' if there is pending rx data, 'false' if there is none.
 */
static bool zynqmp_ipi_peek_data(struct mbox_chan *chan)
{
	struct device *dev = chan->mbox->dev;
	struct zynqmp_ipi_mbox *ipi_mbox = dev_get_drvdata(dev);
	struct zynqmp_ipi_mchan *mchan = chan->con_priv;
	int ret;
	u64 arg0;
	struct arm_smccc_res res;

	if (WARN_ON(!ipi_mbox)) {
		dev_err(dev, "no platform drv data??\n");
		return false;
	}

	arg0 = SMC_IPI_MAILBOX_STATUS_ENQUIRY;
	zynqmp_ipi_fw_call(ipi_mbox, arg0, 0, &res);
	ret = (int)(res.a0 & 0xFFFFFFFF);

	if (mchan->chan_type == IPI_MB_CHNL_TX) {
		/* TX channel, check if the message has been acked
		 * by the remote, if yes, response is available.
		 */
		if (ret < 0 || ret & IPI_MB_STATUS_SEND_PENDING)
			return false;
		else
			return true;
	} else if (ret > 0 && ret & IPI_MB_STATUS_RECV_PENDING) {
		/* RX channel, check if there is message arrived. */
		return true;
	}
	return false;
}

/**
 * zynqmp_ipi_last_tx_done - See if the last tx message is sent
 *
 * @chan: Channel pointer
 *
 * Return: 'true' is no pending tx data, 'false' if there are any.
 */
static bool zynqmp_ipi_last_tx_done(struct mbox_chan *chan)
{
	struct device *dev = chan->mbox->dev;
	struct zynqmp_ipi_mbox *ipi_mbox = dev_get_drvdata(dev);
	struct zynqmp_ipi_mchan *mchan = chan->con_priv;
	int ret;
	u64 arg0;
	struct arm_smccc_res res;

	if (WARN_ON(!ipi_mbox)) {
		dev_err(dev, "no platform drv data??\n");
		return false;
	}

	if (mchan->chan_type == IPI_MB_CHNL_TX) {
		/* We only need to check if the message been taken
		 * by the remote in the TX channel
		 */
		arg0 = SMC_IPI_MAILBOX_STATUS_ENQUIRY;
		zynqmp_ipi_fw_call(ipi_mbox, arg0, 0, &res);
		/* Check the SMC call status, a0 of the result */
		ret = (int)(res.a0 & 0xFFFFFFFF);
		if (ret < 0 || ret & IPI_MB_STATUS_SEND_PENDING)
			return false;
		return true;
	}
	/* Always true for the response message in RX channel */
	return true;
}

/**
 * zynqmp_ipi_send_data - Send data
 *
 * @chan: Channel Pointer
 * @data: Message Pointer
 *
 * Return: 0 if all goes good, else appropriate error messages.
 */
static int zynqmp_ipi_send_data(struct mbox_chan *chan, void *data)
{
	struct device *dev = chan->mbox->dev;
	struct zynqmp_ipi_mbox *ipi_mbox = dev_get_drvdata(dev);
	struct zynqmp_ipi_mchan *mchan = chan->con_priv;
	struct zynqmp_ipi_message *msg = data;
	u64 arg0;
	struct arm_smccc_res res;

	if (WARN_ON(!ipi_mbox)) {
		dev_err(dev, "no platform drv data??\n");
		return -EINVAL;
	}

	if (mchan->chan_type == IPI_MB_CHNL_TX) {
		/* Send request message */
		if (msg && msg->len > mchan->req_buf_size && mchan->req_buf) {
			dev_err(dev, "channel %d message length %u > max %lu\n",
				mchan->chan_type, (unsigned int)msg->len,
				mchan->req_buf_size);
			return -EINVAL;
		}
		if (msg && msg->len && mchan->req_buf)
			memcpy_toio(mchan->req_buf, msg->data, msg->len);
		/* Kick IPI mailbox to send message */
		arg0 = SMC_IPI_MAILBOX_NOTIFY;
		zynqmp_ipi_fw_call(ipi_mbox, arg0, 0, &res);
	} else {
		/* Send response message */
		if (msg && msg->len > mchan->resp_buf_size && mchan->resp_buf) {
			dev_err(dev, "channel %d message length %u > max %lu\n",
				mchan->chan_type, (unsigned int)msg->len,
				mchan->resp_buf_size);
			return -EINVAL;
		}
		if (msg && msg->len && mchan->resp_buf)
			memcpy_toio(mchan->resp_buf, msg->data, msg->len);
		arg0 = SMC_IPI_MAILBOX_ACK;
		zynqmp_ipi_fw_call(ipi_mbox, arg0, IPI_SMC_ACK_EIRQ_MASK,
				   &res);
	}
	return 0;
}

/**
 * zynqmp_ipi_startup - Startup the IPI channel
 *
 * @chan: Channel pointer
 *
 * Return: 0 if all goes good, else return corresponding error message
 */
static int zynqmp_ipi_startup(struct mbox_chan *chan)
{
	struct device *dev = chan->mbox->dev;
	struct zynqmp_ipi_mbox *ipi_mbox = dev_get_drvdata(dev);
	struct zynqmp_ipi_mchan *mchan = chan->con_priv;
	u64 arg0;
	struct arm_smccc_res res;
	int ret = 0;
	unsigned int nchan_type;

	if (mchan->is_opened)
		return 0;

	/* If no channel has been opened, open the IPI mailbox */
	nchan_type = (mchan->chan_type + 1) % 2;
	if (!ipi_mbox->mchans[nchan_type].is_opened) {
		arg0 = SMC_IPI_MAILBOX_OPEN;
		zynqmp_ipi_fw_call(ipi_mbox, arg0, 0, &res);
		/* Check the SMC call status, a0 of the result */
		ret = (int)(res.a0 & 0xFFFFFFFF);
		if (ret < 0) {
			dev_err(dev, "SMC to open the IPI channel failed.\n");
			return ret;
		}
		ret = 0;
	}

	/* If it is RX channel, enable the IPI notification interrupt */
	if (mchan->chan_type == IPI_MB_CHNL_RX) {
		arg0 = SMC_IPI_MAILBOX_ENABLE_IRQ;
		zynqmp_ipi_fw_call(ipi_mbox, arg0, 0, &res);
	}
	mchan->is_opened = 1;

	return ret;
}

/**
 * zynqmp_ipi_shutdown - Shutdown the IPI channel
 *
 * @chan: Channel pointer
 */
static void zynqmp_ipi_shutdown(struct mbox_chan *chan)
{
	struct device *dev = chan->mbox->dev;
	struct zynqmp_ipi_mbox *ipi_mbox = dev_get_drvdata(dev);
	struct zynqmp_ipi_mchan *mchan = chan->con_priv;
	u64 arg0;
	struct arm_smccc_res res;
	unsigned int chan_type;

	if (!mchan->is_opened)
		return;

	/* If it is RX channel, disable notification interrupt */
	chan_type = mchan->chan_type;
	if (chan_type == IPI_MB_CHNL_RX) {
		arg0 = SMC_IPI_MAILBOX_DISABLE_IRQ;
		zynqmp_ipi_fw_call(ipi_mbox, arg0, 0, &res);
	}
	/* Release IPI mailbox if no other channel is opened */
	chan_type = (chan_type + 1) % 2;
	if (!ipi_mbox->mchans[chan_type].is_opened) {
		arg0 = SMC_IPI_MAILBOX_RELEASE;
		zynqmp_ipi_fw_call(ipi_mbox, arg0, 0, &res);
	}

	mchan->is_opened = 0;
}

/* ZynqMP IPI mailbox operations */
static const struct mbox_chan_ops zynqmp_ipi_chan_ops = {
	.startup = zynqmp_ipi_startup,
	.shutdown = zynqmp_ipi_shutdown,
	.peek_data = zynqmp_ipi_peek_data,
	.last_tx_done = zynqmp_ipi_last_tx_done,
	.send_data = zynqmp_ipi_send_data,
};

/**
 * zynqmp_ipi_of_xlate - Translate of phandle to IPI mailbox channel
 *
 * @mbox: mailbox controller pointer
 * @p:    phandle pointer
 *
 * Return: Mailbox channel, else return error pointer.
 */
static struct mbox_chan *zynqmp_ipi_of_xlate(struct mbox_controller *mbox,
					     const struct of_phandle_args *p)
{
	struct mbox_chan *chan;
	struct device *dev = mbox->dev;
	unsigned int chan_type;

	/* Only supports TX and RX channels */
	chan_type = p->args[0];
	if (chan_type != IPI_MB_CHNL_TX && chan_type != IPI_MB_CHNL_RX) {
		dev_err(dev, "req chnl failure: invalid chnl type %u.\n",
			chan_type);
		return ERR_PTR(-EINVAL);
	}
	chan = &mbox->chans[chan_type];
	return chan;
}

/**
 * zynqmp_ipi_mbox_get_buf_res - Get buffer resource from the IPI dev node
 *
 * @node: IPI mbox device child node
 * @name: name of the IPI buffer
 * @res: pointer to where the resource information will be stored.
 *
 * Return: 0 for success, negative value for failure
 */
static int zynqmp_ipi_mbox_get_buf_res(struct device_node *node,
				       const char *name,
				       struct resource *res)
{
	int ret, index;

	index = of_property_match_string(node, "reg-names", name);
	if (index >= 0) {
		ret = of_address_to_resource(node, index, res);
		if (ret < 0)
			return -EINVAL;
		return 0;
	}
	return -ENODEV;
}

/**
 * zynqmp_ipi_mbox_dev_release() - release the existence of a ipi mbox dev
 *
 * @dev: the ipi mailbox device
 *
 * This is to avoid the no device release() function kernel warning.
 *
 */
static void zynqmp_ipi_mbox_dev_release(struct device *dev)
{
	(void)dev;
}

/**
 * zynqmp_ipi_mbox_probe - probe IPI mailbox resource from device node
 *
 * @ipi_mbox: pointer to IPI mailbox private data structure
 * @node: IPI mailbox device node
 *
 * Return: 0 for success, negative value for failure
 */
static int zynqmp_ipi_mbox_probe(struct zynqmp_ipi_mbox *ipi_mbox,
				 struct device_node *node)
{
	struct mbox_chan *chans;
	struct mbox_controller *mbox;
	struct device *dev, *mdev;
	int ret;

	dev = ipi_mbox->pdata->dev;
	/* Initialize dev for IPI mailbox */
	ipi_mbox->dev.parent = dev;
	ipi_mbox->dev.release = NULL;
	ipi_mbox->dev.of_node = node;
	dev_set_name(&ipi_mbox->dev, "%s", of_node_full_name(node));
	dev_set_drvdata(&ipi_mbox->dev, ipi_mbox);
	ipi_mbox->dev.release = zynqmp_ipi_mbox_dev_release;
	ipi_mbox->dev.driver = &zynqmp_ipi_mbox_driver;
	ret = device_register(&ipi_mbox->dev);
	if (ret) {
		dev_err(dev, "Failed to register ipi mbox dev.\n");
		put_device(&ipi_mbox->dev);
		return ret;
	}
	mdev = &ipi_mbox->dev;

	/* Get the IPI remote agent ID */
	ret = of_property_read_u32(node, "xlnx,ipi-id", &ipi_mbox->remote_id);
	if (ret < 0) {
		dev_err(dev, "No IPI remote ID is specified.\n");
		return ret;
	}

	ret = ipi_mbox->setup_ipi_fn(ipi_mbox, node);
	if (ret) {
		dev_err(dev, "Failed to set up IPI Buffers.\n");
		return ret;
	}

	mbox = &ipi_mbox->mbox;
	mbox->dev = mdev;
	mbox->ops = &zynqmp_ipi_chan_ops;
	mbox->num_chans = 2;
	mbox->txdone_irq = false;
	mbox->txdone_poll = true;
	mbox->txpoll_period = tx_poll_period;
	mbox->of_xlate = zynqmp_ipi_of_xlate;
	chans = devm_kzalloc(mdev, 2 * sizeof(*chans), GFP_KERNEL);
	if (!chans)
		return -ENOMEM;
	mbox->chans = chans;
	chans[IPI_MB_CHNL_TX].con_priv = &ipi_mbox->mchans[IPI_MB_CHNL_TX];
	chans[IPI_MB_CHNL_RX].con_priv = &ipi_mbox->mchans[IPI_MB_CHNL_RX];
	ipi_mbox->mchans[IPI_MB_CHNL_TX].chan_type = IPI_MB_CHNL_TX;
	ipi_mbox->mchans[IPI_MB_CHNL_RX].chan_type = IPI_MB_CHNL_RX;
	ret = devm_mbox_controller_register(mdev, mbox);
	if (ret)
		dev_err(mdev,
			"Failed to register mbox_controller(%d)\n", ret);
	else
		dev_info(mdev,
			 "Registered ZynqMP IPI mbox with TX/RX channels.\n");
	return ret;
}

/**
 * zynqmp_ipi_setup - set up IPI Buffers for classic flow
 *
 * @ipi_mbox: pointer to IPI mailbox private data structure
 * @node: IPI mailbox device node
 *
 * This will be used to set up IPI Buffers for ZynqMP SOC if user
 * wishes to use classic driver usage model on new SOC's with only
 * buffered IPIs.
 *
 * Note that bufferless IPIs and mixed usage of buffered and bufferless
 * IPIs are not supported with this flow.
 *
 * This will be invoked with compatible string "xlnx,zynqmp-ipi-mailbox".
 *
 * Return: 0 for success, negative value for failure
 */
static int zynqmp_ipi_setup(struct zynqmp_ipi_mbox *ipi_mbox,
			    struct device_node *node)
{
	struct zynqmp_ipi_mchan *mchan;
	struct device *mdev;
	struct resource res;
	const char *name;
	int ret;

	mdev = &ipi_mbox->dev;

	mchan = &ipi_mbox->mchans[IPI_MB_CHNL_TX];
	name = "local_request_region";
	ret = zynqmp_ipi_mbox_get_buf_res(node, name, &res);
	if (!ret) {
		mchan->req_buf_size = resource_size(&res);
		mchan->req_buf = devm_ioremap(mdev, res.start,
					      mchan->req_buf_size);
		if (!mchan->req_buf) {
			dev_err(mdev, "Unable to map IPI buffer I/O memory\n");
			return -ENOMEM;
		}
	} else if (ret != -ENODEV) {
		dev_err(mdev, "Unmatched resource %s, %d.\n", name, ret);
		return ret;
	}

	name = "remote_response_region";
	ret = zynqmp_ipi_mbox_get_buf_res(node, name, &res);
	if (!ret) {
		mchan->resp_buf_size = resource_size(&res);
		mchan->resp_buf = devm_ioremap(mdev, res.start,
					       mchan->resp_buf_size);
		if (!mchan->resp_buf) {
			dev_err(mdev, "Unable to map IPI buffer I/O memory\n");
			return -ENOMEM;
		}
	} else if (ret != -ENODEV) {
		dev_err(mdev, "Unmatched resource %s.\n", name);
		return ret;
	}
	mchan->rx_buf = devm_kzalloc(mdev,
				     mchan->resp_buf_size +
				     sizeof(struct zynqmp_ipi_message),
				     GFP_KERNEL);
	if (!mchan->rx_buf)
		return -ENOMEM;

	mchan = &ipi_mbox->mchans[IPI_MB_CHNL_RX];
	name = "remote_request_region";
	ret = zynqmp_ipi_mbox_get_buf_res(node, name, &res);
	if (!ret) {
		mchan->req_buf_size = resource_size(&res);
		mchan->req_buf = devm_ioremap(mdev, res.start,
					      mchan->req_buf_size);
		if (!mchan->req_buf) {
			dev_err(mdev, "Unable to map IPI buffer I/O memory\n");
			return -ENOMEM;
		}
	} else if (ret != -ENODEV) {
		dev_err(mdev, "Unmatched resource %s.\n", name);
		return ret;
	}

	name = "local_response_region";
	ret = zynqmp_ipi_mbox_get_buf_res(node, name, &res);
	if (!ret) {
		mchan->resp_buf_size = resource_size(&res);
		mchan->resp_buf = devm_ioremap(mdev, res.start,
					       mchan->resp_buf_size);
		if (!mchan->resp_buf) {
			dev_err(mdev, "Unable to map IPI buffer I/O memory\n");
			return -ENOMEM;
		}
	} else if (ret != -ENODEV) {
		dev_err(mdev, "Unmatched resource %s.\n", name);
		return ret;
	}
	mchan->rx_buf = devm_kzalloc(mdev,
				     mchan->resp_buf_size +
				     sizeof(struct zynqmp_ipi_message),
				     GFP_KERNEL);
	if (!mchan->rx_buf)
		return -ENOMEM;

	return 0;
}

/**
 * versal_ipi_setup - Set up IPIs to support mixed usage of
 *				 Buffered and Bufferless IPIs.
 *
 * @ipi_mbox: pointer to IPI mailbox private data structure
 * @node: IPI mailbox device node
 *
 * Return: 0 for success, negative value for failure
 */
static int versal_ipi_setup(struct zynqmp_ipi_mbox *ipi_mbox,
			    struct device_node *node)
{
	struct zynqmp_ipi_mchan *tx_mchan, *rx_mchan;
	struct resource host_res, remote_res;
	struct device_node *parent_node;
	int host_idx, remote_idx;
	struct device *mdev;

	tx_mchan = &ipi_mbox->mchans[IPI_MB_CHNL_TX];
	rx_mchan = &ipi_mbox->mchans[IPI_MB_CHNL_RX];
	parent_node = of_get_parent(node);
	mdev = &ipi_mbox->dev;

	host_idx = zynqmp_ipi_mbox_get_buf_res(parent_node, "msg", &host_res);
	remote_idx = zynqmp_ipi_mbox_get_buf_res(node, "msg", &remote_res);

	/*
	 * Only set up buffers if both sides claim to have msg buffers.
	 * This is because each buffered IPI's corresponding msg buffers
	 * are reserved for use by other buffered IPI's.
	 */
	if (!host_idx && !remote_idx) {
		u32 host_src, host_dst, remote_src, remote_dst;
		u32 buff_sz;

		buff_sz = resource_size(&host_res);

		host_src = host_res.start & SRC_BITMASK;
		remote_src = remote_res.start & SRC_BITMASK;

		host_dst = (host_src >> DST_BIT_POS) * DEST_OFFSET;
		remote_dst = (remote_src >> DST_BIT_POS) * DEST_OFFSET;

		/* Validate that IPI IDs is within IPI Message buffer space. */
		if (host_dst >= buff_sz || remote_dst >= buff_sz) {
			dev_err(mdev,
				"Invalid IPI Message buffer values: %x %x\n",
				host_dst, remote_dst);
			return -EINVAL;
		}

		tx_mchan->req_buf = devm_ioremap(mdev,
						 host_res.start | remote_dst,
						 IPI_BUF_SIZE);
		if (!tx_mchan->req_buf) {
			dev_err(mdev, "Unable to map IPI buffer I/O memory\n");
			return -ENOMEM;
		}

		tx_mchan->resp_buf = devm_ioremap(mdev,
						  (remote_res.start | host_dst) +
						  RESP_OFFSET, IPI_BUF_SIZE);
		if (!tx_mchan->resp_buf) {
			dev_err(mdev, "Unable to map IPI buffer I/O memory\n");
			return -ENOMEM;
		}

		rx_mchan->req_buf = devm_ioremap(mdev,
						 remote_res.start | host_dst,
						 IPI_BUF_SIZE);
		if (!rx_mchan->req_buf) {
			dev_err(mdev, "Unable to map IPI buffer I/O memory\n");
			return -ENOMEM;
		}

		rx_mchan->resp_buf = devm_ioremap(mdev,
						  (host_res.start | remote_dst) +
						  RESP_OFFSET, IPI_BUF_SIZE);
		if (!rx_mchan->resp_buf) {
			dev_err(mdev, "Unable to map IPI buffer I/O memory\n");
			return -ENOMEM;
		}

		tx_mchan->resp_buf_size = IPI_BUF_SIZE;
		tx_mchan->req_buf_size = IPI_BUF_SIZE;
		tx_mchan->rx_buf = devm_kzalloc(mdev, IPI_BUF_SIZE +
						sizeof(struct zynqmp_ipi_message),
						GFP_KERNEL);
		if (!tx_mchan->rx_buf)
			return -ENOMEM;

		rx_mchan->resp_buf_size = IPI_BUF_SIZE;
		rx_mchan->req_buf_size = IPI_BUF_SIZE;
		rx_mchan->rx_buf = devm_kzalloc(mdev, IPI_BUF_SIZE +
						sizeof(struct zynqmp_ipi_message),
						GFP_KERNEL);
		if (!rx_mchan->rx_buf)
			return -ENOMEM;
	}

	return 0;
}

static int xlnx_mbox_cpuhp_start(unsigned int cpu)
{
	struct zynqmp_ipi_pdata *pdata;

	pdata = get_cpu_var(per_cpu_pdata);
	put_cpu_var(per_cpu_pdata);
	enable_percpu_irq(pdata->virq_sgi, IRQ_TYPE_NONE);

	return 0;
}

static int xlnx_mbox_cpuhp_down(unsigned int cpu)
{
	struct zynqmp_ipi_pdata *pdata;

	pdata = get_cpu_var(per_cpu_pdata);
	put_cpu_var(per_cpu_pdata);
	disable_percpu_irq(pdata->virq_sgi);

	return 0;
}

static void xlnx_disable_percpu_irq(void *data)
{
	struct zynqmp_ipi_pdata *pdata;

	pdata = *this_cpu_ptr(&per_cpu_pdata);

	disable_percpu_irq(pdata->virq_sgi);
}

static int xlnx_mbox_init_sgi(struct platform_device *pdev,
			      int sgi_num,
			      struct zynqmp_ipi_pdata *pdata)
{
	int ret = 0;
	int cpu;

	/*
	 * IRQ related structures are used for the following:
	 * for each SGI interrupt ensure its mapped by GIC IRQ domain
	 * and that each corresponding linux IRQ for the HW IRQ has
	 * a handler for when receiving an interrupt from the remote
	 * processor.
	 */
	struct irq_domain *domain;
	struct irq_fwspec sgi_fwspec;
	struct device_node *interrupt_parent = NULL;
	struct device *dev = &pdev->dev;

	/* Find GIC controller to map SGIs. */
	interrupt_parent = of_irq_find_parent(dev->of_node);
	if (!interrupt_parent) {
		dev_err(&pdev->dev, "Failed to find property for Interrupt parent\n");
		return -EINVAL;
	}

	/* Each SGI needs to be associated with GIC's IRQ domain. */
	domain = irq_find_host(interrupt_parent);
	of_node_put(interrupt_parent);

	/* Each mapping needs GIC domain when finding IRQ mapping. */
	sgi_fwspec.fwnode = domain->fwnode;

	/*
	 * When irq domain looks at mapping each arg is as follows:
	 * 3 args for: interrupt type (SGI), interrupt # (set later), type
	 */
	sgi_fwspec.param_count = 1;

	/* Set SGI's hwirq */
	sgi_fwspec.param[0] = sgi_num;
	pdata->virq_sgi = irq_create_fwspec_mapping(&sgi_fwspec);

	for_each_possible_cpu(cpu)
		per_cpu(per_cpu_pdata, cpu) = pdata;

	ret = request_percpu_irq(pdata->virq_sgi, zynqmp_sgi_interrupt, pdev->name,
				 &per_cpu_pdata);
	WARN_ON(ret);
	if (ret) {
		irq_dispose_mapping(pdata->virq_sgi);
		return ret;
	}

	irq_set_status_flags(pdata->virq_sgi, IRQ_PER_CPU);

	/* Setup function for the CPU hot-plug cases */
	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "mailbox/sgi:starting",
			  xlnx_mbox_cpuhp_start, xlnx_mbox_cpuhp_down);

	return ret;
}

static void xlnx_mbox_cleanup_sgi(struct zynqmp_ipi_pdata *pdata)
{
	cpuhp_remove_state(CPUHP_AP_ONLINE_DYN);

	on_each_cpu(xlnx_disable_percpu_irq, NULL, 1);

	irq_clear_status_flags(pdata->virq_sgi, IRQ_PER_CPU);
	free_percpu_irq(pdata->virq_sgi, &per_cpu_pdata);
	irq_dispose_mapping(pdata->virq_sgi);
}

/**
 * zynqmp_ipi_free_mboxes - Free IPI mailboxes devices
 *
 * @pdata: IPI private data
 */
static void zynqmp_ipi_free_mboxes(struct zynqmp_ipi_pdata *pdata)
{
	struct zynqmp_ipi_mbox *ipi_mbox;
	int i;

	if (pdata->irq < MAX_SGI)
		xlnx_mbox_cleanup_sgi(pdata);

	i = pdata->num_mboxes;
	for (; i >= 0; i--) {
		ipi_mbox = &pdata->ipi_mboxes[i];
		if (ipi_mbox->dev.parent) {
			mbox_controller_unregister(&ipi_mbox->mbox);
			if (device_is_registered(&ipi_mbox->dev))
				device_unregister(&ipi_mbox->dev);
		}
	}
}

static int zynqmp_ipi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *nc, *np = pdev->dev.of_node;
	struct zynqmp_ipi_pdata *pdata;
	struct of_phandle_args out_irq;
	struct zynqmp_ipi_mbox *mbox;
	int num_mboxes, ret = -EINVAL;
	setup_ipi_fn ipi_fn;

	num_mboxes = of_get_available_child_count(np);
	if (num_mboxes == 0) {
		dev_err(dev, "mailbox nodes not available\n");
		return -EINVAL;
	}

	pdata = devm_kzalloc(dev, struct_size(pdata, ipi_mboxes, num_mboxes),
			     GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;
	pdata->dev = dev;

	/* Get the IPI local agents ID */
	ret = of_property_read_u32(np, "xlnx,ipi-id", &pdata->local_id);
	if (ret < 0) {
		dev_err(dev, "No IPI local ID is specified.\n");
		return ret;
	}

	ipi_fn = (setup_ipi_fn)device_get_match_data(&pdev->dev);
	if (!ipi_fn) {
		dev_err(dev,
			"Mbox Compatible String is missing IPI Setup fn.\n");
		return -ENODEV;
	}

	pdata->num_mboxes = num_mboxes;

	mbox = pdata->ipi_mboxes;
	for_each_available_child_of_node(np, nc) {
		mbox->pdata = pdata;
		mbox->setup_ipi_fn = ipi_fn;

		ret = zynqmp_ipi_mbox_probe(mbox, nc);
		if (ret) {
			of_node_put(nc);
			dev_err(dev, "failed to probe subdev.\n");
			ret = -EINVAL;
			goto free_mbox_dev;
		}
		mbox++;
	}

	ret = of_irq_parse_one(dev_of_node(dev), 0, &out_irq);
	if (ret < 0) {
		dev_err(dev, "failed to parse interrupts\n");
		goto free_mbox_dev;
	}
	ret = out_irq.args[1];

	/*
	 * If Interrupt number is in SGI range, then request SGI else request
	 * IPI system IRQ.
	 */
	if (ret < MAX_SGI) {
		pdata->irq = ret;
		ret = xlnx_mbox_init_sgi(pdev, pdata->irq, pdata);
		if (ret)
			goto free_mbox_dev;
	} else {
		ret = platform_get_irq(pdev, 0);
		if (ret < 0)
			goto free_mbox_dev;

		pdata->irq = ret;
		ret = devm_request_irq(dev, pdata->irq, zynqmp_ipi_interrupt,
				       IRQF_SHARED, dev_name(dev), pdata);
	}

	if (ret) {
		dev_err(dev, "IRQ %d is not requested successfully.\n",
			pdata->irq);
		goto free_mbox_dev;
	}

	platform_set_drvdata(pdev, pdata);
	return ret;

free_mbox_dev:
	zynqmp_ipi_free_mboxes(pdata);
	return ret;
}

static void zynqmp_ipi_remove(struct platform_device *pdev)
{
	struct zynqmp_ipi_pdata *pdata;

	pdata = platform_get_drvdata(pdev);
	zynqmp_ipi_free_mboxes(pdata);
}

static const struct of_device_id zynqmp_ipi_of_match[] = {
	{ .compatible = "xlnx,zynqmp-ipi-mailbox",
	  .data = &zynqmp_ipi_setup,
	},
	{ .compatible = "xlnx,versal-ipi-mailbox",
	  .data = &versal_ipi_setup,
	},
	{},
};
MODULE_DEVICE_TABLE(of, zynqmp_ipi_of_match);

static struct platform_driver zynqmp_ipi_driver = {
	.probe = zynqmp_ipi_probe,
	.remove_new = zynqmp_ipi_remove,
	.driver = {
		   .name = "zynqmp-ipi",
		   .of_match_table = of_match_ptr(zynqmp_ipi_of_match),
	},
};

static int __init zynqmp_ipi_init(void)
{
	return platform_driver_register(&zynqmp_ipi_driver);
}
subsys_initcall(zynqmp_ipi_init);

static void __exit zynqmp_ipi_exit(void)
{
	platform_driver_unregister(&zynqmp_ipi_driver);
}
module_exit(zynqmp_ipi_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Xilinx ZynqMP IPI Mailbox driver");
MODULE_AUTHOR("Xilinx Inc.");
