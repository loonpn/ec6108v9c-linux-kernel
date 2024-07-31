// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon STB SoCs DMA Controller
 *
 * Copyright (c) 2024 David Yang
 */
#define DEBUG
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/string.h>

#include "virt-dma.h"

/******** hardware definitions ********/

#define DMAC_INT_STATUS		0x00  /* interrupt status */
#define DMAC_INT_TC_STATUS	0x04  /* transmission completion interrupt status */
#define DMAC_INT_TC_CLR		0x08  /* transmission completion interrupt clear */
#define DMAC_INT_ERR_STATUS	0x0c  /* error interrupt status */
#define DMAC_INT_ERR_CLR	0x10  /* error interrupt clear */
#define DMAC_RAW_INT_TC_STATUS	0x14
#define DMAC_RAW_INT_ERR_STATUS	0x18
#define DMAC_ENBLD_CHNS		0x1c  /* enabled channels */
#define DMAC_SOFT_BRST_REQ	0x20  /* software burst request */
#define DMAC_SOFT_SGLE_REQ	0x24  /* software single request */
#define DMAC_SOFT_LST_BRST_REQ	0x28  /* software last burst request */
#define DMAC_SOFT_LST_SGLE_REQ	0x2c  /* software last single request */
#define DMAC_CFG		0x30
#define  DMAC_CFG_M2			BIT(2)  /* use big endian for Master 2 */
#define  DMAC_CFG_M1			BIT(1)  /* use big endian for Master 1 */
#define  DMAC_CFG_EN			BIT(0)
#define DMAC_SYNC		0x34

#define DMAC_CHAN_SRC_ADDR(n)		(0x100 + 0x20 * (n) + 0x00)
#define DMAC_CHAN_DST_ADDR(n)		(0x100 + 0x20 * (n) + 0x04)
#define DMAC_CHAN_LLI(n)		(0x100 + 0x20 * (n) + 0x08)  /* channel link list item */
#define  DMAC_CHAN_LLI_W0			BIT(1)
#define  DMAC_CHAN_LLI_NEXT			GENMASK(31, 2)  /* addr / 4, 0: disable */
#define  DMAC_CHAN_LLI_MST			BIT(0)  /* master to be loaded, 0: 1, 1: 2 */
#define DMAC_CHAN_CTRL(n)		(0x100 + 0x20 * (n) + 0x0c)
#define  DMAC_CHAN_CTRL_INT			BIT(31)  /* interrupt for this node */
#define  DMAC_CHAN_CTRL_PROT			GENMASK(30, 28)  /* bus HPROT value */
#define   DMAC_CHAN_CTRL_PROT_CACHEABLE			BIT(30)
#define   DMAC_CHAN_CTRL_PROT_BUFFERABLE		BIT(29)
#define   DMAC_CHAN_CTRL_PROT_PRIVILEGED		BIT(28)
#define  DMAC_CHAN_CTRL_DST_INC			BIT(27)
#define  DMAC_CHAN_CTRL_SRC_INC			BIT(26)
#define  DMAC_CHAN_CTRL_DST_MST			BIT(25)
#define  DMAC_CHAN_CTRL_SRC_MST			BIT(24)
#define  DMAC_CHAN_CTRL_DST_WIDTH		GENMASK(23, 21)
#define  DMAC_CHAN_CTRL_SRC_WIDTH		GENMASK(20, 18)
#define   DMAC_WIDTH_8					0
#define   DMAC_WIDTH_16					1
#define   DMAC_WIDTH_32					2
#define  DMAC_CHAN_CTRL_DST_BSIZE		GENMASK(17, 15)
#define  DMAC_CHAN_CTRL_SRC_BSIZE		GENMASK(14, 12)
#define   DMAC_BSIZE_1					0
#define   DMAC_BSIZE_4					1
#define   DMAC_BSIZE_8					2
#define   DMAC_BSIZE_16					3
#define   DMAC_BSIZE_32					4
#define   DMAC_BSIZE_64					5
#define   DMAC_BSIZE_128				6
#define   DMAC_BSIZE_256				7
#define  DMAC_CHAN_CTRL_TX			GENMASK(11, 0)
#define DMAC_CHAN_CFG(n)		(0x100 + 0x20 * (n) + 0x10)
#define  DMAC_CHAN_CFG_W0			(GENMASK(31, 19) & BIT(10) & BIT(5))
#define  DMAC_CHAN_CFG_HALT			BIT(18)  /* halt, ignore further requests */
#define  DMAC_CHAN_CFG_ACTIVE			BIT(17)  /* active, data exists */
#define  DMAC_CHAN_CFG_LOCK			BIT(16)  /* lock bus */
#define  DMAC_CHAN_CFG_INT_TC_MASK		BIT(15)
#define  DMAC_CHAN_CFG_INT_ERR_MASK		BIT(14)
#define  DMAC_CHAN_CFG_FLOW			GENMASK(13, 11)  /* src, dst, controller */
#define   DMAC_FLOW_MMC					0  /* memory -> memory (DMAC) */
#define   DMAC_FLOW_MDC					1  /* memory -> peripheral (DMAC) */
#define   DMAC_FLOW_SMC					2  /* peripheral -> memory (DMAC) */
#define   DMAC_FLOW_SDC					3  /* peripheral -> peripheral (DMAC) */
#define   DMAC_FLOW_SDD					4  /* peripheral -> peripheral (dst) */
#define   DMAC_FLOW_MDD					5  /* memory -> peripheral (dst) */
#define   DMAC_FLOW_SMS					6  /* peripheral -> memory (src) */
#define   DMAC_FLOW_SDS					7  /* peripheral -> peripheral (src) */
#define  DMAC_CHAN_CFG_DST_PERI			GENMASK(9, 6)
#define  DMAC_CHAN_CFG_SRC_PERI			GENMASK(4, 1)
#define  DMAC_CHAN_CFG_EN			BIT(0)

