/*
 * This file is part of wl12xx
 *
 * Copyright (c) 1998-2007 Texas Instruments Incorporated
 * Copyright (C) 2008 Nokia Corporation
 *
 * Contact: Kalle Valo <kalle.valo@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>

#include "wl12xx.h"
#include "reg.h"
#include "spi.h"
#include "tx.h"
#include "ps.h"

static bool wl12xx_tx_double_buffer_busy(struct wl12xx *wl, u32 data_out_count)
{
	int used, data_in_count;

	data_in_count = wl->data_in_count;

	if (data_in_count < data_out_count)
		/* data_in_count has wrapped */
		data_in_count += TX_STATUS_DATA_OUT_COUNT_MASK + 1;

	used = data_in_count - data_out_count;

	WARN_ON(used < 0);
	WARN_ON(used > DP_TX_PACKET_RING_CHUNK_NUM);

	if (used >= DP_TX_PACKET_RING_CHUNK_NUM)
		return true;
	else
		return false;
}

static int wl12xx_tx_path_status(struct wl12xx *wl)
{
	u32 status, addr, data_out_count;
	bool busy;

	addr = wl->data_path->tx_control_addr;
	status = wl12xx_mem_read32(wl, addr);
	data_out_count = status & TX_STATUS_DATA_OUT_COUNT_MASK;
	busy = wl12xx_tx_double_buffer_busy(wl, data_out_count);

	if (busy)
		return -EBUSY;

	return 0;
}

static int wl12xx_tx_id(struct wl12xx *wl, struct sk_buff *skb)
{
	int i;

	for (i = 0; i < FW_TX_CMPLT_BLOCK_SIZE; i++)
		if (wl->tx_frames[i] == NULL) {
			wl->tx_frames[i] = skb;
			return i;
		}

	return -EBUSY;
}

static void wl12xx_tx_control(struct tx_double_buffer_desc *tx_hdr,
			      struct ieee80211_tx_info *control, u16 fc)
{
	*(u16 *)&tx_hdr->control = 0;

	tx_hdr->control.rate_policy = 0;

	/* 802.11 packets */
	tx_hdr->control.packet_type = 0;

	if (control->flags & IEEE80211_TX_CTL_NO_ACK)
		tx_hdr->control.ack_policy = 1;

	tx_hdr->control.tx_complete = 1;

	if ((fc & IEEE80211_FTYPE_DATA) &&
	    ((fc & IEEE80211_STYPE_QOS_DATA) ||
	     (fc & IEEE80211_STYPE_QOS_NULLFUNC)))
		tx_hdr->control.qos = 1;
}

/* RSN + MIC = 8 + 8 = 16 bytes (worst case - AES). */
#define MAX_MSDU_SECURITY_LENGTH      16
#define MAX_MPDU_SECURITY_LENGTH      16
#define WLAN_QOS_HDR_LEN              26
#define MAX_MPDU_HEADER_AND_SECURITY  (MAX_MPDU_SECURITY_LENGTH + \
				       WLAN_QOS_HDR_LEN)
#define HW_BLOCK_SIZE                 252
static void wl12xx_tx_frag_block_num(struct tx_double_buffer_desc *tx_hdr)
{
	u16 payload_len, frag_threshold, mem_blocks;
	u16 num_mpdus, mem_blocks_per_frag;

	frag_threshold = IEEE80211_MAX_FRAG_THRESHOLD;
	tx_hdr->frag_threshold = cpu_to_le16(frag_threshold);

	payload_len = tx_hdr->length + MAX_MSDU_SECURITY_LENGTH;

	if (payload_len > frag_threshold) {
		mem_blocks_per_frag =
			((frag_threshold + MAX_MPDU_HEADER_AND_SECURITY) /
			 HW_BLOCK_SIZE) + 1;
		num_mpdus = payload_len / frag_threshold;
		mem_blocks = num_mpdus * mem_blocks_per_frag;
		payload_len -= num_mpdus * frag_threshold;
		num_mpdus++;

	} else {
		mem_blocks_per_frag = 0;
		mem_blocks = 0;
		num_mpdus = 1;
	}

	mem_blocks += (payload_len / HW_BLOCK_SIZE) + 1;

	if (num_mpdus > 1)
		mem_blocks += min(num_mpdus, mem_blocks_per_frag);

	tx_hdr->num_mem_blocks = mem_blocks;
}

