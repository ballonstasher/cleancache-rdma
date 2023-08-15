#include <linux/slab.h>

#include "rdma_mem.h"
#include "rdma.h"

extern bool dma_mem_for_filter;

struct rdma_mm *rdma_mem_init(void)
{
	struct rdma_mm *mm;

	mm = kzalloc(sizeof(struct rdma_mm), GFP_KERNEL);
	if (!mm) {
		pr_err("failed to allocate mm");
		return ERR_PTR(-ENOMEM);
	}

	return mm;
}

void rdma_mem_exit(struct rdma_mm *mm)
{
	if (mm)
		kfree(mm);	
}
