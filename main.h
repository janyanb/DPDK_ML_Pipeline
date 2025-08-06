#ifndef _MAIN_H_
#define _MAIN_H_

#include <rte_common.h>   
#include <stdbool.h> 

#ifndef APP_MBUF_ARRAY_SIZE
#define APP_MBUF_ARRAY_SIZE 256
#endif


struct app_mbuf_array {
	struct rte_mbuf *array[APP_MBUF_ARRAY_SIZE];
	uint16_t n_mbufs;
};

#ifndef APP_MAX_PORTS
#define APP_MAX_PORTS 4
#endif

extern uint32_t good_port_id;


/* ML Model Configuration */
#define MODEL_PATH "/home/janyanb/dpdk-24.03/app/test-pipeline/model/network_anomaly_flow_model.onnx"

#define MAX_BATCH_SIZE 10

/* Feature sizes */
#define INPUT_SIZE (2 * sizeof(uint32_t))  // signature + dst_ip
#define OUTPUT_SIZE sizeof(float)          // classification result

#define WIN_PKTS 100
#define CYCLES_HZ rte_get_tsc_hz()
#define FLOWCTX_PTR_OFFSET   APP_METADATA_OFFSET(24)
/* Model parameters structure */
struct ml_model_params {
    const char *path;
    uint32_t input_size;
    uint32_t output_size;
};

// Define ML input features structure
struct ml_input_features {
    /* ─── Basic header/flow metrics ────────────────────────────── */
    float header_length;   /* “Header_Length”  */
    float time_to_live;    /* “Time_To_Live”   */
    float rate;            /* packets-per-second “Rate” */

    /* ─── TCP flag relative counts (per-flow averages) ─────────── */
    float fin_flag_number;
    float syn_flag_number;
    float rst_flag_number;
    float psh_flag_number;
    float ack_flag_number;

    /* ─── Application / L4 protocol fractions (0-1) ────────────── */
    float http;   /* port  80  */
    float https;  /* port 443  */
    float dns;    /* port  53  */
    float ssh;    /* port  22  */
    float tcp;    /* aggregate TCP packets in flow   */
    float udp;    /* aggregate UDP packets in flow   */
    float arp;    /* ARP frames inside the flow (!)  */
    float icmp;   /* ICMP echo etc.                  */

    /* ─── Packet-size statistics over the flow window —─────────── */
    float tot_sum;    /* Σ bytes in window (“Tot sum”) */
    float min;        /* smallest packet size          */
    float max;        /* largest  packet size          */
    float avg;        /* mean packet size              */
    float std;        /* standard deviation            */
    float iat;        /* mean Inter-Arrival-Time (µs)  */
    float variance;   /* variance of packet size       */
} __rte_cache_aligned;


#define ML_VEC_DATA  RTE_ALIGN_CEIL(sizeof(struct ml_input_features), \
                                    RTE_CACHE_LINE_SIZE)

#define MODEL_IN_BYTES (sizeof(struct ml_input_features))

/* Packet feature structure for ML processing */

struct packet_features {
    // Header info
    uint8_t header_length;
    uint8_t protocol_type;
    
    // TCP Flags (packed into one byte)
    struct {
        uint8_t fin:1;
        uint8_t syn:1;
        uint8_t rst:1;
        uint8_t psh:1;
        uint8_t ack:1;
        uint8_t ece:1;
        uint8_t cwr:1;
        uint8_t reserved:1;
    } tcp_flags;
    
    // Protocol indicators (packed into three bytes)
    struct {
        uint16_t http:1;
        uint16_t https:1;
        uint16_t dns:1;
        uint16_t telnet:1;
        uint16_t smtp:1;
        uint16_t ssh:1;
        uint16_t irc:1;
        uint16_t tcp:1;
        uint16_t udp:1;
        uint16_t dhcp:1;
        uint16_t arp:1;
        uint16_t icmp:1;
        uint16_t ipv4:1;
        uint16_t llc:1;
        uint16_t reserved:2;
    } protocols;
} __rte_cache_aligned;


#define APP_ML_METADATA_OFFSET 0
#define KEY_LEN 16

struct app_params {
	/* CPU cores */
	uint32_t core_rx;
	uint32_t core_worker;
	uint32_t core_tx;
	uint32_t core_ml;

	/* Ports*/
	uint32_t ports[APP_MAX_PORTS];  //array of DPDK port IDs(NIC)
	uint32_t n_ports;  //len of ports
	uint32_t port_rx_ring_size;
	uint32_t port_tx_ring_size;

	/* Rings */
	struct rte_ring *rings_rx[APP_MAX_PORTS];  //pointers to rings used to pass packets between cores
	struct rte_ring *rings_tx[APP_MAX_PORTS];
	uint32_t ring_rx_size;
	uint32_t ring_tx_size;

	/* Internal buffers */
	struct app_mbuf_array mbuf_rx;  //buffer for packets received from NIC
	struct app_mbuf_array mbuf_tx[APP_MAX_PORTS]; //buffer for packets to be transmitted to NIC

	/* Buffer pool */
	struct rte_mempool *pool;
	uint32_t pool_buffer_size;
	uint32_t pool_size;
	uint32_t pool_cache_size;

	/* Burst sizes */
	uint32_t burst_size_rx_read;
	uint32_t burst_size_rx_write;
	uint32_t burst_size_worker_read;
	uint32_t burst_size_worker_write;
	uint32_t burst_size_tx_read;
	uint32_t burst_size_tx_write;

	/* App behavior */
	uint32_t pipeline_type;
} __rte_cache_aligned;

