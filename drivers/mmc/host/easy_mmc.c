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
#include <linux/mmc/mmc.h>

#include "mmci.h"

#define MMCI_POLL_TIMEOUT_US	500000

struct mmci_poll_host {
	void __iomem *base;
	struct clk *clk;
	struct clk *pclk;
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

static int mmci_poll_wait_ready(struct mmci_poll_host *host)
{
	unsigned int timeout = MMCI_POLL_TIMEOUT_US;
	u32 status, r1;

	while (timeout--) {
		writel(0xffffffff, host->base + MMCICLEAR);
		writel(1 << 16, host->base + MMCIARGUMENT);
		writel(MCI_CPSM_ENABLE |
		       MCI_CPSM_RESPONSE |
		       MMC_SEND_STATUS,
		       host->base + MMCICOMMAND);

		if (mmci_poll_wait(host,
				   MCI_CMDRESPEND |
				   MCI_CMDTIMEOUT |
				   MCI_CMDCRCFAIL,
				   &status))
			return -ETIMEDOUT;

		if (status & (MCI_CMDTIMEOUT | MCI_CMDCRCFAIL))
			return -EIO;

		r1 = readl(host->base + MMCIRESPONSE0);

		pr_info("mmci-poll: busy poll r1=%08x\n", r1);

		/*
		 * MSM8960/SDCC4 does not always transition cleanly back to
		 * TRAN after CMD12. Some eMMC parts remain reporting RCV
		 * while simultaneously asserting READY_FOR_DATA.
		 */
		if ((r1 & R1_READY_FOR_DATA) &&
		    R1_CURRENT_STATE(r1) != R1_STATE_PRG)
			return 0;

		udelay(10);
	}

	pr_err("mmci-poll: card never became ready\n");
	return -ETIMEDOUT;
}

static u32 mmci_poll_datactrl(struct mmc_data *data)
{
	u32 ctrl = MCI_DPSM_ENABLE | (data->blksz << 4);

	if (data->flags & MMC_DATA_READ)
		ctrl |= MCI_DPSM_DIRECTION;
	else
		ctrl |= MCI_DPSM_QCOM_DATA_PEND;

	return ctrl;
}

static int mmci_poll_stop(struct mmci_poll_host *host,
                          struct mmc_command *stop)
{
    u32 status;
    u32 cmdreg;
    int ret;

    if (!stop)
        return 0;

    /*
     * CMD12: STOP_TRANSMISSION
     *
     * No data phase.
     * R1 response.
     */
    cmdreg = MCI_CPSM_ENABLE |
             stop->opcode |
             MCI_CPSM_RESPONSE;

    /*
     * Don't leave the previous CPSM command active.
     */
    if (readl(host->base + MMCICOMMAND) & MCI_CPSM_ENABLE) {
        writel(0, host->base + MMCICOMMAND);
        udelay(2);
    }

    writel(0xffffffff, host->base + MMCICLEAR);

    writel(stop->arg, host->base + MMCIARGUMENT);
    writel(cmdreg, host->base + MMCICOMMAND);

    pr_info("mmci-poll: STOP CMD%d arg=%08x cmdreg=%08x\n",
            stop->opcode, stop->arg, cmdreg);

    ret = mmci_poll_wait(host,
                         MCI_CMDRESPEND |
                         MCI_CMDSENT |
                         MCI_CMDTIMEOUT |
                         MCI_CMDCRCFAIL,
                         &status);

    if (ret) {
        pr_err("mmci-poll: STOP CMD%d timeout status=%08x\n",
               stop->opcode,
               readl(host->base + MMCISTATUS));
        stop->error = ret;
        return ret;
    }

    if (status & MCI_CMDTIMEOUT) {
        stop->error = -ETIMEDOUT;
        return -ETIMEDOUT;
    }

    if ((status & MCI_CMDCRCFAIL) &&
        (stop->flags & MMC_RSP_CRC)) {
        stop->error = -EIO;
        return -EIO;
    }