#define DMAC_CHAN_NUM	4u

struct histb_dma_item {
	__le32 src_addr;
	__le32 dst_addr;
	__le32 lli;
	__le32 ctrl;
} __packed __aligned(4);

/******** driver definitions ********/

struct histb_dma_desc {
	struct virt_dma_desc vdesc;

	dma_addr_t src_addr;
	dma_addr_t dst_addr;
	u32 lli;
	u32 ctrl;

	struct histb_dma_item *list;
	dma_addr_t list_addr;
	unsigned int list_size;

	/* memset src buf */
	int *value;
	/* to free value */
	struct dma_pool *pool;
};

struct histb_dma_chan {
	void __iomem *base;
	struct device *dev;
	unsigned char id;
	/* managed DMA pool of memset srcs */
	struct dma_pool *pool;

	struct virt_dma_chan vchan;

	struct histb_dma_desc *desc;
	bool completed;
	bool error;
};

struct histb_dma_priv {
	void __iomem *base;
	struct device *dev;

	struct reset_control *rst;
	struct clk_bulk_data *clks;
	unsigned int clks_n;
	int irq;

	struct dma_device dmadev;

	struct histb_dma_chan chans[DMAC_CHAN_NUM];
};

static struct histb_dma_desc *to_histb_dma_desc(struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct histb_dma_desc, vdesc);
}

static struct histb_dma_chan *to_histb_dma_chan(struct dma_chan *dmachan)
{
	return container_of(dmachan, struct histb_dma_chan, vchan.chan);
}

/******** desc ********/

static void histb_dma_desc_free(struct histb_dma_desc *desc)
{
	if (desc->value)
		dma_pool_free(desc->pool, desc->value, desc->src_addr);

	kfree(desc);
}

static struct histb_dma_desc *
histb_dma_desc_new_memset(struct dma_pool *pool, unsigned char value)
{
	struct histb_dma_desc *desc;
	int *buf;

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;
	buf = dma_pool_alloc(pool, GFP_NOWAIT, &desc->src_addr);
	if (!buf) {
		kfree(desc);
		return NULL;
	}

	desc->value = buf;
	desc->pool = pool;

	for (int i = 0; i < sizeof(*buf); i++)
		((char *) buf)[i] = value;

	return desc;
}

static void histb_dma_vdesc_free(struct virt_dma_desc *vdesc)
{
	histb_dma_desc_free(to_histb_dma_desc(vdesc));
}

/******** channel ********/

