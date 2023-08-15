#include <iostream>
#include <thread>

#include "stats.hpp"

using namespace std;

struct timespec start_ts[NUM_RDMA_STATS];
uint64_t elapsed_stats[NUM_RDMA_STATS];

static inline int get_num_stats(int type) {
    switch (type) {
        case RDMA_STATS:
            return NUM_RDMA_STATS;
        case KV_STATS:
            return NUM_KV_STATS;
        default:
            fprintf(stderr, "%s: unknown stat types=%d\n", 
                    __func__, type);
    }
    return -1;
} 

Stats::Stats(int stats_type) {
    num_stats = get_num_stats(stats_type);
    num_cpus = thread::hardware_concurrency();

    cnts = new uint64_t *[num_stats];
    for (int j = 0; j < num_stats; j++) {
        cnts[j] = new uint64_t[num_cpus];
    }
}

Stats::~Stats() {
    for (int i = 0; i < num_stats; i++) { 
        delete cnts[i];
    }
    delete[] cnts;
}