    stop->resp[0] = readl(host->base + MMCIRESPONSE0);
    stop->resp[1] = readl(host->base + MMCIRESPONSE1);
    stop->resp[2] = readl(host->base + MMCIRESPONSE2);
    stop->resp[3] = readl(host->base + MMCIRESPONSE3);

    stop->error = 0;

    pr_info("mmci-poll: STOP CMD%d resp=%08x\n",
            stop->opcode, stop->resp[0]);

    return 0;
}

static int mmci_poll_xfer(struct mmci_poll_host *host,
			  struct mmc_data *data)
{
	struct scatterlist *sg;
	unsigned int timeout;
	int i, words;
	u32 *buf;
	u32 status;

	/*
	 * QCOM SDCC4 starts the data phase noticeably later than the
	 * command response phase.
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

		while (words) {
			if (data->flags & MMC_DATA_READ) {
				if (mmci_poll_wait(host,
						   MCI_RXFIFOHALFFULL |
						   MCI_RXDATAAVLBL |
						   MCI_DATATIMEOUT |
						   MCI_DATACRCFAIL |
						   MCI_RXOVERRUN,
						   &status)) {
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

					while (burst--) {
						*buf++ = readl(host->base + MMCIFIFO);
						words--;
					}
				} else {
					*buf++ = readl(host->base + MMCIFIFO);
					words--;
				}
			} else {
				u32 datacnt, fifocnt;

				if (mmci_poll_wait(host,
						   MCI_TXFIFOHALFEMPTY |
						   MCI_TXFIFOEMPTY |
						   MCI_DATATIMEOUT |
						   MCI_DATACRCFAIL |
						   MCI_TXUNDERRUN,
						   &status)) {
					pr_err("mmci-poll: TX wait timeout status=%08x datacnt=%08x fifocnt=%08x\n",
					       readl(host->base + MMCISTATUS),
					       readl(host->base + MMCIDATACNT),
					       readl(host->base + MMCIFIFOCNT));
					return -ETIMEDOUT;
				}

				datacnt = readl(host->base + MMCIDATACNT);
				fifocnt = readl(host->base + MMCIFIFOCNT);

				pr_info("mmci-poll: TX status=%08x datacnt=%08x fifocnt=%08x words=%d\n",
					status, datacnt, fifocnt, words + 1);

				if (status & (MCI_DATATIMEOUT |
					      MCI_DATACRCFAIL |
					      MCI_TXUNDERRUN)) {
					pr_err("mmci-poll: TX error status=%08x datacnt=%08x fifocnt=%08x\n",
					       status, datacnt, fifocnt);
					return -EIO;
				}

				if (status & MCI_TXFIFOHALFEMPTY) {
					int burst = min(words + 1, 8);

					pr_info("mmci-poll: TX burst=%d\n", burst);

					while (burst--)
					{
						writel(*buf++, host->base + MMCIFIFO);
						words--;
					}
				} else {
					writel(*buf++, host->base + MMCIFIFO);
					words--;
				}
			}
		}
	}

	timeout = MMCI_POLL_TIMEOUT_US;

	while (timeout--) {
		status = readl(host->base + MMCISTATUS);

		if (!(timeout % 100000))
			pr_info("mmci-poll: DATAEND wait status=%08x datacnt=%08x fifocnt=%08x\n",
				status,
				readl(host->base + MMCIDATACNT),
				readl(host->base + MMCIFIFOCNT));

		if (status & (MCI_DATAEND |
			      MCI_DATATIMEOUT |
			      MCI_DATACRCFAIL))
			break;

		udelay(1);
	}

	if (!(status & (MCI_DATAEND |
			MCI_DATATIMEOUT |
			MCI_DATACRCFAIL))) {

		status = readl(host->base + MMCISTATUS);

		pr_err("mmci-poll: DATAEND timeout status=%08x\n",
		       readl(host->base + MMCISTATUS));
		return -ETIMEDOUT;
	}

	if (status & (MCI_DATATIMEOUT | MCI_DATACRCFAIL)) {
		pr_err("mmci-poll: DATAEND error status=%08x\n", status);
		return -EIO;
	}

	pr_info("mmci-poll: DATAEND done status=%08x datacnt=%08x fifocnt=%08x\n",
		status,
		readl(host->base + MMCIDATACNT),
		readl(host->base + MMCIFIFOCNT));

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
	int ret;

	cmd->error = 0;

	if (mrq->data) {
		mrq->data->error = 0;
		mrq->data->bytes_xfered = 0;
	}

	if (mrq->stop)
		mrq->stop->error = 0;

	cmdreg = MCI_CPSM_ENABLE | cmd->opcode;

	if (cmd->flags & MMC_RSP_PRESENT)
		cmdreg |= MCI_CPSM_RESPONSE;

	if (cmd->flags & MMC_RSP_136)
		cmdreg |= MCI_CPSM_LONGRSP;

	if (mrq->data)
		cmdreg |= MCI_CPSM_QCOM_DATCMD;

	/*
	 * PL18x/Qualcomm CPSM can wedge if a previous command leaves
	 * CPSM enabled.
	 */
	if (readl(host->base + MMCICOMMAND) & MCI_CPSM_ENABLE) {
		writel(0, host->base + MMCICOMMAND);
		udelay(2);
	}