static void
_histb_dma_chan_debug(struct device *dev, unsigned int id, unsigned int n,
		      u32 src_addr, u32 dst_addr, u32 lli, u32 ctrl)
{
	static const unsigned int width[] = { 8, 16, 32, 0 };
	static const unsigned int bsize[] = { 1, 4, 8, 16, 32, 64, 128, 256 };

	dev_dbg(dev,
		"%u: %2u %c[%lu], %08x%c @ %2u-%2u -> %08x%c @ %2u-%2u * %lu\n",
		id, n, (ctrl & DMAC_CHAN_CTRL_INT) ? 'I' : ' ',
		(ctrl & DMAC_CHAN_CTRL_PROT) >> 28,
		src_addr, (ctrl & DMAC_CHAN_CTRL_SRC_INC) ? '+' : ' ',
		width[(ctrl & DMAC_CHAN_CTRL_SRC_WIDTH) >> 18],
		bsize[(ctrl & DMAC_CHAN_CTRL_SRC_BSIZE) >> 12],
		dst_addr, (ctrl & DMAC_CHAN_CTRL_DST_INC) ? '+' : ' ',
		width[(ctrl & DMAC_CHAN_CTRL_DST_WIDTH) >> 21],
		bsize[(ctrl & DMAC_CHAN_CTRL_DST_BSIZE) >> 15],
		ctrl & DMAC_CHAN_CTRL_TX);
}

static void histb_dma_chan_debug(struct histb_dma_chan *chan)
{
	static const char * const flow[] = {
		"MMC", "MDC", "SMC", "SDC", "SDD", "MDD", "SMS", "SDS"
	};

	void __iomem *base = chan->base;
	struct device *dev = chan->dev;
	unsigned int id = chan->id;

	u32 src_addr = readl_relaxed(base + DMAC_CHAN_SRC_ADDR(id));
	u32 dst_addr = readl_relaxed(base + DMAC_CHAN_DST_ADDR(id));
	u32 lli = readl_relaxed(base + DMAC_CHAN_LLI(id));
	u32 ctrl = readl_relaxed(base + DMAC_CHAN_CTRL(id));
	u32 cfg = readl_relaxed(base + DMAC_CHAN_CFG(id));

	dev_dbg(dev, "%u: flow %s, peri %lu -> %lu, %c%c%c%c%c%c\n", id,
		flow[(cfg & DMAC_CHAN_CFG_FLOW) >> 1],
		(cfg & DMAC_CHAN_CFG_SRC_PERI) >> 6,
		(cfg & DMAC_CHAN_CFG_DST_PERI) >> 1,
		(cfg & DMAC_CHAN_CFG_HALT) ? 'H' : ' ',
		(cfg & DMAC_CHAN_CFG_ACTIVE) ? 'A' : ' ',
		(cfg & DMAC_CHAN_CFG_LOCK) ? 'L' : ' ',
		(cfg & DMAC_CHAN_CFG_INT_TC_MASK) ? 'i' : ' ',
		(cfg & DMAC_CHAN_CFG_INT_ERR_MASK) ? 'e' : ' ',
		(cfg & DMAC_CHAN_CFG_EN) ? 'E' : ' ');

	_histb_dma_chan_debug(dev, id, 0, src_addr, dst_addr, lli, ctrl);
}

static int histb_dma_chan_halt(struct histb_dma_chan *chan)
{
	void __iomem *cfg = chan->base + DMAC_CHAN_CFG(chan->id);

	u32 val;

	val = readl_relaxed(cfg);
	if (!(val & DMAC_CHAN_CFG_EN))
		return 0;

	if (!(val & DMAC_CHAN_CFG_HALT)) {
		val &= ~DMAC_CHAN_CFG_W0;
		val |= DMAC_CHAN_CFG_HALT;
		writel(val, cfg);
	}

	return readl_relaxed_poll_timeout_atomic(cfg, val,
						 !(val & DMAC_CHAN_CFG_ACTIVE),
						 10, USEC_PER_SEC);
}

static void histb_dma_chan_enable(struct histb_dma_chan *chan)
{
	void __iomem *cfg = chan->base + DMAC_CHAN_CFG(chan->id);

	u32 val;

	val = readl_relaxed(cfg);
	if (val & DMAC_CHAN_CFG_EN)
		return;

	if (val & DMAC_CHAN_CFG_HALT) {
		val &= ~DMAC_CHAN_CFG_HALT;
		writel_relaxed(val, cfg);
	}
}

static int histb_dma_chan_wait_disable(struct histb_dma_chan *chan)
{
	void __iomem *base = chan->base;
	unsigned int id = chan->id;

	u32 val;

	return readl_relaxed_poll_timeout_atomic(base + DMAC_ENBLD_CHNS, val,
						 !(val & BIT(id)),
						 10, USEC_PER_SEC);
}