static int wl12xx_tx_fill_hdr(struct wl12xx *wl, struct sk_buff *skb,
			      struct ieee80211_tx_info *control)
{
	struct tx_double_buffer_desc *tx_hdr;
	struct ieee80211_rate *rate;
	int id;
	u16 fc;

	if (!skb)
		return -EINVAL;

	id = wl12xx_tx_id(wl, skb);
	if (id < 0)
		return id;

	fc = *(u16 *)skb->data;
	tx_hdr = (struct tx_double_buffer_desc *) skb_push(skb,
							   sizeof(*tx_hdr));

	tx_hdr->length = cpu_to_le16(skb->len - sizeof(*tx_hdr));
	rate = ieee80211_get_tx_rate(wl->hw, control);
	tx_hdr->rate = cpu_to_le16(rate->hw_value);
	tx_hdr->expiry_time = cpu_to_le32(1 << 16);
	tx_hdr->id = id;

	/* FIXME: how to get the correct queue id? */
	tx_hdr->xmit_queue = 0;

	wl12xx_tx_control(tx_hdr, control, fc);
	wl12xx_tx_frag_block_num(tx_hdr);

	return 0;
}

/* We copy the packet to the target */
static int wl12xx_tx_send_packet(struct wl12xx *wl, struct sk_buff *skb,
				 struct ieee80211_tx_info *control)
{
	struct tx_double_buffer_desc *tx_hdr;
	int len;
	u32 addr;

	if (!skb)
		return -EINVAL;

	tx_hdr = (struct tx_double_buffer_desc *) skb->data;

	if (control->control.hw_key &&
	    control->control.hw_key->alg == ALG_TKIP) {
		int hdrlen;
		u16 fc;
		u8 *pos;

		fc = *(u16 *)(skb->data + sizeof(*tx_hdr));
		tx_hdr->length += WL12XX_TKIP_IV_SPACE;

		hdrlen = ieee80211_hdrlen(fc);

		pos = skb_push(skb, WL12XX_TKIP_IV_SPACE);
		memmove(pos, pos + WL12XX_TKIP_IV_SPACE,
			sizeof(*tx_hdr) + hdrlen);
	}

	/* Revisit. This is a workaround for getting non-aligned packets.
	   This happens at least with EAPOL packets from the user space.
	   Our DMA requires packets to be aligned on a 4-byte boundary.
	*/
	if (unlikely((long)skb->data & 0x03)) {
		int offset = (4 - (long)skb->data) & 0x03;
		wl12xx_debug(DEBUG_TX, "skb offset %d", offset);

		/* check whether the current skb can be used */
		if (!skb_cloned(skb) && (skb_tailroom(skb) >= offset)) {
			unsigned char *src = skb->data;

			/* align the buffer on a 4-byte boundary */
			skb_reserve(skb, offset);
			memmove(skb->data, src, skb->len);
		} else {
			wl12xx_info("No handler, fixme!");
			return -EINVAL;
		}
	}

	/* Our skb->data at this point includes the HW header */
	len = WL12XX_TX_ALIGN(skb->len);

	if (wl->data_in_count & 0x1)
		addr = wl->data_path->tx_packet_ring_addr +
			wl->data_path->tx_packet_ring_chunk_size;
	else
		addr = wl->data_path->tx_packet_ring_addr;

	wl12xx_spi_mem_write(wl, addr, skb->data, len);

	wl12xx_debug(DEBUG_TX, "tx id %u skb 0x%p payload %u rate 0x%x",
		     tx_hdr->id, skb, tx_hdr->length, tx_hdr->rate);

	return 0;
}

static void wl12xx_tx_trigger(struct wl12xx *wl)
{
	u32 data, addr;

	if (wl->data_in_count & 0x1) {
		addr = ACX_REG_INTERRUPT_TRIG_H;
		data = INTR_TRIG_TX_PROC1;
	} else {
		addr = ACX_REG_INTERRUPT_TRIG;
		data = INTR_TRIG_TX_PROC0;
	}

	wl12xx_reg_write32(wl, addr, data);

	/* Bumping data in */
	wl->data_in_count = (wl->data_in_count + 1) &
		TX_STATUS_DATA_OUT_COUNT_MASK;
}

