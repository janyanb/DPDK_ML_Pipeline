/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_malloc.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_mldev.h>
#include <rte_hash.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_bus_vdev.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_lpm.h>
#include <rte_lpm6.h>


#include "main.h"
#include "onnx_inference.h"

struct app_params app = {
	/* Ports*/
	.n_ports = APP_MAX_PORTS,
	.port_rx_ring_size = 128,
	.port_tx_ring_size = 512,

	/* Rings */
	.ring_rx_size = 4096,
	.ring_tx_size = 4096,

	/* Buffer pool */
	.pool_buffer_size = 2048 + RTE_PKTMBUF_HEADROOM,
	.pool_size = 32 * 1024,
	.pool_cache_size = 256,

	/* Burst sizes */
	.burst_size_rx_read = 64,
	.burst_size_rx_write = 10,
	.burst_size_worker_read = 10,
	.burst_size_worker_write = 10,
	.burst_size_tx_read = 10,
	.burst_size_tx_write = 10,
};



struct rte_ring *ml_ring;
struct rte_hash      *flow_ht   = NULL;
struct flow_ctx     **flow_pool = NULL;
struct rte_mempool   *ctx_mp    = NULL;

struct rte_ring      *quar_ring = NULL;
struct rte_mempool   *vec_mp    = NULL;

/* shared handles for pipeline / ML worker */
struct rte_pipeline  *pipeline_core1 = NULL;
void                 *class_tbl_core1 = NULL;
uint32_t              class_table_id  = 0;
uint32_t              good_port_id    = 0; 


static struct rte_eth_conf port_conf = {
	.rxmode = {
		.offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = RTE_ETH_RSS_IP,
		},
	},
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
	},
};

static struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = 8,
		.hthresh = 8,
		.wthresh = 4,
	},
	.rx_free_thresh = 64,
	.rx_drop_en = 0,
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = 36,
		.hthresh = 0,
		.wthresh = 0,
	},
	.tx_free_thresh = 0,
	.tx_rs_thresh = 0,
};

static void
app_init_mbuf_pools(void)
{
	/* Init the buffer pool used for packet storage*/
	RTE_LOG(INFO, USER1, "Creating the mbuf pool ...\n");
	app.pool = rte_pktmbuf_pool_create("mempool", app.pool_size,
		app.pool_cache_size, 0, app.pool_buffer_size, rte_socket_id());
	if (app.pool == NULL)
		rte_panic("Cannot create mbuf pool\n");
}

static void
app_init_rings(void)
{
	uint32_t i;

	for (i = 0; i < app.n_ports; i++) {
		char name[32];

		snprintf(name, sizeof(name), "app_ring_rx_%u", i);

		app.rings_rx[i] = rte_ring_create(
			name,
			app.ring_rx_size,
			rte_socket_id(),
			RING_F_SP_ENQ | RING_F_SC_DEQ);

		if (app.rings_rx[i] == NULL)
			rte_panic("Cannot create RX ring %u\n", i);
	}

	for (i = 0; i < app.n_ports; i++) {
		char name[32];

		snprintf(name, sizeof(name), "app_ring_tx_%u", i);

		app.rings_tx[i] = rte_ring_create(
			name,
			app.ring_tx_size,
			rte_socket_id(),
			RING_F_SP_ENQ | RING_F_SC_DEQ);

		if (app.rings_tx[i] == NULL)
			rte_panic("Cannot create TX ring %u\n", i);
	}

}


static void 
app_init_ml_ring(void) {
    ml_ring = rte_ring_create
	("ML_RING", 
		4096,  // Size of the ML ring
		rte_socket_id(), 
		RING_F_SP_ENQ | RING_F_SC_DEQ);  //single-producer, single-consumer(lockless rings)

    if (ml_ring == NULL)
        rte_panic("Cannot create ML ring\n");
}


static void
app_ports_check_link(void)
{
	uint32_t all_ports_up, i;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];
	all_ports_up = 1;

	for (i = 0; i < app.n_ports; i++) {
		struct rte_eth_link link;
		uint16_t port;
		int ret;

		port = app.ports[i];
		memset(&link, 0, sizeof(link));
		ret = rte_eth_link_get_nowait(port, &link);
		if (ret < 0) {
			RTE_LOG(INFO, USER1,
				"Failed to get port %u link status: %s\n",
				port, rte_strerror(-ret));
			all_ports_up = 0;
			continue;
		}
		rte_eth_link_to_str(link_status_text, sizeof(link_status_text),
				    &link);
		RTE_LOG(INFO, USER1, "Port %u %s\n",
			port,
			link_status_text);
		if (link.link_status == RTE_ETH_LINK_DOWN)
			all_ports_up = 0;
	}

	if (all_ports_up == 0)
		rte_panic("Some NIC ports are DOWN\n");
}

