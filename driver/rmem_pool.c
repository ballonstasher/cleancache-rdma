#include <linux/slab.h>

#include "rmem_pool.h"

rmem_pool_t *rmem_pool_init(size_t num_pages) 
{
	rmem_pool_t *pool;
	int64_t i;
	
	pool = kzalloc(sizeof(rmem_pool_t), GFP_KERNEL);
	if (!pool) {
		pr_err("failed to allocate pool");
		goto out_error;
	}

	pool->freelist = stack_init(num_pages);
	if (!pool->freelist) {
		pr_err("failed to allocate freelist");
		goto out_error2;
	}

	for (i = num_pages - 1; i >= 0; i--)
		stack_push(pool->freelist, (u64)i << PAGE_SHIFT);

	pool->size = num_pages;

	return pool;

out_error2:
	kfree(pool);
out_error:
	return NULL;
}

void rmem_pool_exit(rmem_pool_t *pool) 
{
	stack_exit(pool->freelist);
	kfree(pool);
}
