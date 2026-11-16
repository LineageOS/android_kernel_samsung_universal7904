// SPDX-License-Identifier: GPL-2.0
/*
 * Secure RPMB proxy for Exynos eMMC.
 *
 * The secure world (Kinibi on m20lte) cannot drive the eMMC itself. It
 * puts an RPMB request in a shared buffer and raises an interrupt; this
 * driver runs the RPMB transfer and reports the result in the buffer.
 * Ported from the 4.4 Samsung driver (Yongtaek Kwon); the transfer now
 * goes through mmc_blk_srpmb_xfer() instead of a block_device_operations
 * hook.
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/smc.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

#define SMC_SRPMB_WSM			0x82003811

#define GET_WRITE_COUNTER		1
#define WRITE_DATA			2
#define READ_DATA			3

#define RESULT_READ_REQ			0x0005
#define RPMB_PACKET_SIZE		512
#define RPMB_REQRES			510
#define RPMB_RESULT			508

#define WRITE_COUNTER_SECURITY_OUT_ERROR	0x602
#define WRITE_COUNTER_SECURITY_IN_ERROR		0x603
#define WRITE_DATA_LEN_ERROR			0x604
#define WRITE_DATA_SECURITY_OUT_ERROR		0x605
#define WRITE_DATA_SECURITY_IN_ERROR		0x607
#define READ_LEN_ERROR				0x608
#define READ_DATA_SECURITY_OUT_ERROR		0x609
#define READ_DATA_SECURITY_IN_ERROR		0x60A
#define RPMB_INVALID_COMMAND			0x60B
#define RPMB_FAIL_SUSPEND_STATUS		0x60C

#define RPMB_IN_PROGRESS		0xDCDC
#define RPMB_PASSED			0xBABA

#define RPMB_BUF_MAX_SIZE		(32 * 1024)

int mmc_blk_srpmb_xfer(bool write, bool reliable, unsigned int blocks,
		       void *buf);

struct srpmb_req {
	u32 cmd;
	volatile u32 status_flag;
	u32 type;
	u32 data_len;
	u32 inlen;
	u32 outlen;
	u8 rpmb_data[];
};

struct srpmb_ctx {
	struct device *dev;
	int irq;
	struct srpmb_req *req;
	dma_addr_t wsm_phys;
	size_t wsm_size;
	struct workqueue_struct *wq;
	struct work_struct work;
	spinlock_t lock;
	struct notifier_block pm_nb;
};

static void srpmb_set_status(struct srpmb_ctx *ctx, u32 status)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);
	ctx->req->status_flag = status;
	spin_unlock_irqrestore(&ctx->lock, flags);
}

static void srpmb_log_result(struct srpmb_ctx *ctx, const char *what,
			     const u8 *f)
{
	if (f[RPMB_RESULT] || f[RPMB_RESULT + 1])
		dev_warn(ctx->dev, "%s: REQ/RES = %02x%02x, RESULT = %02x%02x\n",
			 what, f[RPMB_REQRES], f[RPMB_REQRES + 1],
			 f[RPMB_RESULT], f[RPMB_RESULT + 1]);
}

static void srpmb_access(struct srpmb_ctx *ctx)
{
	struct srpmb_req *req = ctx->req;
	u32 len = req->data_len;
	unsigned int blocks = len / RPMB_PACKET_SIZE;
	u8 *buf;
	int ret;

	/* The request comes from the secure world: only whole RPMB frames. */
	if (len > ctx->wsm_size - sizeof(*req) || len % RPMB_PACKET_SIZE) {
		srpmb_set_status(ctx, READ_LEN_ERROR);
		return;
	}

	/* The shared buffer is coherent memory; transfer through a bounce
	 * buffer the block layer can map.
	 */
	buf = kmalloc(max_t(u32, len, RPMB_PACKET_SIZE), GFP_KERNEL);
	if (!buf) {
		srpmb_set_status(ctx, RPMB_INVALID_COMMAND);
		return;
	}

	switch (req->type) {
	case GET_WRITE_COUNTER:
		memcpy(buf, req->rpmb_data, RPMB_PACKET_SIZE);
		ret = mmc_blk_srpmb_xfer(true, false, 1, buf);
		if (ret) {
			dev_err(ctx->dev, "write counter out: %d\n", ret);
			srpmb_set_status(ctx, WRITE_COUNTER_SECURITY_OUT_ERROR);
			break;
		}
		ret = mmc_blk_srpmb_xfer(false, false, 1, buf);
		if (ret) {
			dev_err(ctx->dev, "write counter in: %d\n", ret);
			srpmb_set_status(ctx, WRITE_COUNTER_SECURITY_IN_ERROR);
			break;
		}
		memcpy(req->rpmb_data, buf, RPMB_PACKET_SIZE);
		srpmb_log_result(ctx, "GET_WRITE_COUNTER", buf);
		srpmb_set_status(ctx, RPMB_PASSED);
		break;

	case WRITE_DATA:
		if (!blocks) {
			srpmb_set_status(ctx, WRITE_DATA_LEN_ERROR);
			break;
		}
		memcpy(buf, req->rpmb_data, len);
		ret = mmc_blk_srpmb_xfer(true, true, blocks, buf);
		if (ret) {
			dev_err(ctx->dev, "write data: %d\n", ret);
			srpmb_set_status(ctx, WRITE_DATA_SECURITY_OUT_ERROR);
			break;
		}
		/* result read request, type 0x0005 (big-endian at 510) */
		memset(buf, 0, RPMB_PACKET_SIZE);
		buf[RPMB_REQRES + 1] = RESULT_READ_REQ;
		ret = mmc_blk_srpmb_xfer(true, false, 1, buf);
		if (ret) {
			dev_err(ctx->dev, "result request: %d\n", ret);
			srpmb_set_status(ctx, WRITE_DATA_SECURITY_OUT_ERROR);
			break;
		}
		ret = mmc_blk_srpmb_xfer(false, false, 1, buf);
		if (ret) {
			dev_err(ctx->dev, "result read: %d\n", ret);
			srpmb_set_status(ctx, WRITE_DATA_SECURITY_IN_ERROR);
			break;
		}
		srpmb_log_result(ctx, "WRITE_DATA", buf);
		memcpy(req->rpmb_data, buf, RPMB_PACKET_SIZE);
		srpmb_set_status(ctx, RPMB_PASSED);
		break;

	case READ_DATA:
		if (!blocks) {
			srpmb_set_status(ctx, READ_LEN_ERROR);
			break;
		}
		/* The request frame must carry block count 0 for eMMC
		 * (wire bytes 506-507).
		 */
		memcpy(buf, req->rpmb_data, RPMB_PACKET_SIZE);
		buf[RPMB_PACKET_SIZE - 1 - 4] = 0;
		buf[RPMB_PACKET_SIZE - 1 - 5] = 0;
		ret = mmc_blk_srpmb_xfer(true, false, 1, buf);
		if (ret) {
			dev_err(ctx->dev, "read data out: %d\n", ret);
			srpmb_set_status(ctx, READ_DATA_SECURITY_OUT_ERROR);
			break;
		}
		ret = mmc_blk_srpmb_xfer(false, false, blocks, buf);
		if (ret) {
			dev_err(ctx->dev, "read data in: %d\n", ret);
			srpmb_set_status(ctx, READ_DATA_SECURITY_IN_ERROR);
			break;
		}
		memcpy(req->rpmb_data, buf, len);
		srpmb_log_result(ctx, "READ_DATA", buf);
		srpmb_set_status(ctx, RPMB_PASSED);
		break;

	default:
		dev_err(ctx->dev, "invalid command %#x\n", req->type);
		srpmb_set_status(ctx, RPMB_INVALID_COMMAND);
	}

	kfree(buf);
}

