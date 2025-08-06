#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <rte_log.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_byteorder.h>

#include <rte_port_ring.h>
#include <rte_table_hash.h>
#include <rte_hash.h>
#include <rte_table_hash_cuckoo.h>
#include <rte_pipeline.h>
#include <math.h>

#include "main.h"

static void
translate_options(uint32_t *special, uint32_t *ext, uint32_t *key_size)
{
	switch (app.pipeline_type) {
	case e_APP_PIPELINE_HASH_KEY8_EXT:
		*special = 0; *ext = 1; *key_size = 8; return;
	case e_APP_PIPELINE_HASH_KEY8_LRU:
		*special = 0; *ext = 0; *key_size = 8; return;
	case e_APP_PIPELINE_HASH_KEY16_EXT:
		*special = 0; *ext = 1; *key_size = 16; return;
	case e_APP_PIPELINE_HASH_KEY16_LRU:
		*special = 0; *ext = 0; *key_size = 16; return;
	case e_APP_PIPELINE_HASH_KEY32_EXT:
		*special = 0; *ext = 1; *key_size = 32; return;
	case e_APP_PIPELINE_HASH_KEY32_LRU:
		*special = 0; *ext = 0; *key_size = 32; return;

	case e_APP_PIPELINE_HASH_SPEC_KEY8_EXT:
		*special = 1; *ext = 1; *key_size = 8; return;
	case e_APP_PIPELINE_HASH_SPEC_KEY8_LRU:
		*special = 1; *ext = 0; *key_size = 8; return;
	case e_APP_PIPELINE_HASH_SPEC_KEY16_EXT:
		*special = 1; *ext = 1; *key_size = 16; return;
	case e_APP_PIPELINE_HASH_SPEC_KEY16_LRU:
		*special = 1; *ext = 0; *key_size = 16; return;
	case e_APP_PIPELINE_HASH_SPEC_KEY32_EXT:
		*special = 1; *ext = 1; *key_size = 32; return;
	case e_APP_PIPELINE_HASH_SPEC_KEY32_LRU:
		*special = 1; *ext = 0; *key_size = 32; return;

	case e_APP_PIPELINE_HASH_CUCKOO_KEY8:
		*special = 0; *ext = 0; *key_size = 8; return;
	case e_APP_PIPELINE_HASH_CUCKOO_KEY16:
		*special = 0; *ext = 0; *key_size = 16; return;
	case e_APP_PIPELINE_HASH_CUCKOO_KEY32:
		*special = 0; *ext = 0; *key_size = 32; return;
	case e_APP_PIPELINE_HASH_CUCKOO_KEY48:
		*special = 0; *ext = 0; *key_size = 48; return;
	case e_APP_PIPELINE_HASH_CUCKOO_KEY64:
		*special = 0; *ext = 0; *key_size = 64; return;
	case e_APP_PIPELINE_HASH_CUCKOO_KEY80:
		*special = 0; *ext = 0; *key_size = 80; return;
	case e_APP_PIPELINE_HASH_CUCKOO_KEY96:
		*special = 0; *ext = 0; *key_size = 96; return;
	case e_APP_PIPELINE_HASH_CUCKOO_KEY112:
		*special = 0; *ext = 0; *key_size = 112; return;
	case e_APP_PIPELINE_HASH_CUCKOO_KEY128:
		*special = 0; *ext = 0; *key_size = 128; return;

	default:
		rte_panic("Invalid hash table type or key size\n");
	}
}


// static int
// pipeline_action_miss(
//     struct rte_pipeline *p,
//     struct rte_mbuf **pkts,
//     uint64_t n,
//     struct rte_pipeline_table_entry *entries,
//     void *arg)
// {
//     RTE_LOG(INFO, USER1, "DROPPED: %" PRIu64 " packets due to lookup miss\n", n);
//     for (uint64_t i = 0; i < n; i++)
//         rte_pktmbuf_free(pkts[i]);
//     return 0;
// }

