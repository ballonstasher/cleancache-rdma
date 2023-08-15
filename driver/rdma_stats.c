#include <linux/types.h>

#include "rdma_stats.h"

struct dcc_rdma_stats_t dcc_rdma_stats = {};

const char *dcc_rdma_stats_names[] = {
	"steering_storage",
	"steering_memory",
	
        "steering_to_mn",
};
