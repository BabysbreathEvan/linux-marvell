/*
 * offload engine driver for the Marvell XOR engine
 * Copyright (C) 2007, 2008, Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/memory.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <linux/platform_data/dma-mv_xor.h>
#include <linux/crc32c.h>

#include "dmaengine.h"
#include "mv_xor.h"

unsigned int dummy1[MV_XOR_MIN_BYTE_COUNT];
unsigned int dummy2[MV_XOR_MIN_BYTE_COUNT];
dma_addr_t dummy1_addr, dummy2_addr;

enum mv_xor_mode {
	XOR_MODE_IN_REG,
	XOR_MODE_IN_DESC,
};

/* engine coefficients  */
static u8 mv_xor_raid6_coefs[8] = {
	0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
};

static void mv_xor_issue_pending(struct dma_chan *chan);

#define to_mv_xor_chan(chan)		\
	container_of(chan, struct mv_xor_chan, dmachan)

#define to_mv_xor_slot(tx)		\
	container_of(tx, struct mv_xor_desc_slot, async_tx)

#define mv_chan_to_devp(chan)           \
	((chan)->dmadev.dev)

static void mv_desc_init(struct mv_xor_desc_slot *desc, unsigned long flags)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	u32 command = 0;

	hw_desc->status = (1 << 31);
	hw_desc->phy_next_desc = 0;

	if (flags & DMA_PREP_INTERRUPT)
		command |= (1 << 31);

	if (desc->type == DMA_CRC32C)
		command |= (1 << 30);	/* CRCLast */

	hw_desc->desc_command = command;
}

static void mv_desc_set_mode(struct mv_xor_desc_slot *desc)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;

	switch (desc->type) {
	case DMA_XOR:
	case DMA_INTERRUPT:
		hw_desc->desc_command |= XOR_DESC_OPERATION_XOR;
		break;
	case DMA_CRC32C:
		hw_desc->desc_command |= XOR_DESC_OPERATION_CRC32C;
		break;
	case DMA_MEMCPY:
		hw_desc->desc_command |= XOR_DESC_OPERATION_MEMCPY;
		break;
	case DMA_PQ:
		hw_desc->desc_command |= XOR_DESC_OPERATION_PQ;
		break;
	default:
		BUG();
		return;
	}
}

static u32 mv_desc_get_dest_addr(struct mv_xor_desc_slot *desc)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	return hw_desc->phy_dest_addr;
}

static u32 mv_desc_get_q_dest_addr(struct mv_xor_desc_slot *desc)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	return hw_desc->phy_q_dest_addr;
}

static u32 mv_desc_get_src_addr(struct mv_xor_desc_slot *desc,
				int src_idx)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	return hw_desc->phy_src_addr[mv_phy_src_idx(src_idx)];
}

static void mv_desc_set_byte_count(struct mv_xor_desc_slot *desc,
				   u32 byte_count)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	hw_desc->byte_count = byte_count;
}

static void mv_desc_set_next_desc(struct mv_xor_desc_slot *desc,
				  u32 next_desc_addr)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	BUG_ON(hw_desc->phy_next_desc);
	hw_desc->phy_next_desc = next_desc_addr;
}

static void mv_desc_set_dest_addr(struct mv_xor_desc_slot *desc,
				  dma_addr_t addr)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	hw_desc->phy_dest_addr = addr;
	if (desc->type == DMA_PQ)
		hw_desc->desc_command |= (1 << 8);
}

static void mv_desc_set_q_dest_addr(struct mv_xor_desc_slot *desc,
				  dma_addr_t addr)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	hw_desc->phy_q_dest_addr = addr;
	if (desc->type == DMA_PQ)
		hw_desc->desc_command |= (1 << 9);
}

static void mv_desc_set_src_addr(struct mv_xor_desc_slot *desc,
				 int index, dma_addr_t addr)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	hw_desc->phy_src_addr[mv_phy_src_idx(index)] = addr;
	if ((desc->type == DMA_XOR) || (desc->type == DMA_PQ))
		hw_desc->desc_command |= (1 << index);
}

static int mv_desc_is_src_used(struct mv_xor_desc_slot *desc, int index)
{
	struct mv_xor_desc *hw_desc = desc->hw_desc;
	return hw_desc->desc_command & (1 << index) ? 1 : 0;
}

static u32 mv_chan_get_current_desc(struct mv_xor_chan *chan)
{
	return readl_relaxed(XOR_CURR_DESC(chan));
}

static void mv_chan_set_next_descriptor(struct mv_xor_chan *chan,
					u32 next_desc_addr)
{
	writel_relaxed(next_desc_addr, XOR_NEXT_DESC(chan));
}

static void mv_chan_unmask_interrupts(struct mv_xor_chan *chan)
{
	u32 val = readl_relaxed(XOR_INTR_MASK(chan));
	val |= XOR_INTR_MASK_VALUE << (chan->idx * 16);
	writel_relaxed(val, XOR_INTR_MASK(chan));
}

static u32 mv_chan_get_intr_cause(struct mv_xor_chan *chan)
{
	u32 intr_cause = readl_relaxed(XOR_INTR_CAUSE(chan));
	intr_cause = (intr_cause >> (chan->idx * 16)) & 0xFFFF;
	return intr_cause;
}

static int mv_is_err_intr(u32 intr_cause)
{
	if (intr_cause & ((1<<4)|(1<<5)|(1<<6)|(1<<7)|(1<<8)|(1<<9)))
		return 1;

	return 0;
}

static void mv_xor_device_clear_eoc_cause(struct mv_xor_chan *chan)
{
	u32 val = ~(3 << (chan->idx * 16));
	dev_dbg(mv_chan_to_devp(chan), "%s, val 0x%08x\n", __func__, val);
	writel_relaxed(val, XOR_INTR_CAUSE(chan));
}

static void mv_xor_device_clear_err_status(struct mv_xor_chan *chan)
{
	u32 val = 0xFFFF0000 >> (chan->idx * 16);
	writel_relaxed(val, XOR_INTR_CAUSE(chan));
}

static void mv_set_mode(struct mv_xor_chan *chan,
			       enum dma_transaction_type type)
{
	u32 op_mode;
	u32 config = readl_relaxed(XOR_CONFIG(chan));

	switch (type) {
	case DMA_XOR:
		op_mode = XOR_OPERATION_MODE_XOR;
		break;
	case DMA_MEMCPY:
		op_mode = XOR_OPERATION_MODE_MEMCPY;
		break;
	case DMA_CRC32C:
		op_mode = XOR_OPERATION_MODE_CRC32C;
		break;
	default:
		dev_err(mv_chan_to_devp(chan),
			"error: unsupported operation %d\n",
			type);
		BUG();
		return;
	}

	config &= ~0x7;
	config |= op_mode;

#if defined(__BIG_ENDIAN)
	config |= XOR_DESCRIPTOR_SWAP;
#else
	config &= ~XOR_DESCRIPTOR_SWAP;
#endif

	writel_relaxed(config, XOR_CONFIG(chan));
	chan->current_type = type;
}

static void mv_set_mode_on_desc(struct mv_xor_chan *chan)
{
	u32 op_mode;
	u32 config = readl_relaxed(XOR_CONFIG(chan));

	op_mode = XOR_OPERATION_MODE_IN_DESC;

	config &= ~0x7;
	config |= op_mode;

#if defined(__BIG_ENDIAN)
	config |= XOR_DESCRIPTOR_SWAP;
#else
	config &= ~XOR_DESCRIPTOR_SWAP;
#endif

	writel_relaxed(config, XOR_CONFIG(chan));
}

