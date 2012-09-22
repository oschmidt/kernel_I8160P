/*
 * Block driver for media (i.e., flash cards)
 *
 * Copyright 2002 Hewlett-Packard Company
 * Copyright 2005-2008 Pierre Ossman
 *
 * Use consistent with the GNU GPL is permitted,
 * provided that this copyright notice is
 * preserved in its entirety in all copies and derived works.
 *
 * HEWLETT-PACKARD COMPANY MAKES NO WARRANTIES, EXPRESSED OR IMPLIED,
 * AS TO THE USEFULNESS OR CORRECTNESS OF THIS CODE OR ITS
 * FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 * Many thanks to Alessandro Rubini and Jonathan Corbet!
 *
 * Author:  Andrew Christian
 *          28 May 2002
 */
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/hdreg.h>
#include <linux/kdev_t.h>
#include <linux/blkdev.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/string_helpers.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>

#include <asm/system.h>
#include <asm/uaccess.h>

#include "queue.h"

#include "../core/core.h"


MODULE_ALIAS("mmc:block");
#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif

#ifdef MOVI_DEBUG
typedef struct{
	u32 cmd;
	u32 arg;
	u32 cnt;
	u32 rsp;
	u32 stoprsp;
} CMD_LOG;
CMD_LOG gaCmdLog[5];
gnCmdLogIdx = 0;

#endif

#define MODULE_PARAM_PREFIX "mmcblk."

static DEFINE_MUTEX(block_mutex);

/*
 * The defaults come from config options but can be overriden by module
 * or bootarg options.
 */
static int perdev_minors = CONFIG_MMC_BLOCK_MINORS;

/*
 * We've only got one major, so number of mmcblk devices is
 * limited to 256 / number of minors per device.
 */
static int max_devices;

/* 256 minors, so at most 256 separate devices */
static DECLARE_BITMAP(dev_use, 256);

/*
 * There is one mmc_blk_data per slot.
 */
struct mmc_blk_data {
	spinlock_t	lock;
	struct gendisk	*disk;
	struct mmc_queue queue;

	unsigned int	usage;
	unsigned int	read_only;
};

static DEFINE_MUTEX(open_lock);

module_param(perdev_minors, int, 0444);
MODULE_PARM_DESC(perdev_minors, "Minors numbers to allocate per device");

static struct mmc_blk_data *mmc_blk_get(struct gendisk *disk)
{
	struct mmc_blk_data *md;

	mutex_lock(&open_lock);
	md = disk->private_data;
	if (md && md->usage == 0)
		md = NULL;
	if (md)
		md->usage++;
	mutex_unlock(&open_lock);

	return md;
}

static void mmc_blk_put(struct mmc_blk_data *md)
{
	mutex_lock(&open_lock);
	md->usage--;
	if (md->usage == 0) {
		int devmaj = MAJOR(disk_devt(md->disk));
		int devidx = MINOR(disk_devt(md->disk)) / perdev_minors;

		if (!devmaj)
			devidx = md->disk->first_minor / perdev_minors;

		blk_cleanup_queue(md->queue.queue);

		__clear_bit(devidx, dev_use);

		put_disk(md->disk);
		kfree(md);
	}
	mutex_unlock(&open_lock);
}

static int mmc_blk_open(struct block_device *bdev, fmode_t mode)
{
	struct mmc_blk_data *md = mmc_blk_get(bdev->bd_disk);
	int ret = -ENXIO;

	if (md) {
		if (md->usage == 2)
			check_disk_change(bdev);
		ret = 0;

		if ((mode & FMODE_WRITE) && md->read_only) {
			mmc_blk_put(md);
			ret = -EROFS;
		}
	}

	return ret;
}

static int mmc_blk_release(struct gendisk *disk, fmode_t mode)
{
	struct mmc_blk_data *md = disk->private_data;

	mmc_blk_put(md);
	return 0;
}

