// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MutiCipher - cipher for multiple blocks (i.e. DMA)
 *
 * Copyright (c) 2024 David Yang
 */

#include <crypto/aes.h>
#include <crypto/des.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/crypto.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/string.h>

/******** hardware definitions ********/

#define MUC_CHAN0_DATA_OUT0	0x00  /* till 3 (0x0c) */
#define MUC_CHANn_IV_OUT0(n)	(0x10 + 0x10 * (n))  /* till 3 (0x1c) */
#define MUC_CHANn_KEY0(n)	(0x90 + 0x20 * (n))  /* till 7 (0xac) */

#define MUC_SEC_CHAN_CFG	0x824
#define  MUC_SEC_CHANn_BIT(n)		BIT(n)

#define MUC_CHAN0_CTRL		0x1000
#define MUC_CHAN0_IV_IN0	0x1004  /* till 3 (0x1010) */
#define MUC_CHAN0_DATA_IN0	0x1014  /* till 3 (0x1020) */

/* LIST is the ring buffer, consists of BUF (DMA region records) */
#define MUC_CHANn_IN_BUF_NUM(n)		(0x1000 + 0x80 * (n) + 0x00)  /* list size */
#define MUC_CHANn_IN_BUF_CNT(n)		(0x1000 + 0x80 * (n) + 0x04)  /* available, write to increase */
#define MUC_CHANn_IN_EMPTY_CNT(n)	(0x1000 + 0x80 * (n) + 0x08)  /* used, write to decrease */
#define MUC_CHANn_INT_IN_CNT_CFG(n)	(0x1000 + 0x80 * (n) + 0x0c)
#define MUC_CHANn_CTRL(n)		(0x1000 + 0x80 * (n) + 0x10)
#define MUC_CHANn_SRC_LST_ADDR(n)	(0x1000 + 0x80 * (n) + 0x14)  /* list addr */
#define MUC_CHANn_IN_AGE_TIMER(n)	(0x1000 + 0x80 * (n) + 0x18)
#define MUC_CHANn_IN_AGE_CNT(n)		(0x1000 + 0x80 * (n) + 0x1c)
#define MUC_CHANn_SRC_LST_PTR(n)	(0x1000 + 0x80 * (n) + 0x20)  /* list offset */
#define MUC_CHANn_SRC_ADDR(n)		(0x1000 + 0x80 * (n) + 0x24)  /* addr + offset */
#define MUC_CHANn_SRC_LENGTH(n)		(0x1000 + 0x80 * (n) + 0x28)  /* len - offset */
#define MUC_CHANn_IN_LEFT(n)		(0x1000 + 0x80 * (n) + 0x2c)  /* how many words left */
#define MUC_CHANn_IN_LEFT_WORD0(n)	(0x1000 + 0x80 * (n) + 0x30)  /* till 2 (0x38) */

#define MUC_CHANn_OUT_BUF_NUM(n)	(0x1000 + 0x80 * (n) + 0x3c)
#define MUC_CHANn_OUT_BUF_CNT(n)	(0x1000 + 0x80 * (n) + 0x40)
#define MUC_CHANn_OUT_FULL_CNT(n)	(0x1000 + 0x80 * (n) + 0x44)
#define MUC_CHANn_INT_OUT_CNT_CFG(n)	(0x1000 + 0x80 * (n) + 0x48)
#define MUC_CHANn_DST_LST_ADDR(n)	(0x1000 + 0x80 * (n) + 0x4c)
#define MUC_CHANn_OUT_AGE_TIMER(n)	(0x1000 + 0x80 * (n) + 0x50)
#define MUC_CHANn_OUT_AGE_CNT(n)	(0x1000 + 0x80 * (n) + 0x54)
#define MUC_CHANn_DST_LST_PTR(n)	(0x1000 + 0x80 * (n) + 0x58)
#define MUC_CHANn_DST_ADDR(n)		(0x1000 + 0x80 * (n) + 0x5c)
#define MUC_CHANn_DST_LENGTH(n)		(0x1000 + 0x80 * (n) + 0x60)
#define MUC_CHANn_OUT_LEFT(n)		(0x1000 + 0x80 * (n) + 0x64)
#define MUC_CHANn_OUT_LEFT_WORD0(n)	(0x1000 + 0x80 * (n) + 0x68)  /* till 2 (0x70) */

/* for MUC_CHAN{0,n}_CTRL reg */
#define MUC_CTRL_DECRYPT	BIT(0)
#define MUC_CTRL_MODE		GENMASK(3, 1)  /* other: as 0 */
#define  MUC_MODE_ECB			0
#define  MUC_MODE_CBC			1
#define  MUC_MODE_CFB			2
#define  MUC_MODE_OFB			3
#define  MUC_MODE_CTR			4  /* not for DES */
#define MUC_CTRL_ALG		GENMASK(5, 4)  /* other: as 0 */
#define  MUC_ALG_DES			0
#define  MUC_ALG_DES3_EDE		1
#define  MUC_ALG_AES			2
#define MUC_CTRL_WIDTH		GENMASK(7, 6)  /* other: as 0 */
#define  MUC_WIDTH_BLOCK		0
#define  MUC_WIDTH_8B			1
#define  MUC_WIDTH_1B			2
#define MUC_CTRL_CHAN0_IV_SET	BIT(8)
#define MUC_CTRL_KEY		GENMASK(10, 9)  /* other: as 0 */
#define  MUC_KEY_AES_128B		0
#define  MUC_KEY_AES_192B		1
#define  MUC_KEY_AES_256B		2
#define  MUC_KEY_DES			0
#define  MUC_KEY_DES3_EDE_3KEY		0
#define  MUC_KEY_DES3_EDE_2KEY		3
#define MUC_CTRL_KEY_FROM_MKL	BIT(13)  /* key from (not directly accessible) Machine Key Ladder (DRM) */
#define MUC_CTRL_KEY_ID		GENMASK(16, 14)  /* use which MUC_CHANn_KEY; ignored if MUC_CTRL_KEY_FROM_MKL */
#define MUC_CTRL_WEIGHT		GENMASK(31, 22)

/* for BUF_NUM / BUF_CNT reg */
#define MUC_BUF_NUM_MAX	GENMASK(15, 0)

#define MUC_INT_STATUS			0x1400
#define  MUC_INT_CHANn_IN_BUF(n)		BIT(n)
#define  MUC_INT_CHAN0_DATA_DISPOSE		BIT(8)
#define  MUC_INT_CHANn_OUT_BUF(n)		BIT(8 + n)
#define MUC_INT_CFG			0x1404
#define  MUC_INT_CFG_SEC_EN			BIT(30)  /* can't set w/ TEE */
#define  MUC_INT_CFG_NSEC_EN			BIT(31)  /* useless w/o TEE */
#define MUC_INT_RAW			0x1408
#define MUC_RST_STATUS			0x140c
#define  MUC_STATE_VALID			BIT(0)
#define MUC_CHAN0_CFG			0x1410
#define  MUC_CHAN0_START			BIT(0)
#define  MUC_CHAN0_BUSY				BIT(1)
#define MUC_SRC_ADDR_SMMU_BYPASS	0x1418
#define  MUC_ADDR_SMMU_BYPASS(n)		BIT(n - 1)
#define MUC_DST_ADDR_SMMU_BYPASS	0x141c

