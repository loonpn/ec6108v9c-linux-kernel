// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SHA - hash device for SHA1/2
 *
 * Copyright (c) 2024 David Yang
 */

#include <crypto/internal/hash.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <linux/clk.h>
#include <linux/crypto.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/string.h>

/******** hardware definitions ********/

#define SHA_TOTAL_LEN_LOW	0x00
#define SHA_TOTAL_LEN_HIGH	0x04
#define SHA_STATUS		0x08
#define  SHA_HASH_READY			BIT(0)
#define  SHA_DMA_READY			BIT(1)
#define  SHA_MSG_READY			BIT(2)
#define  SHA_RECORD_READY		BIT(3)
#define  SHA_ERR_STATE			GENMASK(5, 4)
#define  SHA_LEN_ERR			BIT(6)
#define SHA_CTRL		0x0c
#define  SHA_CTRL_SINGLE_READ		BIT(0)
#define  SHA_CTRL_ALG			GENMASK(2, 1)
#define   SHA_ALG_SHA1				0
#define   SHA_ALG_SHA256			1
#define   SHA_ALG_SHA224			5
#define  SHA_CTRL_HMAC			BIT(3)
#define  SHA_CTRL_KEY_FROM_MKL		BIT(4)  /* key from (not directly accessible) Machine Key Ladder (DRM) */
#define  SHA_CTRL_ENDIAN		BIT(5)  /* misleading reg; always set it */
#define  SHA_CTRL_USED_BY_ARM		BIT(6)  /* v1 only */
#define  SHA_CTRL_USED_BY_C51		BIT(7)  /* v1 only */
#define  SHA_CTRL_SET_INIT		BIT(6)  /* v2 only; use designated initial status */
#define SHA_START		0x10
#define  SHA_START_BIT			BIT(0)
#define SHA_DMA_ADDR		0x14
#define SHA_DMA_LEN		0x18
#define SHA_DATA_IN		0x1c
#define SHA_RECORD_LEN_LOW	0x20
#define SHA_RECORD_LEN_HIGH	0x24
#define SHA_OUT0		0x30  /* till 7 (0x4c) */
#define SHA_MCU_KEY0		0x70  /* till 3 (0x7c) */
#define SHA_KL_KEY0		0x80  /* till 3 (0x8c) */
#define SHA_INIT0		0x90  /* till 7 (0xac) */

#define SHA_KEY_SIZE	16u
#define SHA_DIGEST_SIZE	32u
#define SHA_BLOCK_SIZE	64u

/******** driver definitions ********/

#define SHA_TYPE_HASH	0
/* untested; do test before actually using it */
#define SHA_TYPE_MHASH	1

struct hica_sha_ctrl {
	unsigned int alg;
};

struct hica_sha_alg {
	struct shash_alg alg;
	struct hica_sha_ctrl ctrl;
	struct hica_sha_priv *priv;
};

/* only used for driver registration */
struct hica_sha_tmpl {
	struct hica_sha_ctrl ctrl;

	unsigned int digestsize;
	unsigned int statesize;
	unsigned int blocksize;

	int (*update)(struct shash_desc *desc, const u8 *data,
		      unsigned int len);

	const char *alg_name;
};

struct hica_sha_priv {
	void __iomem *base;
	struct device *dev;

	struct reset_control *rst;
	struct clk_bulk_data *clks;
	unsigned int clks_n;

	struct hica_sha_alg *algs;
	unsigned int algs_n;

	unsigned int type;
	struct mutex lock;
};

struct hica_sha_tfm_ctx {
	struct hica_sha_priv *priv;
	struct hica_sha_ctrl ctrl;

	unsigned int digestsize;
	unsigned int blocksize;

	struct crypto_shash *fallback;
};

struct hica_sha_desc_ctx {
	bool bypass;

	/* keep this at the end of struct! */
	struct shash_desc fallback;
};

static unsigned int bypass_size = 16 * SHA_BLOCK_SIZE;
module_param(bypass_size, uint, S_IRUGO | S_IWUSR);

/******** reg ********/

static int hica_sha_wait(const struct hica_sha_priv *priv, u32 mask,
			 bool nonblocking)
{
	u32 val;

	if (nonblocking)
		return readl_relaxed_poll_timeout_atomic(priv->base + SHA_STATUS,
							 val, val & mask,
							 USEC_PER_MSEC,
							 500 * USEC_PER_MSEC);
	else
		return readl_relaxed_poll_timeout(priv->base + SHA_STATUS, val,
						  val & mask, USEC_PER_MSEC,
						  500 * USEC_PER_MSEC);
}

