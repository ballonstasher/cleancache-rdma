#ifndef __RDMA_STATS_H__
#define __RDMA_STATS_H__

#include <linux/types.h>

enum dcc_rdma_stats_type {
    STEERING_STORAGE,
    STEERING_MEMORY,
    
    STEERING_TO_MN,
	RDMA_ST_MAX
};

struct dcc_rdma_stats_t {
	u64 cnts[RDMA_ST_MAX];
};

extern struct dcc_rdma_stats_t dcc_rdma_stats;
extern const char *dcc_rdma_stats_names[];

static inline void dcc_rdma_stats_inc(int name) 
{
	dcc_rdma_stats.cnts[name]++;
}

static inline u64 dcc_rdma_stats_get(int name) 
{
	return dcc_rdma_stats.cnts[name];
}

#endif // __RDMA_STATS_H__