static int
mmc_blk_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	geo->cylinders = get_capacity(bdev->bd_disk) / (4 * 16);
	geo->heads = 4;
	geo->sectors = 16;
	return 0;
}

static const struct block_device_operations mmc_bdops = {
	.open			= mmc_blk_open,
	.release		= mmc_blk_release,
	.getgeo			= mmc_blk_getgeo,
	.owner			= THIS_MODULE,
};

struct mmc_blk_request {
	struct mmc_request	mrq;
	struct mmc_command	cmd;
	struct mmc_command	stop;
	struct mmc_data		data;
};

static u32 mmc_sd_num_wr_blocks(struct mmc_card *card)
{
	int err;
	u32 result;
	__be32 *blocks;

	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	unsigned int timeout_us;

	struct scatterlist sg;

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = MMC_APP_CMD;
	cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err)
		return (u32)-1;
	if (!mmc_host_is_spi(card->host) && !(cmd.resp[0] & R1_APP_CMD))
		return (u32)-1;

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = SD_APP_SEND_NUM_WR_BLKS;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	memset(&data, 0, sizeof(struct mmc_data));

	data.timeout_ns = card->csd.tacc_ns * 100;
	data.timeout_clks = card->csd.tacc_clks * 100;

	timeout_us = data.timeout_ns / 1000;
	timeout_us += data.timeout_clks * 1000 /
		(card->host->ios.clock / 1000);

	if (timeout_us > 100000) {
		data.timeout_ns = 100000000;
		data.timeout_clks = 0;
	}

	data.blksz = 4;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	memset(&mrq, 0, sizeof(struct mmc_request));

	mrq.cmd = &cmd;
	mrq.data = &data;

	blocks = kmalloc(4, GFP_KERNEL);
	if (!blocks)
		return (u32)-1;

	sg_init_one(&sg, blocks, 4);

	mmc_wait_for_req(card->host, &mrq);

	result = ntohl(*blocks);
	kfree(blocks);

	if (cmd.error || data.error)
		result = (u32)-1;

	return result;
}

static int get_card_status(struct mmc_card *card,
				struct request *req,
				u32 *status,
				int retries)
{
	struct mmc_command cmd;
	int err;

	memset(&cmd, 0, sizeof(struct mmc_command));
	cmd.opcode = MMC_SEND_STATUS;
	if (!mmc_host_is_spi(card->host))
		cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, retries);
	if (err)
		printk(KERN_ERR "%s: error %d sending status comand",
		       req->rq_disk->disk_name, err);
	else
		*status = cmd.resp[0];

	return err;
}

static int wait_for_ready_state(struct mmc_card *card, struct request *req)
{
	u32 status;
	int err = 0;

	if (!mmc_host_is_spi(card->host) && rq_data_dir(req) != READ) {
	/* timeout value set 0x30000 : It works just SDcard case.
	 * It means send CMD sequencially about 7.8sec.
	 * If SDcard's data line stays low, timeout is about 4sec.
	 * max timeout is up to 300ms
	 */
		u32 timeout = 0x30000;
		do {
			err = get_card_status(card, req, &status, 5);
			if (err)
				break;

			/*
			 * Some cards mishandle the status bits,
			 * so make sure to check both the busy
			 * indication and the card state.
			 */
	/* Just SDcard case, decrease timeout */
			if (mmc_card_sd(card))
				timeout--;
		} while ((!(status & R1_READY_FOR_DATA) ||
			(R1_CURRENT_STATE(status) == 7)) &&
			timeout);
	/* If SDcard stays busy status, timeout is to be zero */
		if (!timeout) {
			pr_err("%s: card state has been never changed "
					"to trans.!\n",
					req->rq_disk->disk_name);
			return err;
		}
	}

	return err;
}

