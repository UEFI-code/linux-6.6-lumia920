// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal MMCI eMMC polling driver.
 *
 * Bring-up oriented:
 *  - no IRQ
 *  - no DMA
 *  - pure polling
 *  - direct FIFO access
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>

#include <linux/mmc/host.h>

#include "mmci.h"

#define MMCI_POLL_TIMEOUT_US	500000

struct mmci_poll_host {
	void __iomem *base;
	struct clk *clk;
	struct clk *pclk;
	u32 last_clk;
};

static int mmci_poll_wait(struct mmci_poll_host *host, u32 mask, u32 *status)
{
	unsigned int timeout = MMCI_POLL_TIMEOUT_US;

	while (timeout--) {
		*status = readl(host->base + MMCISTATUS);

		if (*status & mask)
			return 0;

		udelay(1);
	}

	return -ETIMEDOUT;
}

static int mmci_poll_xfer(struct mmci_poll_host *host,
				  struct mmc_data *data)
{
	struct scatterlist *sg;
	u32 *buf;
	u32 status;
	bool saw_data = false;
	unsigned int timeout;
	int i;
	int words;

	/*
	 * QCOM SDCC4 starts the data phase noticeably later than the
	 * command response phase. Wait for the RX state machine to enter
	 * active/data-available state before declaring FIFO timeout.
	 */
	timeout = MMCI_POLL_TIMEOUT_US;
	while (timeout--) {
		status = readl(host->base + MMCISTATUS);

		if (status & (MCI_RXACTIVE |
			      MCI_RXDATAAVLBL |
			      MCI_RXFIFOHALFFULL |
			      MCI_DATATIMEOUT |
			      MCI_DATACRCFAIL |
			      MCI_RXOVERRUN))
			break;

		udelay(1);
	}

	pr_info("mmci-poll: xfer entry status=%08x datacnt=%08x fifocnt=%08x\n",
		status,
		readl(host->base + MMCIDATACNT),
		readl(host->base + MMCIFIFOCNT));

	for_each_sg(data->sg, sg, data->sg_len, i) {
		buf = sg_virt(sg);
		words = sg->length >> 2;

		while (words--) {
			if (data->flags & MMC_DATA_READ) {
				u32 datacnt;
				if (mmci_poll_wait(host,
						   MCI_RXFIFOHALFFULL |
						   MCI_RXDATAAVLBL |
						   MCI_DATATIMEOUT |
						   MCI_DATACRCFAIL |
						   MCI_RXOVERRUN,
						   &status))
				{
					datacnt = readl(host->base + MMCIDATACNT);
					status = readl(host->base + MMCISTATUS);

					pr_err("mmci-poll: RX wait timeout status=%08x\n",
					       status);
					return -ETIMEDOUT;
				}

				if (status & (MCI_DATATIMEOUT |
					      MCI_DATACRCFAIL |
					      MCI_RXOVERRUN)) {
					pr_err("mmci-poll: RX error status=%08x\n",
					       status);
					return -EIO;
				}

				if (status & MCI_RXFIFOHALFFULL) {
					int burst = min(words + 1, 8);

					saw_data = true;

					while (burst--) {
						*buf++ = readl(host->base + MMCIFIFO);
						words--;
					}

					words++;
				} else {
					saw_data = true;
					*buf++ = readl(host->base + MMCIFIFO);
				}
			} else {
				if (mmci_poll_wait(host,
						   MCI_TXFIFOHALFEMPTY |
						   MCI_TXFIFOEMPTY |
						   MCI_TXACTIVE |
						   MCI_DATATIMEOUT |
						   MCI_DATACRCFAIL |
						   MCI_TXUNDERRUN,
						   &status))
					return -ETIMEDOUT;

				if (status & (MCI_DATATIMEOUT |
					      MCI_DATACRCFAIL |
					      MCI_TXUNDERRUN))
					return -EIO;

				/*
				 * QCOM SDCC4 write path behaves like upstream PIO mode:
				 * feed the FIFO as soon as HALFEMPTY becomes asserted
				 * instead of waiting for a completely empty FIFO.
				 */
				writel(*buf++, host->base + MMCIFIFO);
			}
		}
	}