/* caller must hold wl->mutex */
static int wl12xx_tx_frame(struct wl12xx *wl, struct sk_buff *skb)
{
	struct ieee80211_tx_info *info;
	int ret = 0;
	u8 idx;

	info = IEEE80211_SKB_CB(skb);

	if (info->control.hw_key) {
		idx = info->control.hw_key->hw_key_idx;
		if (unlikely(wl->default_key != idx)) {
			ret = wl12xx_acx_default_key(wl, idx);
			if (ret < 0)
				return ret;
		}
	}

	ret = wl12xx_tx_path_status(wl);
	if (ret < 0)
		return ret;

	ret = wl12xx_tx_fill_hdr(wl, skb, info);
	if (ret < 0)
		return ret;

	ret = wl12xx_tx_send_packet(wl, skb, info);
	if (ret < 0)
		return ret;

	wl12xx_tx_trigger(wl);

	return ret;
}

void wl12xx_tx_work(struct work_struct *work)
{
	struct wl12xx *wl = container_of(work, struct wl12xx, tx_work);
	struct sk_buff *skb;
	bool woken_up = false;
	int ret;

	mutex_lock(&wl->mutex);

	if (unlikely(wl->state == WL12XX_STATE_OFF))
		goto out;

	while ((skb = skb_dequeue(&wl->tx_queue))) {
		if (!woken_up) {
			wl12xx_ps_elp_wakeup(wl);
			woken_up = true;
		}

		ret = wl12xx_tx_frame(wl, skb);
		if (ret == -EBUSY) {
			/* firmware buffer is full, stop queues */
			wl12xx_debug(DEBUG_TX, "tx_work: fw buffer full, "
				     "stop queues");
			ieee80211_stop_queues(wl->hw);
			wl->tx_queue_stopped = true;
			skb_queue_head(&wl->tx_queue, skb);
			goto out;
		} else if (ret < 0) {
			dev_kfree_skb(skb);
			goto out;
		}
	}

out:
	if (woken_up)
		wl12xx_ps_elp_sleep(wl);

	mutex_unlock(&wl->mutex);
}

static const char *wl12xx_tx_parse_status(u8 status)
{
	/* 8 bit status field, one character per bit plus null */
	static char buf[9];
	int i = 0;

	memset(buf, 0, sizeof(buf));

	if (status & TX_DMA_ERROR)
		buf[i++] = 'm';
	if (status & TX_DISABLED)
		buf[i++] = 'd';
	if (status & TX_RETRY_EXCEEDED)
		buf[i++] = 'r';
	if (status & TX_TIMEOUT)
		buf[i++] = 't';
	if (status & TX_KEY_NOT_FOUND)
		buf[i++] = 'k';
	if (status & TX_ENCRYPT_FAIL)
		buf[i++] = 'e';
	if (status & TX_UNAVAILABLE_PRIORITY)
		buf[i++] = 'p';

	/* bit 0 is unused apparently */

	return buf;
}

static void wl12xx_tx_packet_cb(struct wl12xx *wl,
				struct tx_result *result)
{
	struct ieee80211_tx_info *info;
	struct sk_buff *skb;
	int hdrlen, ret;
	u8 *frame;

	skb = wl->tx_frames[result->id];
	if (skb == NULL) {
		wl12xx_error("SKB for packet %d is NULL", result->id);
		return;
	}

	info = IEEE80211_SKB_CB(skb);

	if (!(info->flags & IEEE80211_TX_CTL_NO_ACK) &&
	    (result->status == TX_SUCCESS))
		info->flags |= IEEE80211_TX_STAT_ACK;

	info->status.rates[0].count = result->ack_failures + 1;
	wl->stats.retry_count += result->ack_failures;

	/*
	 * We have to remove our private TX header before pushing
	 * the skb back to mac80211.
	 */
	frame = skb_pull(skb, sizeof(struct tx_double_buffer_desc));
	if (info->control.hw_key &&
	    info->control.hw_key->alg == ALG_TKIP) {
		hdrlen = ieee80211_get_hdrlen_from_skb(skb);
		memmove(frame + WL12XX_TKIP_IV_SPACE, frame, hdrlen);
		skb_pull(skb, WL12XX_TKIP_IV_SPACE);
	}

	wl12xx_debug(DEBUG_TX, "tx status id %u skb 0x%p failures %u rate 0x%x"
		     " status 0x%x (%s)",
		     result->id, skb, result->ack_failures, result->rate,
		     result->status, wl12xx_tx_parse_status(result->status));


	ieee80211_tx_status(wl->hw, skb);

	wl->tx_frames[result->id] = NULL;

