/*
 * Driver for SpreadTrum SC88XX Series SPI Controllers
 *
 * Copyright (C) 2010 SpreadTrum Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/spi/spi.h>

#include <asm/io.h>
#include <mach/board.h>
#include <mach/gpio.h>

#include "spi_sc88xx.h"
#define SPRD_SPI_DEBUG  0
#define SPRD_SPI_CS_GPIO 1 /* you should also modify the spi_func_cfg[] */

// lock is held, spi irq is disabled
static int sprd_spi_do_transfer(struct sprd_spi_data *sprd_data)
{
    if (sprd_data->cspi_trans == 0) return 0; /* when both tx & rx mode starts, first tx irq enters, 
                                               * then rx irq enters -- this time tx 
                                               * had set sprd_data->cspi_trans to 0, so return 0 [luther.ge]
                                               */

    spi_do_reset(sprd_data->cspi_trans->tx_dma,
                 sprd_data->cspi_trans->rx_dma); // spi hardwrare module has a bug, we must read busy bit twice to soft fix

    if (sprd_data->cspi_trans_num >= sprd_data->cspi_trans->len) {
        sprd_data->cspi_msg->actual_length += sprd_data->cspi_trans_num;
#if SPRD_SPI_DEBUG
        printk(KERN_WARNING "send ok\n");
#endif
        if (!sprd_data->cspi_msg->is_dma_mapped)
            sprd_spi_dma_unmap_transfer(sprd_data, sprd_data->cspi_trans);
        if (sprd_data->cspi_msg->transfers.prev != &sprd_data->cspi_trans->transfer_list) {
            grab_subsibling(sprd_data);
        } else {
            cs_deactivate(sprd_data, sprd_data->cspi); // all msg sibling data trans done
            list_del(&sprd_data->cspi_msg->queue);
            sprd_data->cspi_msg->status = 0; // msg tranfsered successfully
            spin_unlock(&sprd_data->lock);
            if (sprd_data->cspi_msg->complete) // spi_sync call
                sprd_data->cspi_msg->complete(sprd_data->cspi_msg->context);
            spin_lock(&sprd_data->lock);
            sprd_data->cspi_trans = 0;
            if (list_empty(&sprd_data->queue)) {
                // no spi data on queue to transfer
                // cs_deactivate(sprd_data, sprd_data->cspi);
#if SPRD_SPI_DEBUG
                printk(KERN_WARNING "spi msg queue done.\n");
#endif
                return 0;
            }
            grab_sibling(sprd_data);
        }
    }
#if SPRD_SPI_DEBUG
    else {
        if (sprd_data->cspi_trans_num)
            printk(KERN_WARNING "Left data [%d]:[%d]\n", sprd_data->cspi_trans_num, sprd_data->cspi_trans->len);
    }
#endif

    if (sprd_data->cspi_trans == 0) return -ENOSR;

/*
    if (sprd_data->cspi != sprd_data->cspi_msg->spi) {
        cs_deactivate(sprd_data, sprd_data->cspi);
        cs_activate(sprd_data, sprd_data->cspi_msg->spi);
    }
*/
    cs_activate(sprd_data, sprd_data->cspi_msg->spi);
    sprd_data->cspi_trans_num += sprd_dma_update_spi(sprd_data->cspi_trans->tx_dma,
                                            sprd_data->cspi_trans->len,
                                            sprd_data->cspi_trans->rx_dma,
                                            sprd_data->cspi_trans->len,
                                            sprd_data->cspi,
                                            sprd_data);

    return 0;
}