static int
pipeline_action_miss(
    struct rte_pipeline *p,
    struct rte_mbuf **pkts,
    uint64_t pkts_mask,
    struct rte_pipeline_table_entry *entry,
    void *arg)
{
	// //uint32_t cnt = __builtin_popcountll(n);
	// uint32_t cnt = 0;
	// struct rte_mbuf *valid_pkts[64];

    // for (uint64_t m = pkts_mask; m; m &= m - 1) {        //loop runs O(set bits)
    //     uint32_t i = __builtin_ctzll(m);
    //     valid_pkts[cnt++] = pkts[i];
    // }

	// if (cnt == 0)
    //     return 0;  


    // int ret = rte_ring_sp_enqueue_bulk(ml_ring, (void **)valid_pkts, cnt, NULL); //ret = status flag 
    // if (ret == 0) {
    //    //RTE_LOG(WARNING, USER1, "ML ring full, dropping %" PRIu64 " packets\n", n);
	// 	rte_pipeline_ah_packet_drop(p, pkts_mask);
	// 	} 
	// return 0;

    uint32_t cnt = __builtin_popcountll(pkts_mask);
    struct rte_mbuf *buf[64];                    //64 packets?
    uint32_t w = 0;
    for (uint64_t m = pkts_mask; m; m &= m - 1)
                buf[w++] = pkts[__builtin_ctzll(m)];

    rte_ring_sp_enqueue_bulk(quar_ring, (void **)buf, cnt, NULL);
    return 0;      /* do not touch ml_ring here */	
    
}