static void mv_chan_activate(struct mv_xor_chan *chan)
{
	dev_dbg(mv_chan_to_devp(chan), "activate chan %d\n", chan->dmadev.dev_id);

	/* writel ensures all descriptors are flushed before
	   activation*/
	writel(0x1, XOR_ACTIVATION(chan));
}

static char mv_chan_is_busy(struct mv_xor_chan *chan)
{
	u32 state = readl_relaxed(XOR_ACTIVATION(chan));

	state = (state >> 4) & 0x3;

	return (state == 1) ? 1 : 0;
}

/*
 * mv_xor_start_new_chain - program the engine to operate on new chain headed by
 * sw_desc
 * Caller must hold &mv_chan->lock while calling this function
 */
static void mv_xor_start_new_chain(struct mv_xor_chan *mv_chan,
				   struct mv_xor_desc_slot *sw_desc)
{
	dev_dbg(mv_chan_to_devp(mv_chan), "%s %d: sw_desc %p phys %x\n",
		__func__, __LINE__, sw_desc, sw_desc->async_tx.phys);

	/* set the hardware chain */
	mv_chan_set_next_descriptor(mv_chan, sw_desc->async_tx.phys);
	mv_chan->pending++;
	mv_xor_issue_pending(&mv_chan->dmachan);
}

static inline void mv_xor_unmap(struct device *dev, dma_addr_t addr, size_t len,
			      int direction, enum dma_ctrl_flags flags, bool dest)
{
	if ((dest && (flags & DMA_COMPL_DEST_UNMAP_SINGLE)) ||
	    (!dest && (flags & DMA_COMPL_SRC_UNMAP_SINGLE)))
		dma_unmap_single(dev, addr, len, direction);
	else
		dma_unmap_page(dev, addr, len, direction);
}

static void mv_xor_unmap_desc(struct mv_xor_desc_slot *desc,
			      struct mv_xor_chan *mv_chan)
{
	if (desc->unmap_len) {
		struct mv_xor_desc_slot *unmap = desc;
		struct device *dev = mv_chan_to_devp(mv_chan);
		u32 len = unmap->unmap_len;
		enum dma_ctrl_flags flags = unmap->async_tx.flags;
		dma_addr_t src;
		dma_addr_t dest; /* and p_dest */
		dma_addr_t q_dest;
		enum dma_data_direction dir;
		u32 src_cnt = unmap->unmap_src_cnt;

		switch (desc->type) {
		case DMA_MEMCPY:
			if (!(flags & DMA_COMPL_SKIP_DEST_UNMAP)) {
				dest = mv_desc_get_dest_addr(unmap);
				mv_xor_unmap(dev, dest, len, DMA_FROM_DEVICE, flags, 1);
			}

			if (!(flags & DMA_COMPL_SKIP_SRC_UNMAP)) {
				src = mv_desc_get_src_addr(unmap, 0);
				mv_xor_unmap(dev, src, len, DMA_TO_DEVICE, flags, 0);
			}
			break;
		case DMA_XOR:
			dest = mv_desc_get_dest_addr(unmap);
			if (!(flags & DMA_COMPL_SKIP_DEST_UNMAP)) {
				if (src_cnt > 1) /* is xor ? */
					dir = DMA_BIDIRECTIONAL;
				else
					dir = DMA_FROM_DEVICE;

				mv_xor_unmap(dev, dest, len, dir, flags, 1);
			}

			if (!(flags & DMA_COMPL_SKIP_SRC_UNMAP)) {
				while (src_cnt--) {
					src = mv_desc_get_src_addr(unmap, src_cnt);

					/* unmap dest address once */
					if (src == dest)
						continue;

					mv_xor_unmap(dev, src, len, DMA_TO_DEVICE, flags, 0);
				}
			}
			break;
		case DMA_PQ:
			if (!(flags & DMA_COMPL_SKIP_DEST_UNMAP)) {
				if (!(flags & DMA_PREP_PQ_DISABLE_P)) {
					dest = mv_desc_get_dest_addr(unmap);
					mv_xor_unmap(dev, dest, len, DMA_BIDIRECTIONAL, flags, 1);
				}
				if (!(flags & DMA_PREP_PQ_DISABLE_Q)) {
					q_dest = mv_desc_get_q_dest_addr(unmap);
					mv_xor_unmap(dev, q_dest, len, DMA_BIDIRECTIONAL, flags, 1);
				}
			}

			if (!(flags & DMA_COMPL_SKIP_SRC_UNMAP)) {
				int src_i;
				for (src_i = 0; src_i < 8; src_i++) {
					if (!mv_desc_is_src_used(unmap, src_i))
						continue;
					src = mv_desc_get_src_addr(unmap, src_i);

					mv_xor_unmap(dev, src, len, DMA_TO_DEVICE, flags, 0);
				}
			}
			break;
		case DMA_CRC32C:
			if (!(flags & DMA_COMPL_SKIP_SRC_UNMAP)) {
				src = mv_desc_get_src_addr(unmap, 0);
				mv_xor_unmap(dev, src, len, DMA_TO_DEVICE, flags, 0);
			}
			break;
		default:
			dev_err(mv_chan_to_devp(mv_chan),
				"wrong operation type %d\n",
				desc->type);
			BUG();
		}
	}
}

static dma_cookie_t
mv_xor_run_tx_complete_actions(struct mv_xor_desc_slot *desc,
	struct mv_xor_chan *mv_chan, dma_cookie_t cookie)
{
	BUG_ON(desc->async_tx.cookie < 0);

	if (desc->async_tx.cookie > 0) {
		cookie = desc->async_tx.cookie;

		/* call the callback (must not sleep or submit new
		 * operations to this channel)
		 */
		if (desc->async_tx.callback)
			desc->async_tx.callback(
				desc->async_tx.callback_param);

		/* unmap the descriptor */
		mv_xor_unmap_desc(desc, mv_chan);
	}

	/* run dependent operations */
	dma_run_dependencies(&desc->async_tx);

	return cookie;
}

static int
mv_xor_clean_completed_slots(struct mv_xor_chan *mv_chan)
{
	struct mv_xor_desc_slot *iter, *_iter;

	dev_dbg(mv_chan_to_devp(mv_chan), "%s %d\n", __func__, __LINE__);
	list_for_each_entry_safe(iter, _iter, &mv_chan->completed_slots,
				 node) {

		if (async_tx_test_ack(&iter->async_tx))
			list_move_tail(&iter->node, &mv_chan->free_slots);
	}
	return 0;
}

static int
mv_xor_clean_slot(struct mv_xor_desc_slot *desc,
	struct mv_xor_chan *mv_chan)
{
	dev_dbg(mv_chan_to_devp(mv_chan), "%s %d: desc %p flags %d\n",
		__func__, __LINE__, desc, desc->async_tx.flags);

	/* the client is allowed to attach dependent operations
	 * until 'ack' is set
	 */
	if (!async_tx_test_ack(&desc->async_tx))
		/* move this slot to the completed_slots */
		list_move_tail(&desc->node, &mv_chan->completed_slots);
	else
		list_move_tail(&desc->node, &mv_chan->free_slots);

	return 0;
}