static int hica_sha_record(const struct hica_sha_priv *priv, dma_addr_t addr,
			   unsigned int len, bool nonblocking)
{
	if (WARN_ON(addr & 3 || len & 3))
		return -EINVAL;

	if (hica_sha_wait(priv, SHA_RECORD_READY, nonblocking))
		return -ETIMEDOUT;

	writel_relaxed(addr, priv->base + SHA_DMA_ADDR);
	writel(len, priv->base + SHA_DMA_LEN);
	return 0;
}

/* must be called before setting SHA_START, and not for SHA_TYPE_HASH */
static void hica_sha_import(const struct hica_sha_priv *priv, const void *state)
{
	for (unsigned int i = 0; i < SHA_DIGEST_SIZE; i += sizeof(u32))
		writel_relaxed((__force __u32)
			       cpu_to_be32(*(const u32 *) (state + i)),
			       priv->base + SHA_INIT0 + i);
}

static int
hica_sha_init(const struct hica_sha_priv *priv,
	      const struct hica_sha_ctrl *ctrl, bool imported, bool nonblocking)
{
	void __iomem *base = priv->base;

	u32 val;
	int ret;

	/* re-enable SHA_START */
	ret = reset_control_assert(priv->rst) ?:
	      reset_control_deassert(priv->rst);
	if (ret)
		return ret;

	/* config SHA_CTRL */
	val = readl_relaxed(base + SHA_CTRL);

	val &= ~SHA_CTRL_SINGLE_READ;
	val &= ~SHA_CTRL_ALG;
	val |= (ctrl->alg << 1) & SHA_CTRL_ALG;
	val &= ~SHA_CTRL_HMAC;
	/* evil config; it is the endianness of every 4-byte input data */
	val |= SHA_CTRL_ENDIAN;

	if (priv->type == SHA_TYPE_HASH)
		val |= SHA_CTRL_USED_BY_ARM;
	else if (imported)
		val |= SHA_CTRL_SET_INIT;

	writel(val, base + SHA_CTRL);

	/* check if we acquired SHA */
	val = readl_relaxed(base + SHA_CTRL);
	if (val & SHA_CTRL_USED_BY_C51)
		return -EBUSY;

	/* wait hardware ready */
	if (hica_sha_wait(priv, SHA_HASH_READY, nonblocking))
		return -ENODEV;

	/* ask hardware to set internal state */
	writel(SHA_START_BIT, base + SHA_START);

	dev_dbg(priv->dev, "alg %u\n", ctrl->alg);
	return 0;
}

static int hica_sha_update(const struct hica_sha_priv *priv, const void *data,
			   unsigned int len, bool nonblocking)
{
	struct device *dev = priv->dev;

	bool inplace = !((uintptr_t) data & 3);
	void *newbuf = NULL;
	dma_addr_t addr;
	int ret;

	if (!len)
		return 0;

	if (WARN_ON(len % SHA_BLOCK_SIZE))
		return -EINVAL;

	/* hardware wants aligned data; check if we can use phyaddr directly */
	if (inplace) {
		addr = dma_map_single(dev, (void *) data, len, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, addr)) {
			dev_err(dev, "error mapping src\n");
			return -EIO;
		}

		inplace = !(addr & 3);
		if (!inplace)
			dma_unmap_single(dev, addr, len, DMA_TO_DEVICE);
	}

	/* if not, manually request an aligned space */
	if (!inplace) {
		newbuf = dma_alloc_attrs(dev, len, &addr,
					 nonblocking ? GFP_ATOMIC : GFP_KERNEL,
					 0);
		if (!newbuf)
			return -ENOMEM;
		memcpy(newbuf, data, len);
	}

	dma_sync_single_for_device(dev, addr, len, DMA_TO_DEVICE);
	ret = hica_sha_record(priv, addr, len, nonblocking) ?:
	      hica_sha_wait(priv, SHA_RECORD_READY, nonblocking);

	if (!newbuf)
		dma_unmap_single(dev, addr, len, DMA_TO_DEVICE);
	else {
		memzero_explicit(newbuf, len);
		dma_free_attrs(dev, len, newbuf, addr, 0);
	}

	dev_dbg(dev, "read %u\n", len);
	return ret;
}