static int sprd_spi_transfer(struct spi_device *spi, struct spi_message *msg)
{
    struct sprd_spi_data *sprd_data;
    struct spi_transfer *trans;
    unsigned long flags;
    int ret = 0;

    sprd_data = spi_master_get_devdata(spi->master);

	if (unlikely(list_empty(&msg->transfers)))
		return -EINVAL;

	if (sprd_data->stopping)
		return -ESHUTDOWN;

	list_for_each_entry(trans, &msg->transfers, transfer_list) {
		/* FIXME implement these protocol options!! */
		if (trans->bits_per_word || trans->speed_hz) {
			dev_dbg(&spi->dev, "no protocol options yet\n");
			return -ENOPROTOOPT;
		}

		if (!msg->is_dma_mapped) {
			if (sprd_spi_dma_map_transfer(sprd_data, trans) < 0)
				return -ENOMEM;
		}
#if SPRD_SPI_DEBUG
        else {
            printk(KERN_WARNING "spi dma msg\n");
        }
#endif
#if SPRD_SPI_ONLY_RX_AND_TXRX_BUG_FIX
        if (!trans->tx_dma && trans->rx_dma) {
            trans->tx_buf = SPRD_SPI_ONLY_RX_AND_TXRX_BUG_FIX_IGNORE_ADDR;
            trans->tx_dma = sprd_data->buffer_dma;
        }
#endif
        if (!(trans->tx_dma || trans->rx_dma) || !trans->len)
            return -ENOMEM;
	}

	msg->status = -EINPROGRESS;
	msg->actual_length = 0;

    spin_lock_irqsave(&sprd_data->lock, flags);
    list_add_tail(&msg->queue, &sprd_data->queue);
    if (sprd_data->cspi_trans == 0) {
        grab_sibling(sprd_data);
        ret = sprd_spi_do_transfer(sprd_data);
    }
    spin_unlock_irqrestore(&sprd_data->lock, flags);

    return ret;
}

#if SPRD_SPI_DMA_MODE
static void
#else
static irqreturn_t
#endif
sprd_spi_interrupt(int irq, void *dev_id)
{
    struct spi_master *master = dev_id;
    struct sprd_spi_data *sprd_data = spi_master_get_devdata(master);

    spin_lock(&sprd_data->lock);
#if SPRD_SPI_DEBUG
    // lprintf("spi irq [ %d ]\n", irq);
#endif
    if (sprd_data->cspi_trans &&
        sprd_data->cspi_trans->tx_dma && 
        sprd_data->cspi_trans->rx_dma) {
        sprd_dma_stop(irq);
        if (++sprd_data->tx_rx_finish < 2) {
            // printk("ignore tx_rx first irq\n");
            spin_unlock(&sprd_data->lock);
#if !SPRD_SPI_DMA_MODE
            return IRQ_HANDLED;
#endif
        }
        sprd_data->tx_rx_finish = 0;
        // printk("tx_rx irq ok\n");
    }
    if (sprd_spi_do_transfer(sprd_data) < 0)
        printk(KERN_ERR "error : %s\n", __func__);
    spin_unlock(&sprd_data->lock);

#if !SPRD_SPI_DMA_MODE
    return IRQ_HANDLED;
#endif
}

static inline void cs_activate(struct sprd_spi_data *sprd_data, struct spi_device *spi)
{
    if (sprd_data->cs_null) {
        struct sprd_spi_controller_data *sprd_ctrl_data = spi->controller_data;

        spi_writel(sprd_ctrl_data->spi_clkd, SPI_CLKD);
        spi_writel(sprd_ctrl_data->spi_ctl0, SPI_CTL0);

        __raw_bits_or((sprd_ctrl_data->clk_spi_and_div & 0xffff) << 21, GR_GEN2);
        __raw_bits_or((sprd_ctrl_data->clk_spi_and_div >> 16) << 26, GR_CLK_DLY);

#if SPRD_SPI_CS_GPIO
         __gpio_set_value(sprd_ctrl_data->cs_gpio, spi->mode & SPI_CS_HIGH);
#endif

        sprd_data->cspi = spi;

        sprd_data->cs_null = 0;
    }
}

static inline void cs_deactivate(struct sprd_spi_data *sprd_data, struct spi_device *spi)
{
#if SPRD_SPI_CS_GPIO
    if (spi) {
        struct sprd_spi_controller_data *sprd_ctrl_data = spi->controller_data;
        u32 cs_gpio = sprd_ctrl_data->cs_gpio;
        __gpio_set_value(cs_gpio, !(spi->mode & SPI_CS_HIGH));
    }
#else
    spi_bits_or(0x0F << 8, SPI_CTL0);
#endif
    sprd_data->cs_null = 1;
}