/*
 * start transferring chan->desc, chan->desc.vdesc.lock should be held by caller
 */
static void histb_dma_chan_start(struct histb_dma_chan *chan)
{
	struct histb_dma_desc *desc = chan->desc;
	void __iomem *base = chan->base;
	unsigned int id = chan->id;

	u32 val;

	writel_relaxed(desc->src_addr, base + DMAC_CHAN_SRC_ADDR(id));
	writel_relaxed(desc->dst_addr, base + DMAC_CHAN_DST_ADDR(id));
	writel_relaxed(desc->lli, base + DMAC_CHAN_LLI(id));
	writel_relaxed(desc->ctrl, base + DMAC_CHAN_CTRL(id));

	// set in chan enable?
	val = readl_relaxed(base + DMAC_CHAN_CFG(id));
	val &= ~DMAC_CHAN_CFG_W0;
	val |= DMAC_CHAN_CFG_INT_TC_MASK;
	val |= DMAC_CHAN_CFG_INT_ERR_MASK;
	val &= ~DMAC_CHAN_CFG_FLOW;
	val |= DMAC_FLOW_MMC << 11;
	val |= DMAC_CHAN_CFG_EN;
	writel_relaxed(val, base + DMAC_CHAN_CFG(id));

	histb_dma_chan_debug(chan);
}

static struct dma_async_tx_descriptor *
histb_dma_chan_prep(struct histb_dma_chan *chan, struct histb_dma_desc *desc,
		    size_t len, unsigned long flags)
{
	u32 ctrl = DMAC_CHAN_CTRL_INT | DMAC_CHAN_CTRL_PROT |
		   DMAC_CHAN_CTRL_DST_INC |
		   (DMAC_WIDTH_32 << 21) | (DMAC_WIDTH_32 << 18) | (len / 4);

	dev_dbg(chan->dev, "prep dma operation on channel %d\n", chan->id);
	dev_dbg(chan->dev, "dst = %llu, src = %llu, len = %zu\n", dst, src, len);

	if (!desc->value)
		ctrl |= DMAC_CHAN_CTRL_SRC_INC;

	desc->ctrl = ctrl;

	return vchan_tx_prep(&chan->vchan, &desc->vdesc, flags);
}

static int
histb_dma_chan_init(struct histb_dma_chan *chan, struct histb_dma_priv *priv,
		    unsigned int id, struct dma_pool *pool)
{
	chan->base = priv->base;
	chan->id = id;
	chan->dev = priv->dev;
	chan->pool = pool;

	chan->vchan.desc_free = histb_dma_vdesc_free;
	vchan_init(&chan->vchan, &priv->dmadev);

	return 0;
}

/******** dma_chan ********/

static int histb_dma_alloc_chan_resources(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);

	histb_dma_chan_enable(chan);
	return 0;
}

static void histb_dma_free_chan_resources(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);

	histb_dma_chan_halt(chan);
	vchan_free_chan_resources(&chan->vchan);
}

static struct dma_async_tx_descriptor *
histb_dma_prep_dma_memcpy(struct dma_chan *dmachan, dma_addr_t dst,
			  dma_addr_t src, size_t len, unsigned long flags)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	struct histb_dma_desc *desc = kzalloc(sizeof(*desc), GFP_NOWAIT);

	if (!desc)
		return NULL;

	desc->src_addr = src;
	desc->dst_addr = dst;

	return histb_dma_chan_prep(chan, desc, len, flags);
}

static struct dma_async_tx_descriptor *
histb_dma_prep_dma_memset(struct dma_chan *dmachan, dma_addr_t dst, int value,
			  size_t len, unsigned long flags)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	struct histb_dma_desc *desc = histb_dma_desc_new_memset(chan->pool,
								value);

	if (!desc)
		return NULL;

	desc->dst_addr = dst;

	return histb_dma_chan_prep(chan, desc, len, flags);
}

static struct dma_async_tx_descriptor *
histb_dma_dma_memset_sg(struct dma_chan *dmachan, struct scatterlist *sgl,
			unsigned int sg_len, int value, unsigned long flags)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	struct histb_dma_desc *desc = histb_dma_desc_new_memset(chan->pool,
								value);

	if (!desc)
		return NULL;

	return NULL;
}

static enum dma_status
histb_dma_tx_status(struct dma_chan *dmachan, dma_cookie_t cookie,
		    struct dma_tx_state *txstate)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);

	enum dma_status status;

	status = dma_cookie_status(dmachan, cookie, txstate);
	if (status == DMA_COMPLETE)
		return status;

	return DMA_COMPLETE;
}