static int hica_sha_export(const struct hica_sha_priv *priv, void *out,
			   unsigned int digestsize)
{
	if (hica_sha_wait(priv, SHA_RECORD_READY, false))
		return -ETIMEDOUT;

	for (unsigned int i = 0; i < digestsize; i += sizeof(u32))
		*(u32 *) (out + i) =
			be32_to_cpu((__force __be32)
				    readl_relaxed(priv->base + SHA_OUT0 + i));

	return 0;
}

/******** shash_alg ********/

static int hica_sha_alg_init(struct shash_desc *desc)
{
	struct hica_sha_desc_ctx *dctx = shash_desc_ctx(desc);
	struct hica_sha_tfm_ctx *ctx = crypto_shash_ctx(desc->tfm);

	dctx->bypass = false;

	dctx->fallback.tfm = ctx->fallback;
	return crypto_shash_init(&dctx->fallback);
}

static int
_hica_sha_alg_update(struct shash_desc *desc, const u8 *data, unsigned int len,
		     void *buf, u32 *state, u64 *count)
{
	struct hica_sha_desc_ctx *dctx = shash_desc_ctx(desc);
	struct hica_sha_tfm_ctx *ctx = crypto_shash_ctx(desc->tfm);
	struct hica_sha_priv *priv = ctx->priv;
	struct device *dev = priv->dev;

	unsigned int top;
	unsigned int bottom;
	int ret;

	if (dctx->bypass || len < bypass_size)
		/* use software directly */
		goto fallback;

	/* how much data has been processed? (struct sha*_state *)buf->count */
	ret = crypto_shash_export(&dctx->fallback, buf);
	if (ret)
		return ret;

	/* chop input data; hardware can only handle full blocks */
	bottom = ALIGN(*count, ctx->blocksize);
	top = ALIGN_DOWN(*count + len, ctx->blocksize);
	/* try using acceleration */
	if ((priv->type == SHA_TYPE_MHASH || !*count) && bottom < top &&
	    top - bottom >= bypass_size) {
		unsigned int head_len = bottom - *count;
		unsigned int body_len = top - bottom;

		/* head: software */
		if (head_len) {
			ret = crypto_shash_update(&dctx->fallback, data,
						  head_len);
			if (ret)
				return ret;

			data += head_len;
			len -= head_len;
		}

		/* body: hardware */
		do {
			bool nonblocking = crypto_shash_get_flags(desc->tfm)  &
					   CRYPTO_TFM_REQ_MAY_SLEEP;

			bool imported;

			if (!bypass_size)
				/* testing; force hardware */
				mutex_lock(&priv->lock);
			else if (!mutex_trylock(&priv->lock))
				break;

			if (head_len) {
				ret = crypto_shash_export(&dctx->fallback, buf);
				if (ret) {
					mutex_unlock(&priv->lock);
					return ret;
				}
			}

			dev_dbg(dev, "before %llu\n", *count);
			pm_runtime_get_sync(dev);

			imported = priv->type == SHA_TYPE_MHASH && *count;
			if (imported)
				hica_sha_import(priv, state);
			ret = hica_sha_init(priv, &ctx->ctrl, imported,
					    nonblocking) ?:
			      hica_sha_update(priv, data, body_len,
					      nonblocking) ?:
			      hica_sha_export(priv, state, ctx->digestsize);

			pm_runtime_mark_last_busy(dev);
			pm_runtime_put_autosuspend(dev);
			mutex_unlock(&priv->lock);
			if (ret)
				return ret;

			*count += body_len;
			dev_dbg(dev, "after %llu\n", *count);

			ret = crypto_shash_import(&dctx->fallback, buf);
			if (ret)
				return ret;

			data += body_len;
			len -= body_len;
		} while (0);

		/* tail: software -> fallback */
	}

	if (priv->type != SHA_TYPE_MHASH)
		dctx->bypass = true;

fallback:
	return crypto_shash_update(&dctx->fallback, data, len);
}

static int hica_sha_alg_update_sha1(struct shash_desc *desc, const u8 *data,
				    unsigned int len)
{
	struct sha1_state state;

	return !len ? 0 : _hica_sha_alg_update(desc, data, len, &state,
					       state.state, &state.count);
}

static int hica_sha_alg_update_sha256(struct shash_desc *desc, const u8 *data,
				      unsigned int len)
{
	struct sha256_state state;

	return !len ? 0 : _hica_sha_alg_update(desc, data, len, &state,
					       state.state, &state.count);
}

