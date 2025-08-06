#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>  
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_byteorder.h>
#include <rte_common.h>

#include <rte_port_ring.h>
#include <rte_table_hash.h>
#include <rte_hash.h>
#include <rte_pipeline.h>
#include <unistd.h> 

#include "onnx_inference.h"
#include "main.h"


// DDOS-PSHACK_FLOOD (normalized)
struct ml_input_features pshack_flood = {
    .header_length = 19.92f / 100.0f,     // Normalize header length
    .time_to_live = 63.36f / 255.0f,      // Normalize TTL
    .rate = 25893.96222f / 100000.0f,     // Normalize rate (cap at 1.0 if needed)
    .fin_flag_number = 0.0f,
    .syn_flag_number = 0.0f,
    .rst_flag_number = 0.0f,
    .psh_flag_number = 0.99f,             // High PSH flag ratio
    .ack_flag_number = 0.99f,             // High ACK flag ratio
    .http = 0.0f,
    .https = 0.01f,
    .dns = 0.0f,
    .ssh = 0.0f,
    .tcp = 0.99f,
    .udp = 0.0f,
    .arp = 0.01f,
    .icmp = 0.0f,
    .tot_sum = 6421.0f / 50000.0f,        // Normalize total bytes
    .min = 60.0f / 1500.0f,
    .max = 481.0f / 1500.0f,
    .avg = 64.21f / 1500.0f,
    .std = 42.1f / 500.0f,
    .iat = 0.0000386f / 0.01f,            // Normalize inter-arrival time
    .variance = 1772.41f / 10000.0f
};

// DOS-UDP_FLOOD (normalized)
struct ml_input_features udp_flood = {
    .header_length = 7.92f / 100.0f,      // Normalize header length
    .time_to_live = 65.91f / 255.0f,      // Normalize TTL
    .rate = 19673.09568f / 100000.0f,     // Cap at 1.0 if higher than training max
    .fin_flag_number = 0.0f,
    .syn_flag_number = 0.0f,
    .rst_flag_number = 0.0f,
    .psh_flag_number = 0.0f,
    .ack_flag_number = 0.0f,
    .http = 0.0f,
    .https = 0.0f,
    .dns = 0.0f,
    .ssh = 0.0f,
    .tcp = 0.0f,
    .udp = 0.99f,                         // High UDP ratio
    .arp = 0.0f,
    .icmp = 0.01f,
    .tot_sum = 6010.0f / 50000.0f,
    .min = 60.0f / 1500.0f,
    .max = 70.0f / 1500.0f,
    .avg = 60.1f / 1500.0f,
    .std = 1.0f / 500.0f,
    .iat = 0.0000574f / 0.01f,
    .variance = 1.0f / 10000.0f
};


// DDOS-ICMP_FLOOD ( normalized)
struct ml_input_features icmp_flood = {
    .header_length = 0.32f / 100.0f,
    .time_to_live = 63.96f / 255.0f,
    .rate = 28944.19985f / 100000.0f,     // High rate (cap at 1.0)
    .fin_flag_number = 0.0f,
    .syn_flag_number = 0.0f,
    .rst_flag_number = 0.0f,
    .psh_flag_number = 0.0f,
    .ack_flag_number = 0.01f,
    .http = 0.0f,
    .https = 0.01f,
    .dns = 0.0f,
    .ssh = 0.0f,
    .tcp = 0.01f,
    .udp = 0.0f,
    .arp = 0.0f,
    .icmp = 0.99f,                         // High ICMP ratio
    .tot_sum = 6006.0f / 50000.0f,
    .min = 60.0f / 1500.0f,
    .max = 66.0f / 1500.0f,
    .avg = 60.06f / 1500.0f,
    .std = 0.6f / 500.0f,
    .iat = 0.0000346f / 0.01f,
    .variance = 0.36f / 10000.0f
};


//----------------------------------TEST ONNX MODEL^^


// Feature buffer for batched inference
static
struct ml_input_features feature_buffer[MAX_BATCH_SIZE] __rte_cache_aligned;


