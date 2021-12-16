#include <signal.h>
#include <stdbool.h>
#include <getopt.h>

#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_common.h>
#include <rte_config.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_ether.h>

#define APP "dpu_fwd"

uint32_t DPU_FWD_LOG_LEVEL = RTE_LOG_DEBUG;

#define MAX_PKT_BURST 32
#define MEMPOOL_CACHE_SIZE 128

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct rte_ether_addr dpu_fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

int RTE_LOGTYPE_DPU_FWD;

struct rte_mempool *dpu_fwd_pktmbuf_pool = NULL;

static volatile bool force_quit;

static struct rte_eth_conf port_conf = {
    .rxmode = {
        .split_hdr_size = 0,
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

/* statistics struct */
struct dpu_fwd_stat {
    uint64_t recv;
    uint64_t sent;
} __rte_cache_aligned;

pthread_spinlock_t stat_spinlock;
struct dpu_fwd_stat stat_global;

static inline void initlize_statistics(struct dpu_fwd_stat *stat)
{
    stat->recv = 0;
    stat->sent = 0;
}

static inline void destroy_statistics(struct dpu_fwd_stat *stat)
{
}

static inline void print_statistics(struct dpu_fwd_stat *stat)
{
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_DPU_FWD,
            "==== dpu_fwd statistics ====\n"
            "recv %" PRIu64 " packets\n"
            "sent %" PRIu64 " packets\n"
            "============================\n",
            stat->recv, stat->sent);
}

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        rte_log(RTE_LOG_INFO, RTE_LOGTYPE_DPU_FWD, "\n\nSignal %d received, preparing to exit...\n", signum);
        force_quit = true;
    }
}

void init_port(int portid) {
    int ret;
    struct rte_eth_rxconf rxq_conf;
    struct rte_eth_txconf txq_conf;
    struct rte_eth_conf local_port_conf = port_conf;
    struct rte_eth_dev_info dev_info;

    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_DPU_FWD, "Initializing port %u...\n", portid);
    fflush(stdout);
    rte_eth_dev_info_get(portid, &dev_info);
    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        local_port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;

    ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
                 ret, portid);

    ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot adjust number of descriptors: err=%d, "
                               "port=%u\n", ret, portid);

    ret = rte_eth_macaddr_get(portid,
                              &dpu_fwd_ports_eth_addr[portid]);
    if (ret < 0)
      rte_exit(EXIT_FAILURE,
               "Cannot get MAC address: err=%d, port=%u\n",
               ret, portid);

    /* init one RX queue */
    fflush(stdout);
    rxq_conf = dev_info.default_rxconf;

    rxq_conf.offloads = local_port_conf.rxmode.offloads;
    ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
                                 rte_eth_dev_socket_id(portid),
                                 &rxq_conf, dpu_fwd_pktmbuf_pool);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
                 ret, portid);

    /* init one TX queue on each port */
    fflush(stdout);
    txq_conf = dev_info.default_txconf;
    txq_conf.offloads = local_port_conf.txmode.offloads;
    ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
                                 rte_eth_dev_socket_id(portid),
                                 &txq_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
                 ret, portid);

    /* Start device */
    ret = rte_eth_dev_start(portid);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
                 ret, portid);

    ret = rte_eth_promiscuous_enable(portid);
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "rte_eth_promiscuous_enable:err=%s, port=%u\n",
                 rte_strerror(-ret), portid);

    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_DPU_FWD,
            "Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
           portid,
           dpu_fwd_ports_eth_addr[portid].addr_bytes[0],
           dpu_fwd_ports_eth_addr[portid].addr_bytes[1],
           dpu_fwd_ports_eth_addr[portid].addr_bytes[2],
           dpu_fwd_ports_eth_addr[portid].addr_bytes[3],
           dpu_fwd_ports_eth_addr[portid].addr_bytes[4],
           dpu_fwd_ports_eth_addr[portid].addr_bytes[5]);
}