static void srpmb_worker(struct work_struct *work)
{
	struct srpmb_ctx *ctx = container_of(work, struct srpmb_ctx, work);

	pm_stay_awake(ctx->dev);
	dev_dbg(ctx->dev, "rpmb request type %u len %u\n",
		ctx->req->type, ctx->req->data_len);
	srpmb_access(ctx);
	pm_relax(ctx->dev);
}

static irqreturn_t srpmb_irq(int irq, void *arg)
{
	struct srpmb_ctx *ctx = arg;

	srpmb_set_status(ctx, RPMB_IN_PROGRESS);
	queue_work(ctx->wq, &ctx->work);
	return IRQ_HANDLED;
}

static int srpmb_pm_notifier(struct notifier_block *nb, unsigned long event,
			     void *unused)
{
	struct srpmb_ctx *ctx = container_of(nb, struct srpmb_ctx, pm_nb);

	switch (event) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
	case PM_RESTORE_PREPARE:
		flush_workqueue(ctx->wq);
		srpmb_set_status(ctx, RPMB_FAIL_SUSPEND_STATUS);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		srpmb_set_status(ctx, 0);
		break;
	}
	return NOTIFY_OK;
}

static int srpmb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct srpmb_ctx *ctx;
	irq_hw_number_t hwirq;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = dev;
	spin_lock_init(&ctx->lock);
	INIT_WORK(&ctx->work, srpmb_worker);

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ctx->wsm_size = sizeof(struct srpmb_req) + RPMB_BUF_MAX_SIZE;
	ctx->req = dmam_alloc_coherent(dev, ctx->wsm_size, &ctx->wsm_phys,
				       GFP_KERNEL);
	if (!ctx->req)
		return -ENOMEM;

	ctx->irq = platform_get_irq(pdev, 0);
	if (ctx->irq < 0)
		return ctx->irq;
	hwirq = irqd_to_hwirq(irq_get_irq_data(ctx->irq));

	ctx->wq = alloc_workqueue("srpmb_wq",
				  WQ_MEM_RECLAIM | WQ_UNBOUND | WQ_HIGHPRI, 1);
	if (!ctx->wq)
		return -ENOMEM;

	device_init_wakeup(dev, true);
	platform_set_drvdata(pdev, ctx);

	ret = devm_request_irq(dev, ctx->irq, srpmb_irq, 0, pdev->name, ctx);
	if (ret)
		goto err_wq;

	ctx->pm_nb.notifier_call = srpmb_pm_notifier;
	ret = register_pm_notifier(&ctx->pm_nb);
	if (ret)
		goto err_wq;

	ret = exynos_smc(SMC_SRPMB_WSM, ctx->wsm_phys, hwirq, 0);
	dev_dbg(dev, "wsm %pad hwirq %lu smc %d\n", &ctx->wsm_phys,
		 hwirq, ret);
	if (ret) {
		unregister_pm_notifier(&ctx->pm_nb);
		ret = -EIO;
		goto err_wq;
	}
	return 0;

err_wq:
	destroy_workqueue(ctx->wq);
	return ret;
}

static const struct of_device_id srpmb_of_match[] = {
	{ .compatible = "samsung,mmc-srpmb" },
	{ }
};

static struct platform_driver srpmb_driver = {
	.probe = srpmb_probe,
	.driver = {
		.name = "exynos-mmc-srpmb",
		.of_match_table = srpmb_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init srpmb_init(void)
{
	return platform_driver_register(&srpmb_driver);
}
late_initcall(srpmb_init);

MODULE_DESCRIPTION("Exynos eMMC secure RPMB proxy");
MODULE_LICENSE("GPL");
