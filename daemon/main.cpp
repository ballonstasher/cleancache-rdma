#include <iostream>
#include <gflags/gflags.h>

#include "rdma.hpp"
#include "config.hpp"

using namespace std;

DEFINE_uint32(tcp_port, 7777, "tcp port for connection");
DEFINE_uint32(num_clients, 1, "num of clients");
DEFINE_bool(exclusive_policy, true, "cache inclusion policy");
DEFINE_int64(mem_pool_size_mb, 4096, "memory pool size in MB");

struct daemon_config_t config;
struct dcc_clients **clients;


void set_default_config() {
    config.tcp_port = FLAGS_tcp_port;
    config.num_clients = FLAGS_num_clients;
    config.exclusive_policy = FLAGS_exclusive_policy;
    config.mem_pool_size = FLAGS_mem_pool_size_mb << 20;
}

void print_config() {
    printf("------------------- Config -------------------\n");
    printf("tcp_port=%d, num_clients=%d\n", 
            config.tcp_port, config.num_clients);
    printf("exclusive_policy=%d\n", config.exclusive_policy);
    printf("mem_pool_size(MB)=%lu\n", config.mem_pool_size >> 20);
    printf("----------------------------------------------\n");
}

int main(int argc, char **argv) {
    google::SetUsageMessage("some usage message");
    google::ParseCommandLineFlags(&argc, &argv, true);

    srand(time(NULL));
    
    set_default_config();

    print_config();

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
    }

    run_rdma(config.tcp_port, config.num_clients);

    exit_rdma(); // XXX: anyway exit away...

    for (int i = 0; i < config.num_clients; i++)
        free(clients[i]);
    free(clients);

    google::ShutDownCommandLineFlags();

    return 0;
}