/* main ping loop */
static void dpu_fwd_main_loop(int portid)
{
    unsigned lcore_id;
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    struct dpu_fwd_stat stat_local;
    initlize_statistics(&stat_local);

    lcore_id = rte_lcore_id();

    if (rte_eth_dev_socket_id(portid) >= 0 &&
        rte_eth_dev_socket_id(portid) !=
        (int)rte_socket_id())
        rte_log(RTE_LOG_INFO, RTE_LOGTYPE_DPU_FWD,
                "lcore %d WARNING: port %u is on remote NUMA node to "
                "polling thread.\n\tPerformance will not be optimal.\n",
                lcore_id, portid);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_DPU_FWD, "entering dpu_fwd loop for port "
                                               "%d on lcore %u\n",
                                               portid, lcore_id);

    /* wait for message */
    while (!force_quit)
    {
        uint16_t nb_rx = rte_eth_rx_burst(portid, 0, pkts_burst,
                                          MAX_PKT_BURST);
        if (nb_rx) {
            stat_local.recv += nb_rx;

            /* Send burst of TX packets, to second port of pair. */
            const uint16_t nb_tx = rte_eth_tx_burst(portid ^ 1, 0, pkts_burst,
                                                    nb_rx);
            stat_local.sent += nb_tx;

            /* Free any unsent packets. */
            if (unlikely(nb_tx < nb_rx)) {
                uint16_t buf;
                for (buf = nb_tx; buf < nb_rx; buf++)
                    rte_pktmbuf_free(pkts_burst[buf]);
            }
        }
    }
    /* print port statistics when ping main loop finishes */
    pthread_spin_lock(&stat_spinlock);
    stat_global.recv += stat_local.recv;
    stat_global.sent += stat_local.sent;
    pthread_spin_unlock(&stat_spinlock);
    destroy_statistics(&stat_local);
}

struct lcore_worker_args {
    uint32_t portid;
};

static int dpu_fwd_launch_one_lcore(void *args)
{
    struct lcore_worker_args *worker_args = args;

    dpu_fwd_main_loop(worker_args->portid);
    return 0;
}

int main(int argc, char **argv)
{
    int ret;
    uint16_t nb_ports;
    int nb_lcores;
    int nb_sockets;
    unsigned int nb_mbufs;

    initlize_statistics(&stat_global);
    pthread_spin_init(&stat_spinlock, 0);

    /* init EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    argc -= ret;
    argv += ret;

    /* init log */
    RTE_LOGTYPE_DPU_FWD = rte_log_register(APP);
    ret = rte_log_set_level(RTE_LOGTYPE_DPU_FWD, DPU_FWD_LOG_LEVEL);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Set log level to %u failed\n", DPU_FWD_LOG_LEVEL);

    /* get system information */
    nb_sockets = rte_socket_count();
    nb_lcores = rte_lcore_count();
    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No Ethernet ports, bye...\n");

    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_DPU_FWD, "%d socket(s) %d lcore(s) "
                                                "%u port(s) detected\n",
                                                nb_sockets, nb_lcores, nb_ports);

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    nb_mbufs = RTE_MAX((unsigned int)(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST + MEMPOOL_CACHE_SIZE)), 8192U);
    dpu_fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
                                                    MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                                    rte_socket_id());
    if (dpu_fwd_pktmbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

    /* init port */
    int portid;
    int portids[RTE_MAX_ETHPORTS]; // port id may be non-contiguous
    int idx = 0;
    RTE_ETH_FOREACH_DEV(portid) {
        portids[idx++] = portid;
        init_port(portid);
    }

    struct lcore_worker_args worker_args;
    unsigned int lcore_id;
    idx = 1;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (idx < nb_ports)
            worker_args.portid = portids[idx++];
        else
            break;
        rte_eal_remote_launch(dpu_fwd_launch_one_lcore, &worker_args, lcore_id);
    }

    worker_args.portid = portids[0];
    dpu_fwd_launch_one_lcore(&worker_args);

    rte_eal_mp_wait_lcore();

    rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_DPU_FWD, "Bye.\n");
    rte_eal_cleanup();

    print_statistics(&stat_global);
    pthread_spin_destroy(&stat_spinlock);
    destroy_statistics(&stat_global);
    return 0;
}