#define MUC_CHAN_PKG1		0u  /* only register operations */
#define MUC_CHAN_PKGn_MIN	1u  /* support DMA ring buffer */
#define MUC_CHAN_NUM		8u

#define MUC_IV_SIZE	16u
#define MUC_BLOCK_SIZE	16u
#define MUC_KEY_SIZE	32u

struct hica_muc_buf {
	__le32 addr;
	__le32 flags;
#define MUC_BUF_FLAG_DUMMY		BIT(20)
#define MUC_BUF_FLAG_SET_IV		BIT(21)
#define MUC_BUF_FLAG_END_OF_LIST	BIT(22)
	__le32 len;
/* max is GENMASK(19, 0), but use multiples of block size for safety */
#define MUC_BUF_LEN_MAX	0xffff0u
	__le32 iv_addr;
} __packed;

/******** driver definitions ********/

/* 2 * MUC_BUF_NUM * sizeof(struct hica_muc_buf) + MUC_IV_SIZE + MUC_BLOCK_SIZE = 512 */
#define MUC_BUF_NUM 15u

static struct hica_muc_ctrl_key_map {
	unsigned int alg;
	unsigned int key;
	unsigned int keylen;
} hica_muc_ctrl_key_maps[] = {
	{      MUC_ALG_AES,      MUC_KEY_AES_256B,   AES_KEYSIZE_256 },
	{      MUC_ALG_AES,      MUC_KEY_AES_192B,   AES_KEYSIZE_192 },
	{      MUC_ALG_AES,      MUC_KEY_AES_128B,   AES_KEYSIZE_128 },
	{      MUC_ALG_DES,           MUC_KEY_DES,      DES_KEY_SIZE },
	{ MUC_ALG_DES3_EDE, MUC_KEY_DES3_EDE_3KEY, DES3_EDE_KEY_SIZE },
	{ MUC_ALG_DES3_EDE, MUC_KEY_DES3_EDE_2KEY,  2 * DES_KEY_SIZE },
	{ }
};

/* designed to fit MUC_CHAN{0,n}_CTRL reg */
struct hica_muc_ctrl {
	unsigned int		: 1;
	unsigned int mode	: 3;
	unsigned int alg	: 2;
	unsigned int width	: 2;
	unsigned int		: 1;
	unsigned int key	: 2;
};

struct hica_muc_alg {
	struct skcipher_alg alg;
	struct hica_muc_ctrl ctrl;
	struct hica_muc_priv *priv;
};

/* only used for driver registration */
struct hica_muc_tmpl {
	struct hica_muc_ctrl ctrl;

	unsigned int min_keysize;
	unsigned int max_keysize;
	unsigned int ivsize;
	unsigned int chunksize;
	unsigned int blocksize;

	const char *alg_name;
	const char *mode_name;
};

struct hica_muc_chan {
	void __iomem *base;
	struct device *dev;
	unsigned char id;

	/*
	 * dirty bit to prevent re-submitting
	 * set by hica_muc_chan_push(), cleared by hica_muc_handle()
	 */
	bool dirty;
	/*
	 * current request and channel locking
	 * NULL: idle, IS_ERR(): invalid or processing, other: busy; atomic
	 */
	struct skcipher_request *req;

	union {
		/* for channel 0 */
		struct {
			/* flattened input/output data */
			void *inout;
			/* must be power of 2 */
			unsigned int inout_size;
		};

		/* for channel n */
		struct {
			struct hica_muc_buf *src;
			struct hica_muc_buf *dst;
			/* static iv buffer and stream padding */
			void *iv;

			dma_addr_t src_addr;
			dma_addr_t dst_addr;
			dma_addr_t iv_addr;
			dma_addr_t pad_addr;

			unsigned int src_emit_n;
			unsigned int dst_emit_n;
			unsigned int list_size;
		};
	};
};

struct hica_muc_priv {
	void __iomem *base;
	struct device *dev;

	struct reset_control *rst;
	struct clk_bulk_data *clks;
	unsigned int clks_n;
	int irqs[2];

	struct hica_muc_alg *algs;
	unsigned int algs_n;

	/* no DMA channels available */
	bool no_dma;

	struct task_struct *task;
	struct completion cond;

	struct hica_muc_chan chans[MUC_CHAN_NUM];
};

struct hica_muc_tfm_ctx {
	struct hica_muc_priv *priv;
	struct hica_muc_ctrl ctrl;

	unsigned int ivsize;
	unsigned int chunksize;
	unsigned int keysize;

	u8 key[MUC_KEY_SIZE] __aligned(4);
};

struct sg_iter {
	struct scatterlist *sg;
	unsigned int sg_offset;
	unsigned int offset;
};

struct hica_muc_req_ctx {
	struct hica_muc_tfm_ctx *tfm;

	bool decrypting;

	union {
		/* for channel 0 */
		struct {
			unsigned int offset;
		};

		/* for channel n */
		struct {
			/* padded length (for stream mode) */
			unsigned int runlen;
			bool eof;

			int src_nents;
			int dst_nents;

			struct sg_iter src;
			struct sg_iter dst;
		};
	};
};

static bool extra_check = false;
module_param(extra_check, bool, S_IRUGO | S_IWUSR);

/* when to consider channel 0 (for example ECB as RNG source) */
static unsigned int small_request = 256u;
module_param(small_request, uint, S_IRUGO | S_IWUSR);

static unsigned int disable_n = 0;
static unsigned int disable[MUC_CHAN_NUM];
module_param_array(disable, uint, &disable_n, S_IRUGO);

static bool hica_muc_req_is_short(const struct skcipher_request *req)
{
	return req->cryptlen <= small_request;
}

static void hica_readl_seq(void *buf, const void __iomem *addr,
			   unsigned int len)
{
	for (unsigned int i = 0; i < len; i += sizeof(u32))
		*(u32 *) (buf + i) = readl_relaxed(addr + i);
}

static void hica_writel_seq(const void *buf, void __iomem *addr,
			    unsigned int len)
{
	for (unsigned int i = 0; i < len; i += sizeof(u32))
		writel_relaxed(*(const u32 *) (buf + i), addr + i);
}

static void hica_setl_seq(u32 value, void __iomem *addr, unsigned int len)
{
	for (unsigned int i = 0; i < len; i += sizeof(u32))
		writel_relaxed(value, addr + i);
}

static bool sg_iter_valid(struct sg_iter *iter)
{
	return iter->sg && iter->sg_offset < iter->sg->length;
}

static unsigned int sg_iter_len(struct sg_iter *iter)
{
	return iter->sg->length - iter->sg_offset;
}

static dma_addr_t sg_iter_dma_address(struct sg_iter *iter)
{
	return sg_dma_address(iter->sg) + iter->sg_offset;
}

static bool sg_iter_normalize(struct sg_iter *iter)
{
	while (iter->sg && iter->sg_offset >= iter->sg->length) {
		iter->sg_offset -= iter->sg->length;
		iter->sg = sg_next(iter->sg);
	}

	return !!iter->sg;
}

static bool sg_iter_inc(struct sg_iter *iter, unsigned int len)
{
	iter->sg_offset += len;
	iter->offset += len;
	return sg_iter_normalize(iter);
}

static void sg_iter_init(struct sg_iter *iter, struct scatterlist *sg)
{
	iter->sg = sg;
	iter->sg_offset = 0;
	iter->offset = 0;
}

