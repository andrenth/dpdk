/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <rte_cycles.h>
#include <rte_ethdev.h>

#include "req.h"

void
print_req_queue(struct req_queue *req_queue)
{
	struct priority_ll *pll = req_queue->head;
	printf("request priority queue:\n");
	while (pll) {
		printf("%hhu->", pll->priority);
		pll = pll->prev;
	}
	printf("\n");
}

void
req_send_burst(struct gk_data *gk, struct req_queue *req_queue)
{
	uint32_t sent = 0;
	uint16_t ret;

	do {
		ret = rte_eth_tx_burst(gk->tx_port, gk->tx_queue,
			req_queue->pkts_out + sent, req_queue->n_pkts_out);

		/* We cannot drop the packets, so re-send. */
		req_queue->n_pkts_out -= ret;
		sent += ret;
	} while (req_queue->n_pkts_out > 0);
}

/* XXX Dummy implementation. */
static inline uint8_t
req_get_priority(struct rte_mbuf *pkt)
{
	return ((*(uint8_t *)pkt + rte_rand()) % 62) + 2;
}

static void
insert_new_priority_req(struct req_queue *req_queue, struct priority_ll *pll)
{
	uint8_t next;
	uint8_t priority = pll->priority;

	req_queue->priorities[priority] = pll;

	/* This is the first packet in the queue. */
	if (req_queue->length == 0) {
		req_queue->head = pll;
		req_queue->highest_priority = priority;
		req_queue->lowest_priority = priority;
		return;
	}

	/* Insert at head of queue. */

	if (priority > req_queue->highest_priority) {
		pll->prev = req_queue->head;
		req_queue->head->next = pll;
		req_queue->head = pll;
		req_queue->highest_priority = priority;
		return;
	}

	/* Insert in middle or end of queue. */

	if (priority < req_queue->lowest_priority) {
		req_queue->lowest_priority = priority;
	}

	for (next = priority + 1; next < req_queue->num_priorities; next++) {
		if (rte_bitmap_get(req_queue->bmp, next) != 0) {
			pll->next = req_queue->priorities[next];
			pll->prev = pll->next->prev;
			pll->next->prev = pll;
			if (pll->prev != NULL)
				pll->prev->next = pll;
			return;
		}
	}
}

static inline void
insert_req(struct priority_ll *last_req_of_pri, struct priority_ll *new_req)
{
	struct priority_ll *prev = last_req_of_pri->prev;
	last_req_of_pri->prev = new_req;
	new_req->next = last_req_of_pri;
	if (prev) {
		prev->next = new_req;
		new_req->prev = prev;
	}
}

static uint32_t
drop_lowest_priority_pkt(struct req_queue *req_queue, struct priority_ll *pll)
{
	struct priority_ll *lowest_pll;

	/* New packet is lowest priority, so drop it. */
	if (pll->priority <= req_queue->lowest_priority) {
		rte_pktmbuf_free(pll->mbuf);
		return 0;
	}

	lowest_pll = req_queue->priorities[req_queue->lowest_priority];

	/* Only one packet in the queue. */
	if (unlikely(lowest_pll->next == NULL)) {
		req_queue->priorities[lowest_pll->priority] = NULL;
		rte_bitmap_clear(req_queue->bmp, lowest_pll->priority);
		req_queue->lowest_priority = 0;
		goto out;
	}

	/* The lowest priority packet was the only one of that priority. */
	if (lowest_pll->priority != lowest_pll->next->priority) {
		req_queue->priorities[lowest_pll->priority] = NULL;
		rte_bitmap_clear(req_queue->bmp, lowest_pll->priority);
		lowest_pll->next->prev = NULL;
		req_queue->lowest_priority = lowest_pll->next->priority;
		goto out;
	}

	req_queue->priorities[lowest_pll->priority] = lowest_pll->next;
	lowest_pll->next->prev = NULL;

out:
	rte_pktmbuf_free(lowest_pll->mbuf);
	req_queue->length--;
	return 1;
}