static int histb_dma_terminate_all(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	LIST_HEAD(list);
	unsigned long int flags;
	int ret;

	spin_lock_irqsave(&chan->vchan.lock, flags);

	ret = histb_dma_chan_halt(chan);
	if (ret < 0)
		return ret;

	if (chan->desc) {
		vchan_terminate_vdesc(&chan->desc->vdesc);
		chan->desc = NULL;
	}

	vchan_get_all_descriptors(&chan->vchan, &list);

	spin_unlock_irqrestore(&chan->vchan.lock, flags);

	vchan_dma_desc_free_list(&chan->vchan, &list);

	return 0;
}

static void histb_dma_synchronize(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	histb_dma_chan_wait_disable(chan);
	return;
}

static void histb_dma_issue_pending(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);

	unsigned long flags;

	spin_lock_irqsave(&chan->vchan.lock, flags);
	/*
	 *	chan->desc is NULL when the channel is available
	 */
	if (vchan_issue_pending(&chan->vchan) && !chan->desc) {
		pr_debug("%s %u: vchan %pK issued\n", __func__, chan->id,
			 &chan->vchan);

		struct virt_dma_desc *vnext	= vchan_next_desc(&chan->vchan);
		struct histb_dma_desc *next	= to_histb_dma_desc(vnext);
		chan->desc			= next;
		list_del(&vnext->node);
		histb_dma_chan_start(chan);
	}
	spin_unlock_irqrestore(&chan->vchan.lock, flags);
}

/******** irq ********/

static void
histb_dma_handle_chan(struct histb_dma_chan *chan)
{
	unsigned long flags;

	spin_lock_irqsave(&chan->vchan.lock, flags);

	struct histb_dma_desc *old = chan->desc;
	vchan_cookie_complete(&old->vdesc);

	struct virt_dma_desc *vnext = vchan_next_desc(&chan->vchan);
	if (!vnext) {		// no more issued descriptors
		chan->desc = NULL;
		goto end;
	}

	/*	set new descriptor	*/
	struct histb_dma_desc *next = to_histb_dma_desc(vnext);
	chan->desc = next;
	list_del(&vnext->node);

	histb_dma_chan_start(chan);	// start the new descritpor

end:
	spin_unlock_irqrestore(&chan->vchan.lock, flags);
	return;
}

static irqreturn_t histb_dma_handle(int irq, void *dev_id)
{
	struct histb_dma_priv *priv = dev_id;

	u32 stat = readl_relaxed(priv->base + DMAC_INT_STATUS);
	if (!stat)
		return IRQ_NONE;

	u32 tc = readl_relaxed(priv->base + DMAC_INT_TC_STATUS);
	if (tc)
		writel_relaxed(tc, priv->base + DMAC_INT_TC_CLR);
	u32 err = readl_relaxed(priv->base + DMAC_INT_ERR_STATUS);
	if (err)
		writel_relaxed(tc, priv->base + DMAC_INT_ERR_CLR);

	for (unsigned int i = 0; i < DMAC_CHAN_NUM; i++) {
		if (err & BIT(i))
			dev_err(priv->dev, "Channel %i transfer error\n", i);
		else if (tc & BIT(i))
			histb_dma_handle_chan(priv->chans + i);
	}

	return IRQ_HANDLED;
}

/******** device ********/

static int __maybe_unused histb_dma_suspend(struct device *dev)
{
	struct histb_dma_priv *priv = dev_get_drvdata(dev);

	int ret;

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		return ret;

	clk_bulk_unprepare(priv->clks_n, priv->clks);

	return 0;
}

static int __maybe_unused histb_dma_resume(struct device *dev)
{
	struct histb_dma_priv *priv = dev_get_drvdata(dev);
	return clk_bulk_prepare(priv->clks_n, priv->clks) ?:
	       pm_runtime_force_resume(dev);
}

static int __maybe_unused histb_dma_runtime_suspend(struct device *dev)
{
	struct histb_dma_priv *priv = dev_get_drvdata(dev);

	u32 val;

	for (unsigned int id = 0; id < DMAC_CHAN_NUM; id++) {
		val = readl_relaxed(priv->base + DMAC_CHAN_CFG(id));
		val &= ~DMAC_CHAN_CFG_EN;
		val &= ~DMAC_CHAN_CFG_W0;
		writel_relaxed(val, priv->base + DMAC_CHAN_CFG(id));
	}
	val = readl_relaxed(priv->base + DMAC_CFG);
	writel_relaxed(val & ~DMAC_CFG_EN, priv->base + DMAC_CFG);

	clk_bulk_disable(priv->clks_n, priv->clks);

	return 0;
}