static int
mmc_blk_set_blksize(struct mmc_blk_data *md, struct mmc_card *card)
{
	int err;

	mmc_claim_host(card->host);
	err = mmc_set_blocklen(card, 512);
	mmc_release_host(card->host);

	if (err) {
		printk(KERN_ERR "%s: unable to set block size to 512: %d\n",
			md->disk->disk_name, err);
		return -EINVAL;
	}

	return 0;
}

static int mmc_blk_issue_rq(struct mmc_queue *mq, struct request *req)
{
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;
	struct mmc_blk_request brq;
	int ret = 1, disable_multi = 0;

#ifdef CONFIG_MMC_BLOCK_DEFERRED_RESUME
	if (mmc_bus_needs_resume(card->host)) {
		mmc_resume_bus(card->host);
		mmc_blk_set_blksize(md, card);
	}
#endif

	mmc_claim_host(card->host);

	do {
		struct mmc_command cmd;
		u32 readcmd, writecmd, status = 0;

		memset(&brq, 0, sizeof(struct mmc_blk_request));
		brq.mrq.cmd = &brq.cmd;
		brq.mrq.data = &brq.data;

		brq.cmd.arg = blk_rq_pos(req);
		if (!mmc_card_blockaddr(card))
			brq.cmd.arg <<= 9;
		brq.cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
		brq.data.blksz = 512;
		brq.stop.opcode = MMC_STOP_TRANSMISSION;
		brq.stop.arg = 0;
		brq.stop.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
		brq.data.blocks = blk_rq_sectors(req);

		/*
		 * The block layer doesn't support all sector count
		 * restrictions, so we need to be prepared for too big
		 * requests.
		 */
		if (brq.data.blocks > card->host->max_blk_count)
			brq.data.blocks = card->host->max_blk_count;

		/*
		 * After a read error, we redo the request one sector at a time
		 * in order to accurately determine which sectors can be read
		 * successfully.
		 */
		if (disable_multi && brq.data.blocks > 1)
			brq.data.blocks = 1;

		if (brq.data.blocks > 1) {
			/* SPI multiblock writes terminate using a special
			 * token, not a STOP_TRANSMISSION request.
			 */
			if (!mmc_host_is_spi(card->host)
					|| rq_data_dir(req) == READ)
				brq.mrq.stop = &brq.stop;
			readcmd = MMC_READ_MULTIPLE_BLOCK;
			writecmd = MMC_WRITE_MULTIPLE_BLOCK;
		} else {
			brq.mrq.stop = NULL;
			readcmd = MMC_READ_SINGLE_BLOCK;
			writecmd = MMC_WRITE_BLOCK;
		}
		if (rq_data_dir(req) == READ) {
			brq.cmd.opcode = readcmd;
			brq.data.flags |= MMC_DATA_READ;
		} else {
			brq.cmd.opcode = writecmd;
			brq.data.flags |= MMC_DATA_WRITE;
		}

#ifdef _MMC_SAFE_ACCESS_
		if (card->type == MMC_TYPE_SD)
			if (!mmc_is_available)
				goto cmd_sdremove;
#endif

		mmc_set_data_timeout(&brq.data, card);

		brq.data.sg = mq->sg;
		brq.data.sg_len = mmc_queue_map_sg(mq);

		/*
		 * Adjust the sg list so it is the same size as the
		 * request.
		 */
		if (brq.data.blocks != blk_rq_sectors(req)) {
			int i, data_size = brq.data.blocks << 9;
			struct scatterlist *sg;

			for_each_sg(brq.data.sg, sg, brq.data.sg_len, i) {
				data_size -= sg->length;
				if (data_size <= 0) {
					sg->length += data_size;
					i++;
					break;
				}
			}
			brq.data.sg_len = i;
		}

#ifdef _MMC_SAFE_ACCESS_
		if (card->type == MMC_TYPE_SD)
			if (!mmc_is_available)
				goto cmd_sdremove;
#endif


		mmc_queue_bounce_pre(mq);

		mmc_wait_for_req(card->host, &brq.mrq);

		mmc_queue_bounce_post(mq);

/* debug code */
#ifdef MOVI_DEBUG
		if (card->type == MMC_TYPE_MMC) {

			gaCmdLog[gnCmdLogIdx].cmd = brq.cmd.opcode;
			gaCmdLog[gnCmdLogIdx].arg = brq.cmd.arg;
			gaCmdLog[gnCmdLogIdx].cnt = brq.data.blocks;
			gaCmdLog[gnCmdLogIdx].rsp = brq.cmd.resp[0];
			gaCmdLog[gnCmdLogIdx].stoprsp = brq.stop.resp[0];
			gnCmdLogIdx++;

			if (gnCmdLogIdx >= 5)
				gnCmdLogIdx = 0;
		}
#endif
		/*
		 * Check for errors here, but don't jump to cmd_err
		 * until later as we need to wait for the card to leave
		 * programming mode even when things go wrong.
		 */
		if (!brq.cmd.error && !brq.stop.error &&
			brq.data.error == -EAGAIN) {
			printk(KERN_WARNING "%s: retrying transfer\n",
					req->rq_disk->disk_name);
			if (wait_for_ready_state(card, req))
				goto cmd_err;
			continue;
		}

		if (brq.cmd.error || brq.data.error || brq.stop.error) {
			if (brq.data.blocks > 1 && rq_data_dir(req) == READ) {
				/* Redo read one sector at a time */
				printk(KERN_WARNING "%s: retrying using single "
				       "block read\n", req->rq_disk->disk_name);
				disable_multi = 1;
				continue;
			}
			get_card_status(card, req, &status, 0);
		} else if (disable_multi == 1) {
			disable_multi = 0;
		}

#ifdef MOVI_DEBUG
		if (brq.cmd.error) {
			if (card->type == MMC_TYPE_MMC) {

				status = get_card_status(card, req, &status, 0);
				if (!status) {
/* ! put the panic code here */
					int err, i, j;
					for (i = 0; i < 5; i++) {
					printk("[CMD LOG] CMD:%d, ARG:0x%x, CNT:%d, RSP:0x%x, STRSP:0x%x\n",
						gaCmdLog[gnCmdLogIdx].cmd, gaCmdLog[gnCmdLogIdx].arg,
						gaCmdLog[gnCmdLogIdx].cnt, gaCmdLog[gnCmdLogIdx].rsp,
						gaCmdLog[gnCmdLogIdx].stoprsp);
						gnCmdLogIdx++;
						if (gnCmdLogIdx >= 5)
							gnCmdLogIdx = 0;
					}

					status = get_card_status(card, req, &status, 0);
					printk("COMMAND13 response = 0x%x\n", status);

					cmd.opcode = 12;
					cmd.arg = 0;
					cmd.flags = MMC_RSP_R1;
					err = mmc_wait_for_cmd(card->host, &cmd, 0);
					if (err) {
						printk(KERN_ERR "%s: error %d CMD12\n",
					       req->rq_disk->disk_name, err);
					}
					printk("COMD12 RESP = 0x%x\n", cmd.resp[0]);
					msleep(100);

					status = get_card_status(card, req, &status, 0);
					printk("COMMAND13 response = 0x%x\n", status);

					mmc_set_clock(card->host, 400000);

					for (i = 0; i < 3; i++) {
						cmd.opcode = 1;
						cmd.arg = 0x40ff8080;
						cmd.flags = MMC_RSP_R3 | MMC_CMD_BCR;
						err = mmc_wait_for_cmd(card->host, &cmd, 0);
						if (err) {
							printk(KERN_ERR "%s: error %d CMD0\n",
							       req->rq_disk->disk_name, err);
						}
						printk("COMD1 RESP = 0x%x\n", cmd.resp[0]);
						msleep(50);
					}

					for (i = 0; i < 3; i++) {
						cmd.opcode = 0;
						cmd.arg = 0x20110210;
						cmd.flags = MMC_RSP_NONE | MMC_CMD_BC;
						err = mmc_wait_for_cmd(card->host, &cmd, 0);
						if (err) {
							printk(KERN_ERR "%s: error %d CMD0\n",
						       req->rq_disk->disk_name, err);
						}
						msleep(50);
						cmd.opcode = 0;
						cmd.arg = 0x60FACC06;
						cmd.flags = MMC_RSP_NONE | MMC_CMD_BC;
						err = mmc_wait_for_cmd(card->host, &cmd, 0);
						if (err) {
							printk(KERN_ERR "%s: error %d CMD0\n",
							       req->rq_disk->disk_name, err);
						}
						for (j = 0; j < 3; j++) {
							msleep(50);
							cmd.opcode = 1;
							cmd.arg = 0x0;
							cmd.flags = MMC_RSP_R3 | MMC_CMD_BCR;
							err = mmc_wait_for_cmd(card->host, &cmd, 0);
							if (err) {
								printk(KERN_ERR "%s: error %d CMD0\n",
							       req->rq_disk->disk_name, err);
							}

							printk("COMD1 RESP = 0x%x\n", cmd.resp[0]);
						}
					}
				panic(" MOVINAND DEBUG PANIC\n");
				}
			}
		}
#endif
		if (brq.cmd.error) {
			printk(KERN_ERR "%s: error %d sending read/write "
			       "command, response %#x, card status %#x\n",
			       req->rq_disk->disk_name, brq.cmd.error,
			       brq.cmd.resp[0], status);
			if (R1_CURRENT_STATE(status) == 6 || R1_CURRENT_STATE(status) == 5) {
				struct mmc_command cmd;
				memset(&cmd, 0, sizeof(struct mmc_command));

				cmd.opcode = MMC_STOP_TRANSMISSION;
				cmd.arg = 0;
				cmd.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
				mmc_wait_for_cmd(card->host, &cmd, 0);
				printk(KERN_ERR "mmc_blk_issue_rq: sent CMD12 due to error!!! Resp:0x%08x\n", cmd.resp[0]);
			}
		}

		if (brq.data.error) {
			if (brq.data.error == -ETIMEDOUT && brq.mrq.stop)
				/* 'Stop' response contains card status */
				status = brq.mrq.stop->resp[0];
			printk(KERN_ERR "%s: error %d transferring data,"
			       " sector %u, nr %u, card status %#x\n",
			       req->rq_disk->disk_name, brq.data.error,
			       (unsigned)blk_rq_pos(req),
			       (unsigned)blk_rq_sectors(req), status);
		}

		if (brq.stop.error) {
			printk(KERN_ERR "%s: error %d sending stop command, "
			       "response %#x, card status %#x\n",
			       req->rq_disk->disk_name, brq.stop.error,
			       brq.stop.resp[0], status);
		}

#ifdef _MMC_SAFE_ACCESS_
		if (brq.cmd.error || brq.data.error || brq.stop.error) {
			if (card->type == MMC_TYPE_SD) {
				if (mmc_is_available)
					status = get_card_status(card,
						req, &status, 0);
				else
					printk(KERN_ERR"[MMC]SD card is removed\n");
			}
		}
#endif

		if (wait_for_ready_state(card, req))
			goto cmd_err;

		if (brq.cmd.error || brq.stop.error || brq.data.error) {
			if (rq_data_dir(req) == READ) {
				/*
				 * After an error, we redo I/O one sector at a
				 * time, so we only reach here after trying to
				 * read a single sector.
				 */
				spin_lock_irq(&md->lock);
				ret = __blk_end_request(req,
					-EIO, brq.data.blksz);
				spin_unlock_irq(&md->lock);
				continue;
			}
			goto cmd_err;
		}

		/*
		 * A block was successfully transferred.
		 */
		spin_lock_irq(&md->lock);
		ret = __blk_end_request(req, 0, brq.data.bytes_xfered);
		spin_unlock_irq(&md->lock);
	} while (ret);

	mmc_release_host(card->host);

	return 1;

cmd_sdremove:
	spin_lock_irq(&md->lock);
	__blk_end_request_all(req, -EIO);
	spin_unlock_irq(&md->lock);
	mmc_release_host(card->host);
	return 0;

 cmd_err:
 /*
 * If this is an SD card and we're writing, we can first
 * mark the known good sectors as ok.
 *
 * If the card is not SD, we can still ok written sectors
 * as reported by the controller (which might be less than
 * the real number of written sectors, but never more).
 */
	if (mmc_card_sd(card)) {
		u32 blocks;

		blocks = mmc_sd_num_wr_blocks(card);
		if (blocks != (u32)-1) {
			spin_lock_irq(&md->lock);
			ret = __blk_end_request(req, 0, blocks << 9);
			spin_unlock_irq(&md->lock);
		}
	} else {
		spin_lock_irq(&md->lock);
		ret = __blk_end_request(req, 0, brq.data.bytes_xfered);
		spin_unlock_irq(&md->lock);
	}

	mmc_release_host(card->host);

	spin_lock_irq(&md->lock);
	while (ret)
		ret = __blk_end_request(req, -EIO, blk_rq_cur_bytes(req));
	spin_unlock_irq(&md->lock);

	return 0;
}