static void __mv_xor_slot_cleanup(struct mv_xor_chan *mv_chan)
{
	struct mv_xor_desc_slot *iter, *_iter;
	dma_cookie_t cookie = 0;
	int busy = mv_chan_is_busy(mv_chan);
	u32 current_desc = mv_chan_get_current_desc(mv_chan);
	int current_cleaned = 0;
	struct mv_xor_desc *hw_desc;
	struct dma_chan *dma_chan;
	dma_chan = &mv_chan->dmachan;

	/* IO sync must be after reading the current_desc to unsure all descriptors are
	 * updated currectly in DRAM, and no XOR -> DRAM transactions are buffered
	 * this ensure all descriptors are synced to the current_desc position
	 */
	dma_sync_single_for_cpu(dma_chan->device->dev, (dma_addr_t) NULL,
				(size_t) NULL, DMA_FROM_DEVICE);

	dev_dbg(mv_chan_to_devp(mv_chan), "%s %d\n", __func__, __LINE__);
	dev_dbg(mv_chan_to_devp(mv_chan), "current_desc %x\n", current_desc);
	mv_xor_clean_completed_slots(mv_chan);

	/* free completed slots from the chain starting with
	 * the oldest descriptor
	 */

	list_for_each_entry_safe(iter, _iter, &mv_chan->chain,
				 node) {
		/* clean finished descriptors */
		hw_desc = iter->hw_desc;
		if (hw_desc->status & XOR_DESC_SUCCESS) {
			if (iter->type == DMA_CRC32C) {
				struct mv_xor_desc *hw_desc = iter->hw_desc;
				BUG_ON(!iter->crc32_result);
				*iter->crc32_result = ~hw_desc->crc32_result;
			}

			cookie = mv_xor_run_tx_complete_actions(iter, mv_chan, cookie);

			/* done processing desc, clean slot */
			mv_xor_clean_slot(iter, mv_chan);

			/* break if we did cleaned the current */
			if (iter->async_tx.phys == current_desc) {
				current_cleaned = 1;
				break;
			}
		} else {
			if (iter->async_tx.phys == current_desc) {
				current_cleaned = 0;
				break;
			}
		}
	}

	if ((busy == 0) && !list_empty(&mv_chan->chain)) {
		if (current_cleaned) {
			/* current descriptor cleaned and removed, run from list head */
			iter = list_entry(mv_chan->chain.next,
						struct mv_xor_desc_slot,
						node);
			mv_xor_start_new_chain(mv_chan, iter);
		} else {
			if (!list_is_last(&iter->node, &mv_chan->chain)) {
				/* descriptors are still waiting after current, trigger them */
				iter = list_entry(iter->node.next, struct mv_xor_desc_slot, node);
				mv_xor_start_new_chain(mv_chan, iter);
			} else {
				/* some descriptors are still waiting to be cleaned */
				tasklet_schedule(&mv_chan->irq_tasklet);
			}
		}
	}

	if (cookie > 0)
		mv_chan->dmachan.completed_cookie = cookie;
}

static void
mv_xor_slot_cleanup(struct mv_xor_chan *mv_chan)
{
	spin_lock_bh(&mv_chan->lock);
	__mv_xor_slot_cleanup(mv_chan);
	spin_unlock_bh(&mv_chan->lock);
}

static void mv_xor_tasklet(unsigned long data)
{
	struct mv_xor_chan *chan = (struct mv_xor_chan *) data;
	mv_xor_slot_cleanup(chan);
}

static struct mv_xor_desc_slot *mv_xor_alloc_slot(struct mv_xor_chan *mv_chan)
{
	struct mv_xor_desc_slot *iter;

	spin_lock_bh(&mv_chan->lock);

	if (!list_empty(&mv_chan->free_slots)) {
		iter = list_first_entry(&mv_chan->free_slots,
					struct mv_xor_desc_slot,
					node);

		list_move_tail(&iter->node, &mv_chan->allocated_slots);

		spin_unlock_bh(&mv_chan->lock);

		/* pre-ack descriptor */
		async_tx_ack(&iter->async_tx);
		iter->async_tx.cookie = -EBUSY;

		return iter;
	}

	spin_unlock_bh(&mv_chan->lock);

	/* try to free some slots if the allocation fails */
	tasklet_schedule(&mv_chan->irq_tasklet);

	return NULL;
}

/************************ DMA engine API functions ****************************/
static dma_cookie_t
mv_xor_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct mv_xor_desc_slot *sw_desc = to_mv_xor_slot(tx);
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(tx->chan);
	struct mv_xor_desc_slot *old_chain_tail;
	dma_cookie_t cookie;
	int new_hw_chain = 1;

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s sw_desc %p: async_tx %p\n",
		__func__, sw_desc, &sw_desc->async_tx);


	spin_lock_bh(&mv_chan->lock);
	cookie = dma_cookie_assign(tx);

	if (list_empty(&mv_chan->chain))
		list_move_tail(&sw_desc->node, &mv_chan->chain);
	else {
		new_hw_chain = 0;

		old_chain_tail = list_entry(mv_chan->chain.prev,
					    struct mv_xor_desc_slot,
					    node);
		list_move_tail(&sw_desc->node, &mv_chan->chain);

		dev_dbg(mv_chan_to_devp(mv_chan), "Append to last desc %x\n",
			old_chain_tail->async_tx.phys);

		/* fix up the hardware chain */
		mv_desc_set_next_desc(old_chain_tail, sw_desc->async_tx.phys);

		/* if the channel is not busy */
		if (!mv_chan_is_busy(mv_chan)) {
			u32 current_desc = mv_chan_get_current_desc(mv_chan);
			/*
			 * and the curren desc is the end of the chain before
			 * the append, then we need to start the channel
			 */
			if (current_desc == old_chain_tail->async_tx.phys)
				new_hw_chain = 1;
		}
	}

	if (new_hw_chain)
		mv_xor_start_new_chain(mv_chan, sw_desc);

	spin_unlock_bh(&mv_chan->lock);

	return cookie;
}

/* returns the number of allocated descriptors */
static int mv_xor_alloc_chan_resources(struct dma_chan *chan)
{
	char *hw_desc;
	int idx;
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);
	struct mv_xor_desc_slot *slot = NULL;
	int num_descs_in_pool = MV_XOR_POOL_SIZE/MV_XOR_SLOT_SIZE;

	/* Allocate descriptor slots */
	idx = mv_chan->slots_allocated;
	while (idx < num_descs_in_pool) {
		slot = kzalloc(sizeof(*slot), GFP_KERNEL);
		if (!slot) {
			printk(KERN_INFO "MV XOR Channel only initialized"
				" %d descriptor slots", idx);
			break;
		}
		hw_desc = (char *) mv_chan->dma_desc_pool_virt;
		slot->hw_desc = (void *) &hw_desc[idx * MV_XOR_SLOT_SIZE];

		dma_async_tx_descriptor_init(&slot->async_tx, chan);
		slot->async_tx.tx_submit = mv_xor_tx_submit;
		INIT_LIST_HEAD(&slot->node);
		hw_desc = (char *) mv_chan->dma_desc_pool;
		slot->async_tx.phys =
			(dma_addr_t) &hw_desc[idx * MV_XOR_SLOT_SIZE];
		slot->idx = idx++;

		spin_lock_bh(&mv_chan->lock);
		mv_chan->slots_allocated = idx;
		list_add_tail(&slot->node, &mv_chan->free_slots);
		spin_unlock_bh(&mv_chan->lock);
	}

	dev_dbg(mv_chan_to_devp(mv_chan),
		"allocated %d descriptor slots\n",
		mv_chan->slots_allocated);

	return mv_chan->slots_allocated ? : -ENOMEM;
}