	while (1) {
		status = readl(host->base + MMCISTATUS);

		if (status & (MCI_DATAEND |
			      MCI_DATATIMEOUT |
			      MCI_DATACRCFAIL))
			break;

		udelay(1);
	}

	if (!(status & (MCI_DATAEND |
			 MCI_DATATIMEOUT |
			 MCI_DATACRCFAIL))) {
		u32 datacnt;
		u32 fifocnt;
		u32 fifo;

		datacnt = readl(host->base + MMCIDATACNT);
		status = readl(host->base + MMCISTATUS);
		fifocnt = readl(host->base + MMCIFIFOCNT);

		/*
		 * Match upstream QCOM PIO behavior more closely.
		 *
		 * MSM8960 occasionally finishes the transfer without ever
		 * asserting DATAEND. At this point RXACTIVE already dropped
		 * and only a single stale byte remains accounted in DATACNT.
		 *
		 * Drain a final FIFO word if one is still present and accept
		 * completion once the data state machine is idle.
		 */
		if (!(status & MCI_RXACTIVE) && datacnt <= 1) {
			if (!(status & MCI_RXFIFOEMPTY) && fifocnt) {
				fifo = readl(host->base + MMCIFIFO);
				pr_info("mmci-poll: drained final fifo word=%08x fifocnt=%u\n",
					fifo,
					fifocnt);
			}

			pr_warn("mmci-poll: missing DATAEND, forcing completion status=%08x datacnt=%u\n",
				 status,
				 datacnt);

			data->bytes_xfered = data->blocks * data->blksz;
			return 0;
		}

		pr_err("mmci-poll: DATAEND timeout status=%08x\n",
		       readl(host->base + MMCISTATUS));
		return -ETIMEDOUT;
	}

	if (status & (MCI_DATATIMEOUT | MCI_DATACRCFAIL)) {
		pr_err("mmci-poll: DATAEND error status=%08x\n",
		       status);
		return -EIO;
	}

	if ((data->flags & MMC_DATA_READ) && !saw_data) {
		pr_err("mmci-poll: no RX data observed status=%08x datacnt=%08x fifocnt=%08x\n",
		       readl(host->base + MMCISTATUS),
		       readl(host->base + MMCIDATACNT),
		       readl(host->base + MMCIFIFOCNT));
		return -EIO;
	}

	data->bytes_xfered = data->blocks * data->blksz;

	return 0;
}

static void mmci_poll_request(struct mmc_host *mmc,
				      struct mmc_request *mrq)
{
	struct mmci_poll_host *host = mmc_priv(mmc);
	struct mmc_command *cmd = mrq->cmd;
	u32 status;
	u32 cmdreg;

	cmdreg = MCI_CPSM_ENABLE | cmd->opcode;

	if (cmd->flags & MMC_RSP_PRESENT)
		cmdreg |= MCI_CPSM_RESPONSE;

	if (cmd->flags & MMC_RSP_136)
		cmdreg |= MCI_CPSM_LONGRSP;

	/*
	 * Qualcomm MMCI requires DATCMD for commands carrying a data
	 * phase (CMD8 EXT_CSD, reads, writes, etc).
	 *
	 * Upstream mmci.c sets variant->data_cmd_enable which maps to
	 * MCI_CPSM_QCOM_DATCMD.
	 */
	if (mrq->data)
		cmdreg |= MCI_CPSM_QCOM_DATCMD;

	/*
	 * PL18x/Qualcomm CPSM occasionally wedges if a previous command
	 * leaves CPSM enabled. Match upstream mmci.c behavior and hard
	 * stop CPSM before issuing a new command.
	 */
	if (readl(host->base + MMCICOMMAND) & MCI_CPSM_ENABLE) {
		writel(0, host->base + MMCICOMMAND);
		udelay(2);
	}