void
app_main_loop_worker_pipeline_hash(void) {
	struct rte_pipeline_params pipeline_params = {
		.name = "pipeline",
		.socket_id = rte_socket_id(),
	};

	struct rte_pipeline *p;
	uint32_t port_in_id[APP_MAX_PORTS];
	uint32_t port_out_id[APP_MAX_PORTS];
	uint32_t table_id;
	uint32_t i;
	uint32_t special, ext, key_size;

	translate_options(&special, &ext, &key_size);

	RTE_LOG(INFO, USER1, "Core %u is doing work "
		"(pipeline with hash table, %s, %s, %d-byte key)\n",
		rte_lcore_id(),
		special ? "specialized" : "non-specialized",
		ext ? "extendible bucket" : "LRU",
		key_size);

	/* Pipeline configuration */
	p = rte_pipeline_create(&pipeline_params);
	pipeline_core1 = p;   
	if (p == NULL)
		rte_panic("Unable to configure the pipeline\n");

	/* Input port configuration */
	for (i = 0; i < app.n_ports; i++) {
		struct rte_port_ring_reader_params port_ring_params = {  //data structures
			.ring = app.rings_rx[i],       //points to rte_ring buffer  RX core -> worker
		};

		struct rte_pipeline_port_in_params port_params = {      //operations
			.ops = &rte_port_ring_reader_ops,
			.arg_create = (void *) &port_ring_params,
			.f_action = NULL,
			.arg_ah = NULL,
			.burst_size = app.burst_size_worker_read,
		};

		if (rte_pipeline_port_in_create(p, &port_params,
			&port_in_id[i]))     //
			rte_panic("Unable to configure input port for "
				"ring %d\n", i);
	}

	/* Output port configuration */
	for (i = 0; i < app.n_ports; i++) {
		struct rte_port_ring_writer_params port_ring_params = {
			.ring = app.rings_tx[i],      //tx ring for this port
			.tx_burst_sz = app.burst_size_worker_write,
		};

		struct rte_pipeline_port_out_params port_params = {
			.ops = &rte_port_ring_writer_ops,    //DPDK ring writing callbacks
			.arg_create = (void *) &port_ring_params,
			.f_action = NULL,
			.arg_ah = NULL,          //action handler
		};

		if (rte_pipeline_port_out_create(p, &port_params,
			&port_out_id[i]))    //stores pipeline output port ID                   //OUTPUT PORT
			rte_panic("Unable to configure output port for "
				"ring %d\n", i);
	}

	good_port_id = port_out_id[0];

	struct rte_table_hash_params table_hash_params = {
		.name = "TABLE",
		.key_size = key_size,
		.key_offset = APP_METADATA_OFFSET(32),
		.key_mask = NULL,
		.n_keys = 1 << 18,
		.n_buckets = 1 << 16,
		.f_hash = test_hash,
		.seed = 0,
	};

	switch (app.pipeline_type) {


	case e_APP_PIPELINE_HASH_KEY8_LRU:
	case e_APP_PIPELINE_HASH_KEY16_LRU:
	case e_APP_PIPELINE_HASH_KEY32_LRU:
	{
		struct rte_pipeline_table_params table_params = {
			.ops = &rte_table_hash_lru_ops,
			.arg_create = &table_hash_params,
			.f_action_hit = NULL,
			.f_action_miss = NULL,
			.arg_ah = NULL,
			.action_data_size = 0,
		};

		if (rte_pipeline_table_create(p, &table_params, &table_id))
			rte_panic("Unable to configure the hash table\n");
	}
	break;


	case e_APP_PIPELINE_HASH_SPEC_KEY16_EXT:
	{
		struct rte_pipeline_table_params table_params = {
			.ops = &rte_table_hash_key16_ext_ops,
			.arg_create = &table_hash_params,
			.f_action_hit = NULL,
			.f_action_miss = pipeline_action_miss,
			.arg_ah = NULL,
			.action_data_size = 0,
		};

		if (rte_pipeline_table_create(p, &table_params, &table_id))
			rte_panic("Unable to configure the hash table)\n");
	}
	break;

	case e_APP_PIPELINE_HASH_SPEC_KEY16_LRU:
	{
		struct rte_pipeline_table_params table_params = {
			.ops = &rte_table_hash_key16_lru_ops,
			.arg_create = &table_hash_params,
			.f_action_hit = NULL,
			.f_action_miss = pipeline_action_miss,
			.arg_ah = NULL,
			.action_data_size = 0,
		};

		if (rte_pipeline_table_create(p, &table_params, &table_id) < 0)
				rte_panic("Unable to configure the hash table\n");

		class_table_id = table_id;

	}
	break;


	default:
		rte_panic("Invalid hash table type or key size\n");
	}


	

	/* Interconnecting ports and tables */
	for (i = 0; i < app.n_ports; i++)
		if (rte_pipeline_port_in_connect_to_table(p, port_in_id[i],
			table_id))
			rte_panic("Unable to connect input port %u to "
				"table %u\n", port_in_id[i],  table_id);

	/* Add entries to tables prepopulation */
	for (i = 0; i < (1 << 18); i++) {
		struct rte_pipeline_table_entry entry = {
			.action = RTE_PIPELINE_ACTION_PORT,   //forward to port
			{.port_id = port_out_id[i & (app.n_ports - 1)]},
		};
		struct rte_pipeline_table_entry *entry_ptr;

		//build key
		uint8_t key[32]; //table expects 32 bytes
		uint32_t *k32 = (uint32_t *) key;
		int key_found, status;

		memset(key, 0, sizeof(key));
		k32[0] = rte_be_to_cpu_32(i); //stored 24-bit key in first 4 bytes

		status = rte_pipeline_table_entry_add(p, table_id, key, &entry,
			&key_found, &entry_ptr);
		if (status < 0)
			rte_panic("Unable to add entry to table %u (%d)\n",
				table_id, status);

		// RTE_LOG(INFO, USER1, "Prepopulating table: key=0x%06x -> port_out_id[%u]=%u\n",
        // i, i & (app.n_ports - 1), port_out_id[i & (app.n_ports - 1)]);
	}

	/* Enable input ports */
	for (i = 0; i < app.n_ports; i++)
		if (rte_pipeline_port_in_enable(p, port_in_id[i]))
			rte_panic("Unable to enable input port %u\n",
				port_in_id[i]);

	/* Check pipeline consistency */
	if (rte_pipeline_check(p) < 0)
		rte_panic("Pipeline consistency check failed\n");

	/* Run-time */
#if APP_FLUSH == 0
	while (!force_quit) {
        rte_pipeline_run(p);
		struct rte_mbuf *q;
	while (rte_ring_sc_dequeue(quar_ring, (void **)&q) == 0) {

			struct flow_ctx *ctx =
				*(struct flow_ctx **)RTE_MBUF_METADATA_UINT8_PTR(q,
										FLOWCTX_PTR_OFFSET);

			/* make sure we read ctx->verdict after ML thread’s write */
			rte_smp_rmb();

			if (likely(ctx->verdict == 1)) {           /* PASS */
					rte_pipeline_port_out_packet_insert(p,
														good_port_id,
														q);
					continue;                          /* next packet */
			}

			if (likely(ctx->verdict == 2)) {           /* DROP */
					rte_pktmbuf_free(q);
					continue;
			}

			/* verdict == 0  → still waiting; put it back and exit loop */
			rte_ring_sp_enqueue(quar_ring, q);
			break;
	}
}
		
#else
	i = 0;
	while (!force_quit) {
		rte_pipeline_run(p);

		if ((i & APP_FLUSH) == 0)
			rte_pipeline_flush(p);
		i++;
	}
#endif
}