static void
app_init_ports(void)
{
	uint32_t i;
	struct rte_eth_dev_info dev_info;

	/* Init NIC ports, then start the ports */
	for (i = 0; i < app.n_ports; i++) {
		uint16_t port;
		int ret;
		struct rte_eth_conf local_port_conf = port_conf;

		port = app.ports[i];
		RTE_LOG(INFO, USER1, "Initializing NIC port %u ...\n", port);

		ret = rte_eth_dev_info_get(port, &dev_info);
		if (ret != 0)
			rte_panic("Error during getting device (port %u) info: %s\n",
					port, rte_strerror(-ret));

		/* Init port */
		local_port_conf.rx_adv_conf.rss_conf.rss_hf &=
			dev_info.flow_type_rss_offloads;
		if (local_port_conf.rx_adv_conf.rss_conf.rss_hf !=
				port_conf.rx_adv_conf.rss_conf.rss_hf) {
			printf("Warning:"
				"Port %u modified RSS hash function based on hardware support,"
				"requested:%#"PRIx64" configured:%#"PRIx64"\n",
				port,
				port_conf.rx_adv_conf.rss_conf.rss_hf,
				local_port_conf.rx_adv_conf.rss_conf.rss_hf);
		}

		ret = rte_eth_dev_configure(
			port,
			1,
			1,
			&local_port_conf);
		if (ret < 0)
			rte_panic("Cannot init NIC port %u (%d)\n", port, ret);

		ret = rte_eth_promiscuous_enable(port);
		if (ret != 0)
			rte_panic("Cannot enable promiscuous mode for port %u: %s\n",
				port, rte_strerror(-ret));

		/* Init RX queues */
		ret = rte_eth_rx_queue_setup(
			port,
			0,
			app.port_rx_ring_size,
			rte_eth_dev_socket_id(port),
			&rx_conf,
			app.pool);
		if (ret < 0)
			rte_panic("Cannot init RX for port %u (%d)\n",
				(uint32_t) port, ret);

		/* Init TX queues */
		ret = rte_eth_tx_queue_setup(
			port,
			0,
			app.port_tx_ring_size,
			rte_eth_dev_socket_id(port),
			&tx_conf);
		if (ret < 0)
			rte_panic("Cannot init TX for port %u (%d)\n",
				(uint32_t) port, ret);

		/* Start port */
		ret = rte_eth_dev_start(port);
		if (ret < 0)
			rte_panic("Cannot start port %u (%d)\n", port, ret);
	}

	app_ports_check_link();
}

static void 
app_init_flow_state(void)
{
    // ─── flow-state hash, only Core-0 writes ──────────────
    flow_ht = rte_hash_create(&(struct rte_hash_parameters){
        .name = "flow_ctx_ht",
        .key_len = 20,                    // 5-tuple
        .entries = 1 << 20,
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY   // read-only from Core-1
    });
    if (!flow_ht)
        rte_panic("Cannot create flow hash table\n");

    ctx_mp  = rte_pktmbuf_pool_create("ctx_mp", 1<<20, 0, 0,
                                      sizeof(struct flow_ctx), rte_socket_id());
    if (!ctx_mp)
        rte_panic("Cannot create flow_ctx mempool\n");

    // ─── tiny mempool for ML messages (vectors) ───────────
    vec_mp  = rte_pktmbuf_pool_create("vec_mp", 64*1024, 0, 0,
                                      ML_VEC_DATA + RTE_PKTMBUF_HEADROOM,
                                      rte_socket_id());
    if (!vec_mp)
        rte_panic("Cannot create ML vector mempool\n");

    // ─── quarantine ring seen only by Core-1 ──────────────
    quar_ring = rte_ring_create("quar", 1<<16, rte_socket_id(),
                                RING_F_SP_ENQ | RING_F_SC_DEQ);   // single prod/cons
    if (!quar_ring){
		rte_panic("Cannot create quarantine ring\n");
	}
        

	flow_pool = rte_zmalloc_socket("flow_pool",
                                   (1<<20) * sizeof(struct flow_ctx *),
                                   RTE_CACHE_LINE_SIZE,
                                   rte_socket_id());
    if (!flow_pool){
        rte_panic("Cannot alloc flow_pool\n");
    }
}

double tsc_to_sec  = 0.0;
double tsc_to_usec = 0.0;


void
app_init(void)
{
	app_init_mbuf_pools();
	app_init_rings();
	app_init_ports();
	app_init_ml_ring();
	app_init_flow_state();
	tsc_to_sec  = 1.0 / (double)rte_get_tsc_hz();
    tsc_to_usec = tsc_to_sec * 1e6;


	if (onnx_init(MODEL_PATH) != 0)
    	rte_exit(EXIT_FAILURE, "Failed to initialize ONNX Runtime\n");




	RTE_LOG(INFO, USER1, "Initialization completed\n");
}