/*
 * (Observed) Rules:
 *  - use MUC_BUF_FLAG_END_OF_LIST to make request
 *  - request length must be multiples of chunksize
 *  - if to set IV within the list, request length must be exactly one chunksize
 *  - (3)DES cannot correctly handle < 4-byte dst buffer at the end of request
 */
static int
hica_muc_list_append(struct hica_muc_buf *list, unsigned int i,
		     struct sg_iter *iter, struct hica_muc_chan *chan,
		     struct skcipher_request *req, bool is_dst)
{
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;

	int n;

	for (n = 0; iter->offset < r_ctx->runlen && n < chan->list_size; n++) {
		struct hica_muc_buf *buf = &list[i];
		unsigned int req_remain = r_ctx->runlen - iter->offset;

		unsigned int sg_remain;
		dma_addr_t addr;
		unsigned int len;
		unsigned int flags;

		if (iter->offset >= req->cryptlen) {
			/* pad for stream cipher mode (CFB/OFB...) */
			sg_remain = 0;
			addr = chan->pad_addr;
			len = req_remain;
			flags = MUC_BUF_FLAG_END_OF_LIST;
		} else {
			/* push one BUF */
			if (WARN_ON(!sg_iter_valid(iter)))
				return -EFAULT;

			sg_remain = sg_iter_len(iter);
			addr = sg_iter_dma_address(iter);
			len = min3(sg_remain, req_remain, MUC_BUF_LEN_MAX);
			flags = len == req_remain ?
				MUC_BUF_FLAG_END_OF_LIST : 0;
		}

		/* if to set IV, limit request to chunk border */
		if (!is_dst && ctx->ctrl.mode != MUC_MODE_ECB &&
		    iter->offset < ctx->chunksize &&
		    iter->offset + len >= ctx->chunksize) {
			len = ctx->chunksize - iter->offset;
			flags = MUC_BUF_FLAG_SET_IV | MUC_BUF_FLAG_END_OF_LIST;
			buf->iv_addr = cpu_to_le32(chan->iv_addr);
		}

		buf->addr = cpu_to_le32(addr);
		buf->len = cpu_to_le32(len);
		buf->flags = cpu_to_le32(flags);

		dev_dbg(chan->dev, "%u: add %s %4u +%4u (%4u) %x\n",
			chan->id, is_dst ? "dst" : "src",
			req_remain, len, sg_remain, flags >> 20);

		i++;
		if (i >= chan->list_size)
			i = 0;

		sg_iter_inc(iter, len);
	}

	return n;
}

/******** channel ********/

static void
hica_muc_chan_ctrl(struct hica_muc_chan *chan, struct skcipher_request *req,
		   bool key_from_mkl)
{
	unsigned int id = chan->id;
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;
	void __iomem *ctrl = chan->base + (id == MUC_CHAN_PKG1 ?
					   MUC_CHAN0_CTRL : MUC_CHANn_CTRL(id));

	u32 val;

	val = readl_relaxed(ctrl);

	if (r_ctx->decrypting)
		val |= MUC_CTRL_DECRYPT;
	else
		val &= ~MUC_CTRL_DECRYPT;

	val &= ~MUC_CTRL_MODE;
	val |= (ctx->ctrl.mode << 1) & MUC_CTRL_MODE;

	val &= ~MUC_CTRL_ALG;
	val |= (ctx->ctrl.alg << 4) & MUC_CTRL_ALG;

	val &= ~MUC_CTRL_WIDTH;
	val |= (ctx->ctrl.width << 6) & MUC_CTRL_WIDTH;

	if (id == MUC_CHAN_PKG1 && ctx->ctrl.mode != MUC_MODE_ECB)
		val |= MUC_CTRL_CHAN0_IV_SET;
	else
		val &= ~MUC_CTRL_CHAN0_IV_SET;

	val &= ~MUC_CTRL_KEY;
	val |= (ctx->ctrl.key << 9) & MUC_CTRL_KEY;

	if (key_from_mkl)
		val |= MUC_CTRL_KEY_FROM_MKL;
	else {
		val &= ~MUC_CTRL_KEY_FROM_MKL;
		val &= ~MUC_CTRL_KEY_ID;
		val |= (id << 14) & MUC_CTRL_KEY_ID;
	}

	writel_relaxed(val, ctrl);

	dev_dbg(chan->dev, "%u: ctrl %x, alg %u, mod %u, key %u, len %u\n",
		id, val, ctx->ctrl.alg, ctx->ctrl.mode,
		ctx->ctrl.key, req->cryptlen);
}

static void
hica_muc_chan_iv_get(struct hica_muc_chan *chan, struct skcipher_request *req)
{
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;

	if (ctx->ctrl.mode == MUC_MODE_ECB)
		return;

	hica_readl_seq(req->iv, chan->base + MUC_CHANn_IV_OUT0(chan->id),
		       ctx->ivsize);
}

static int
_hica_muc_chan_push_0(struct hica_muc_chan *chan, struct skcipher_request *req)
{
	void __iomem *base = chan->base;
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;
	unsigned int offset_mod = r_ctx->offset & (chan->inout_size - 1);

	u32 val;

	if (readl(base + MUC_CHAN0_CFG) & MUC_CHAN0_BUSY)
		return -EBUSY;

	/* get previous block */
	if (r_ctx->offset)
		hica_readl_seq(chan->inout + ((r_ctx->offset - ctx->chunksize) &
					      (chan->inout_size - 1)),
			       base + MUC_CHAN0_DATA_OUT0,
			       ctx->chunksize);

	if (r_ctx->offset >= req->cryptlen)
		return 0;

	/* swap buf (quick workaround for chan 0 test only) */
	if (r_ctx->offset && !offset_mod) {
		scatterwalk_map_and_copy(chan->inout, req->dst,
					 r_ctx->offset - chan->inout_size,
					 chan->inout_size, 1);
		scatterwalk_map_and_copy(chan->inout, req->src, r_ctx->offset,
					 min(req->cryptlen - r_ctx->offset,
					     chan->inout_size), 0);
	}

	/* set next block */
	if (ctx->ctrl.mode != MUC_MODE_ECB &&
	    r_ctx->offset == ctx->chunksize) {
		val = readl_relaxed(base + MUC_CHAN0_CTRL);
		val &= ~MUC_CTRL_CHAN0_IV_SET;
		writel_relaxed(val, base + MUC_CHAN0_CTRL);
	}

	hica_writel_seq(chan->inout + offset_mod,
			base + MUC_CHAN0_DATA_IN0, ctx->chunksize);

	r_ctx->offset += ctx->chunksize;
	return -EINPROGRESS;
}

static void _hica_muc_chan_emit_0(struct hica_muc_chan *chan)
{
	writel(MUC_CHAN0_START, chan->base + MUC_CHAN0_CFG);
}

static void
_hica_muc_chan_unprepare_0(struct hica_muc_chan *chan,
			   struct skcipher_request *req, bool no_output)
{
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;

	/* output */
	if (!no_output) {
		unsigned int cryptlen_mod = req->cryptlen & (chan->inout_size -
							     1);

		hica_muc_chan_iv_get(chan, req);
		scatterwalk_map_and_copy(chan->inout, req->dst,
					 req->cryptlen - cryptlen_mod,
					 cryptlen_mod, 1);
	}