uint64_t test_hash(
	void *key,
	__rte_unused void *key_mask,
	__rte_unused uint32_t key_size,
	__rte_unused uint64_t seed)
{
	uint32_t *k32 = key;
	uint32_t ip_dst = rte_be_to_cpu_32(k32[0]);
	uint64_t signature = (ip_dst >> 2) | ((ip_dst & 0x3) << 30);

	return signature;
}




//feature extraction function
static void extract_packet_features(struct rte_mbuf *m, struct packet_features *features) {
    uint8_t *m_data = rte_pktmbuf_mtod(m, uint8_t *);
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)m_data;
    
    // Clear features
    memset(features, 0, sizeof(struct packet_features));
    
    // Get Ethernet type
    uint16_t ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);
    features->protocols.arp = (ether_type == RTE_ETHER_TYPE_ARP);
    features->protocols.ipv4 = (ether_type == RTE_ETHER_TYPE_IPV4);
    
    if (features->protocols.ipv4) {
        struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        features->header_length = (ip_hdr->version_ihl & 0x0f) * 4;
        features->protocol_type = ip_hdr->next_proto_id;
        
        if (ip_hdr->next_proto_id == IPPROTO_TCP) {
            features->protocols.tcp = 1;
            struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)
                ((uint8_t *)ip_hdr + features->header_length);
            
            // TCP flags
            features->tcp_flags.fin = (tcp_hdr->tcp_flags & RTE_TCP_FIN_FLAG) != 0;
            features->tcp_flags.syn = (tcp_hdr->tcp_flags & RTE_TCP_SYN_FLAG) != 0;
            features->tcp_flags.rst = (tcp_hdr->tcp_flags & RTE_TCP_RST_FLAG) != 0;
            features->tcp_flags.psh = (tcp_hdr->tcp_flags & RTE_TCP_PSH_FLAG) != 0;
            features->tcp_flags.ack = (tcp_hdr->tcp_flags & RTE_TCP_ACK_FLAG) != 0;
            features->tcp_flags.ece = (tcp_hdr->tcp_flags & RTE_TCP_ECE_FLAG) != 0;
            features->tcp_flags.cwr = (tcp_hdr->tcp_flags & RTE_TCP_CWR_FLAG) != 0;
            
            // Port-based protocol detection
            uint16_t dst_port = rte_be_to_cpu_16(tcp_hdr->dst_port);
            features->protocols.http = (dst_port == 80);
            features->protocols.https = (dst_port == 443);
            features->protocols.ssh = (dst_port == 22);
            features->protocols.telnet = (dst_port == 23);
            features->protocols.smtp = (dst_port == 25);
            features->protocols.irc = (dst_port == 6667);
            
        } else if (ip_hdr->next_proto_id == IPPROTO_UDP) {
            features->protocols.udp = 1;
            struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)
                ((uint8_t *)ip_hdr + features->header_length);
            
            uint16_t dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);
            features->protocols.dns = (dst_port == 53);
            features->protocols.dhcp = (dst_port == 67 || dst_port == 68);
        }
    }
}