static inline int mmc_blk_readonly(struct mmc_card *card)
{
	return mmc_card_readonly(card) ||
		!(card->csd.cmdclass & CCC_BLOCK_WRITE);
}

static struct mmc_blk_data *mmc_blk_alloc(struct mmc_card *card)
{
	struct mmc_blk_data *md;
	int devidx, ret;

	devidx = find_first_zero_bit(dev_use, max_devices);
	if (devidx >= max_devices)
		return ERR_PTR(-ENOSPC);
	__set_bit(devidx, dev_use);

	md = kzalloc(sizeof(struct mmc_blk_data), GFP_KERNEL);
	if (!md) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Set the read-only status based on the supported commands
	 * and the write protect switch.
	 */
	md->read_only = mmc_blk_readonly(card);

	md->disk = alloc_disk(perdev_minors);
	if (md->disk == NULL) {
		ret = -ENOMEM;
		goto err_kfree;
	}

	spin_lock_init(&md->lock);
	md->usage = 1;

	ret = mmc_init_queue(&md->queue, card, &md->lock);
	if (ret)
		goto err_putdisk;

	md->queue.issue_fn = mmc_blk_issue_rq;
	md->queue.data = md;

	md->disk->major	= MMC_BLOCK_MAJOR;
	md->disk->first_minor = devidx * perdev_minors;
	md->disk->fops = &mmc_bdops;
	md->disk->private_data = md;
	md->disk->queue = md->queue.queue;
	md->disk->driverfs_dev = &card->dev;
	md->disk->flags = GENHD_FL_EXT_DEVT;