static const float header_length_min = 20.0f;  // based ontraining data
static const float header_length_max = 60.0f;
static const float protocol_type_min = 0.0f;
static const float protocol_type_max = 255.0f;



void
app_main_loop_worker_ml(void)
{
    //struct rte_mbuf *pkts[MAX_BATCH_SIZE];
   // unsigned n, i;


   //--------------------------TEST-------------------------
    
        struct ml_input_features samples[] = {
        pshack_flood,
        udp_flood,
        icmp_flood
    };
    
    const char* attack_names[] = {
        "DDOS-PSHACK_FLOOD",
        "DOS-UDP_FLOOD",
        "DDOS-ICMP_FLOOD"
    };



    //---------------------------------------------------------


   struct rte_mbuf *msg;

    RTE_LOG(INFO, USER1, "Core %u is executing ML worker\n", rte_lcore_id());


    while (!force_quit) {
        if (rte_ring_sc_dequeue(ml_ring, (void **)&msg) != 0)
            continue;

        struct ml_input_features *in = rte_pktmbuf_mtod(msg, struct ml_input_features *);
        float out;
        uint64_t t_before = rte_rdtsc();
        if (onnx_infer(in, &out) != 0) {          //fix input
            RTE_LOG(ERR, USER1, "ONNX inference failed\n");
            rte_pktmbuf_free(msg);
            continue;
        }
        uint64_t t_after  = rte_rdtsc();

        uint64_t cycles_infer = t_after - t_before;          /* pure ML runtime */


        uint8_t verdict = (out > 0.5f) ? 2 : 1; // 1 = good, 2 = bad
        struct flow_ctx *ctx = *(struct flow_ctx **)RTE_MBUF_METADATA_UINT8_PTR(msg, 0);

        struct rte_pipeline_table_entry e;
        int key_found; struct rte_pipeline_table_entry *dummy;

        if (verdict == 1) {                 /* benign  → forward         */
            e.action  = RTE_PIPELINE_ACTION_PORT;
            e.port_id = good_port_id;          /* TX-ring that sends to NIC */
        } else {                            /* malicious → DROP          */
            e.action  = RTE_PIPELINE_ACTION_DROP;
            /* port_id is ignored when action == DROP                    */
        }
        rte_pipeline_table_entry_add(pipeline_core1, class_table_id,                  //VALUE FOR TALE ID AND PIPELINE?
                                    ctx->signature_key, &e,
                                    &key_found, &dummy);

        ctx->t_rule_ready = rte_rdtsc();

        uint64_t cycles_total = ctx->t_rule_ready - ctx->t_first_pkt;
        double latency_infer_us = cycles_infer * tsc_to_usec;
        double latency_total_us = cycles_total * tsc_to_usec;

        RTE_LOG(INFO, USER1,
                "ML latency = %.2f us, End-to-End latency = %.2f us, Flow ID = 0x%" PRIx32 "\n",
                latency_infer_us, latency_total_us, ctx->signature_key[0]);


        ctx->verdict = verdict;
        rte_smp_wmb();
        ctx->inflight = 0; 

        // rte_hash_del_key(flow_ht, ctx->signature_key);
        // rte_mempool_put(ctx_mp, ctx);
        rte_pktmbuf_free(msg);


        for (int i = 0; i < 3; i++) {
        float result;
        printf("Testing %s sample:\n", attack_names[i]);
        
        // Print input features for debugging
        float *feature_array = (float*)&samples[i];
        printf("Feature values: ");
        for (int j = 0; j < 23; j++) {
            printf("%f, ", feature_array[j]);
        }
        printf("\n");
        
        if (onnx_infer(&samples[i], &result) != 0) {
            printf("ONNX inference failed\n");
        } else {
            printf("ONNX output: %f (Verdict: %s)\n", 
                   result, (result > 0.5f ? "ATTACK" : "BENIGN"));
        }
        printf("\n");
    }





    }
}



