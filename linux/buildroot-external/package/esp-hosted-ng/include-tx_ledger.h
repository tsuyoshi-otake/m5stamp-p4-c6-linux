/* EasyStick 0023-A: test-only P4 host TX stage ledger helpers.
 * Observe TCP source port 22 with payload_len > 0 only.
 * No ciphertext; no behaviour change when included.
 * Experiment A follow-up: optional per-call CMD53 claim/memcpy
 * markers keyed by caller-allocated trace_id (no global flag).
 */
#ifndef EASYSTICK_TX_LEDGER_H
#define EASYSTICK_TX_LEDGER_H

#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/skbuff.h>
#include "adapter.h"

#define ES_TX_PREFIX		"ES_TX"
#define ES_TX_SSH_SPORT		22
#define ES_TX_LOG_CAP		192

struct es_tx_id {
	u16 dport;
	u32 seq;
	u32 ack;
	u16 plen;
	u8 flags;
};

/* Caller-owned CMD53 identity. Passed into esp_write_block only for
 * sport22 payload frames; NULL elsewhere. Never a global active flag.
 */
struct es_tx_cmd53_ctx {
	struct es_tx_id id;
	u32 trace;
	u32 reg;
	u16 xfer;
};

static inline int es_tx_parse_ssh_payload(const u8 *data, unsigned int len,
					  int has_esp_hdr, struct es_tx_id *id)
{
	const struct esp_payload_header *ph;
	const struct ethhdr *eth;
	const struct iphdr *iph;
	const struct tcphdr *th;
	u16 offset = 0;
	u16 ip_total;
	u16 tcp_hdrlen;
	u16 payload_len;
	unsigned int need;

	if (!data || !id || len < sizeof(struct ethhdr))
		return 0;

	if (has_esp_hdr) {
		if (len < sizeof(*ph))
			return 0;
		ph = (const struct esp_payload_header *)data;
		offset = le16_to_cpu(ph->offset);
		if (offset < sizeof(*ph) || offset >= len)
			return 0;
		data += offset;
		len -= offset;
	}

	if (len < sizeof(*eth) + sizeof(*iph) + sizeof(*th))
		return 0;

	eth = (const struct ethhdr *)data;
	if (eth->h_proto != htons(ETH_P_IP))
		return 0;

	iph = (const struct iphdr *)(data + sizeof(*eth));
	if (iph->version != 4 || iph->protocol != IPPROTO_TCP)
		return 0;
	if (iph->ihl < 5)
		return 0;

	need = sizeof(*eth) + (iph->ihl * 4u) + sizeof(*th);
	if (len < need)
		return 0;

	th = (const struct tcphdr *)(data + sizeof(*eth) + (iph->ihl * 4u));
	if (th->source != htons(ES_TX_SSH_SPORT))
		return 0;
	if (th->doff < 5)
		return 0;

	tcp_hdrlen = th->doff * 4u;
	ip_total = ntohs(iph->tot_len);
	if (ip_total < (iph->ihl * 4u) + tcp_hdrlen)
		return 0;
	if (len < sizeof(*eth) + ip_total) {
		/* truncated skb: still trust IP total_len for plen */
	}

	payload_len = ip_total - (iph->ihl * 4u) - tcp_hdrlen;
	if (payload_len == 0)
		return 0;

	id->dport = ntohs(th->dest);
	id->seq = ntohl(th->seq);
	id->ack = ntohl(th->ack_seq);
	id->plen = payload_len;
	/* TCP flags live at offset 13 of the fixed header. */
	id->flags = ((const u8 *)th)[13];
	return 1;
}

static inline int es_tx_parse_skb(const struct sk_buff *skb, int has_esp_hdr,
				  struct es_tx_id *id)
{
	if (!skb || !skb->data || !skb->len)
		return 0;
	return es_tx_parse_ssh_payload(skb->data, skb->len, has_esp_hdr, id);
}

static inline u32 es_tx_alloc_trace(void)
{
	static unsigned n;

	n++;
	if (!n)
		n = 1;
	return n;
}

static inline void es_tx_mark(const char *stage, const struct es_tx_id *id,
			      const char *reason)
{
	static unsigned n;

	if (!stage || !id)
		return;
	n++;
	if (n > ES_TX_LOG_CAP)
		return;

	if (reason && reason[0])
		printk(KERN_EMERG ES_TX_PREFIX
		       " %s reason=%s dport=%u seq=%u ack=%u plen=%u flags=0x%x\n",
		       stage, reason, id->dport, id->seq, id->ack, id->plen,
		       id->flags);
	else
		printk(KERN_EMERG ES_TX_PREFIX
		       " %s dport=%u seq=%u ack=%u plen=%u flags=0x%x\n",
		       stage, id->dport, id->seq, id->ack, id->plen, id->flags);
}

/* Correlated CMD53 stages: always print the same trace/seq/plen/addr/xfer. */
static inline void es_tx_mark_cmd53(const char *stage,
				    const struct es_tx_cmd53_ctx *tr,
				    int have_ret, int ret)
{
	static unsigned n;

	if (!stage || !tr)
		return;
	n++;
	if (n > ES_TX_LOG_CAP)
		return;

	if (have_ret)
		printk(KERN_EMERG ES_TX_PREFIX
		       " %s trace=%u seq=%u plen=%u addr=0x%x xfer=%u ret=%d\n",
		       stage, tr->trace, tr->id.seq, tr->id.plen, tr->reg,
		       tr->xfer, ret);
	else
		printk(KERN_EMERG ES_TX_PREFIX
		       " %s trace=%u seq=%u plen=%u addr=0x%x xfer=%u\n",
		       stage, tr->trace, tr->id.seq, tr->id.plen, tr->reg,
		       tr->xfer);
}

#endif /* EASYSTICK_TX_LEDGER_H */