static inline void
build_ml_features_from_ctx(const struct flow_ctx *c,
                           struct ml_input_features *ml)
{
    /* Protect against divide-by-zero on the very first packet. */
    const uint32_t N    = c->pkt_cnt ? c->pkt_cnt : 1;
    const float    invN = 1.0f / (float)N;

    /* ─── Basic header / flow metrics ──────────────────────────── */
    ml->header_length = (float)c->hdr_len_sum * invN;                  /* mean */
    ml->time_to_live  = (float)c->ttl_sum     * invN;                  /* mean */

    /* ‘Rate’ = packets per second over the current window. */
    double duration_sec =
        (double)(c->last_tsc - c->first_tsc) * tsc_to_sec;
    ml->rate = (float)(N / (duration_sec > 0.0 ? duration_sec : 1e-9));

    /* ─── TCP flag relative counts (flags per packet) ──────────── */
    ml->fin_flag_number = (float)c->fin_cnt * invN;
    ml->syn_flag_number = (float)c->syn_cnt * invN;
    ml->rst_flag_number = (float)c->rst_cnt * invN;
    ml->psh_flag_number = (float)c->psh_cnt * invN;
    ml->ack_flag_number = (float)c->ack_cnt * invN;

    /* ─── Application / L4 protocol fractions (packets per pkt) ── */
    ml->http  = (float)c->http_cnt  * invN;
    ml->https = (float)c->https_cnt * invN;
    ml->dns   = (float)c->dns_cnt   * invN;
    ml->ssh   = (float)c->ssh_cnt   * invN;

    /* fields not tracked in the datapath → zero */
    ml->tcp   = (float)c->tcp_cnt   * invN;
    ml->udp   = (float)c->udp_cnt   * invN;
    ml->arp   = (float)c->arp_cnt   * invN;
    ml->icmp  = (float)c->icmp_cnt  * invN;

    /* ─── Packet-size statistics over the window ──────────────── */
    ml->tot_sum = (float)c->byte_cnt;
    ml->min     = (float)c->size_min;
    ml->max     = (float)c->size_max;
    ml->avg     = (float)c->byte_cnt * invN;

    /* Std-dev & variance of packet size */
    double mean = (double)c->byte_cnt / (double)N;
    double var  = ((double)c->size_sq / (double)N) - mean * mean;
    var         = var < 0.0 ? 0.0 : var;           /* guard FP noise */

    ml->std      = (float)sqrt(var);
    ml->variance = (float)var;

    /* Mean inter-arrival time in micro-seconds */
    ml->iat = (float)(c->iat_sum * tsc_to_usec * invN);

	printf("ML Features populated: header=%f, ttl=%f, rate=%f, var=%f\n", 
           ml->header_length, ml->time_to_live, ml->rate, ml->variance);
}