/*
 * For DMA, tx_buf/tx_dma have the same relationship as rx_buf/rx_dma:
 *  - The buffer is either valid for CPU access, else NULL
 *  - If the buffer is valid, so is its DMA addresss
 *
 * This driver manages the dma addresss unless message->is_dma_mapped.
 */
static int sprd_spi_dma_map_transfer(struct sprd_spi_data *sprd_data, struct spi_transfer *trans)
{
    struct device *dev = &sprd_data->pdev->dev;

    trans->tx_dma = trans->rx_dma = 0;
    if (trans->tx_buf) {
        trans->tx_dma = dma_map_single(dev,
                (void *) trans->tx_buf, trans->len,
                DMA_TO_DEVICE);
        if (dma_mapping_error(dev, trans->tx_dma))
            return -ENOMEM;
    }
    if (trans->rx_buf) {
        trans->rx_dma = dma_map_single(dev,
                trans->rx_buf, trans->len,
                DMA_FROM_DEVICE);
        if (dma_mapping_error(dev, trans->rx_dma)) {
            if (trans->tx_buf)
                dma_unmap_single(dev,
                        trans->tx_dma, trans->len,
                        DMA_TO_DEVICE);
            return -ENOMEM;
        }
    }
    return 0;
}

static void sprd_spi_dma_unmap_transfer(struct sprd_spi_data *sprd_data, struct spi_transfer *trans)
{
    struct device *dev = &sprd_data->pdev->dev;

    if (trans->tx_dma 
#if SPRD_SPI_ONLY_RX_AND_TXRX_BUG_FIX
        && (trans->tx_buf != SPRD_SPI_ONLY_RX_AND_TXRX_BUG_FIX_IGNORE_ADDR)
#endif
       )
        dma_unmap_single(dev, trans->tx_dma, trans->len, DMA_TO_DEVICE);

    if (trans->rx_dma)
        dma_unmap_single(dev, trans->rx_dma, trans->len, DMA_FROM_DEVICE);
}

static int sprd_spi_setup_dma(struct sprd_spi_data *sprd_data, struct spi_device *spi)
{
    int i, ch_id;
    u32 dsrc, ddst;
    int width;
    int autodma_src, autodma_dst, autodma_burst_mod_src, autodma_burst_mod_dst;
    struct sprd_spi_controller_data *sprd_ctrl_data = spi->controller_data;
    sprd_dma_ctrl ctrl;// = {.dma_desc = &sprd_ctrl_data->dma_desc};

    for (i = 0; i < 2; i++) {
        if (i == 0) ch_id = DMA_SPI_RX;
        else ch_id = DMA_SPI_TX;

        if (ch_id == DMA_SPI_RX) {
            ctrl.dma_desc = &sprd_ctrl_data->dma_desc_rx;
            autodma_src = DMA_NOCHANGE;
            autodma_dst = DMA_INCREASE;
            autodma_burst_mod_src = SRC_BURST_MODE_SINGLE;
            autodma_burst_mod_dst = SRC_BURST_MODE_4;
        } else {
            ctrl.dma_desc = &sprd_ctrl_data->dma_desc_tx;
            autodma_src = DMA_INCREASE;
            autodma_dst = DMA_NOCHANGE;
            autodma_burst_mod_src = SRC_BURST_MODE_4;
            autodma_burst_mod_dst = SRC_BURST_MODE_SINGLE;
        }

        width = (sprd_ctrl_data->spi_ctl0 >> 2) & 0x1f;
        if (width == 0) width = 32;

#if 0
        if (ch_id == DMA_SPI_RX) {
            dsrc = SPRD_SPI_PHYS + SPI_TXD;
            ddst = 0;
        } else {
            dsrc = 0;
            ddst = SPRD_SPI_PHYS + SPI_TXD;
        }
#endif

        dsrc = ddst = SPRD_SPI_PHYS + SPI_TXD;

        sprd_dma_setup_cfg(&ctrl,
                ch_id,
                DMA_NORMAL,
                TRANS_DONE_EN,
                autodma_src, autodma_dst,
                autodma_burst_mod_src, autodma_burst_mod_dst, // SRC_BURST_MODE_SINGLE, SRC_BURST_MODE_SINGLE,
                SPRD_SPI_BURST_SIZE_DEFAULT, // 16 bytes DMA burst size
                width, width,
                dsrc, ddst, 0);
         sprd_dma_setup(&ctrl);
    }

    // 综合实验决定将SPI_CTL3的rx大小设置为1
    // SPI_CTL3的rx大小决定了DMA数据读取操作,如果非1,有时导致需要读取slave数据
    // 到一定个数之后才会满足DMA传输完毕的条件,才会触发dma irq[luther.ge]
//    spi_writel((width + 7) / 8/*0x0001*/, SPI_CTL3);
    sprd_ctrl_data->data_width = (width + 7) / 8;

    switch (sprd_ctrl_data->data_width) {
        case 1: sprd_ctrl_data->data_width_order = 0; break;
        case 2: sprd_ctrl_data->data_width_order = 1; break;
        case 4: sprd_ctrl_data->data_width_order = 2; break;
    }

    sprd_ctrl_data->data_max = sprd_ctrl_data->data_width * SPRD_SPI_DMA_BLOCK_MAX;

    // only rx watermark for dma, 0x02 means slave device must send at least 2 bytes
    // when i set 0x01 to SPI_CTL3, Atheros 16 bits data read is wrong,
    // after i change 0x01 to 0x02, everything is ok
#if !SPRD_SPI_RX_WATERMARK_BUG_FIX
    spi_writel(0x02 | (0x00 << 8), SPI_CTL3);
#endif
    spi_writel(0x01 | (0x00 << 8), SPI_CTL6); // tx watermark for dma

    return 0;
}