	/* erase */
#ifndef DEBUG
	hica_setl_seq(0, chan->base + MUC_CHAN0_DATA_IN0, ctx->chunksize);
	if (ctx->ctrl.mode != MUC_MODE_ECB)
		hica_setl_seq(0, chan->base + MUC_CHAN0_IV_IN0, ctx->ivsize);
	memzero_explicit(chan->inout, chan->inout_size);
#endif
}

static int _hica_muc_chan_prepare_0(struct hica_muc_chan *chan,
				    struct skcipher_request *req)
{
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;

	if (ctx->ctrl.mode != MUC_MODE_ECB)
		hica_writel_seq(req->iv, chan->base + MUC_CHAN0_IV_IN0,
				ctx->ivsize);

	r_ctx->offset = 0;
	scatterwalk_map_and_copy(chan->inout, req->src, 0,
				 min(req->cryptlen, chan->inout_size), 0);

	return 0;
}

static void
hica_muc_chan_debug_n(struct hica_muc_chan *chan, bool unpreparing)
{
	void __iomem *base = chan->base;
	struct device *dev = chan->dev;
	unsigned int id = chan->id;
	const char *direction = unpreparing ? "unprepare" : "  prepare";

	dev_dbg(dev, "%u: %s, ctrl %x\n", id, direction,
		readl_relaxed(base + MUC_CHANn_CTRL(id)));
	dev_dbg(dev, "%u: %s, src, left %u, list (%u) %u<- %3u ->%u\n",
		id, direction,
		readw_relaxed(base + MUC_CHANn_IN_LEFT(id)) >> 24,
		readw_relaxed(base + MUC_CHANn_IN_BUF_NUM(id)),
		readw_relaxed(base + MUC_CHANn_IN_EMPTY_CNT(id)),
		readw_relaxed(base + MUC_CHANn_SRC_LST_PTR(id)),
		readw_relaxed(base + MUC_CHANn_IN_BUF_CNT(id)));
	dev_dbg(dev, "%u: %s, dst, left %u, list (%u) %u<- %3u ->%u\n",
		id, direction,
		readw_relaxed(base + MUC_CHANn_OUT_LEFT(id)) >> 24,
		readw_relaxed(base + MUC_CHANn_OUT_BUF_NUM(id)),
		readw_relaxed(base + MUC_CHANn_OUT_FULL_CNT(id)),
		readw_relaxed(base + MUC_CHANn_DST_LST_PTR(id)),
		readw_relaxed(base + MUC_CHANn_OUT_BUF_CNT(id)));
}

static int
_hica_muc_chan_push_n(struct hica_muc_chan *chan, struct skcipher_request *req)
{
	void __iomem *base = chan->base;
	struct device *dev = chan->dev;
	unsigned int id = chan->id;
	unsigned int size = 2 * sizeof(chan->src[0]) * chan->list_size;
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);

	bool src_eof = r_ctx->src.offset >= r_ctx->runlen;
	unsigned int src_n = readl_relaxed(base + MUC_CHANn_IN_BUF_CNT(id));
	bool src_todo;
	unsigned int src_i;
	int src_emit_n;

	bool dst_eof = r_ctx->dst.offset >= r_ctx->runlen;
	unsigned int dst_n = readl_relaxed(base + MUC_CHANn_OUT_BUF_CNT(id));
	bool dst_todo;
	unsigned int dst_i;
	int dst_emit_n;

	void __iomem *reg;
	u32 val;

	if (!src_n && !dst_n && r_ctx->eof) {
		dev_dbg(dev, "%u: all set\n", id);
		return 0;
	}

	dev_dbg(dev, "%u: src has %u, dst has %u\n", id, src_n, dst_n);
	if ((src_n && dst_n) || r_ctx->eof)
		return -EBUSY;

	barrier();

	if (src_eof && dst_eof && !src_n) {
		/*
		 * Handle the very annoying EOF quirk, in which:
		 *  - All src buffers are proccessed, but;
		 *  - The last dst buffer are left unproccessed.
		 *
		 * Though all observed quirks only happen when (3)DES and buf
		 * len < 4, the following fixup routine does not rely on this
		 * hypothesis.
		 */
		dev_dbg(dev, "%u: reach EOF\n", id);
		r_ctx->eof = true;

		/* first, IV is already done */
		hica_muc_chan_iv_get(chan, req);

		/* if no dst buffer left, no stuck happened (and we are done) */
		if (!dst_n)
			return 0;

		/* check MUC_CHANn_OUT_LEFT to see if it's really stucked */
		if (!(readl_relaxed(base + MUC_CHANn_OUT_LEFT(id)) >> 24))
			/* nothing; maybe it's still proccessing */
			return -EBUSY;

		/* stucked; issue one more request to push the hardware */
		src_todo = true;
		dst_todo = true;
	} else {
		if (!src_n && src_eof)
			dev_dbg(dev, "%u: src done\n", id);
		if (!dst_n && dst_eof)
			dev_dbg(dev, "%u: dst done\n", id);

		src_todo = !src_n && !src_eof;
		dst_todo = !dst_n && !dst_eof;
	}

	/* read ring buffer status */
	if (src_todo) {
		reg = base + MUC_CHANn_IN_EMPTY_CNT(id);
		val = readw_relaxed(reg);
		if (val)
			writew_relaxed(val, reg);

		if (extra_check) {
			reg = base + MUC_CHANn_SRC_LST_ADDR(id);
			val = readl_relaxed(reg);
			if (WARN_ON(val != chan->src_addr))
				writel_relaxed(chan->src_addr, reg);

			reg = base + MUC_CHANn_IN_BUF_NUM(id);
			val = readw_relaxed(reg);
			if (WARN_ON(val != chan->list_size))
				writew_relaxed(chan->list_size, reg);
		}

		src_i = readw_relaxed(base + MUC_CHANn_SRC_LST_PTR(id));
		if (WARN_ON(src_i >= chan->list_size))
			return -EIO;
	}

	if (dst_todo) {
		reg = base + MUC_CHANn_OUT_FULL_CNT(id);
		val = readw_relaxed(reg);
		if (val)
			writew_relaxed(val, reg);

		if (extra_check) {
			reg = base + MUC_CHANn_DST_LST_ADDR(id);
			val = readl_relaxed(reg);
			if (WARN_ON(val != chan->dst_addr))
				writel_relaxed(chan->dst_addr, reg);

			reg = base + MUC_CHANn_OUT_BUF_NUM(id);
			val = readw_relaxed(reg);
			if (WARN_ON(val != chan->list_size))
				writew_relaxed(chan->list_size, reg);
		}

		dst_i = readw_relaxed(base + MUC_CHANn_DST_LST_PTR(id));
		if (WARN_ON(dst_i >= chan->list_size))
			return -EIO;
	}

	/* set buffers */
	dma_sync_single_for_cpu(dev, chan->src_addr, size, DMA_TO_DEVICE);
	if (!r_ctx->eof) {
		if (!src_todo)
			src_emit_n = 0;
		else {
			src_emit_n = hica_muc_list_append(chan->src, src_i,
							  &r_ctx->src, chan,
							  req, false);
			if (src_emit_n < 0)
				return src_emit_n;
		}
		if (!dst_todo)
			dst_emit_n = 0;
		else {
			dst_emit_n = hica_muc_list_append(chan->dst, dst_i,
							  &r_ctx->dst, chan,
							  req, true);
			if (dst_emit_n < 0)
				return dst_emit_n;
		}
	} else {
		struct hica_muc_buf *buf;

		buf = &chan->src[src_i];
		buf->addr = cpu_to_le32(chan->pad_addr);
		buf->len = cpu_to_le32(MUC_BLOCK_SIZE);
		buf->flags = cpu_to_le32(MUC_BUF_FLAG_END_OF_LIST);
		src_emit_n = 1;

		dst_i += dst_n;
		if (dst_i >= chan->list_size)
			dst_i -= chan->list_size;
		buf = &chan->dst[dst_i];
		buf->addr = cpu_to_le32(chan->pad_addr);
		buf->len = cpu_to_le32(MUC_BLOCK_SIZE);
		buf->flags = cpu_to_le32(MUC_BUF_FLAG_END_OF_LIST);
		dst_emit_n = 1;
	}
	dma_sync_single_for_device(dev, chan->src_addr, size, DMA_TO_DEVICE);

	chan->src_emit_n = src_emit_n;
	chan->dst_emit_n = dst_emit_n;
	if (!r_ctx->eof)
		dev_dbg(dev, "%u: put src %u, dst %u\n", id, src_n, dst_n);
	else
		dev_dbg(dev, "%u: dealing with stuck\n", id);
	return -EINPROGRESS;
}