static int __maybe_unused histb_dma_runtime_resume(struct device *dev)
{
	struct histb_dma_priv *priv = dev_get_drvdata(dev);

	u32 val;
	int ret;

	ret = clk_bulk_enable(priv->clks_n, priv->clks);
	if (ret)
		return ret;

	val = readl_relaxed(priv->base + DMAC_CFG);
	val |= DMAC_CFG_M2;
	val |= DMAC_CFG_M1;
	val |= DMAC_CFG_EN;
	writel_relaxed(val, priv->base + DMAC_CFG);

	val = readl_relaxed(priv->base + DMAC_CFG);
	if (!val) {
		ret = -ENODEV;
		goto err;
	}

	writel_relaxed(~0, priv->base + DMAC_INT_ERR_CLR);
	writel_relaxed(~0, priv->base + DMAC_INT_TC_CLR);

	for (unsigned int id = 0; id < DMAC_CHAN_NUM; id++) {
		val = readl_relaxed(priv->base + DMAC_CHAN_CFG(id));
		val &= ~DMAC_CHAN_CFG_EN;
		val &= ~DMAC_CHAN_CFG_W0;
		writel_relaxed(val, priv->base + DMAC_CHAN_CFG(id));
	}

	return 0;

err:
	clk_bulk_disable(priv->clks_n, priv->clks);
	return ret;
}

static const struct dev_pm_ops histb_dma_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(histb_dma_suspend, histb_dma_resume)
	SET_RUNTIME_PM_OPS(histb_dma_runtime_suspend,
			   histb_dma_runtime_resume, NULL)
};

static void histb_dma_remove(struct platform_device *pdev)
{
	struct histb_dma_priv *priv = platform_get_drvdata(pdev);
	struct device *dev = priv->dev;

	/*pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);*/
	clk_bulk_disable_unprepare(priv->clks_n, priv->clks);
	reset_control_assert(priv->rst);
}

static int histb_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	struct histb_dma_priv *priv;
	struct dma_device *dmadev;
	struct dma_pool *pool;
	u32 val;
	int ret;

	/* acquire resources */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	pool = dmam_pool_create("histb-dma-memset-src", dev, sizeof(int),
				sizeof(int), 0);
	if (!pool)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	ret = devm_clk_bulk_get_all(dev, &priv->clks);
	if (ret < 0)
		return ret;
	priv->clks_n = ret;

	priv->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(priv->rst))
		return PTR_ERR(priv->rst);

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	priv->irq = ret;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);
	dev_set_drvdata(dev, priv);

	/* bring up device */
	ret = reset_control_assert(priv->rst);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(priv->clks_n, priv->clks);
	if (ret)
		goto err_rst;
	ret = reset_control_deassert(priv->rst);
	if (ret)
		goto err_clk;

	val = readl_relaxed(priv->base + DMAC_CFG);
	if (!val) {
		writel_relaxed(val | DMAC_CFG_EN, priv->base + DMAC_CFG);
		val = readl_relaxed(priv->base + DMAC_CFG);
		if (!(val & DMAC_CFG_EN)) {
			dev_err(dev, "cannot bring up device\n");
			ret = -ENODEV;
			goto err_clk;
		}
	}

	/* register irq */
	ret = devm_request_irq(dev, priv->irq, histb_dma_handle,
			       IRQF_SHARED, pdev->name, priv);
	if (ret)
		goto err_clk;


	/* register dma engine */
	dmadev = &priv->dmadev;
	dma_cap_set(DMA_MEMCPY, dmadev->cap_mask);
	dma_cap_set(DMA_MEMSET, dmadev->cap_mask);
	dmadev->copy_align = DMAENGINE_ALIGN_4_BYTES;
	dmadev->fill_align = DMAENGINE_ALIGN_4_BYTES;
	dmadev->dev = dev;
	dmadev->descriptor_reuse = true;
	dmadev->directions = BIT(DMA_MEM_TO_MEM);
	dmadev->device_alloc_chan_resources = histb_dma_alloc_chan_resources;
	dmadev->device_free_chan_resources = histb_dma_free_chan_resources;
	dmadev->device_prep_dma_memcpy = histb_dma_prep_dma_memcpy;
	dmadev->device_prep_dma_memset = histb_dma_prep_dma_memset;
	dmadev->device_prep_dma_memset_sg = histb_dma_prep_dma_memset_sg;
	dmadev->device_tx_status = histb_dma_tx_status;
	dmadev->device_terminate_all = histb_dma_terminate_all;
	dmadev->device_synchronize = histb_dma_synchronize;
	dmadev->device_issue_pending = histb_dma_issue_pending;
	INIT_LIST_HEAD(&dmadev->channels);

	for (unsigned int id = 0; id < DMAC_CHAN_NUM; id++) {
		histb_dma_chan_init(&priv->chans[id], priv, id, pool);
	}

	ret = dmaenginem_async_device_register(dmadev);
	if (ret)
		goto err_clk;