static inline int sprd_dma_update_spi(u32 sptr, u32 slen, u32 dptr, u32 dlen,
                        struct spi_device *spi, struct sprd_spi_data *sprd_data)
{
    // static int chip_select = -1;
    struct sprd_spi_controller_data *sprd_ctrl_data = spi->controller_data;
    sprd_dma_desc *dma_desc;
    int flag, len, offset;
    int blocks = 1;

    offset = sprd_data->cspi_trans_num;

    if (unlikely(sptr && dptr))
        flag = 0xff;        // for both
    else if (likely(sptr))
        flag = 0x02;        // for tx only
    else {
        flag = 0x01;        // for rx only
    }

    if (flag & 0x01) {
        // only rx or tx both
        dlen -= offset;
#if SPRD_SPI_RX_WATERMARK_BUG_FIX
        len = dlen;
    {
        int water_mark;
        water_mark = len > SPRD_SPI_RX_WATERMARK_MAX ? SPRD_SPI_RX_WATERMARK_MAX:len;
        spi_writel(water_mark | (water_mark << 8), SPI_CTL3);
    }
#else
        len = dlen > sprd_ctrl_data->data_max ? sprd_ctrl_data->data_max : dlen;
        if (likely(sprd_ctrl_data->data_width != 3)) {
            blocks = len >> sprd_ctrl_data->data_width_order;
        } else blocks = len / sprd_ctrl_data->data_width;
        // blocks = len / sprd_ctrl_data->data_width; // for 1byte 2bytes 3bytes 4bytes
#endif
        dma_desc = &sprd_ctrl_data->dma_desc_rx;
        dma_desc->tlen = len;
        dma_desc->ddst = dptr + offset;

        sprd_spi_update_burst_size();
        sprd_dma_update(DMA_SPI_RX, dma_desc); // use const ch_id value to speed up code exec [luther.ge]
    }

    if (flag & 0x02) {
        if ((flag & 0x01) == 0) len = slen; // only tx
        dma_desc = &sprd_ctrl_data->dma_desc_tx;
        dma_desc->tlen = len;
        dma_desc->dsrc = sptr + offset;
#if SPRD_SPI_ONLY_RX_AND_TXRX_BUG_FIX
        if (sprd_data->cspi_trans->tx_buf == SPRD_SPI_ONLY_RX_AND_TXRX_BUG_FIX_IGNORE_ADDR)
            dma_desc->dsrc = sptr;
#endif
        sprd_spi_update_burst_size();
        sprd_dma_update(DMA_SPI_TX, dma_desc); // use const ch_id value to speed up code exec [luther.ge]
    }
#if SPRD_SPI_DEBUG
    printk(KERN_WARNING "sending[%02x][%d]...\n", flag, len);
#endif