static void _hica_muc_chan_emit_n(struct hica_muc_chan *chan)
{
	void __iomem *base = chan->base;
	unsigned int id = chan->id;
	unsigned int src_emit_n = chan->src_emit_n;
	unsigned int dst_emit_n = chan->dst_emit_n;

	chan->src_emit_n = 0;
	chan->dst_emit_n = 0;

	if (dst_emit_n) {
		writew(dst_emit_n, base + MUC_CHANn_INT_OUT_CNT_CFG(id));
		writew(dst_emit_n, base + MUC_CHANn_OUT_BUF_CNT(id));
	}
	if (src_emit_n) {
		writew(src_emit_n, base + MUC_CHANn_INT_IN_CNT_CFG(id));
		writew(src_emit_n, base + MUC_CHANn_IN_BUF_CNT(id));
	}
}

static void
_hica_muc_chan_unprepare_n(struct hica_muc_chan *chan,
			   struct skcipher_request *req, bool no_output)
{
	struct device *dev = chan->dev;
	bool bidirectional = req->src == req->dst;
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;

	hica_muc_chan_debug_n(chan, true);

	/* output */
	if (!no_output)
		dma_sync_sg_for_cpu(dev, req->dst, r_ctx->dst_nents,
				    DMA_FROM_DEVICE);

	dma_unmap_sg(dev, req->src, r_ctx->src_nents,
		     bidirectional ? DMA_BIDIRECTIONAL : DMA_TO_DEVICE);
	if (!bidirectional)
		dma_unmap_sg(dev, req->dst, r_ctx->dst_nents, DMA_FROM_DEVICE);

	/* erase */
#ifndef DEBUG
	if (ctx->ctrl.mode != MUC_MODE_ECB)
		memzero_explicit(chan->iv, MUC_IV_SIZE);
#endif
}

static int _hica_muc_chan_prepare_n(struct hica_muc_chan *chan,
				    struct skcipher_request *req)
{
	void __iomem *base = chan->base;
	struct device *dev = chan->dev;
	unsigned int id = chan->id;
	bool bidirectional = req->src == req->dst;
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;

	int src_nents;
	int dst_nents;
	u32 val;

	src_nents = sg_nents_for_len(req->src, req->cryptlen);
	if (src_nents < 0)
		return src_nents;
	if (!bidirectional) {
		dst_nents = sg_nents_for_len(req->dst, req->cryptlen);
		if (dst_nents < 0)
			return dst_nents;
	}

	src_nents = dma_map_sg(dev, req->src, src_nents,
			       bidirectional ? DMA_BIDIRECTIONAL :
					       DMA_TO_DEVICE);
	if (src_nents <= 0) {
		dev_err(dev, "error mapping src\n");
		return -EIO;
	}
	if (bidirectional)
		dst_nents = src_nents;
	else {
		dst_nents = dma_map_sg(dev, req->dst, dst_nents,
				       DMA_FROM_DEVICE);
		if (dst_nents <= 0) {
			dev_err(dev, "error mapping dst\n");
			dma_unmap_sg(dev, req->src, src_nents,
				     DMA_TO_DEVICE);
			return -EIO;
		}
	}

	r_ctx->src_nents = src_nents;
	r_ctx->dst_nents = dst_nents;

	/* pad request length to multiples of chunksize */
	r_ctx->runlen = ALIGN(req->cryptlen, ctx->chunksize);

	r_ctx->eof = false;
	sg_iter_init(&r_ctx->src, req->src);
	sg_iter_init(&r_ctx->dst, req->dst);

	hica_muc_chan_debug_n(chan, false);

	/* setup ring buffers */
	writel_relaxed(chan->src_addr, base + MUC_CHANn_SRC_LST_ADDR(id));
	writew_relaxed(chan->list_size, base + MUC_CHANn_IN_BUF_NUM(id));
	writew_relaxed(0, base + MUC_CHANn_IN_AGE_CNT(id));

	writel_relaxed(chan->dst_addr, base + MUC_CHANn_DST_LST_ADDR(id));
	writew_relaxed(chan->list_size, base + MUC_CHANn_OUT_BUF_NUM(id));
	writew_relaxed(0, base + MUC_CHANn_OUT_AGE_CNT(id));

	/* erase in case of not being 0 */
	writel_relaxed(0, base + MUC_CHANn_IN_LEFT(id));
	val = readw_relaxed(base + MUC_CHANn_OUT_BUF_CNT(id));
	if (val)
		writew_relaxed(0x10000 - val,
			       base + MUC_CHANn_OUT_BUF_CNT(id));

	/* setup data */
	if (ctx->ctrl.mode != MUC_MODE_ECB) {
		memcpy(chan->iv, req->iv, MUC_IV_SIZE);
		dma_sync_single_for_device(dev, chan->iv_addr, MUC_IV_SIZE,
					   DMA_TO_DEVICE);
	}
	dma_sync_sg_for_device(dev, req->src, r_ctx->src_nents, DMA_TO_DEVICE);

	return 0;
}

/*
 * 0: Everything is done, fetch output via hica_muc_chan_unprepare().
 * -EINPROGRESS: Hardware is already set for next round, call
 *               hica_muc_chan_emit() exactly once to emit processing.
 * -EBUSY: Hardware is busy (waitting emission or doing process), do not call
 *         hica_muc_chan_emit().
 * other: Error.
 */
static int
hica_muc_chan_push(struct hica_muc_chan *chan, struct skcipher_request *req)
{
	int ret;

	if (chan->dirty)
		return -EBUSY;

	ret = chan->id == MUC_CHAN_PKG1 ? _hica_muc_chan_push_0(chan, req) :
					  _hica_muc_chan_push_n(chan, req);

	if (ret == -EINPROGRESS)
		WRITE_ONCE(chan->dirty, true);

	return ret;
}

static void hica_muc_chan_emit(struct hica_muc_chan *chan)
{
	if (!chan->dirty)
		return;

	if (chan->id == MUC_CHAN_PKG1)
		_hica_muc_chan_emit_0(chan);
	else
		_hica_muc_chan_emit_n(chan);

	/* dirty bit is cleared in interrupt handler */
}

static void
hica_muc_chan_unprepare(struct hica_muc_chan *chan,
			struct skcipher_request *req, bool no_output)
{
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;

