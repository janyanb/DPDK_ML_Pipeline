#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_byteorder.h>

#include <rte_port_ring.h>
#include <rte_table_hash.h>
#include <rte_hash.h>
#include <rte_pipeline.h>
#include <rte_mldev.h>

#include "main.h"


// Feature buffer for batched inference
static
struct packet_features feature_buffer[MAX_BATCH_SIZE] __rte_cache_aligned;

void
app_main_loop_worker_ml(void)
{
    struct rte_mbuf *pkts[MAX_BATCH_SIZE];
    struct rte_ml_op *ops[MAX_BATCH_SIZE];
    unsigned n, i;
    uint16_t ml_dev_id = 0;  // ML device ID
    uint16_t qp_id = 0;      // Queue pair ID ???


    // Allocate ML ops
    for (i = 0; i < MAX_BATCH_SIZE; i++) {
        ops[i] = rte_ml_op_alloc();
        if (ops[i] == NULL)
            rte_exit(EXIT_FAILURE, "Failed to allocate ML op\n");
    }

    RTE_LOG(INFO, USER1, "Core %u is executing ML worker\n", rte_lcore_id());


    while (!force_quit) {
        // Dequeue packets from the ML ring
        n = rte_ring_sc_dequeue_bulk(ml_ring, (void **)pkts, MAX_BATCH_SIZE, NULL);
        //RTE_LOG(INFO, USER1, "ML worker loop running\n");
        if (n == 0) {
            continue;
        }

        //Extract features for ML ops
        for (i = 0; i < n; i++) {

            struct rte_mbuf *m = pkts[i];
            uint32_t *signature = RTE_MBUF_METADATA_UINT32_PTR(m, APP_METADATA_OFFSET(0));
            uint8_t *key = RTE_MBUF_METADATA_UINT8_PTR(m, APP_METADATA_OFFSET(32));

            // Extract features
            feature_buffer[i].signature = *signature;
            feature_buffer[i].dst_ip = *(uint32_t *)key;


            // Setup ML operation
            ops[i]->model_id = MODEL_ID; 
            ops[i]->nb_batches = 1;
            ops[i]->input[0].addr = &feature_buffer[i];
            ops[i]->input[0].length = sizeof(struct packet_features);
            ops[i]->mempool = NULL;


            // // Check if packet is IPv4
            // if (RTE_ETH_IS_IPV4_HDR(m->packet_type)) {
            //     // Print signature
            //     RTE_LOG(INFO, USER1, "IPv4 packet %p: signature=0x%08x\n", m, *signature);

            //     // Print key as IPv4 address
            //     uint32_t ip = *(uint32_t *)key;
            //     RTE_LOG(INFO, USER1, "  Key (IPv4 dst): %u.%u.%u.%u\n",
            //         ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff);
            // }

            // rte_pktmbuf_free(m);
        }

        // Enqueue batch for inference
        uint16_t enq = rte_ml_enqueue_burst(ml_dev_id, qp_id, ops, n);
        if (enq < n) {
            RTE_LOG(WARNING, USER1, "Only enqueued %u of %u ops\n", enq, n);
            // Free packets that couldn't be enqueued
            for (i = enq; i < n; i++) {
                rte_pktmbuf_free(pkts[i]);
            }
        }

        // Dequeue inference results
        uint16_t deq = rte_ml_dequeue_burst(ml_dev_id, qp_id, ops, enq);
        for (i = 0; i < deq; i++) {
            if (ops[i]->status == RTE_ML_OP_STATUS_SUCCESS) {
                // Process inference result
                float *result = (float *)ops[i]->output[0].addr;
                RTE_LOG(INFO, USER1, "Packet %p inference result: %f\n", 
                       pkts[i], *result);

                // Take action based on inference result
                if (*result > 0.5) {  // Example threshold
                    RTE_LOG(INFO, USER1, "Suspicious packet detected!\n");
                    // Add your handling logic here
                }
            } else {
                RTE_LOG(ERR, USER1, "Inference failed for packet %p\n", pkts[i]);
            }
            rte_pktmbuf_free(pkts[i]);
        }

    }

        for (i = 0; i < MAX_BATCH_SIZE; i++) {
        rte_ml_op_free(ops[i]);
    }
}