    /* if (spi->chip_select != chip_select) */ {
        /* when slot0,slot1,...,slotx,同时启动spi_dummy.c的8个测试线程的时,
         * dma传输会时常自动停止,必须在每次启动时加入如下300us延时
         * 8个测试线程才能正常的操作spclk分别为8M和500K的2个slot [luther.ge]
         * 但是如果单独启动slot0的4个线程或者单独启动slot1的4个线程都不会出现该问题
         * 只要8个线程同时启动就会出现上面的问题.
         *
         * 当打开SPRD_SPI_ONLY_RX_AND_TXRX_BUG_FIX之后,该udelay(300us)可以取消[luther.ge]
         *
         */
//        udelay(300);
        // chip_select = spi->chip_select;
    }

    if (flag == 0xff)
        spi_start();
    else if (flag & 0x01)
        spi_start_rx(blocks);
    else spi_start_tx();

    spi_dma_start(); // Must Enable SPI_DMA_EN last

    return len;
}

static int sprd_spi_setup(struct spi_device *spi)
{
    struct sprd_spi_data *sprd_data;
    struct sprd_spi_controller_data *sprd_ctrl_data = spi->controller_data;
    u8 bits = spi->bits_per_word;
    u32 clk_spi;
    u8  clk_spi_mode;
    u32 cs_gpio;
    u8 clk_spi_div;
    u32 spi_clkd;
    u32 spi_ctl0 = 0;
    int ret;

    sprd_data = spi_master_get_devdata(spi->master);

    if (sprd_data->stopping)
        return -ESHUTDOWN;
    if (spi->chip_select > spi->master->num_chipselect)
        return -EINVAL;
    if (bits > 32)
        return -EINVAL;
    if (bits == 32) bits = 0;

    clk_spi_mode = (__raw_readl(GR_CLK_DLY) >> 26) & 0x03;
    switch (clk_spi_mode) {
        case 0: clk_spi = 192   * 1000 * 1000; break;
        case 1: clk_spi = 153.6 * 1000 * 1000; break;
        case 2: clk_spi = 96    * 1000 * 1000; break;
        case 3: clk_spi = 26    * 1000 * 1000; break; 
    }
    clk_spi_div = (__raw_readl(GR_GEN2) >> 21) & 0x07;
    clk_spi /= (clk_spi_div + 1);

    // spi_clk = clk_spi / (2 * (spi_clkd + 1));
    if (spi->max_speed_hz) {
        spi_clkd = clk_spi / (2*spi->max_speed_hz) - 1;
    } else {
        spi_clkd = 0xffff;
    }
    if (spi_clkd < 0) {
        printk(KERN_WARNING "Warning: %s your spclk %d is so big!\n", __func__, spi->max_speed_hz);
        spi_clkd = 0;
    }

    /* chipselect must have been muxed as GPIO (e.g. in board setup) 
     * and we assume cs_gpio real gpio number not exceeding 0xffff
     */
    cs_gpio = (u32)spi->controller_data;
    if (cs_gpio < 0xffff) {
        sprd_ctrl_data = kzalloc(sizeof *sprd_ctrl_data, GFP_KERNEL);
        if (!sprd_ctrl_data)
            return -ENOMEM;

        ret = gpio_request(cs_gpio, dev_name(&spi->dev));
        if (ret) {
            printk(KERN_WARNING "%s[%s] cs_gpio %d is busy!", spi->modalias, dev_name(&spi->dev), cs_gpio);
            // kfree(sprd_ctrl_data);
            // return ret;
        }
        sprd_ctrl_data->cs_gpio = cs_gpio;
        spi->controller_data = sprd_ctrl_data;
        gpio_direction_output(cs_gpio, !(spi->mode & SPI_CS_HIGH));
    } else {
        unsigned long flags;
        spin_lock_irqsave(&sprd_data->lock, flags);
        if (sprd_data->cspi == spi)
            sprd_data->cspi = NULL;
        cs_deactivate(sprd_data, spi);
        spin_unlock_irqrestore(&sprd_data->lock, flags);
    }
    sprd_ctrl_data->clk_spi_and_div = clk_spi_div | (clk_spi_mode << 16);
    sprd_ctrl_data->spi_clkd = spi_clkd;

	if (spi->mode & SPI_CPHA)
         spi_ctl0 |= 0x01;
    else spi_ctl0 |= 0x02;

	if (spi->mode & SPI_CPOL)
        spi_ctl0 |= 1 << 13;
    else spi_ctl0 |= 0 << 13;

    spi_ctl0 |= bits << 2;
#if SPRD_SPI_CS_GPIO
    spi_ctl0 |= 0x0F << 8;
#else
    switch (spi->chip_select) {
        case 2:
        case 0: spi_ctl0 |= 0x0E << 8; break;
        case 3:
        case 1: spi_ctl0 |= 0x0D << 8; break;
        default: spi_ctl0 |= 0x0F << 8;break;
    }
#endif

    sprd_ctrl_data->spi_ctl0 = spi_ctl0;

    sprd_spi_setup_dma(sprd_data, spi);

    return 0;
}