#if 0
	unsigned int *buf;
	dma_addr_t buf_addr;
	buf = dmam_alloc_attrs(dev, PAGE_SIZE, &buf_addr, GFP_KERNEL | __GFP_ZERO, 0);
	buf[0] = 0x11223344;
printk("%s %d %llx %llx\n", __func__, __LINE__, buf_addr, buf_addr + PAGE_SIZE / 2);

	writel_relaxed(buf_addr, priv->base + DMAC_CHAN_SRC_ADDR(0));
	writel_relaxed(buf_addr + PAGE_SIZE / 2, priv->base + DMAC_CHAN_DST_ADDR(0));
	writel_relaxed(0, priv->base + DMAC_CHAN_LLI(0));

	val = readl_relaxed(priv->base + DMAC_CHAN_CTRL(0));
	val |= DMAC_CHAN_CTRL_INT;
	val |= DMAC_CHAN_CTRL_PROT_CACHEABLE;
	val |= DMAC_CHAN_CTRL_PROT_BUFFERABLE;
	val |= DMAC_CHAN_CTRL_PROT_PRIVILEGED;
	//val |= DMAC_CHAN_CTRL_DST_INC;
	val |= DMAC_CHAN_CTRL_SRC_INC;
	val &= ~DMAC_CHAN_CTRL_DST_WIDTH;
	val |= DMAC_WIDTH_32 << 21;
	val &= ~DMAC_CHAN_CTRL_SRC_WIDTH;
	val |= DMAC_WIDTH_32 << 18;
	/*val &= ~DMAC_CHAN_CTRL_DST_BSIZE;
	val |= DMAC_BSIZE_1 << 15;
	val &= ~DMAC_CHAN_CTRL_SRC_BSIZE;
	val |= DMAC_BSIZE_4 << 12;*/
	val &= ~DMAC_CHAN_CTRL_TX;
	val |= 4;
	writel_relaxed(val, priv->base + DMAC_CHAN_CTRL(0));

	val = readl_relaxed(priv->base + DMAC_CHAN_CFG(0));
	val &= ~DMAC_CHAN_CFG_W0;
	val |= DMAC_CHAN_CFG_INT_TC_MASK;
	val |= DMAC_CHAN_CFG_INT_ERR_MASK;
	val &= ~DMAC_CHAN_CFG_FLOW;
	val |= DMAC_FLOW_MMC << 11;
	val |= DMAC_CHAN_CFG_EN;
	writel_relaxed(val, priv->base + DMAC_CHAN_CFG(0));
#endif // #if 0

//	pm_runtime_set_autosuspend_delay(dev, MSEC_PER_SEC);
//	pm_runtime_use_autosuspend(dev);
//	pm_runtime_set_active(dev);
//	pm_runtime_irq_safe(dev);
//	pm_runtime_enable(dev);

	return 0;

err_clk:
	clk_bulk_disable_unprepare(priv->clks_n, priv->clks);
err_rst:
	reset_control_assert(priv->rst);
	return ret;
}

static const struct of_device_id histb_dma_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-dmac", },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_dma_of_match);

static struct platform_driver histb_dma_driver = {
	.probe = histb_dma_probe,
	.remove_new = histb_dma_remove,
	.driver = {
		.name = "histb-dma",
		.of_match_table = histb_dma_of_match,
		.pm = &histb_dma_pm_ops,
	},
};

module_platform_driver(histb_dma_driver);

MODULE_DESCRIPTION("HiSilicon STB SoCs DMA Controller");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
