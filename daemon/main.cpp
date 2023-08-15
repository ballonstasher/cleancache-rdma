#include <iostream>
#include <gflags/gflags.h>

#include "rdma.hpp"
#include "kv.hpp"
#include "config.hpp"

using namespace std;

DEFINE_uint32(tcp_port, 7777, "tcp port for connection");
DEFINE_uint32(num_clients, 1, "num of clients");
DEFINE_bool(exclusive_policy, true, "cache inclusion policy");
DEFINE_int64(mem_pool_size_mb, 4096, "memory pool size in MB");
DEFINE_string(wc_mode, "busy_waiting", "work completion mode (busy_waiting, ...)");

struct daemon_config_t config;
struct dcc_clients **clients;
KV *kv;

void show_stats() {
    while (1) {
        sleep(5);
        rdma_show_stats();
        kv->ShowStats();
    }
}

void set_default_config() {
    config.tcp_port = FLAGS_tcp_port;
    config.num_clients = FLAGS_num_clients;
    config.exclusive_policy = FLAGS_exclusive_policy;
    config.mem_pool_size = FLAGS_mem_pool_size_mb << 20;
    config.ht_type = HT_LP_APPROX;
    config.num_ht_shards = 1;
    config.cbf_on = 1;
    config.wc_mode = __wc_mode(FLAGS_wc_mode); // HP_BACKOFF;
    if (config.wc_mode == WC_BUSY_WAITING)  
        config.poller_type = GLOBAL_QP_POLLER;
    else 
        config.poller_type = PER_QP_POLLER;
}

void print_config() {
    printf("------------------- Config -------------------\n");
    printf("tcp_port=%d, num_clients=%d\n", 
            config.tcp_port, config.num_clients);
    printf("exclusive_policy=%d\n", config.exclusive_policy);
    printf("mem_pool_size(MB)=%lu\n", config.mem_pool_size >> 20);
    printf("ht_type=%d, num_ht_shards=%d\n", 
            config.ht_type, config.num_ht_shards);
    printf("cbf_on=%d\n", config.cbf_on);
    printf("wc_mode=%d\n", config.wc_mode);  
    printf("poller_type=%d\n", config.poller_type);
    printf("----------------------------------------------\n");
}

int main(int argc, char **argv) {
    google::SetUsageMessage("some usage message");
    google::ParseCommandLineFlags(&argc, &argv, true);

    set_default_config();

    print_config();

    srand(time(NULL));

    clients = (struct dcc_clients **) malloc(sizeof(struct dcc_clients *) *
            config.num_clients);

    for (int i = 0; i < config.num_clients; i++) {
        clients[i] = (struct dcc_clients *) malloc(sizeof(struct dcc_clients));
        if (!clients[i]) {
            fprintf(stderr, "failed to allocate clients");
            return -1;
        }

        clients[i]->ctrl = init_rdma(i);
        if (!clients[i]->ctrl) {
            fprintf(stderr, "Unable to initialize rdma subsystem\n");
            return -1;
        }

        /* kv is single object for twosided type */
        if (i == 0) {
            kv = new KV(clients[i]->ctrl->mm->GetDataMMPool()->ptr);
        }
    }

    run_rdma(config.tcp_port, config.num_clients);

    thread indicator = thread(show_stats);
    indicator.detach();

    exit_rdma(); // XXX: anyway exit away...

    for (int i = 0; i < config.num_clients; i++)
        free(clients[i]);
    free(clients);

    google::ShutDownCommandLineFlags();

    return 0;
}
