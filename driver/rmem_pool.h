#ifndef __RMEMPOOL_H__
#define __RMEMPOOL_H__

#include <linux/types.h>

#include "util/fixed_stack.h"

typedef struct rmem_pool_t {
	fstack_t *freelist;
	size_t size;
} rmem_pool_t;

/*********************************************************************/

rmem_pool_t *rmem_pool_init(size_t size);
void rmem_pool_exit(rmem_pool_t *);
void rmem_pool_reset(rmem_pool_t *);

static inline u64 rmem_pool_alloc(rmem_pool_t *pool) 
{
	return stack_pop(pool->freelist); 
}

static inline int rmem_pool_free(rmem_pool_t *pool, u64 roffset) 
{
	return stack_push(pool->freelist, roffset); 
}

#endif // __RMEMPOOL_H__