	pr_info("mmci-poll: CMD%d arg=%08x flags=%08x data=%p\n",
		cmd->opcode,
		cmd->arg,
		cmd->flags,
		mrq->data);

	writel(0xffffffff, host->base + MMCICLEAR);

	if (mrq->data) {
		u32 datactrl = MCI_DPSM_ENABLE;

		writel(0xffffffff, host->base + MMCIDATATIMER);
		writel(mrq->data->blocks * mrq->data->blksz,
		       host->base + MMCIDATALENGTH);

		/*
		 * Upstream qcom_get_dctrl_cfg() uses the raw block size in
		 * the legacy PL18x position, not the exponent encoding used
		 * by other variants.
		 *
		 * For 512-byte EXT_CSD reads this becomes:
		 *   512 << 4 = 0x2000
		 */
		datactrl |= mrq->data->blksz << 4;

		/*
		 * Upstream qcom_get_dctrl_cfg() does not set the Qualcomm
		 * DATA_PEND/RX_DATA_PEND bits for normal PIO transfers.
		 *
		 * For polling mode on MSM8960 these bits prevent the DPSM
		 * from ever entering RXACTIVE state, leaving FIFOCNT=0.
		 */
		if (mrq->data->flags & MMC_DATA_READ)
			datactrl |= MCI_DPSM_DIRECTION;

		pr_info("mmci-poll: datactrl=%08x blocks=%u blksz=%u\n",
			datactrl,
			mrq->data->blocks,
			mrq->data->blksz);

		/*
		 * Match upstream qcom variant ordering:
		 *
		 *  - reads:  datactrl before command
		 *  - writes: command before datactrl
		 *
		 * Starting DPSM before a write command on MSM8960 causes
		 * the TX state machine to desynchronize and later fail with
		 * generic CMD13 write I/O errors.
		 */
		if (mrq->data->flags & MMC_DATA_READ) {
			writel(datactrl, host->base + MMCIDATACTRL);
			udelay(10);
		}
	}

	writel(cmd->arg, host->base + MMCIARGUMENT);
	writel(cmdreg, host->base + MMCICOMMAND);

	pr_info("mmci-poll: CMD%d issued cmdreg=%08x\n",
		cmd->opcode,
		cmdreg);

	if (mmci_poll_wait(host,
			   MCI_CMDRESPEND |
			   MCI_CMDSENT |
			   MCI_CMDTIMEOUT |
			   MCI_CMDCRCFAIL,
			   &status)) {
		pr_err("mmci-poll: CMD%d wait timeout status=%08x\n",
		       cmd->opcode,
		       readl(host->base + MMCISTATUS));

		cmd->error = -ETIMEDOUT;
		goto done;
	}

	pr_info("mmci-poll: CMD%d completed status=%08x\n",
		cmd->opcode,
		status);

	/*
	 * PL18x/QCOM reports R3-style responses (CMD1/CMD5/etc)
	 * as CMDCRCFAIL because these responses intentionally carry
	 * no CRC. Upstream mmci.c treats this as a valid response
	 * unless MMC_RSP_CRC is requested.
	 */
	if ((status & MCI_CMDCRCFAIL) &&
	    !(cmd->flags & MMC_RSP_CRC)) {
		pr_info("mmci-poll: CMD%d no-crc response arrived\n",
			cmd->opcode);
	}

	if (status & MCI_CMDTIMEOUT) {
		pr_err("mmci-poll: CMD%d command timeout\n",
		       cmd->opcode);

		cmd->error = -ETIMEDOUT;
		goto done;
	}

	if ((status & MCI_CMDCRCFAIL) && (cmd->flags & MMC_RSP_CRC)) {
		pr_err("mmci-poll: CMD%d crc failure status=%08x\n",
		       cmd->opcode,
		       status);

		cmd->error = -EIO;
		goto done;
	}

	cmd->resp[0] = readl(host->base + MMCIRESPONSE0);
	cmd->resp[1] = readl(host->base + MMCIRESPONSE1);
	cmd->resp[2] = readl(host->base + MMCIRESPONSE2);
	cmd->resp[3] = readl(host->base + MMCIRESPONSE3);