static void spi_complete2(void *arg)
{
    *((int *)arg) = 1;
}

int spi_sync2(struct spi_device *spi, struct spi_message *message)
{
    volatile int done = 0;
    int status;

    message->complete = spi_complete2;
    message->context = (void*)&done;
    status = spi_async(spi, message);
    if (status == 0) {
        while (!done);
        status = message->status;
    }
    message->context = NULL;
    return status;
}
EXPORT_SYMBOL_GPL(spi_sync2);

static void sprd_spi_cleanup(struct spi_device *spi)
{
    struct sprd_spi_data *sprd_data = spi_master_get_devdata(spi->master);
    struct sprd_spi_controller_data *sprd_ctrl_data = spi->controller_data;
	u32 cs_gpio = (u32)sprd_ctrl_data;
	unsigned long flags;

    if (cs_gpio < 0xffff)
        return;
    cs_gpio = sprd_ctrl_data->cs_gpio;

	spin_lock_irqsave(&sprd_data->lock, flags);
	if (sprd_data->cspi == spi) {
		sprd_data->cspi = NULL;
		cs_deactivate(sprd_data, spi);
	}
    spi->controller_data = (void*)cs_gpio;
	spin_unlock_irqrestore(&sprd_data->lock, flags);

	gpio_free(cs_gpio);
	kfree(sprd_ctrl_data);
}

static int __init sprd_spi_probe(struct platform_device *pdev)
{
    struct resource *regs;
	int irq, ret;
	struct spi_master *master;
	struct sprd_spi_data *sprd_data;

    regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!regs)
        return -ENXIO;

    irq = platform_get_irq(pdev, 0);
    if (irq < 0)
        return irq;

    ret = -ENOMEM;
    master = spi_alloc_master(&pdev->dev, sizeof *sprd_data);
    if (!master)
        goto out_free;

    /* the spi->mode bits understood by this driver: */
    master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_3WIRE;

    master->bus_num = pdev->id;
    master->num_chipselect = 64;
    master->setup = sprd_spi_setup;
    master->transfer = sprd_spi_transfer;
    master->cleanup = sprd_spi_cleanup;
    platform_set_drvdata(pdev, master);

    sprd_data = spi_master_get_devdata(master);

    /*
     * Scratch buffer is used for throwaway rx and tx data.
     * It's coherent to minimize dcache pollution.
     */
    sprd_data->buffer = dma_alloc_coherent(&pdev->dev, SPRD_SPI_BUFFER_SIZE,
                                        &sprd_data->buffer_dma, GFP_KERNEL);
    if (!sprd_data->buffer)
        goto out_free;

    memset(sprd_data->buffer, 0x55, SPRD_SPI_BUFFER_SIZE);

    spin_lock_init(&sprd_data->lock);
    INIT_LIST_HEAD(&sprd_data->queue);
    sprd_data->pdev = pdev;
    sprd_data->regs = ioremap(regs->start, (regs->end - regs->start) + 1);
    if (!sprd_data->regs)
        goto out_free_buffer;
    sprd_data->irq = irq;

#if SPRD_SPI_DMA_MODE
    ret = sprd_request_dma(DMA_SPI_TX, sprd_spi_interrupt, master);
    ret = sprd_request_dma(DMA_SPI_RX, sprd_spi_interrupt, master);
