#ifndef __STATS_HPP__
#define __STATS_HPP__

#include <cinttypes>

enum stats_type {
    RDMA_STATS,
    KV_STATS,

    NUM_STATS_TYPE,
};

enum kv_stats_t {
    KV_SUCC_PUT,
    KV_FAILED_PUT,
    KV_UPDATED_PUT,
    KV_EVICTED_PUT,
    KV_SUCC_GET,
    KV_FAILED_GET,
    KV_SUCC_INV,
    KV_FAILED_INV,

    KV_EVICTED_BG,

    NUM_KV_STATS,
};

enum rdma_stats_t {
    SVR_PUT,
    SVR_GET,
    SVR_INV,

    RDMA_EVENT_COMP,
    RDMA_SLEEP_WAIT_COMP,
    RDMA_POLL_COMP,

    NUM_RDMA_STATS,
};

class Stats {
    public:
        Stats(int);
        ~Stats();

#ifdef DCC_COUNT_CHECK
        void Inc(int stats_type, int cpu) {
            //printf("%d %d %d\n", stats_type, stats_cnt_type, cpu);
            cnts[stats_type][cpu]++; 
        }
#else
        void Inc(int stats_type, int cpu) {}
#endif
        uint64_t Sum(int stats_type) {
            uint64_t sum = 0;
            for (int i = 0; i < num_cpus; i++)
                sum += cnts[stats_type][i]; 
            return sum;
        }

    private:
        int num_cpus;
        int num_stats;
        uint64_t **cnts;
};

#endif // __STATS_HPP__