static struct dma_async_tx_descriptor *
mv_xor_prep_dma_interrupt(struct dma_chan *chan, unsigned long flags)
{
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);
	struct mv_xor_desc_slot *sw_desc;

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s flags: %ld\n",
		__func__, flags);

	sw_desc = mv_xor_alloc_slot(mv_chan);
	if (sw_desc) {
		sw_desc->type = DMA_XOR;
		sw_desc->async_tx.flags = flags;
		mv_desc_init(sw_desc, DMA_PREP_INTERRUPT);
		if (mv_chan->op_in_desc == XOR_MODE_IN_DESC)
			mv_desc_set_mode(sw_desc);
		/* the byte count field is the same as in memcpy desc*/
		mv_desc_set_byte_count(sw_desc, MV_XOR_MIN_BYTE_COUNT);
		mv_desc_set_dest_addr(sw_desc, dummy1_addr);
		sw_desc->unmap_src_cnt = 0;
		sw_desc->unmap_len = 0;
		mv_desc_set_src_addr(sw_desc, 1, dummy2_addr);
	}

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s sw_desc %p async_tx %p\n",
		__func__, sw_desc, &sw_desc->async_tx);
	return sw_desc ? &sw_desc->async_tx : NULL;
}

static struct dma_async_tx_descriptor *
mv_xor_prep_dma_memcpy(struct dma_chan *chan, dma_addr_t dest, dma_addr_t src,
		size_t len, unsigned long flags)
{
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);
	struct mv_xor_desc_slot *sw_desc;

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s dest: %x src %x len: %u flags: %ld\n",
		__func__, dest, src, len, flags);
	if (unlikely(len < MV_XOR_MIN_BYTE_COUNT))
		return NULL;

	BUG_ON(len > MV_XOR_MAX_BYTE_COUNT);

	sw_desc = mv_xor_alloc_slot(mv_chan);
	if (sw_desc) {
		if (mv_chan->op_in_desc == XOR_MODE_IN_DESC)
			sw_desc->type = DMA_MEMCPY;
		else
			sw_desc->type = DMA_XOR;
		sw_desc->async_tx.flags = flags;
		mv_desc_init(sw_desc, flags);
		if (mv_chan->op_in_desc == XOR_MODE_IN_DESC)
			mv_desc_set_mode(sw_desc);
		mv_desc_set_byte_count(sw_desc, len);
		mv_desc_set_dest_addr(sw_desc, dest);
		mv_desc_set_src_addr(sw_desc, 0, src);
		sw_desc->unmap_src_cnt = 1;
		sw_desc->unmap_len = len;
	}

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s sw_desc %p async_tx %p\n",
		__func__, sw_desc, sw_desc ? &sw_desc->async_tx : 0);

	return sw_desc ? &sw_desc->async_tx : NULL;
}

static struct dma_async_tx_descriptor *
mv_xor_prep_dma_xor(struct dma_chan *chan, dma_addr_t dest, dma_addr_t *src,
		    unsigned int src_cnt, size_t len, unsigned long flags)
{
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);
	struct mv_xor_desc_slot *sw_desc;

	if (unlikely(len < MV_XOR_MIN_BYTE_COUNT))
		return NULL;

	BUG_ON(len > MV_XOR_MAX_BYTE_COUNT);

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s src_cnt: %d len: dest %x %u flags: %ld\n",
		__func__, src_cnt, len, dest, flags);

	sw_desc = mv_xor_alloc_slot(mv_chan);
	if (sw_desc) {
		sw_desc->type = DMA_XOR;
		sw_desc->async_tx.flags = flags;
		mv_desc_init(sw_desc, flags);
		if (mv_chan->op_in_desc == XOR_MODE_IN_DESC)
			mv_desc_set_mode(sw_desc);
		/* the byte count field is the same as in memcpy desc*/
		mv_desc_set_byte_count(sw_desc, len);
		mv_desc_set_dest_addr(sw_desc, dest);
		sw_desc->unmap_src_cnt = src_cnt;
		sw_desc->unmap_len = len;
		while (src_cnt--)
			mv_desc_set_src_addr(sw_desc, src_cnt, src[src_cnt]);
	}

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s sw_desc %p async_tx %p\n",
		__func__, sw_desc, &sw_desc->async_tx);
	return sw_desc ? &sw_desc->async_tx : NULL;
}

static struct dma_async_tx_descriptor *
mv_xor_prep_dma_crc32c(struct dma_chan *chan, dma_addr_t src,
		size_t len, u32 *seed, unsigned long flags)
{
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);
	struct mv_xor_desc_slot *sw_desc;

	dev_dbg(mv_chan_to_devp(mv_chan), "%s src: %x len: %u flags: %lx\n",
		__func__, src, len, flags);

	/* This HW only supports only ~0 seed
	 * Check for data size limitations
	 */
	if (*seed != ~0 ||
	    unlikely(len < MV_XOR_MIN_BYTE_COUNT) ||
	    unlikely(len > XOR_MAX_BYTE_COUNT))
		return NULL;

	sw_desc = mv_xor_alloc_slot(mv_chan);
	if (sw_desc) {
		sw_desc->type = DMA_CRC32C;
		sw_desc->async_tx.flags = flags;
		mv_desc_init(sw_desc, flags);
		if (mv_chan->op_in_desc == XOR_MODE_IN_DESC)
			mv_desc_set_mode(sw_desc);
		mv_desc_set_byte_count(sw_desc, len);
		mv_desc_set_src_addr(sw_desc, 0, src);
		sw_desc->unmap_src_cnt = 1;
		sw_desc->unmap_len = len;
		sw_desc->crc32_result = seed;
	}

	dev_dbg(mv_chan_to_devp(mv_chan), "%s sw_desc %p async_tx %p\n",
		__func__, sw_desc, &sw_desc->async_tx);

	return sw_desc ? &sw_desc->async_tx : NULL;
}

static struct dma_async_tx_descriptor *
mv_xor_prep_dma_pq(struct dma_chan *chan, dma_addr_t *dst, dma_addr_t *src,
		unsigned int src_cnt, const unsigned char *scf,
		size_t len, unsigned long flags)
{
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);
	struct mv_xor_desc_slot *sw_desc;
	int src_i = 0;
	int i = 0;

	if (unlikely(len < MV_XOR_MIN_BYTE_COUNT))
		return NULL;

	BUG_ON(len > MV_XOR_MAX_BYTE_COUNT);

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s src_cnt: %d len: %u flags: %ld\n",
		__func__, src_cnt, len, flags);

	/* since the coefs on Marvell engine are hardcoded, do not support mult
	 * and sum product requests
	 */
	if ((flags & DMA_PREP_PQ_MULT) || (flags & DMA_PREP_PQ_SUM_PRODUCT))
		return NULL;

	sw_desc = mv_xor_alloc_slot(mv_chan);
	if (sw_desc) {
		sw_desc->type = DMA_PQ;
		sw_desc->async_tx.flags = flags;
		mv_desc_init(sw_desc, flags);
		if (mv_chan->op_in_desc == XOR_MODE_IN_DESC)
			mv_desc_set_mode(sw_desc);
		mv_desc_set_byte_count(sw_desc, len);
		if (!(flags & DMA_PREP_PQ_DISABLE_P))
			mv_desc_set_dest_addr(sw_desc, dst[0]);
		if (!(flags & DMA_PREP_PQ_DISABLE_Q))
			mv_desc_set_q_dest_addr(sw_desc, dst[1]);
		sw_desc->unmap_src_cnt = src_cnt;
		sw_desc->unmap_len = len;
		while (src_cnt) {
			if (scf[src_i] == mv_xor_raid6_coefs[i]) {
				/* coefs are hardcoded, assign the src to the
				 * right place
				 */
				mv_desc_set_src_addr(sw_desc, i, src[src_i]);
				src_i++;
				i++;
				src_cnt--;
			} else
				i++;
		}
	}

	dev_dbg(mv_chan_to_devp(mv_chan),
		"%s sw_desc %p async_tx %p\n",
		__func__, sw_desc, &sw_desc->async_tx);
	return sw_desc ? &sw_desc->async_tx : NULL;
}