static int hica_sha_alg_final(struct shash_desc *desc, u8 *out)
{
	struct hica_sha_desc_ctx *dctx = shash_desc_ctx(desc);

	return crypto_shash_final(&dctx->fallback, out);
}

static int hica_sha_alg_export(struct shash_desc *desc, void *out)
{
	struct hica_sha_desc_ctx *dctx = shash_desc_ctx(desc);

	return crypto_shash_export(&dctx->fallback, out);
}

static int hica_sha_alg_import(struct shash_desc *desc, const void *in)
{
	struct hica_sha_desc_ctx *dctx = shash_desc_ctx(desc);
	struct hica_sha_tfm_ctx *ctx = crypto_shash_ctx(desc->tfm);

	dctx->bypass = false;

	dctx->fallback.tfm = ctx->fallback;
	return crypto_shash_import(&dctx->fallback, in);
}

static int hica_sha_alg_init_tfm(struct crypto_shash *tfm)
{
	struct hica_sha_tfm_ctx *ctx = crypto_shash_ctx(tfm);
	struct hash_alg_common *halg =
		__crypto_hash_alg_common(tfm->base.__crt_alg);
	struct hica_sha_alg *p_alg = container_of(halg, typeof(*p_alg),
						  alg.halg);

	/* copy parameters to avoid pointer hell */
	ctx->digestsize = halg->digestsize;
	ctx->blocksize = crypto_shash_blocksize(tfm);
	if (ctx->digestsize > SHA_DIGEST_SIZE ||
	    ctx->blocksize > SHA_BLOCK_SIZE)
		return -EINVAL;

	ctx->fallback = crypto_alloc_shash(crypto_shash_alg_name(tfm), 0,
					   CRYPTO_ALG_ALLOCATES_MEMORY);
	if (IS_ERR(ctx->fallback))
		return PTR_ERR(ctx->fallback);

	/* update statesize from fallback algorithm */
	tfm->descsize += crypto_shash_descsize(ctx->fallback);

	ctx->priv = p_alg->priv;
	ctx->ctrl = p_alg->ctrl;

	return 0;
}

static void hica_sha_alg_exit_tfm(struct crypto_shash *tfm)
{
	struct hica_sha_tfm_ctx *ctx = crypto_shash_ctx(tfm);

	crypto_free_shash(ctx->fallback);
}

static int hica_sha_alg_register(struct hica_sha_alg *p_alg,
				 const struct hica_sha_tmpl *tmpl,
				 struct hica_sha_priv *priv)
{
	struct crypto_alg *base = &p_alg->alg.halg.base;

	*p_alg = (typeof(*p_alg)) {
		.alg = {
			.init = hica_sha_alg_init,
			.update = tmpl->update,
			.final = hica_sha_alg_final,
			.export = hica_sha_alg_export,
			.import = hica_sha_alg_import,
			.init_tfm = hica_sha_alg_init_tfm,
			.exit_tfm = hica_sha_alg_exit_tfm,

			.descsize = sizeof(struct hica_sha_desc_ctx),

			.halg = {
				.digestsize = tmpl->digestsize,
				.statesize = tmpl->statesize,
				.base = {
					.cra_flags = CRYPTO_ALG_TYPE_SHASH |
						     CRYPTO_ALG_NEED_FALLBACK |
						     CRYPTO_ALG_KERN_DRIVER_ONLY |
						     CRYPTO_ALG_ALLOCATES_MEMORY,
					.cra_blocksize = tmpl->blocksize,
					.cra_ctxsize = sizeof(struct hica_sha_tfm_ctx),
					.cra_alignmask = 0,

					.cra_priority = 200,
					.cra_module = THIS_MODULE,
				},
			},
		},
		.ctrl = tmpl->ctrl,
		.priv = priv,
	};

	snprintf(base->cra_name, sizeof(base->cra_name), "%s", tmpl->alg_name);
	snprintf(base->cra_driver_name, sizeof(base->cra_driver_name),
		 "hisi-advca-%s", tmpl->alg_name);

	return crypto_register_shash(&p_alg->alg);
}

#define hica_sha_tmpl_define(_ALG, _alg, state) { \
	.ctrl = { \
		.alg = SHA_ALG_##_ALG, \
	}, \
	.digestsize = _ALG##_DIGEST_SIZE, \
	.statesize = sizeof(struct state##_state), \
	.blocksize = _ALG##_BLOCK_SIZE, \
	.update = hica_sha_alg_update_##state, \
	.alg_name = #_alg, \
}