int
req_enqueue(struct req_queue *req_queue, struct rte_mbuf **mbufs,
	uint32_t num_pkts)
{
	struct priority_ll *pll;
	uint32_t added = 0;
	uint32_t i;

	for (i = 0; i < num_pkts; i++) {
		pll = (struct priority_ll *)rte_pktmbuf_prepend(mbufs[i],
			sizeof(*pll));

		/*
		 * XXX Does mbuf pool creation size guarantee
		 * this should never happen? Add drop statistics.
		 */
		if (!pll) {
			printf("can't prepend data to mbuf\n");
			rte_pktmbuf_free(mbufs[i]);
			continue;
		}

		pll->next = NULL;
		pll->prev = NULL;
		pll->mbuf = mbufs[i];
		pll->priority = req_get_priority(mbufs[i]);

		if (req_queue->length == req_queue->qsize) {
			/* XXX Add drop statistics. */
			if (drop_lowest_priority_pkt(req_queue, pll) == 0) {
				continue;
			}
		}

		if (req_queue->priorities[pll->priority] == NULL) {
			/* Insert request of a priority we don't yet have. */
			insert_new_priority_req(req_queue, pll);
			rte_bitmap_set(req_queue->bmp, pll->priority);
		} else {
			/* Append request to end of appropriate priority. */
			insert_req(req_queue->priorities[pll->priority], pll);
			req_queue->priorities[pll->priority] = pll;
		}

		req_queue->length++;
		added++;
	}

	return added;
}

static inline void
time_resync(struct req_queue *req_queue)
{
	uint64_t cycles = rte_get_tsc_cycles();
	uint64_t cycles_diff = cycles - req_queue->time_cpu_cycles;
	uint64_t bytes_diff;

	/* Compute elapsed time in bytes. */
	bytes_diff = rte_reciprocal_divide(cycles_diff << RTE_SCHED_TIME_SHIFT,
		req_queue->inv_cycles_per_byte);

	/* Advance port time. */
	req_queue->time_cpu_cycles = cycles;
	req_queue->time_cpu_bytes += bytes_diff;
	if (req_queue->time < req_queue->time_cpu_bytes)
		req_queue->time = req_queue->time_cpu_bytes;
}

static inline void
credits_update(struct req_queue *req_queue)
{
	uint64_t n_periods = (req_queue->time - req_queue->tb_time) /
		req_queue->tb_period;
	req_queue->tb_credits += n_periods * req_queue->tb_credits_per_period;
	req_queue->tb_credits = min_val_2_u32(req_queue->tb_credits,
		req_queue->tb_size);
	req_queue->tb_time += n_periods * req_queue->tb_period;
}

static inline int
credits_check(struct req_queue *req_queue, struct rte_mbuf *pkt)
{
	uint32_t pkt_len = pkt->pkt_len + req_queue->frame_overhead;

	if (pkt_len > req_queue->tb_credits)
		return 0;

	req_queue->tb_credits -= pkt_len;
	return 1;
}

uint32_t
req_dequeue(struct req_queue *req_queue, const uint32_t num_pkts)
{
	struct priority_ll *head = req_queue->head;
	struct rte_mbuf *pkt;

	time_resync(req_queue);

	/*
	 * XXX When is the best time to do this? DPDK sched does
	 * this for every packet that it sees while doing prefetching.
	 */
	credits_update(req_queue);

	req_queue->n_pkts_out = 0;
	while (req_queue->n_pkts_out < num_pkts && head != NULL) {

		/* Get packet out of linked list. */
		pkt = head->mbuf;

		/* Check credits. */
		if (!credits_check(req_queue, pkt)) {
			printf("out of credits\n");
			goto out;
		}

		/* Remove request from queue. */
		if (head->prev == NULL ||
			(head->priority != head->prev->priority)) {
			req_queue->priorities[head->priority] = NULL;
			rte_bitmap_clear(req_queue->bmp, head->priority);
		}
		head = head->prev;
		if (head != NULL)
			head->next = NULL;
		req_queue->length--;

		/* Remove extra space in mbuf. */
		if (rte_pktmbuf_adj(pkt, sizeof(*head)) == NULL) {
			printf("bug in request queue: should be able to remove the extra linked list data in a packet, but the removal failed\n");
			rte_pktmbuf_free(pkt);
			continue;
		}

		/* Queue packet for transmission. */
		req_queue->pkts_out[req_queue->n_pkts_out++] = pkt;

		/* Advance port time. */
		req_queue->time += pkt->pkt_len + req_queue->frame_overhead;
	}

out:
	req_queue->head = head;
	if (req_queue->head == NULL)
		req_queue->lowest_priority = req_queue->num_priorities;
	else
		req_queue->highest_priority = head->priority;

	return req_queue->n_pkts_out;
}