	if (wl->tx_queue_stopped) {
		wl12xx_debug(DEBUG_TX, "cb: queue was stopped");

		skb = skb_dequeue(&wl->tx_queue);

		/* The skb can be NULL because tx_work might have been
		   scheduled before the queue was stopped making the
		   queue empty */

		if (skb) {
			ret = wl12xx_tx_frame(wl, skb);
			if (ret == -EBUSY) {
				/* firmware buffer is still full */
				wl12xx_debug(DEBUG_TX, "cb: fw buffer "
					     "still full");
				skb_queue_head(&wl->tx_queue, skb);
				return;
			} else if (ret < 0) {
				dev_kfree_skb(skb);
				return;
			}
		}

		wl12xx_debug(DEBUG_TX, "cb: waking queues");
		ieee80211_wake_queues(wl->hw);
		wl->tx_queue_stopped = false;
	}
}

/* Called upon reception of a TX complete interrupt */
void wl12xx_tx_complete(struct wl12xx *wl)
{
	int i, result_index, num_complete = 0;
	struct tx_result result[FW_TX_CMPLT_BLOCK_SIZE], *result_ptr;

	if (unlikely(wl->state != WL12XX_STATE_ON))
		return;

	/* First we read the result */
	wl12xx_spi_mem_read(wl, wl->data_path->tx_complete_addr,
			    result, sizeof(result));

	result_index = wl->next_tx_complete;

	for (i = 0; i < ARRAY_SIZE(result); i++) {
		result_ptr = &result[result_index];

		if (result_ptr->done_1 == 1 &&
		    result_ptr->done_2 == 1) {
			wl12xx_tx_packet_cb(wl, result_ptr);

			result_ptr->done_1 = 0;
			result_ptr->done_2 = 0;

			result_index = (result_index + 1) &
				(FW_TX_CMPLT_BLOCK_SIZE - 1);
			num_complete++;
		} else {
			break;
		}
	}

	/* Every completed frame needs to be acknowledged */
	if (num_complete) {
		/*
		 * If we've wrapped, we have to clear
		 * the results in 2 steps.
		 */
		if (result_index > wl->next_tx_complete) {
			/* Only 1 write is needed */
			wl12xx_spi_mem_write(wl,
					     wl->data_path->tx_complete_addr +
					     (wl->next_tx_complete *
					      sizeof(struct tx_result)),
					     &result[wl->next_tx_complete],
					     num_complete *
					     sizeof(struct tx_result));


		} else if (result_index < wl->next_tx_complete) {
			/* 2 writes are needed */
			wl12xx_spi_mem_write(wl,
					     wl->data_path->tx_complete_addr +
					     (wl->next_tx_complete *
					      sizeof(struct tx_result)),
					     &result[wl->next_tx_complete],
					     (FW_TX_CMPLT_BLOCK_SIZE -
					      wl->next_tx_complete) *
					     sizeof(struct tx_result));

			wl12xx_spi_mem_write(wl,
					     wl->data_path->tx_complete_addr,
					     result,
					     (num_complete -
					      FW_TX_CMPLT_BLOCK_SIZE +
					      wl->next_tx_complete) *
					     sizeof(struct tx_result));

		} else {
			/* We have to write the whole array */
			wl12xx_spi_mem_write(wl,
					     wl->data_path->tx_complete_addr,
					     result,
					     FW_TX_CMPLT_BLOCK_SIZE *
					     sizeof(struct tx_result));
		}

	}

	wl->next_tx_complete = result_index;
}

/* caller must hold wl->mutex */
void wl12xx_tx_flush(struct wl12xx *wl)
{
	int i;
	struct sk_buff *skb;
	struct ieee80211_tx_info *info;

	/* TX failure */
/* 	control->flags = 0; FIXME */

	while ((skb = skb_dequeue(&wl->tx_queue))) {
		info = IEEE80211_SKB_CB(skb);

		wl12xx_debug(DEBUG_TX, "flushing skb 0x%p", skb);

		if (!(info->flags & IEEE80211_TX_CTL_REQ_TX_STATUS))
				continue;

		ieee80211_tx_status(wl->hw, skb);
	}

	for (i = 0; i < FW_TX_CMPLT_BLOCK_SIZE; i++)
		if (wl->tx_frames[i] != NULL) {
			skb = wl->tx_frames[i];
			info = IEEE80211_SKB_CB(skb);

			if (!(info->flags & IEEE80211_TX_CTL_REQ_TX_STATUS))
				continue;

			ieee80211_tx_status(wl->hw, skb);
			wl->tx_frames[i] = NULL;
		}
}