static const struct hica_sha_tmpl hica_sha_tmpls[] = {
	hica_sha_tmpl_define(SHA1, sha1, sha1),
	hica_sha_tmpl_define(SHA256, sha256, sha256),

	/* MHASH only */
	hica_sha_tmpl_define(SHA224, sha224, sha256),
};

/******** device ********/

static int __maybe_unused hica_sha_suspend(struct device *dev)
{
	struct hica_sha_priv *priv = dev_get_drvdata(dev);

	int ret;

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		return ret;

	clk_bulk_unprepare(priv->clks_n, priv->clks);

	return 0;
}

static int __maybe_unused hica_sha_resume(struct device *dev)
{
	struct hica_sha_priv *priv = dev_get_drvdata(dev);

	return clk_bulk_prepare(priv->clks_n, priv->clks) ?:
	       pm_runtime_force_resume(dev);
}

static int __maybe_unused hica_sha_runtime_suspend(struct device *dev)
{
	struct hica_sha_priv *priv = dev_get_drvdata(dev);

	clk_bulk_disable(priv->clks_n, priv->clks);

	return 0;
}

static int __maybe_unused hica_sha_runtime_resume(struct device *dev)
{
	struct hica_sha_priv *priv = dev_get_drvdata(dev);

	return clk_bulk_enable(priv->clks_n, priv->clks);
}

static const struct dev_pm_ops hica_sha_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(hica_sha_suspend, hica_sha_resume)
	SET_RUNTIME_PM_OPS(hica_sha_runtime_suspend, hica_sha_runtime_resume,
			   NULL)
};

static void hica_sha_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hica_sha_priv *priv = platform_get_drvdata(pdev);

	for (int i = priv->algs_n; i > 0; ) {
		i--;
		crypto_unregister_shash(&priv->algs[i].alg);
	}

	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	clk_bulk_disable_unprepare(priv->clks_n, priv->clks);
	reset_control_assert(priv->rst);
}

static int hica_sha_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int saved_bypass_size = bypass_size;

	struct hica_sha_priv *priv;
	int ret;

	/* acquire resources */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	ret = devm_clk_bulk_get_all(dev, &priv->clks);
	if (ret < 0)
		return ret;
	priv->clks_n = ret;

	priv->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(priv->rst))
		return PTR_ERR(priv->rst);

	priv->type = (uintptr_t) of_device_get_match_data(dev);

	priv->algs_n = ARRAY_SIZE(hica_sha_tmpls);
	if (priv->type != SHA_TYPE_MHASH)
		priv->algs_n -= 1;

	priv->algs = devm_kmalloc_array(dev, priv->algs_n,
					sizeof(priv->algs[0]), GFP_KERNEL);
	if (!priv->algs)
		return -ENOMEM;

	mutex_init(&priv->lock);

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

	if (hica_sha_wait(priv, SHA_HASH_READY, false)) {
		dev_err(dev, "cannot bring up device\n");
		ret = -ENODEV;
		goto err_clk;
	}

	/* register algs */
	bypass_size = 0;
	for (int i = 0; i < priv->algs_n; i++) {
		ret = hica_sha_alg_register(&priv->algs[i], &hica_sha_tmpls[i],
					    priv);
		if (ret) {
			while (i > 0) {
				i--;
				crypto_unregister_shash(&priv->algs[i].alg);
			}
			bypass_size = saved_bypass_size;
			goto err_clk;
		}
	}
	bypass_size = saved_bypass_size;

	pm_runtime_set_autosuspend_delay(dev, MSEC_PER_SEC);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_irq_safe(dev);
	pm_runtime_enable(dev);
	return 0;

err_clk:
	clk_bulk_disable_unprepare(priv->clks_n, priv->clks);
err_rst:
	reset_control_assert(priv->rst);
	return ret;
}

static const struct of_device_id hica_sha_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-advca-sha",
	  .data = (void *) SHA_TYPE_HASH },
	{ }
};
MODULE_DEVICE_TABLE(of, hica_sha_of_match);

static struct platform_driver hica_sha_driver = {
	.probe = hica_sha_probe,
	.remove_new = hica_sha_remove,
	.driver = {
		.name = "hisi-advca-sha",
		.of_match_table = hica_sha_of_match,
		.pm = &hica_sha_pm_ops,
	},
};

module_platform_driver(hica_sha_driver);

MODULE_DESCRIPTION("HiSilicon Advanced Conditional Access Subsystem - SHA");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