	pr_info("mmci-poll: CMD%d resp=%08x %08x %08x %08x\n",
		cmd->opcode,
		cmd->resp[0],
		cmd->resp[1],
		cmd->resp[2],
		cmd->resp[3]);

	if (mrq->data && (mrq->data->flags & MMC_DATA_WRITE)) {
		u32 datactrl = MCI_DPSM_ENABLE;

		datactrl |= mrq->data->blksz << 4;

		pr_info("mmci-poll: write datactrl=%08x\n", datactrl);

		writel(datactrl, host->base + MMCIDATACTRL);
		udelay(10);
	}

	if (mrq->data)
		pr_info("mmci-poll: CMD%d starting data transfer blocks=%u blksz=%u flags=%08x\n",
			cmd->opcode,
			mrq->data->blocks,
			mrq->data->blksz,
			mrq->data->flags);

	if (mrq->data)
		pr_info("mmci-poll: data status=%08x datacnt=%08x datactrl=%08x\n",
			readl(host->base + MMCISTATUS),
			readl(host->base + MMCIDATACNT),
			readl(host->base + MMCIDATACTRL));

	if (mrq->data)
		mrq->data->error = mmci_poll_xfer(host, mrq->data);

	if (mrq->data)
		pr_info("mmci-poll: CMD%d data transfer done err=%d bytes=%u\n",
			cmd->opcode,
			mrq->data->error,
			mrq->data->bytes_xfered);

done:
	pr_info("mmci-poll: CMD%d finished cmd_err=%d\n",
		cmd->opcode,
		cmd->error);

	writel(0xffffffff, host->base + MMCICLEAR);
	mmc_request_done(mmc, mrq);
}

static void mmci_poll_set_ios(struct mmc_host *mmc,
				      struct mmc_ios *ios)
{
	struct mmci_poll_host *host = mmc_priv(mmc);
	u32 clk;
	u32 pwr;

	if (!ios->clock) {
		writel(0, host->base + MMCICLOCK);
		writel(0, host->base + MMCIPOWER);
		return;
	}

	/*
	 * Match upstream Qualcomm variant behavior:
	 * explicit mclk scaling instead of legacy PL18x divider.
	 */
	clk_set_rate(host->clk, ios->clock);

	clk = MCI_CLK_ENABLE;

	if (ios->bus_width == MMC_BUS_WIDTH_4)
		clk |= MCI_4BIT_BUS;
	else if (ios->bus_width == MMC_BUS_WIDTH_8)
		clk |= MCI_QCOM_CLK_WIDEBUS_8;

	/*
	 * Upstream Qualcomm variant always enables FLOWENA and FBCLK
	 * sampling, including during legacy identification mode.
	 *
	 * Original driver log:
	 *   clkreg=00009100
	 */
	clk |= MCI_QCOM_CLK_FLOWENA |
	       MCI_QCOM_CLK_SELECT_IN_FBCLK;

	pr_info("mmci-poll: set_ios clock=%u div=%u clkreg=%08x bus_width=%u timing=%u\n",
		ios->clock,
		0,
		clk,
		ios->bus_width,
		ios->timing);

	/*
	 * Qualcomm variant uses ROD instead of classic OD.
	 * Using the wrong bit lets CMD0 work but breaks open-drain
	 * identification commands like CMD1.
	 */
	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN)
		pwr = MCI_PWR_ON | MCI_ROD;
	else
		pwr = MCI_PWR_ON;

	/*
	 * Qualcomm SDCC/eMMC init is sensitive to power sequencing.
	 * Explicitly perform PWR_UP -> delay -> PWR_ON before enabling
	 * clocking, otherwise CMD1 may never produce a response.
	 */
	writel(MCI_PWR_UP, host->base + MMCIPOWER);
	udelay(200);
	writel(pwr, host->base + MMCIPOWER);
	udelay(200);

	host->last_clk = clk;

	writel(clk, host->base + MMCICLOCK);
	udelay(200);
}