	pr_info("mmci-poll: CMD%d arg=%08x flags=%08x data=%p\n",
		cmd->opcode, cmd->arg, cmd->flags, mrq->data);

	writel(0xffffffff, host->base + MMCICLEAR);

	if (mrq->data) {
		u32 datactrl = mmci_poll_datactrl(mrq->data);

		writel(0xffffffff, host->base + MMCIDATATIMER);
		writel(mrq->data->blocks * mrq->data->blksz,
		       host->base + MMCIDATALENGTH);

		pr_info("mmci-poll: datactrl=%08x blocks=%u blksz=%u\n",
			datactrl,
			mrq->data->blocks,
			mrq->data->blksz);

		/*
		 * QCOM ordering:
		 *   read  -> DATACTRL before command
		 *   write -> DATACTRL after command
		 */
		if (mrq->data->flags & MMC_DATA_READ) {
			writel(datactrl, host->base + MMCIDATACTRL);
			udelay(10);
		}
	}

	writel(cmd->arg, host->base + MMCIARGUMENT);
	writel(cmdreg, host->base + MMCICOMMAND);

	pr_info("mmci-poll: CMD%d issued cmdreg=%08x\n",
		cmd->opcode, cmdreg);

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
		cmd->opcode, status);

	/*
	 * R3 responses intentionally have no CRC and QCOM/PL18x can
	 * report CMDCRCFAIL for them.
	 */
	if ((status & MCI_CMDCRCFAIL) &&
	    !(cmd->flags & MMC_RSP_CRC))
		pr_info("mmci-poll: CMD%d no-crc response arrived\n",
			cmd->opcode);

	if (status & MCI_CMDTIMEOUT) {
		pr_err("mmci-poll: CMD%d command timeout\n", cmd->opcode);
		cmd->error = -ETIMEDOUT;
		goto done;
	}

	if ((status & MCI_CMDCRCFAIL) && (cmd->flags & MMC_RSP_CRC)) {
		pr_err("mmci-poll: CMD%d crc failure status=%08x\n",
		       cmd->opcode, status);
		cmd->error = -EIO;
		goto done;
	}

	cmd->resp[0] = readl(host->base + MMCIRESPONSE0);
	cmd->resp[1] = readl(host->base + MMCIRESPONSE1);
	cmd->resp[2] = readl(host->base + MMCIRESPONSE2);
	cmd->resp[3] = readl(host->base + MMCIRESPONSE3);

	/*
	 * Hack CMD13 RCV->TRAN before mmc core sees the response.
	 */
	if (cmd->opcode == MMC_SEND_STATUS &&
	    (cmd->resp[0] & R1_READY_FOR_DATA) &&
	    R1_CURRENT_STATE(cmd->resp[0]) == R1_STATE_RCV) {
		u32 old = cmd->resp[0];

		cmd->resp[0] &= ~0x1e00;
		cmd->resp[0] |= R1_STATE_TRAN << 9;

		pr_warn("mmci-poll: hacked CMD13 resp %08x -> %08x\n",
			old, cmd->resp[0]);
	}