#else
    ret = request_irq(irq, sprd_spi_interrupt, 0,
                dev_name(&pdev->dev), master);
#endif
    if (ret)
        goto out_unmap_regs;

    /* Initialize the hardware */
    __raw_bits_or(GEN0_SPI_EN, GR_GEN0); // Enable SPI module
    __raw_bits_or(SWRST_SPI_RST, GR_SOFT_RST);
    msleep(1);
    __raw_bits_and(~SWRST_SPI_RST, GR_SOFT_RST);
    spi_writel(0, SPI_INT_EN);
    // clk source selected to 96M
    __raw_bits_and(~(0x03 << 26), GR_CLK_DLY);
    // __raw_bits_or(2 << 26, GR_CLK_DLY);
    /*
     * clk_spi_div sets to 1, so clk_spi=96M/2=48M,
     * and clk_spi is SPI_CTL5 interval base clock.[luther.ge]
     */
    __raw_bits_and(~(0x07 << 21), GR_GEN2);
    // __raw_bits_or(1 << 21, GR_GEN2);

    sprd_data->cs_null = 1;

    ret = spi_register_master(master);
    if (ret)
        goto out_reset_hw;

    return 0;

out_reset_hw:
#if SPRD_SPI_DMA_MODE
    sprd_free_dma(DMA_SPI_TX);
    sprd_free_dma(DMA_SPI_RX);
#else
    free_irq(irq, master);
#endif
out_unmap_regs:
    iounmap(sprd_data->regs);
out_free_buffer:
    dma_free_coherent(&pdev->dev, SPRD_SPI_BUFFER_SIZE, sprd_data->buffer,
                    sprd_data->buffer_dma);
out_free:
    spi_master_put(master);
    return ret;
}

static int __exit sprd_spi_remove(struct platform_device *pdev)
{
    struct spi_master	*master = platform_get_drvdata(pdev);
    struct sprd_spi_data *sprd_data = spi_master_get_devdata(master);
	struct spi_message *msg;
    /* reset the hardware and block queue progress */
    spin_lock_irq(&sprd_data->lock);
    sprd_data->stopping = 1;
    spin_unlock_irq(&sprd_data->lock);

    /* Terminate remaining queued transfers */
    list_for_each_entry(msg, &sprd_data->queue, queue) {
        msg->status = -ESHUTDOWN;
        msg->complete(msg->context);
    }

    dma_free_coherent(&pdev->dev, SPRD_SPI_BUFFER_SIZE, sprd_data->buffer,
                    sprd_data->buffer_dma);
#if SPRD_SPI_DMA_MODE
    sprd_free_dma(DMA_SPI_TX);
    sprd_free_dma(DMA_SPI_RX);
#else
    free_irq(sprd_data->irq, master);
#endif
    iounmap(sprd_data->regs);

    spi_unregister_master(master);

    __raw_bits_and(~GEN0_SPI_EN, GR_GEN0); // Disable SPI module

    return 0;
}

#ifdef CONFIG_PM
static int sprd_spi_suspend(struct platform_device *pdev, pm_message_t mesg)
{
    return 0;
}

static int sprd_spi_resume(struct platform_device *pdev)
{
    return 0;
}
#else
#define	sprd_spi_suspend NULL
#define	sprd_spi_resume NULL
#endif

static struct platform_driver sprd_spi_driver = {
    .driver		= {
        .name	= "sprd_spi",
        .owner	= THIS_MODULE,
    },
    .suspend	= sprd_spi_suspend,
    .resume		= sprd_spi_resume,
    .remove		= __exit_p(sprd_spi_remove),
};

static int __init sprd_spi_init(void)
{
    return platform_driver_probe(&sprd_spi_driver, sprd_spi_probe);
}
module_init(sprd_spi_init);

static void __exit sprd_spi_exit(void)
{
    platform_driver_unregister(&sprd_spi_driver);
}
module_exit(sprd_spi_exit);

MODULE_DESCRIPTION("SpreadTrum SC88XX Series SPI Controller driver");
MODULE_AUTHOR("Luther Ge <luther.ge@spreadtrum.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sprd_spi");