void
app_main_loop_rx_metadata(void) {
	uint32_t i, j;
	int ret;
	uint64_t rx_count = 0;
	

	RTE_LOG(INFO, USER1, "Core %u is doing RX (with meta-data)\n",
		rte_lcore_id()); //current core executing loop

	while (!force_quit) {
		//for each port
		for (i = 0; i < app.n_ports; i++) {
			uint16_t n_mbufs;
			

			//receives packets from port i into app.mbuf_rx.array(pointers to rte_mbuf)
			n_mbufs = rte_eth_rx_burst(
				app.ports[i],
				0,
				app.mbuf_rx.array,              //allocates mbufs from mempool
				app.burst_size_rx_read);

			if (n_mbufs == 0)
				continue;

			rx_count += n_mbufs;
			//RTE_LOG(INFO, USER1, "RX: Port %u received %u packets (Total RX: %"PRIu64 ")\n",
			//	app.ports[i], n_mbufs, rx_count);

				//loop through received packets
			for (j = 0; j < n_mbufs; j++) {
				struct rte_mbuf *m;
				uint8_t *m_data, *key;
				struct rte_ipv4_hdr *ip_hdr;
				struct rte_ipv6_hdr *ipv6_hdr;
				uint32_t ip_dst;
				uint8_t *ipv6_dst;
				uint32_t *signature, *k32;
				uint64_t now = rte_rdtsc();

				m = app.mbuf_rx.array[j];//pointer to jth packet in burst
				m_data = rte_pktmbuf_mtod(m, uint8_t *);  //points to start of pkt data(Eth header, after metadata buffer)
				signature = RTE_MBUF_METADATA_UINT32_PTR(m,
						APP_METADATA_OFFSET(0)); // points to 4-byte metadata field at offset 0
				key = RTE_MBUF_METADATA_UINT8_PTR(m,
						APP_METADATA_OFFSET(32)); // variable len metadata field at offset 32
				struct packet_features *features = (struct packet_features *)RTE_MBUF_METADATA_UINT8_PTR(m, 
            			APP_METADATA_OFFSET(40));

				if (RTE_ETH_IS_IPV4_HDR(m->packet_type)) {
					ip_hdr = (struct rte_ipv4_hdr *)
						&m_data[sizeof(struct rte_ether_hdr)]; //points to start of IPv4 header
					ip_dst = ip_hdr->dst_addr;

					k32 = (uint32_t *) key; //casts key to 32-bit integer pointer
					k32[0] = ip_dst; //& 0xFFFFFF00;     //masks last 8 bits of IP address
				} else if (RTE_ETH_IS_IPV6_HDR(m->packet_type)) {
					ipv6_hdr = (struct rte_ipv6_hdr *)
						&m_data[sizeof(struct rte_ether_hdr)];
					ipv6_dst = ipv6_hdr->dst_addr;

					memcpy(key, ipv6_dst, 16);
				} else
					continue;

				*signature = test_hash(key, NULL, 0, 0);//signature generated from key, fast filtering for exact key match.

				// Extract additional features for ML
            	extract_packet_features(m, features);


					/* ---------- MISS path: get or create flow_ctx -------------------- */
				int32_t fid = rte_hash_lookup(flow_ht, key);
				struct flow_ctx *ctx;

				if (fid < 0) {
						/* first packet of a new, unknown flow */
						fid = rte_hash_add_key(flow_ht, key);

						rte_mempool_get(ctx_mp, (void **)&ctx);
						if (!ctx) rte_panic("ctx_mp exhausted\n");
						memset(ctx, 0, sizeof(*ctx));

						ctx->size_min   = UINT16_MAX;
						ctx->first_tsc  = ctx->last_tsc = now;
						ctx->inflight   = 0;
						memcpy(ctx->signature_key, key, KEY_LEN);

						flow_pool[fid] = ctx;
				}
				/* ctx now valid                                                     */
				ctx = flow_pool[fid];
				*(struct flow_ctx **)
					RTE_MBUF_METADATA_UINT8_PTR(m, FLOWCTX_PTR_OFFSET) = ctx;
				rte_smp_wmb();


				/* ------------ per-packet running sums ---------------------------- */
				ctx->pkt_cnt++;

				uint32_t sz = m->pkt_len;
				ctx->byte_cnt  += sz;
				ctx->size_sq   += (uint64_t)sz * sz;
				if (sz < ctx->size_min) ctx->size_min = sz;
				if (sz > ctx->size_max) ctx->size_max = sz;

				/* header + TTL                                                      */
				ctx->hdr_len_sum += features->header_length;
				uint8_t ttl = features->protocols.ipv4 ? ip_hdr->time_to_live : 0;
				ctx->ttl_sum     += ttl;

				/* inter-arrival time                                                */
				uint64_t prev = ctx->last_tsc;
				ctx->last_tsc  = now;
				if (ctx->pkt_cnt > 1)
						ctx->iat_sum += now - prev;

				/* TCP flag counters                                                 */
				if (features->tcp_flags.fin) ctx->fin_cnt++;
				if (features->tcp_flags.syn) ctx->syn_cnt++;
				if (features->tcp_flags.rst) ctx->rst_cnt++;
				if (features->tcp_flags.psh) ctx->psh_cnt++;
				if (features->tcp_flags.ack) ctx->ack_cnt++;

				/* protocol counters                                                 */
				if (features->protocols.http)  ctx->http_cnt++;
				if (features->protocols.https) ctx->https_cnt++;
				if (features->protocols.dns)   ctx->dns_cnt++;
				if (features->protocols.ssh)   ctx->ssh_cnt++;

				if (features->protocols.tcp)   ctx->tcp_cnt++;
				if (features->protocols.udp)   ctx->udp_cnt++;
				if (features->protocols.arp)   ctx->arp_cnt++;
				if (features->protocols.icmp)  ctx->icmp_cnt++;

				/* -------- emit vector once per 100 packets ----------------------- */
				if (ctx->pkt_cnt == 100 && ctx->inflight == 0) {

						if (ctx->t_first_pkt == 0)          /* set once per window                */
        					ctx->t_first_pkt = now;

						struct rte_mbuf *msg = rte_pktmbuf_alloc(vec_mp);
						if(unlikely(msg ==NULL)){
							rte_panic("vec_mp exhausted\n");

						}

						struct ml_input_features *ml =
							(struct ml_input_features *)
							rte_pktmbuf_append(msg, sizeof(struct ml_input_features));
						if (unlikely(ml == NULL))
								rte_panic("vec_mp data room too small\n");


						RTE_LOG(INFO, USER1,
						"emit-vec: pkt_cnt=%u byte_cnt=%"PRIu64" hdr_sum=%u ttl_sum=%u\n",
						ctx->pkt_cnt, ctx->byte_cnt, ctx->hdr_len_sum, ctx->ttl_sum);

						build_ml_features_from_ctx(ctx, ml);

						ctx->inflight = 1;

						struct flow_ctx **ctx_ptr = (struct flow_ctx **)RTE_MBUF_METADATA_UINT8_PTR(msg, FLOWCTX_PTR_OFFSET);
						*ctx_ptr = ctx;
						rte_ring_sp_enqueue(ml_ring, msg);

						uint64_t saved_t_first_pkt = ctx->t_first_pkt;
						uint64_t saved_signature_key0 = ctx->signature_key[0];
						uint64_t saved_signature_key1 = ctx->signature_key[1];

						/* reset running stats for next 100-packet window            */
						memset(ctx, 0, offsetof(struct flow_ctx, inflight)); /* keep inflight */

						ctx->t_first_pkt = saved_t_first_pkt;
						ctx->signature_key[0] = saved_signature_key0;
						ctx->signature_key[1] = saved_signature_key1;


						ctx->size_min   = UINT16_MAX;
						ctx->first_tsc  = ctx->last_tsc = now;

						/* seed new window with this very packet                     */
						ctx->pkt_cnt    = 1;
						ctx->iat_sum  = 0;
						ctx->byte_cnt   = sz;
						ctx->size_sq    = (uint64_t)sz * sz;
						ctx->size_max   = ctx->size_min = sz;
						ctx->hdr_len_sum = features->header_length;
						ctx->ttl_sum     = ttl;
				}


			}

			//enqueues to RX ring for worker core
			ret = rte_ring_sp_enqueue_bulk(
					app.rings_rx[i],
					(void **) app.mbuf_rx.array,      //packets to enqueue
					n_mbufs,         //number of packets to enqueue 
					NULL);

			if(ret == 0){
				//Drop all packts in burst if ring is full
				for(uint16_t k = 0; k< n_mbufs; k++){
					rte_pktmbuf_free(app.mbuf_rx.array[k]);
					app.mbuf_rx.array[k] = NULL; //set pointer to NULL after freeing
				}
			}


		}
	}
}