	if (ctx->keysize)
		hica_setl_seq(0, chan->base + MUC_CHANn_KEY0(chan->id),
			      ctx->keysize);

	if (chan->id == MUC_CHAN_PKG1)
		_hica_muc_chan_unprepare_0(chan, req, no_output);
	else
		_hica_muc_chan_unprepare_n(chan, req, no_output);

#ifndef DEBUG
	if (ctx->ctrl.mode != MUC_MODE_ECB)
		hica_setl_seq(0, chan->base + MUC_CHANn_IV_OUT0(chan->id),
			      ctx->ivsize);
#endif
}

static int hica_muc_chan_prepare(struct hica_muc_chan *chan,
				 struct skcipher_request *req)
{
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct hica_muc_tfm_ctx *ctx = r_ctx->tfm;
	int ret;

	ret = chan->id == MUC_CHAN_PKG1 ? _hica_muc_chan_prepare_0(chan, req) :
					  _hica_muc_chan_prepare_n(chan, req);
	if (ret)
		return ret;

	hica_muc_chan_ctrl(chan, req, !ctx->keysize);
	if (ctx->keysize)
		hica_writel_seq(ctx->key,
				chan->base + MUC_CHANn_KEY0(chan->id),
				ctx->keysize);

	return 0;
}

static int hica_muc_chan_init(struct hica_muc_chan *chan,
			      struct hica_muc_priv *priv, unsigned int id)
{
	void __iomem *base = priv->base;
	struct device *dev = priv->dev;

	unsigned int src_i;
	unsigned int dst_i;
	unsigned int list_memsize;

	chan->base = priv->base;
	chan->dev = priv->dev;
	chan->id = id;
	chan->req = NULL;

	if (id == MUC_CHAN_PKG1) {
		chan->inout_size = small_request;

		chan->inout = devm_kmalloc(dev, chan->inout_size,
					   GFP_KERNEL);
		if (!chan->inout)
			return -ENOMEM;

		return 0;
	}

	chan->list_size = min(MUC_BUF_NUM, MUC_BUF_NUM_MAX);

	src_i = readw_relaxed(base + MUC_CHANn_SRC_LST_PTR(id));
	dst_i = readw_relaxed(base + MUC_CHANn_DST_LST_PTR(id));
	if (src_i >= chan->list_size || dst_i >= chan->list_size) {
		dev_err(dev,
			"cannot setup channel %u, src ptr %u, dst ptr %u\n",
			id, src_i, dst_i);
		dev_err(dev, "why didn't device reset?\n");
		return -EINVAL;
	}

	list_memsize = chan->list_size * sizeof(chan->src[0]);
	chan->src = dmam_alloc_attrs(dev, 2 * list_memsize +
					  MUC_IV_SIZE + MUC_BLOCK_SIZE,
				     &chan->src_addr, GFP_KERNEL | __GFP_ZERO,
				     0);
	if (!chan->src)
		return -ENOMEM;

	chan->dst = (void *) chan->src + list_memsize;
	chan->dst_addr = chan->src_addr + list_memsize;

	chan->iv = (void *) chan->dst + list_memsize;
	chan->iv_addr = chan->dst_addr + list_memsize;

	chan->pad_addr = chan->iv_addr + MUC_IV_SIZE;

	return 0;
}

/******** irq ********/

static unsigned int hica_muc_process(struct hica_muc_priv *priv)
{
	struct device *dev = priv->dev;

	unsigned int mask = 0;

	for (unsigned int id = 0; id < MUC_CHAN_NUM; id++) {
		struct hica_muc_chan *chan = &priv->chans[id];
		struct skcipher_request *req = chan->req;
		int ret;

		if (IS_ERR_OR_NULL(req))
			continue;

		req = xchg(&chan->req, ERR_PTR(-EBUSY));
		if (req == ERR_PTR(-EBUSY))
			continue;

		do {
			if (IS_ERR_OR_NULL(req))
				break;

			ret = hica_muc_chan_push(chan, req);
			if (ret == -EBUSY)
				break;

			mask |= BIT(id);
			if (ret == -EINPROGRESS)
				break;

			/* task finished (or failed) */
			if (!ret)
				dev_dbg(dev, "%u: done\n", id);
			else
				dev_err(dev,
					"channel %u got unexpected ret %d\n",
					id, ret);

			hica_muc_chan_unprepare(chan, req, !!ret);
			skcipher_request_complete(req, ret);
			req = NULL;
		} while (0);

		smp_store_mb(chan->req, req);

		if (ret == -EINPROGRESS) {
			hica_muc_chan_emit(chan);
			dev_dbg(dev, "%u: pushed\n", id);
		}
	}

	return mask;
}

static int hica_muc_thread(void *data)
{
	struct hica_muc_priv *priv = data;

	while (1) {
		bool timeouted;
		unsigned int mask;

		timeouted = wait_for_completion_interruptible_timeout(&priv->cond,
						msecs_to_jiffies(5000)) <= 0;
		if (kthread_should_stop())
			break;
		reinit_completion(&priv->cond);

		mask = hica_muc_process(priv);
		if (mask && timeouted)
			dev_info(priv->dev,
				 "interrupt gone on channel mask %x\n", mask);
	}

	return 0;
}

static irqreturn_t hica_muc_handle(int irq, void *dev_id)
{
	struct hica_muc_priv *priv = dev_id;

	u32 val;

	/* clear interrupts */
	val = readl_relaxed(priv->base + MUC_INT_STATUS);
	if (!val)
		return IRQ_NONE;
	writel_relaxed(val, priv->base + MUC_INT_RAW);

	/* clear dirty bits */
	val |= val >> MUC_CHAN_NUM;
	for (unsigned int id = 0; id < MUC_CHAN_NUM; id++) {
		struct hica_muc_chan *chan = &priv->chans[id];

		if (val & BIT(id))
			WRITE_ONCE(chan->dirty, false);
	}
	smp_wmb();

	/* feed channel 0 quickly */
	do {
		struct hica_muc_chan *chan = &priv->chans[MUC_CHAN_PKG1];
		struct skcipher_request *req;
		int ret = 0;

		if (val != MUC_INT_CHAN0_DATA_DISPOSE)
			break;

		req = xchg(&chan->req, ERR_PTR(-EBUSY));
		if (req == ERR_PTR(-EBUSY))
			break;

		if (!IS_ERR_OR_NULL(req))
			ret = hica_muc_chan_push(chan, req);

		/* be ready for next interrupt */
		smp_store_mb(chan->req, req);

		if (ret == -EINPROGRESS) {
			hica_muc_chan_emit(chan);
			return IRQ_HANDLED;
		}
	} while (0);

	/* go cleaning */
	complete(&priv->cond);
	return IRQ_HANDLED;
}

/******** skcipher_alg ********/

/* why this function missing from des.h, while self tests do challenge weak keys? */
static int des_check_weakkey_half(const u8 *key)
{
	for (int i = 1; i < DES_KEY_SIZE / 2; i++)
		if ((key[i] ^ key[0]) >> 1)
			return 0;
	return -EINVAL;
}

static int des_check_weakkey(const u8 *key)
{
	return des_check_weakkey_half(key) ?:
	       des_check_weakkey_half(key + DES_KEY_SIZE / 2);
}