	/*
	 * As discussed on lkml, GENHD_FL_REMOVABLE should:
	 *
	 * - be set for removable media with permanent block devices
	 * - be unset for removable block devices with permanent media
	 *
	 * Since MMC block devices clearly fall under the second
	 * case, we do not set GENHD_FL_REMOVABLE.  Userspace
	 * should use the block device creation/destruction hotplug
	 * messages to tell when the card is present.
	 */

	sprintf(md->disk->disk_name, "mmcblk%d", devidx);

	blk_queue_logical_block_size(md->queue.queue, 512);

	if (!mmc_card_sd(card) && mmc_card_blockaddr(card)) {
		/*
		 * The EXT_CSD sector count is in number or 512 byte
		 * sectors.
		 */
		set_capacity(md->disk, card->ext_csd.sectors);
	} else {
		/*
		 * The CSD capacity field is in units of read_blkbits.
		 * set_capacity takes units of 512 bytes.
		 */
		set_capacity(md->disk, card->csd.capacity
			<< (card->csd.read_blkbits - 9));
	}
	return md;

err_putdisk:
	put_disk(md->disk);
err_kfree:
	kfree(md);
out:
	return ERR_PTR(ret);
}

static int mmc_blk_probe(struct mmc_card *card)
{
	struct mmc_blk_data *md;
	int err;
	char cap_str[10];

	/*
	 * Check that the card supports the command class(es) we need.
	 */
	if (!(card->csd.cmdclass & CCC_BLOCK_READ))
		return -ENODEV;

	md = mmc_blk_alloc(card);
	if (IS_ERR(md))
		return PTR_ERR(md);

	err = mmc_blk_set_blksize(md, card);
	if (err)
		goto out;

	string_get_size((u64)get_capacity(md->disk) << 9, STRING_UNITS_2,
			cap_str, sizeof(cap_str));
	printk(KERN_INFO "%s: %s %s %s %s\n",
			md->disk->disk_name, mmc_card_id(card),
			mmc_card_name(card),
			cap_str, md->read_only ? "(ro)" : "");

	mmc_set_drvdata(card, md);
#ifdef CONFIG_MMC_BLOCK_DEFERRED_RESUME
	mmc_set_bus_resume_policy(card->host, 1);
#endif
	add_disk(md->disk);
	return 0;

out:
	mmc_cleanup_queue(&md->queue);
	mmc_blk_put(md);

	return err;
}