static void mv_xor_free_chan_resources(struct dma_chan *chan)
{
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);
	struct mv_xor_desc_slot *iter, *_iter;
	int in_use_descs = 0;

	mv_xor_slot_cleanup(mv_chan);

	spin_lock_bh(&mv_chan->lock);
	list_for_each_entry_safe(iter, _iter, &mv_chan->chain,
					node) {
		in_use_descs++;
		list_move_tail(&iter->node, &mv_chan->free_slots);
	}
	list_for_each_entry_safe(iter, _iter, &mv_chan->completed_slots,
				 node) {
		in_use_descs++;
		list_move_tail(&iter->node, &mv_chan->free_slots);
	}
	list_for_each_entry_safe(iter, _iter, &mv_chan->allocated_slots,
				 node) {
		in_use_descs++;
		list_move_tail(&iter->node, &mv_chan->free_slots);
	}
	list_for_each_entry_safe_reverse(
		iter, _iter, &mv_chan->free_slots, node) {
		list_del(&iter->node);
		kfree(iter);
		mv_chan->slots_allocated--;
	}

	dev_dbg(mv_chan_to_devp(mv_chan), "%s slots_allocated %d\n",
		__func__, mv_chan->slots_allocated);
	spin_unlock_bh(&mv_chan->lock);

	if (in_use_descs)
		dev_err(mv_chan_to_devp(mv_chan),
			"freeing %d in use descriptors!\n", in_use_descs);
}

/**
 * mv_xor_status - poll the status of an XOR transaction
 * @chan: XOR channel handle
 * @cookie: XOR transaction identifier
 * @txstate: XOR transactions state holder (or NULL)
 */
static enum dma_status mv_xor_status(struct dma_chan *chan,
					  dma_cookie_t cookie,
					  struct dma_tx_state *txstate)
{
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);
	enum dma_status ret;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_SUCCESS) {
		spin_lock_bh(&mv_chan->lock);
		mv_xor_clean_completed_slots(mv_chan);
		spin_unlock_bh(&mv_chan->lock);
		return ret;
	}
	mv_xor_slot_cleanup(mv_chan);

	return dma_cookie_status(chan, cookie, txstate);
}

static void mv_dump_xor_regs(struct mv_xor_chan *chan)
{
	u32 val;

	val = readl_relaxed(XOR_CONFIG(chan));
	dev_err(mv_chan_to_devp(chan), "config       0x%08x\n", val);

	val = readl_relaxed(XOR_ACTIVATION(chan));
	dev_err(mv_chan_to_devp(chan), "activation   0x%08x\n", val);

	val = readl_relaxed(XOR_INTR_CAUSE(chan));
	dev_err(mv_chan_to_devp(chan), "intr cause   0x%08x\n", val);

	val = readl_relaxed(XOR_INTR_MASK(chan));
	dev_err(mv_chan_to_devp(chan), "intr mask    0x%08x\n", val);

	val = readl_relaxed(XOR_ERROR_CAUSE(chan));
	dev_err(mv_chan_to_devp(chan), "error cause  0x%08x\n", val);

	val = readl_relaxed(XOR_ERROR_ADDR(chan));
	dev_err(mv_chan_to_devp(chan), "error addr   0x%08x\n", val);
}

static void mv_xor_err_interrupt_handler(struct mv_xor_chan *chan,
					 u32 intr_cause)
{
	if (intr_cause & (1 << 4)) {
	     dev_dbg(mv_chan_to_devp(chan),
		     "ignore this error\n");
	     return;
	}

	dev_err(mv_chan_to_devp(chan),
		"error on chan %d. intr cause 0x%08x\n",
		chan->idx, intr_cause);

	mv_dump_xor_regs(chan);
	BUG();
}

static irqreturn_t mv_xor_interrupt_handler(int irq, void *data)
{
	struct mv_xor_chan *chan = data;
	u32 intr_cause = mv_chan_get_intr_cause(chan);

	dev_dbg(mv_chan_to_devp(chan), "intr cause %x\n", intr_cause);

	if (mv_is_err_intr(intr_cause))
		mv_xor_err_interrupt_handler(chan, intr_cause);

	tasklet_schedule(&chan->irq_tasklet);

	mv_xor_device_clear_eoc_cause(chan);

	return IRQ_HANDLED;
}

static void mv_xor_issue_pending(struct dma_chan *chan)
{
	struct mv_xor_chan *mv_chan = to_mv_xor_chan(chan);

	if (mv_chan->pending >= MV_XOR_THRESHOLD) {
		mv_chan->pending = 0;
		mv_chan_activate(mv_chan);
	}
}

/*
 * Perform a transaction to verify the HW works.
 */
#define MV_XOR_TEST_SIZE 2000