static unsigned int
hica_muc_ctrl_key_lookup(unsigned int alg, unsigned int keylen)
{
	for (int i = 0; hica_muc_ctrl_key_maps[i].keylen; i++)
		if (hica_muc_ctrl_key_maps[i].alg == alg &&
		    hica_muc_ctrl_key_maps[i].keylen == keylen)
				return hica_muc_ctrl_key_maps[i].key;

	return 0;
}

static int hica_muc_alg_setkey(struct crypto_skcipher *tfm, const u8 *key,
			       unsigned int keylen)
{
	struct hica_muc_tfm_ctx *ctx = crypto_skcipher_ctx(tfm);

	if (keylen > MUC_KEY_SIZE)
		return -EINVAL;

	switch (ctx->ctrl.alg) {
	case MUC_ALG_AES:
		if (aes_check_keylen(keylen))
			return -EINVAL;
		break;
	case MUC_ALG_DES:
		if (keylen != DES_KEY_SIZE)
			return -EINVAL;
		if (des_check_weakkey(key))
			return -EINVAL;
		break;
	case MUC_ALG_DES3_EDE:
		if (keylen != DES3_EDE_KEY_SIZE)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	ctx->ctrl.key = hica_muc_ctrl_key_lookup(ctx->ctrl.alg, keylen);
	memcpy(ctx->key, key, keylen);
	ctx->keysize = keylen;

	return 0;
}

static int hica_muc_alg_encdec(struct skcipher_request *req, bool decrypting)
{
	struct hica_muc_req_ctx *r_ctx = skcipher_request_ctx(req);
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct hica_muc_tfm_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct hica_muc_priv *priv = ctx->priv;
	struct device *dev = priv->dev;

	struct hica_muc_chan *chan;
	int ret;

	/* get an idle channel */
	for (unsigned int id = priv->no_dma || hica_muc_req_is_short(req) ?
			       MUC_CHAN_PKG1 : MUC_CHAN_PKGn_MIN;
	     id < MUC_CHAN_NUM; id++) {
		struct skcipher_request *old = NULL;

		chan = &priv->chans[id];
		if (try_cmpxchg_acquire(&chan->req, &old, ERR_PTR(-EBUSY)))
			goto acquired;
	}
	return -EBUSY;

acquired:
	r_ctx->tfm = ctx;
	r_ctx->decrypting = decrypting;

	ret = hica_muc_chan_prepare(chan, req);
	/*
	 * Let sweeper thread make the first request. If we make the request
	 * instead, sweeper might not be able to acquire lock.
	 */
	if (ret) {
		WRITE_ONCE(chan->req, NULL);
		dev_dbg(dev, "%u: returned %d\n", chan->id, ret);
		return ret;
	}

	smp_store_mb(chan->req, req);
	dev_dbg(dev, "%u: prepared\n", chan->id);
	complete(&priv->cond);
	return -EINPROGRESS;
}

static int hica_muc_alg_encrypt(struct skcipher_request *req)
{
	return hica_muc_alg_encdec(req, false);
}

static int hica_muc_alg_decrypt(struct skcipher_request *req)
{
	return hica_muc_alg_encdec(req, true);
}

static int hica_muc_alg_init(struct crypto_skcipher *tfm)
{
	struct hica_muc_tfm_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct hica_muc_alg *p_alg = container_of(alg, typeof(*p_alg), alg);

	/* copy parameters to avoid pointer hell */
	ctx->ivsize = crypto_skcipher_ivsize(tfm);
	ctx->chunksize = crypto_skcipher_chunksize(tfm);
	if (ctx->ivsize > MUC_IV_SIZE || ctx->chunksize > MUC_BLOCK_SIZE)
		return -EINVAL;

	ctx->priv = p_alg->priv;
	ctx->ctrl = p_alg->ctrl;
	ctx->keysize = 0;

	crypto_skcipher_set_reqsize(tfm, sizeof(struct hica_muc_req_ctx));
	return 0;
}

static int hica_muc_alg_register(struct hica_muc_alg *p_alg,
				 const struct hica_muc_tmpl *tmpl,
				 struct hica_muc_priv *priv)
{
	struct crypto_alg *base = &p_alg->alg.base;

	*p_alg = (typeof(*p_alg)) {
		.alg = {
			.setkey = hica_muc_alg_setkey,
			.encrypt = hica_muc_alg_encrypt,
			.decrypt = hica_muc_alg_decrypt,
			.init = hica_muc_alg_init,

			.min_keysize = tmpl->min_keysize,
			.max_keysize = tmpl->max_keysize,
			.ivsize = tmpl->ivsize,
			.chunksize = tmpl->chunksize,

			.base = {
				.cra_flags = CRYPTO_ALG_TYPE_SKCIPHER |
					     CRYPTO_ALG_ASYNC |
					     CRYPTO_ALG_KERN_DRIVER_ONLY |
					     CRYPTO_ALG_OPTIONAL_KEY,
				.cra_blocksize = tmpl->blocksize,
				.cra_ctxsize = sizeof(struct hica_muc_tfm_ctx),
				.cra_alignmask = 0,

				.cra_priority = 400,
				.cra_module = THIS_MODULE,
			},
		},
		.ctrl = tmpl->ctrl,
		.priv = priv,
	};

	snprintf(base->cra_name, sizeof(base->cra_name), "%s(%s)",
		 tmpl->mode_name, tmpl->alg_name);
	snprintf(base->cra_driver_name, sizeof(base->cra_driver_name),
		 "hisi-advca-%s-%s", tmpl->mode_name, tmpl->alg_name);

	return crypto_register_skcipher(&p_alg->alg);
}

#define hica_muc_tmpl_define(_ALG, _MODE, _alg, _mode, minkey, maxkey) { \
	.ctrl = { \
		.alg = MUC_ALG_##_ALG, \
		.mode = MUC_MODE_##_MODE, \
		.width = MUC_WIDTH_BLOCK, \
	}, \
\
	.min_keysize = minkey, \
	.max_keysize = maxkey, \
	.ivsize = _ALG##_BLOCK_SIZE, \
	.chunksize = _ALG##_BLOCK_SIZE, \
	.blocksize = _ALG##_BLOCK_SIZE, \
\
	.alg_name = #_alg, \
	.mode_name = #_mode, \
}

#define hica_muc_tmpl_define_aes(_MODE, _mode) \
	hica_muc_tmpl_define(AES, _MODE, aes, _mode, AES_MIN_KEY_SIZE, \
			     AES_MAX_KEY_SIZE)
#define hica_muc_tmpl_define_des(_MODE, _mode) \
	hica_muc_tmpl_define(DES, _MODE, des, _mode, DES_KEY_SIZE, DES_KEY_SIZE)
#define hica_muc_tmpl_define_des3_ede(_MODE, _mode) \
	hica_muc_tmpl_define(DES3_EDE, _MODE, des3_ede, _mode, \
			     DES3_EDE_KEY_SIZE, DES3_EDE_KEY_SIZE)

static const struct hica_muc_tmpl hica_muc_tmpls[] = {
	hica_muc_tmpl_define_aes(ECB, ecb),
	hica_muc_tmpl_define_aes(CBC, cbc),
	hica_muc_tmpl_define_aes(CFB, cfb),
	hica_muc_tmpl_define_aes(OFB, ofb),
	hica_muc_tmpl_define_aes(CTR, ctr),

	hica_muc_tmpl_define_des(ECB, ecb),
	hica_muc_tmpl_define_des(CBC, cbc),
	hica_muc_tmpl_define_des(CFB, cfb),
	hica_muc_tmpl_define_des(OFB, ofb),
	/* does not support ctr-des, hardware will recognize as ecb-des */

	hica_muc_tmpl_define_des3_ede(ECB, ecb),
	hica_muc_tmpl_define_des3_ede(CBC, cbc),
	hica_muc_tmpl_define_des3_ede(CFB, cfb),
	hica_muc_tmpl_define_des3_ede(OFB, ofb),
	/* does not support ctr-des3_ede, hardware will recognize as ecb-des3_ede */
};