static int mmci_poll_card_busy(struct mmc_host *mmc)
{
	struct mmci_poll_host *host = mmc_priv(mmc);
	u32 status;

	status = readl(host->base + MMCISTATUS);

	/*
	 * Minimal bring-up driver:
	 * MSM8960 DAT0 busy detection is unreliable until full
	 * Qualcomm busy handling is implemented.
	 *
	 * Returning permanently busy causes MMC core init to abort
	 * immediately after successful CMD1.
	 */
	pr_info("mmci-poll: busy status=%08x\n", status);

	return 0;
}

static const struct mmc_host_ops mmci_poll_ops = {
	.request = mmci_poll_request,
	.set_ios = mmci_poll_set_ios,
	.card_busy = mmci_poll_card_busy,
};

static int mmci_poll_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct mmci_poll_host *host;
	struct resource *res;
	int ret;

	mmc = mmc_alloc_host(sizeof(*host), &pdev->dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	host->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(host->base)) {
		ret = PTR_ERR(host->base);
		goto err;
	}

	host->clk = devm_clk_get(&pdev->dev, "mclk");
	if (IS_ERR(host->clk)) {
		ret = PTR_ERR(host->clk);
		goto err;
	}

	ret = clk_prepare_enable(host->clk);
	if (ret)
		goto err;

	host->pclk = devm_clk_get(&pdev->dev, "apb_pclk");
	if (IS_ERR(host->pclk)) {
		ret = PTR_ERR(host->pclk);
		goto disable_mclk;
	}

	ret = clk_prepare_enable(host->pclk);
	if (ret)
		goto disable_mclk;

	mmc->ops = &mmci_poll_ops;

	/*
	 * Lumia 920 internal storage is soldered eMMC.
	 * This minimal bring-up driver only supports MMC mode.
	 */
	mmc->caps |= MMC_CAP_NONREMOVABLE;
	mmc->caps2 |= MMC_CAP2_NO_SD;
	mmc->caps2 |= MMC_CAP2_NO_SDIO;

	/*
	 * Lumia 920 eMMC runs at standard 2.7V-3.6V MMC voltages.
	 * Without OCR capabilities advertised, MMC core rejects the
	 * card immediately after successful CMD1.
	 */
	mmc->ocr_avail = MMC_VDD_27_28 |
			 MMC_VDD_28_29 |
			 MMC_VDD_29_30 |
			 MMC_VDD_30_31 |
			 MMC_VDD_31_32 |
			 MMC_VDD_32_33 |
			 MMC_VDD_33_34 |
			 MMC_VDD_34_35 |
			 MMC_VDD_35_36;

	mmc->f_min = 400000;
	mmc->f_max = 50000000;
	mmc->max_blk_size = 512;
	mmc->max_blk_count = 2048;
	mmc->max_req_size = 1024 * 1024;
	mmc->max_seg_size = mmc->max_req_size;
	mmc->max_segs = 1;

	ret = mmc_add_host(mmc);
	if (ret)
		goto err;

	platform_set_drvdata(pdev, mmc);

	dev_info(&pdev->dev, "minimal polling MMCI enabled\n");
	return 0;

err:
	mmc_free_host(mmc);
	return ret;

disable_mclk:
	clk_disable_unprepare(host->clk);
	goto err;
}

static void mmci_poll_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct mmci_poll_host *host = mmc_priv(mmc);

	clk_disable_unprepare(host->pclk);
	clk_disable_unprepare(host->clk);

	mmc_remove_host(mmc);
	mmc_free_host(mmc);
}

static const struct of_device_id mmci_poll_of_match[] = {
	{ .compatible = "qcom,msm8960-easy-mmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, mmci_poll_of_match);

static struct platform_driver easy_mmc_drv = {
	.probe = mmci_poll_probe,
	.remove_new = mmci_poll_remove,
	.driver = {
		.name = "mmci-polling",
		.of_match_table = mmci_poll_of_match,
	},
};

module_platform_driver(easy_mmc_drv);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal MMCI polling-only eMMC driver");