static int mv_xor_memcpy_self_test(struct mv_xor_chan *mv_chan)
{
	int i;
	void *src, *dest;
	dma_addr_t src_dma, dest_dma;
	struct dma_chan *dma_chan;
	dma_cookie_t cookie;
	struct dma_async_tx_descriptor *tx;
	int err = 0;

	src = kmalloc(sizeof(u8) * MV_XOR_TEST_SIZE, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	dest = kzalloc(sizeof(u8) * MV_XOR_TEST_SIZE, GFP_KERNEL);
	if (!dest) {
		kfree(src);
		return -ENOMEM;
	}

	/* Fill in src buffer */
	for (i = 0; i < MV_XOR_TEST_SIZE; i++)
		((u8 *) src)[i] = (u8)i;

	dma_chan = &mv_chan->dmachan;
	if (mv_xor_alloc_chan_resources(dma_chan) < 1) {
		err = -ENODEV;
		goto out;
	}

	dest_dma = dma_map_single(dma_chan->device->dev, dest,
				  MV_XOR_TEST_SIZE, DMA_FROM_DEVICE);

	src_dma = dma_map_single(dma_chan->device->dev, src,
				 MV_XOR_TEST_SIZE, DMA_TO_DEVICE);

	tx = mv_xor_prep_dma_memcpy(dma_chan, dest_dma, src_dma,
				    MV_XOR_TEST_SIZE, 0);
	cookie = mv_xor_tx_submit(tx);
	mv_xor_issue_pending(dma_chan);
	async_tx_ack(tx);
	msleep(1);

	if (mv_xor_status(dma_chan, cookie, NULL) !=
	    DMA_SUCCESS) {
		dev_err(dma_chan->device->dev,
			"Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto free_resources;
	}

	dma_sync_single_for_cpu(dma_chan->device->dev, dest_dma,
				MV_XOR_TEST_SIZE, DMA_FROM_DEVICE);
	if (memcmp(src, dest, MV_XOR_TEST_SIZE)) {
		dev_err(dma_chan->device->dev,
			"Self-test copy failed compare, disabling\n");
		err = -ENODEV;
		goto free_resources;
	}

free_resources:
	mv_xor_free_chan_resources(dma_chan);
out:
	kfree(src);
	kfree(dest);
	return err;
}

#define MV_XOR_CRC32_TEST_SIZE	PAGE_SIZE

static int mv_xor_crc32_self_test(struct mv_xor_chan *mv_chan)
{
	int i;
	void *src;
	u32 sum;
	dma_addr_t src_dma;
	struct dma_chan *dma_chan;
	dma_cookie_t cookie;
	struct dma_async_tx_descriptor *tx;
	int err = 0;

	src = kmalloc(MV_XOR_CRC32_TEST_SIZE, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	/* Fill in src buffer */
	for (i = 0; i < MV_XOR_CRC32_TEST_SIZE; i++)
		((u8 *) src)[i] = (u8)i;

	dma_chan = &mv_chan->dmachan;

	if (mv_xor_alloc_chan_resources(dma_chan) < 1) {
		err = -ENODEV;
		goto out;
	}

	src_dma = dma_map_single(dma_chan->device->dev, src,
				 MV_XOR_CRC32_TEST_SIZE, DMA_TO_DEVICE);

	sum = ~0;
	tx = mv_xor_prep_dma_crc32c(dma_chan, src_dma,
				    MV_XOR_CRC32_TEST_SIZE, &sum, 0);

	if (unlikely(tx == (struct dma_async_tx_descriptor *)1))
		BUG();

	BUG_ON(!tx);

	cookie = mv_xor_tx_submit(tx);
	msleep(20);

	if (mv_xor_status(dma_chan, cookie, NULL) != DMA_SUCCESS) {
		dev_err(dma_chan->device->dev,
			"Self-test crc32 timed out, disabling\n");
		err = -ENODEV;
		goto free_resources;
	}

	if (crc32c(~(u32)0, src, MV_XOR_CRC32_TEST_SIZE) != sum) {
		dev_err(dma_chan->device->dev,
			"Self-test crc32c failed compare, disabling\n");
		err = -ENODEV;
		goto free_resources;
	}

free_resources:
	mv_xor_free_chan_resources(dma_chan);
out:
	kfree(src);
	return err;
}

#define MV_XOR_NUM_SRC_TEST 4 /* must be <= 15 */
static int
mv_xor_xor_self_test(struct mv_xor_chan *mv_chan)
{
	int i, src_idx;
	struct page *dest;
	struct page *xor_srcs[MV_XOR_NUM_SRC_TEST];
	dma_addr_t dma_srcs[MV_XOR_NUM_SRC_TEST];
	dma_addr_t dest_dma;
	struct dma_async_tx_descriptor *tx;
	struct dma_chan *dma_chan;
	dma_cookie_t cookie;
	u8 cmp_byte = 0;
	u32 cmp_word;
	int err = 0;

	for (src_idx = 0; src_idx < MV_XOR_NUM_SRC_TEST; src_idx++) {
		xor_srcs[src_idx] = alloc_page(GFP_KERNEL);
		if (!xor_srcs[src_idx]) {
			while (src_idx--)
				__free_page(xor_srcs[src_idx]);
			return -ENOMEM;
		}
	}

	dest = alloc_page(GFP_KERNEL);
	if (!dest) {
		while (src_idx--)
			__free_page(xor_srcs[src_idx]);
		return -ENOMEM;
	}

	/* Fill in src buffers */
	for (src_idx = 0; src_idx < MV_XOR_NUM_SRC_TEST; src_idx++) {
		u8 *ptr = page_address(xor_srcs[src_idx]);
		for (i = 0; i < PAGE_SIZE; i++)
			ptr[i] = (1 << src_idx);
	}

	for (src_idx = 0; src_idx < MV_XOR_NUM_SRC_TEST; src_idx++)
		cmp_byte ^= (u8) (1 << src_idx);

	cmp_word = (cmp_byte << 24) | (cmp_byte << 16) |
		(cmp_byte << 8) | cmp_byte;

	memset(page_address(dest), 0, PAGE_SIZE);

	dma_chan = &mv_chan->dmachan;
	if (mv_xor_alloc_chan_resources(dma_chan) < 1) {
		err = -ENODEV;
		goto out;
	}

	/* test xor */
	dest_dma = dma_map_page(dma_chan->device->dev, dest, 0, PAGE_SIZE,
				DMA_FROM_DEVICE);

	for (i = 0; i < MV_XOR_NUM_SRC_TEST; i++)
		dma_srcs[i] = dma_map_page(dma_chan->device->dev, xor_srcs[i],
					   0, PAGE_SIZE, DMA_TO_DEVICE);

	tx = mv_xor_prep_dma_xor(dma_chan, dest_dma, dma_srcs,
				 MV_XOR_NUM_SRC_TEST, PAGE_SIZE, 0);

	cookie = mv_xor_tx_submit(tx);
	mv_xor_issue_pending(dma_chan);
	async_tx_ack(tx);
	msleep(8);

	if (mv_xor_status(dma_chan, cookie, NULL) !=
	    DMA_SUCCESS) {
		dev_err(dma_chan->device->dev,
			"Self-test xor timed out, disabling\n");
		err = -ENODEV;
		goto free_resources;
	}

	dma_sync_single_for_cpu(dma_chan->device->dev, dest_dma,
				PAGE_SIZE, DMA_FROM_DEVICE);
	for (i = 0; i < (PAGE_SIZE / sizeof(u32)); i++) {
		u32 *ptr = page_address(dest);
		if (ptr[i] != cmp_word) {
			dev_err(dma_chan->device->dev,
				"Self-test xor failed compare, disabling. index %d, data %x, expected %x\n",
				i, ptr[i], cmp_word);
			err = -ENODEV;
			goto free_resources;
		}
	}

free_resources:
	mv_xor_free_chan_resources(dma_chan);
out:
	src_idx = MV_XOR_NUM_SRC_TEST;
	while (src_idx--)
		__free_page(xor_srcs[src_idx]);
	__free_page(dest);
	return err;
}

/* This driver does not implement any of the optional DMA operations. */
static int
mv_xor_control(struct dma_chan *chan, enum dma_ctrl_cmd cmd,
	       unsigned long arg)
{
	return -ENOSYS;
}

static int mv_xor_channel_remove(struct mv_xor_chan *mv_chan)
{
	struct dma_chan *chan, *_chan;
	struct device *dev = mv_chan->dmadev.dev;

	dma_async_device_unregister(&mv_chan->dmadev);

	dma_free_coherent(dev, MV_XOR_POOL_SIZE,
			  mv_chan->dma_desc_pool_virt, mv_chan->dma_desc_pool);

	list_for_each_entry_safe(chan, _chan, &mv_chan->dmadev.channels,
				 device_node) {
		list_del(&chan->device_node);
	}

	free_irq(mv_chan->irq, mv_chan);

	return 0;
}

static struct mv_xor_chan *
mv_xor_channel_add(struct mv_xor_device *xordev,
		   struct platform_device *pdev,
		   int idx, dma_cap_mask_t cap_mask, int irq, int op_in_desc)
{
	int ret = 0;
	struct mv_xor_chan *mv_chan;
	struct dma_device *dma_dev;

	mv_chan = devm_kzalloc(&pdev->dev, sizeof(*mv_chan), GFP_KERNEL);
	if (!mv_chan) {
		ret = -ENOMEM;
		goto err_free_dma;
	}

	mv_chan->idx = idx;
	mv_chan->irq = irq;
	mv_chan->op_in_desc = op_in_desc;

	dma_dev = &mv_chan->dmadev;

	/* allocate coherent memory for hardware descriptors
	 * note: writecombine gives slightly better performance, but
	 * requires that we explicitly flush the writes
	 */
	mv_chan->dma_desc_pool_virt =
	  dma_alloc_writecombine(&pdev->dev, MV_XOR_POOL_SIZE,
				 &mv_chan->dma_desc_pool, GFP_KERNEL);
	if (!mv_chan->dma_desc_pool_virt)
		return ERR_PTR(-ENOMEM);

	/* discover transaction capabilites from the platform data */
	dma_dev->cap_mask = cap_mask;

	INIT_LIST_HEAD(&dma_dev->channels);

	/* set base routines */
	dma_dev->device_alloc_chan_resources = mv_xor_alloc_chan_resources;
	dma_dev->device_free_chan_resources = mv_xor_free_chan_resources;
	dma_dev->device_tx_status = mv_xor_status;
	dma_dev->device_issue_pending = mv_xor_issue_pending;
	dma_dev->device_control = mv_xor_control;
	dma_dev->dev = &pdev->dev;

	/* set prep routines based on capability */
	if (dma_has_cap(DMA_MEMCPY, dma_dev->cap_mask))
		dma_dev->device_prep_dma_memcpy = mv_xor_prep_dma_memcpy;
	if (dma_has_cap(DMA_XOR, dma_dev->cap_mask)) {
		dma_dev->max_xor = 8;
		dma_dev->device_prep_dma_xor = mv_xor_prep_dma_xor;
	}
	if (dma_has_cap(DMA_INTERRUPT, dma_dev->cap_mask))
		dma_dev->device_prep_dma_interrupt = mv_xor_prep_dma_interrupt;
	if (dma_has_cap(DMA_CRC32C, dma_dev->cap_mask))
		dma_dev->device_prep_dma_crc32c = mv_xor_prep_dma_crc32c;
	if (dma_has_cap(DMA_PQ, dma_dev->cap_mask)) {
		dma_set_maxpq(dma_dev, 8, 0);
		dma_dev->device_prep_dma_pq = mv_xor_prep_dma_pq;
	}

	mv_chan->mmr_base = xordev->xor_base;
	if (!mv_chan->mmr_base) {
		ret = -ENOMEM;
		goto err_free_dma;
	}
	tasklet_init(&mv_chan->irq_tasklet, mv_xor_tasklet, (unsigned long)
		     mv_chan);

	/* clear errors before enabling interrupts */
	mv_xor_device_clear_err_status(mv_chan);

	ret = request_irq(mv_chan->irq, mv_xor_interrupt_handler,
			  0, dev_name(&pdev->dev), mv_chan);
	if (ret)
		goto err_free_dma;

	mv_chan_unmask_interrupts(mv_chan);

	if (mv_chan->op_in_desc == XOR_MODE_IN_DESC)
		mv_set_mode_on_desc(mv_chan);
	else {
		if (dma_has_cap(DMA_CRC32C, dma_dev->cap_mask)) {
			/* channel can support CRC or XOR mode only, not both */
			if (dma_has_cap(DMA_XOR, dma_dev->cap_mask) ||
			    dma_has_cap(DMA_MEMCPY, dma_dev->cap_mask) ||
			    dma_has_cap(DMA_INTERRUPT, dma_dev->cap_mask)) {
				BUG();
				ret = -EINVAL;
				goto err_free_dma;
			}
			mv_set_mode(mv_chan, DMA_CRC32C);
		} else
			mv_set_mode(mv_chan, DMA_XOR);
	}

	spin_lock_init(&mv_chan->lock);
	INIT_LIST_HEAD(&mv_chan->chain);
	INIT_LIST_HEAD(&mv_chan->completed_slots);
	INIT_LIST_HEAD(&mv_chan->free_slots);
	INIT_LIST_HEAD(&mv_chan->allocated_slots);
	mv_chan->dmachan.device = dma_dev;
	dma_cookie_init(&mv_chan->dmachan);

	list_add_tail(&mv_chan->dmachan.device_node, &dma_dev->channels);

	if (dma_has_cap(DMA_MEMCPY, dma_dev->cap_mask)) {
		ret = mv_xor_memcpy_self_test(mv_chan);
		dev_dbg(&pdev->dev, "memcpy self test returned %d\n", ret);
		if (ret)
			goto err_free_irq;
	}

	if (dma_has_cap(DMA_XOR, dma_dev->cap_mask)) {
		ret = mv_xor_xor_self_test(mv_chan);
		dev_dbg(&pdev->dev, "xor self test returned %d\n", ret);
		if (ret)
			goto err_free_irq;
	}

	if (dma_has_cap(DMA_CRC32C, dma_dev->cap_mask)) {
		ret = mv_xor_crc32_self_test(mv_chan);
		dev_dbg(&pdev->dev, "crc32 self test returned %d\n", ret);
		if (ret)
			goto err_free_irq;
	}

	dev_info(&pdev->dev, "Marvell XOR (%s): ( %s%s%s%s%s)\n",
		 mv_chan->op_in_desc ? "Descriptor Mode" : "Registers Mode",
		 dma_has_cap(DMA_XOR, dma_dev->cap_mask) ? "xor " : "",
		 dma_has_cap(DMA_MEMCPY, dma_dev->cap_mask) ? "cpy " : "",
		 dma_has_cap(DMA_INTERRUPT, dma_dev->cap_mask) ? "intr " : "",
		 dma_has_cap(DMA_CRC32C, dma_dev->cap_mask) ? "crc32c " : "",
		 dma_has_cap(DMA_PQ, dma_dev->cap_mask) ? "pq " : "");

	dma_async_device_register(dma_dev);
	return mv_chan;

err_free_irq:
	free_irq(mv_chan->irq, mv_chan);
 err_free_dma:
	dma_free_coherent(&pdev->dev, MV_XOR_POOL_SIZE,
			  mv_chan->dma_desc_pool_virt, mv_chan->dma_desc_pool);
	return ERR_PTR(ret);
}

static void
mv_xor_conf_mbus_windows(struct mv_xor_device *xordev,
			 const struct mbus_dram_target_info *dram)
{
	void __iomem *base = xordev->xor_base;
	u32 win_enable = 0;
	int i;

	for (i = 0; i < 8; i++) {
		writel(0, base + WINDOW_BASE(i));
		writel(0, base + WINDOW_SIZE(i));
		if (i < 4)
			writel(0, base + WINDOW_REMAP_HIGH(i));
	}

	for (i = 0; i < dram->num_cs; i++) {
		const struct mbus_dram_window *cs = dram->cs + i;

		writel((cs->base & 0xffff0000) |
		       (cs->mbus_attr << 8) |
		       dram->mbus_dram_target_id, base + WINDOW_BASE(i));
		writel((cs->size - 1) & 0xffff0000, base + WINDOW_SIZE(i));

		win_enable |= (1 << i);
		win_enable |= 3 << (16 + (2 * i));
	}

	writel(win_enable, base + WINDOW_BAR_ENABLE(0));
	writel(win_enable, base + WINDOW_BAR_ENABLE(1));
	writel(0, base + WINDOW_OVERRIDE_CTRL(0));
	writel(0, base + WINDOW_OVERRIDE_CTRL(1));
}

#ifdef CONFIG_OF
static struct of_device_id mv_xor_dt_ids[] = {
	{ .compatible = "marvell,orion-xor", .data = (void *)XOR_MODE_IN_REG },
	{ .compatible = "marvell,a38x-xor", .data = (void *)XOR_MODE_IN_DESC },
	{},
};
MODULE_DEVICE_TABLE(of, mv_xor_dt_ids);
#endif

static int mv_xor_probe(struct platform_device *pdev)
{
	const struct mbus_dram_target_info *dram;
	struct mv_xor_device *xordev;
	struct mv_xor_platform_data *pdata = pdev->dev.platform_data;
	struct resource *res;
	int i, ret;
	int op_in_desc;

	dev_notice(&pdev->dev, "Marvell shared XOR driver\n");

	dummy1_addr = dma_map_single(&pdev->dev, (void *)dummy1,
				     MV_XOR_MIN_BYTE_COUNT, DMA_FROM_DEVICE);
	dummy2_addr = dma_map_single(&pdev->dev, (void *)dummy2,
				     MV_XOR_MIN_BYTE_COUNT, DMA_TO_DEVICE);

	xordev = devm_kzalloc(&pdev->dev, sizeof(*xordev), GFP_KERNEL);
	if (!xordev)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	xordev->xor_base = devm_ioremap(&pdev->dev, res->start,
					resource_size(res));
	if (!xordev->xor_base)
		return -EBUSY;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -ENODEV;

	xordev->xor_high_base = devm_ioremap(&pdev->dev, res->start,
					     resource_size(res));
	if (!xordev->xor_high_base)
		return -EBUSY;

	platform_set_drvdata(pdev, xordev);

	/*
	 * (Re-)program MBUS remapping windows if we are asked to.
	 */
	dram = mv_mbus_dram_info();
	if (dram)
		mv_xor_conf_mbus_windows(xordev, dram);

	/* Not all platforms can gate the clock, so it is not
	 * an error if the clock does not exists.
	 */
	xordev->clk = clk_get(&pdev->dev, NULL);
	if (!IS_ERR(xordev->clk))
		clk_prepare_enable(xordev->clk);

	if (pdev->dev.of_node) {
		struct device_node *np;
		int i = 0;
		const struct of_device_id *of_id =
			of_match_device(of_match_ptr(mv_xor_dt_ids), &pdev->dev);

		for_each_child_of_node(pdev->dev.of_node, np) {
			dma_cap_mask_t cap_mask;
			int irq;
			op_in_desc = (int)of_id->data;

			dma_cap_zero(cap_mask);
			if (of_property_read_bool(np, "dmacap,memcpy"))
				dma_cap_set(DMA_MEMCPY, cap_mask);
			if (of_property_read_bool(np, "dmacap,xor"))
				dma_cap_set(DMA_XOR, cap_mask);
			if (of_property_read_bool(np, "dmacap,interrupt"))
				dma_cap_set(DMA_INTERRUPT, cap_mask);
			if (of_property_read_bool(np, "dmacap,crc32c"))
				dma_cap_set(DMA_CRC32C, cap_mask);
			if (of_property_read_bool(np, "dmacap,pq"))
				dma_cap_set(DMA_PQ, cap_mask);

			irq = irq_of_parse_and_map(np, 0);
			if (!irq) {
				ret = -ENODEV;
				goto err_channel_add;
			}

			xordev->channels[i] =
				mv_xor_channel_add(xordev, pdev, i,
						   cap_mask, irq, op_in_desc);
			if (IS_ERR(xordev->channels[i])) {
				ret = PTR_ERR(xordev->channels[i]);
				xordev->channels[i] = NULL;
				irq_dispose_mapping(irq);
				goto err_channel_add;
			}

			i++;
		}
	} else if (pdata && pdata->channels) {
		for (i = 0; i < MV_XOR_MAX_CHANNELS; i++) {
			struct mv_xor_channel_data *cd;
			int irq;

			cd = &pdata->channels[i];
			if (!cd) {
				ret = -ENODEV;
				goto err_channel_add;
			}

			irq = platform_get_irq(pdev, i);
			if (irq < 0) {
				ret = irq;
				goto err_channel_add;
			}

			xordev->channels[i] =
				mv_xor_channel_add(xordev, pdev, i,
						   cd->cap_mask, irq, XOR_MODE_IN_REG);
			if (IS_ERR(xordev->channels[i])) {
				ret = PTR_ERR(xordev->channels[i]);
				goto err_channel_add;
			}
		}
	}

	return 0;

err_channel_add:
	for (i = 0; i < MV_XOR_MAX_CHANNELS; i++)
		if (xordev->channels[i]) {
			mv_xor_channel_remove(xordev->channels[i]);
			if (pdev->dev.of_node)
				irq_dispose_mapping(xordev->channels[i]->irq);
		}

	if (!IS_ERR(xordev->clk)) {
		clk_disable_unprepare(xordev->clk);
		clk_put(xordev->clk);
	}

	return ret;
}

static int mv_xor_remove(struct platform_device *pdev)
{
	struct mv_xor_device *xordev = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < MV_XOR_MAX_CHANNELS; i++) {
		if (xordev->channels[i])
			mv_xor_channel_remove(xordev->channels[i]);
	}

	if (!IS_ERR(xordev->clk)) {
		clk_disable_unprepare(xordev->clk);
		clk_put(xordev->clk);
	}

	return 0;
}

void mv_xor_shutdown(struct platform_device *pdev)
{
	struct mv_xor_device *xordev = platform_get_drvdata(pdev);

	if (!IS_ERR(xordev->clk)) {
		clk_disable_unprepare(xordev->clk);
		clk_put(xordev->clk);
	}
}

static int mv_xor_suspend(struct platform_device *dev, pm_message_t state)
{
	struct mv_xor_device *xordev = platform_get_drvdata(dev);
	int i;

	for (i = 0; i < MV_XOR_MAX_CHANNELS; i++) {
		if (xordev->channels[i]) {
			struct mv_xor_chan *mv_chan = xordev->channels[i];

			mv_chan->suspend_regs.config = readl_relaxed(XOR_CONFIG(mv_chan));
			mv_chan->suspend_regs.int_mask = readl_relaxed(XOR_INTR_MASK(mv_chan));
		}
	}
	return 0;
}

static int mv_xor_resume(struct platform_device *dev)
{
	struct mv_xor_device *xordev = platform_get_drvdata(dev);
	int i;
	const struct mbus_dram_target_info *dram;

	/*
	 * (Re-)program MBUS remapping windows on resume.
	 */
	dram = mv_mbus_dram_info();
	if (dram)
		mv_xor_conf_mbus_windows(xordev, dram);

	for (i = 0; i < MV_XOR_MAX_CHANNELS; i++) {
		if (xordev->channels[i]) {
			struct mv_xor_chan *mv_chan = xordev->channels[i];

			writel_relaxed(mv_chan->suspend_regs.config, XOR_CONFIG(mv_chan));
			writel_relaxed(mv_chan->suspend_regs.int_mask, XOR_INTR_MASK(mv_chan));
		}
	}

	return 0;
}

static struct platform_driver mv_xor_driver = {
	.probe		= mv_xor_probe,
	.remove		= mv_xor_remove,
	.shutdown	= mv_xor_shutdown,
	.suspend	= mv_xor_suspend,
	.resume		= mv_xor_resume,
	.driver		= {
		.owner	        = THIS_MODULE,
		.name	        = MV_XOR_NAME,
		.of_match_table = of_match_ptr(mv_xor_dt_ids),
	},
};


static int __init mv_xor_init(void)
{
	return platform_driver_register(&mv_xor_driver);
}
module_init(mv_xor_init);

/* it's currently unsafe to unload this module */
#if 0
static void __exit mv_xor_exit(void)
{
	platform_driver_unregister(&mv_xor_driver);
	return;
}

module_exit(mv_xor_exit);
#endif

MODULE_AUTHOR("Saeed Bishara <saeed@marvell.com>");
MODULE_DESCRIPTION("DMA engine driver for Marvell's XOR engine");
MODULE_LICENSE("GPL");