static void mmc_blk_remove(struct mmc_card *card)
{
	struct mmc_blk_data *md = mmc_get_drvdata(card);

	if (md) {
		/* Stop new requests from getting into the queue */
		del_gendisk(md->disk);

		/* Then flush out any already in there */
		mmc_cleanup_queue(&md->queue);

		mmc_blk_put(md);
	}
	mmc_set_drvdata(card, NULL);
#ifdef CONFIG_MMC_BLOCK_DEFERRED_RESUME
	mmc_set_bus_resume_policy(card->host, 0);
#endif
}

#ifdef CONFIG_PM
static int mmc_blk_suspend(struct mmc_card *card, pm_message_t state)
{
	struct mmc_blk_data *md = mmc_get_drvdata(card);

	if (md)
		mmc_queue_suspend(&md->queue);
	return 0;
}

static int mmc_blk_resume(struct mmc_card *card)
{
	struct mmc_blk_data *md = mmc_get_drvdata(card);

	if (md) {
#ifndef CONFIG_MMC_BLOCK_DEFERRED_RESUME
		mmc_blk_set_blksize(md, card);
#endif
		mmc_queue_resume(&md->queue);
	}
	return 0;
}
#else
#define	mmc_blk_suspend	NULL
#define mmc_blk_resume	NULL
#endif

static struct mmc_driver mmc_driver = {
	.drv		= {
		.name	= "mmcblk",
	},
	.probe		= mmc_blk_probe,
	.remove		= mmc_blk_remove,
	.suspend	= mmc_blk_suspend,
	.resume		= mmc_blk_resume,
};

static int __init mmc_blk_init(void)
{
	int res;

	if (perdev_minors != CONFIG_MMC_BLOCK_MINORS)
		pr_info("mmcblk: using %d minors per device\n", perdev_minors);

	max_devices = 256 / perdev_minors;

	res = register_blkdev(MMC_BLOCK_MAJOR, "mmc");
	if (res)
		goto out;

	res = mmc_register_driver(&mmc_driver);
	if (res)
		goto out2;

	return 0;
out2:
	unregister_blkdev(MMC_BLOCK_MAJOR, "mmc");
out:
	return res;
}

static void __exit mmc_blk_exit(void)
{
	mmc_unregister_driver(&mmc_driver);
	unregister_blkdev(MMC_BLOCK_MAJOR, "mmc");
}

module_init(mmc_blk_init);
module_exit(mmc_blk_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Multimedia Card (MMC) block device driver");