extern struct app_params app;

struct flow_ctx {
    /* running counters ------------------------------------------------ */
    uint32_t pkt_cnt;               /* N                                */
    uint64_t first_tsc, last_tsc;   /* timestamps                       */

    /* ❶ - Packet size statistics */
    uint64_t byte_cnt;              /* Σ pkt_len                        */
    uint64_t size_sq;               /* Σ (pkt_len)²   for std / var     */
    uint16_t size_min, size_max;    /* min / max pkt_len                */

    /* ❷ - Header / TTL */
    uint32_t hdr_len_sum;           /* Σ header_length                  */
    uint32_t ttl_sum;               /* Σ ttl                            */

    /* ❸ - IAT */
    uint64_t iat_sum;               /* Σ inter-arrival (TSC)            */

    /* ❹ - TCP flag counters */
    uint32_t fin_cnt, syn_cnt, rst_cnt, psh_cnt, ack_cnt;

    /* ❺ - Protocol counters */
    uint16_t http_cnt, https_cnt, dns_cnt, ssh_cnt;
    uint16_t tcp_cnt,  udp_cnt,  arp_cnt,  icmp_cnt;

    /* hand-shake */
	uint8_t  signature_key[KEY_LEN];
    uint8_t  inflight;
    uint8_t  verdict;               /* optional for stats               */

    uint64_t t_first_pkt;   /* when the first packet of the 100-pkt window arrived */
    uint64_t t_rule_ready;  /* when the DROP / PASS rule was installed             */
} __rte_cache_aligned;


extern struct rte_pipeline *pipeline_core1;   /* used by ML core   */
extern uint32_t             class_table_id;   /* used by ML core   */
extern void                *class_tbl_core1;  /* used by RX core   */

extern double tsc_to_sec;   /* 1 / rte_get_tsc_hz()  */
extern double tsc_to_usec;  /* tsc_to_sec * 1e6      */


extern uint32_t mt_infer_id;
extern uint32_t mt_total_id;     // Metrics IDs for ML inference and total cycles



extern struct rte_hash      *flow_ht;    // Flow hash table
extern struct flow_ctx     **flow_pool;  // Array of flow_ctx pointers
extern struct rte_mempool   *ctx_mp;     // Flow context mempool

extern struct rte_ring      *quar_ring;  // Quarantine ring

extern struct rte_mempool   *vec_mp;     // ML vector mempool

extern struct rte_ring *ml_ring; //ring for ML model inference
//extern struct rte_mempool *ml_op_pool; // ML operations pool

extern bool force_quit;

int app_parse_args(int argc, char **argv);
void app_print_usage(void);
void app_init(void);
int app_lcore_main_loop(void *arg);

/* Pipeline */
enum {
	e_APP_PIPELINE_NONE = 0,
	e_APP_PIPELINE_STUB,

	e_APP_PIPELINE_HASH_KEY8_EXT,
	e_APP_PIPELINE_HASH_KEY8_LRU,
	e_APP_PIPELINE_HASH_KEY16_EXT,
	e_APP_PIPELINE_HASH_KEY16_LRU,
	e_APP_PIPELINE_HASH_KEY32_EXT,
	e_APP_PIPELINE_HASH_KEY32_LRU,

	e_APP_PIPELINE_HASH_SPEC_KEY8_EXT,
	e_APP_PIPELINE_HASH_SPEC_KEY8_LRU,
	e_APP_PIPELINE_HASH_SPEC_KEY16_EXT,
	e_APP_PIPELINE_HASH_SPEC_KEY16_LRU,
	e_APP_PIPELINE_HASH_SPEC_KEY32_EXT,
	e_APP_PIPELINE_HASH_SPEC_KEY32_LRU,

	e_APP_PIPELINE_ACL,
	e_APP_PIPELINE_LPM,
	e_APP_PIPELINE_LPM_IPV6,

	e_APP_PIPELINE_HASH_CUCKOO_KEY8,
	e_APP_PIPELINE_HASH_CUCKOO_KEY16,
	e_APP_PIPELINE_HASH_CUCKOO_KEY32,
	e_APP_PIPELINE_HASH_CUCKOO_KEY48,
	e_APP_PIPELINE_HASH_CUCKOO_KEY64,
	e_APP_PIPELINE_HASH_CUCKOO_KEY80,
	e_APP_PIPELINE_HASH_CUCKOO_KEY96,
	e_APP_PIPELINE_HASH_CUCKOO_KEY112,
	e_APP_PIPELINE_HASH_CUCKOO_KEY128,
	e_APP_PIPELINES
};

void app_main_loop_rx(void);
void app_main_loop_rx_metadata(void);
uint64_t test_hash(void *key,
	void *key_mask,
	uint32_t key_size,
	uint64_t seed);

uint32_t test_hash_cuckoo(const void *key,
	uint32_t key_size,
	uint32_t seed);




void app_main_loop_worker(void);
void app_main_loop_worker_pipeline_stub(void);

void app_main_loop_worker_pipeline_hash(void);
void app_main_loop_worker_ml(void);


void app_main_loop_worker_pipeline_acl(void);
void app_main_loop_worker_pipeline_lpm(void);
void app_main_loop_worker_pipeline_lpm_ipv6(void);

void app_main_loop_tx(void);

#define APP_FLUSH 0
#ifndef APP_FLUSH
#define APP_FLUSH 0x3FF
#endif

#define APP_METADATA_OFFSET(offset) (sizeof(struct rte_mbuf) + (offset))

#endif /* _MAIN_H_ */