/******** device ********/

static void hica_muc_remove(struct platform_device *pdev)
{
	struct hica_muc_priv *priv = platform_get_drvdata(pdev);

	for (int i = priv->algs_n; i > 0; ) {
		i--;
		crypto_unregister_skcipher(&priv->algs[i].alg);
	}

	kthread_stop(priv->task);

	clk_bulk_disable_unprepare(priv->clks_n, priv->clks);
	reset_control_assert(priv->rst);
}

static int hica_muc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	void __iomem *base;
	struct hica_muc_priv *priv;
	char status[MUC_CHAN_NUM + 1] = {};
	unsigned int chan_mask = 0;
	unsigned int disable_mask = 0;
	u32 val;
	int ret;

	/* acquire resources */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	priv->base = base;

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
	priv->irqs[0] = ret;
	priv->irqs[1] = platform_get_irq_optional(pdev, 1);

	priv->algs_n = ARRAY_SIZE(hica_muc_tmpls);
	priv->algs = devm_kmalloc_array(dev, priv->algs_n,
					sizeof(priv->algs[0]), GFP_KERNEL);
	if (!priv->algs)
		return -ENOMEM;

	init_completion(&priv->cond);

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

	if (readl_relaxed_poll_timeout(base + MUC_RST_STATUS, val,
				       val & MUC_STATE_VALID,
				       20 * USEC_PER_MSEC,
				       1000 * USEC_PER_MSEC)) {
		dev_err(dev, "cannot bring up device\n");
		ret = -ENODEV;
		goto err_clk;
	}

	/* clear all interrupts */
	writel_relaxed(~0, base + MUC_INT_RAW);

	/* must set this before enabling interrupts */
	val = readl_relaxed(base + MUC_SEC_CHAN_CFG);
	for (unsigned int id = 0; id < MUC_CHAN_NUM; id++)
		val |= MUC_SEC_CHANn_BIT(id);
	writel(val, base + MUC_SEC_CHAN_CFG);

	/* enable interrupts */
	val = readl_relaxed(base + MUC_INT_CFG);
	for (unsigned int id = MUC_CHAN_PKGn_MIN; id < MUC_CHAN_NUM; id++) {
		val |= MUC_INT_CHANn_IN_BUF(id);
		val |= MUC_INT_CHANn_OUT_BUF(id);
	}
	val |= MUC_INT_CHAN0_DATA_DISPOSE;
	val |= MUC_INT_CFG_SEC_EN;
	val |= MUC_INT_CFG_NSEC_EN;
	writel(val, base + MUC_INT_CFG);

	/* test channel availability */
	for (unsigned int i = 0; i < disable_n; i++)
		disable_mask |= BIT(disable[i]);

	val = readl_relaxed(base + MUC_INT_CFG);
	for (unsigned int id = 0; id < MUC_CHAN_NUM; id++) {
		bool int_ok = val & MUC_INT_CHANn_OUT_BUF(id);
		bool enabled = !(disable_mask & BIT(id));

		if (int_ok && enabled) {
			status[id] = 'y';
			chan_mask |= BIT(id);
		} else
			status[id] = int_ok ? '#' : enabled ? 'n' : '!';
	}

	dev_info(dev, "channel status: %s\n", status);
	if (!chan_mask) {
		dev_err(dev, "cannot enable any channels\n");
		ret = -ENODEV;
		goto err_clk;
	}

	priv->no_dma = chan_mask == BIT(MUC_CHAN_PKG1);
	if (priv->no_dma && !disable_mask) {
		dev_err(dev, "only slow channel available, refuse to start\n");
		ret = -EINVAL;
		goto err_clk;
	}

	/* clear bypass */
	val = readl_relaxed(base + MUC_SRC_ADDR_SMMU_BYPASS);
	val &= ~GENMASK(7, 0);
	writel_relaxed(val, base + MUC_SRC_ADDR_SMMU_BYPASS);

	val = readl_relaxed(base + MUC_DST_ADDR_SMMU_BYPASS);
	val &= ~GENMASK(7, 0);
	writel_relaxed(val, base + MUC_DST_ADDR_SMMU_BYPASS);

	/* register irq */
	for (int i = 0; i < ARRAY_SIZE(priv->irqs) && priv->irqs[i] > 0; i++) {
		ret = devm_request_irq(dev, priv->irqs[i], hica_muc_handle,
				       IRQF_SHARED, pdev->name, priv);
		if (ret)
			goto err_clk;
	}

	/* setup channels */
	for (unsigned int id = 0; id < MUC_CHAN_NUM; id++)
		if (!(chan_mask & BIT(id)))
			priv->chans[id].req = ERR_PTR(-EINVAL);
		else {
			ret = hica_muc_chan_init(&priv->chans[id], priv, id);
			if (ret)
				goto err_clk;
		}

	/* pressure on one channel for self tests to detect bug */
	if (!(chan_mask & (chan_mask - 1)) || extra_check)
		chan_mask = 0;
	else {
		chan_mask &= ~BIT(__fls(chan_mask));
		for (unsigned int id = 0; id < MUC_CHAN_NUM; id++)
			if (chan_mask & BIT(id))
				priv->chans[id].req = ERR_PTR(-EBUSY);
	}
	/* commit all writes before threaded accessing */
	smp_wmb();

	/* start sweeper */
	priv->task = kthread_create(hica_muc_thread, priv, dev->driver->name);
	if (IS_ERR(priv->task)) {
		ret = PTR_ERR(priv->task);
		goto err_clk;
	}
	wake_up_process(priv->task);

	/* register algs */
	for (int i = 0; i < priv->algs_n; i++) {
		ret = hica_muc_alg_register(&priv->algs[i], &hica_muc_tmpls[i],
					    priv);
		if (ret) {
			while (i > 0) {
				i--;
				crypto_unregister_skcipher(&priv->algs[i].alg);
			}
			goto err_task;
		}
	}

	barrier();
	/* tests done; release all hung channels */
	if (chan_mask)
		for (unsigned int id = 0; id < MUC_CHAN_NUM; id++)
			if (chan_mask & BIT(id))
				priv->chans[id].req = NULL;

	return 0;

err_task:
	kthread_stop(priv->task);
err_clk:
	clk_bulk_disable_unprepare(priv->clks_n, priv->clks);
err_rst:
	reset_control_assert(priv->rst);
	return ret;
}

static const struct of_device_id hica_muc_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-advca-muc", },
	{ }
};
MODULE_DEVICE_TABLE(of, hica_muc_of_match);

static struct platform_driver hica_muc_driver = {
	.probe = hica_muc_probe,
	.remove_new = hica_muc_remove,
	.driver = {
		.name = "hisi-advca-muc",
		.of_match_table = hica_muc_of_match,
	},
};

module_platform_driver(hica_muc_driver);

MODULE_DESCRIPTION("HiSilicon Advanced Conditional Access Subsystem - MutiCipher");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