	pr_info("mmci-poll: CMD%d resp=%08x %08x %08x %08x\n",
		cmd->opcode,
		cmd->resp[0],
		cmd->resp[1],
		cmd->resp[2],
		cmd->resp[3]);

	if (mrq->data && (mrq->data->flags & MMC_DATA_WRITE)) {
		u32 datactrl = mmci_poll_datactrl(mrq->data);

		pr_info("mmci-poll: write datactrl=%08x\n", datactrl);

		writel(datactrl, host->base + MMCIDATACTRL);
		udelay(10);
	}

	if (mrq->data) {
		pr_info("mmci-poll: CMD%d data transfer blocks=%u blksz=%u flags=%08x\n",
			cmd->opcode,
			mrq->data->blocks,
			mrq->data->blksz,
			mrq->data->flags);

		pr_info("mmci-poll: data status=%08x datacnt=%08x datactrl=%08x\n",
			readl(host->base + MMCISTATUS),
			readl(host->base + MMCIDATACNT),
			readl(host->base + MMCIDATACTRL));

		mrq->data->error = mmci_poll_xfer(host, mrq->data);

		pr_info("mmci-poll: CMD%d data transfer done err=%d bytes=%u\n",
			cmd->opcode,
			mrq->data->error,
			mrq->data->bytes_xfered);
	}

	/*
	* Multi-block transfer termination.
	*
	* CMD25 / CMD18 require CMD12 when the request has
	* a stop command.
	*/
	if (mrq->stop && mrq->data) {
		ret = mmci_poll_stop(host, mrq->stop);

		if (ret) {
			pr_err("mmci-poll: stop command failed %d\n", ret);
			goto done;
		}
	}

	if (mrq->data &&
	    !mrq->data->error &&
	    (mrq->data->flags & MMC_DATA_WRITE)) {
		ret = mmci_poll_wait_ready(host);

		if (ret) {
			pr_err("mmci-poll: write busy wait failed %d\n", ret);
			mrq->data->error = ret;
		}
	}

done:
	pr_info("mmci-poll: CMD%d finished cmd_err=%d data_err=%d stop_err=%d bytes=%u\n",
		cmd->opcode,
		cmd->error,
		mrq->data ? mrq->data->error : 0,
		mrq->stop ? mrq->stop->error : 0,
		mrq->data ? mrq->data->bytes_xfered : 0);

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

	clk_set_rate(host->clk, ios->clock);

	clk = MCI_CLK_ENABLE;

	if (ios->bus_width == MMC_BUS_WIDTH_4)
		clk |= MCI_4BIT_BUS;
	else if (ios->bus_width == MMC_BUS_WIDTH_8)
		clk |= MCI_QCOM_CLK_WIDEBUS_8;

	clk |= MCI_QCOM_CLK_FLOWENA |
	       MCI_QCOM_CLK_SELECT_IN_FBCLK;

	pr_info("mmci-poll: set_ios clock=%u div=%u clkreg=%08x bus_width=%u timing=%u\n",
		ios->clock,
		0,
		clk,
		ios->bus_width,
		ios->timing);

	pwr = MCI_PWR_ON;

	if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN)
		pwr |= MCI_ROD;

	writel(MCI_PWR_UP, host->base + MMCIPOWER);
	udelay(200);

	writel(pwr, host->base + MMCIPOWER);
	udelay(200);

	writel(clk, host->base + MMCICLOCK);
	udelay(200);
}

static const struct mmc_host_ops mmci_poll_ops = {
	.request = mmci_poll_request,
	.set_ios = mmci_poll_set_ios,
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

	mmc->caps |= MMC_CAP_NONREMOVABLE;
	mmc->caps2 |= MMC_CAP2_NO_SD | MMC_CAP2_NO_SDIO;

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

disable_mclk:
	clk_disable_unprepare(host->clk);
err:
	mmc_free_host(mmc);
	return ret;
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