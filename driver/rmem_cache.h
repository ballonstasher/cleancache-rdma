#ifndef __RMEM_CACHE_H__
#define __RMEM_CACHE_H__

#include <linux/types.h>

#include "util/hash.h"
#include "util/pair.h"

typedef spinlock_t cache_lock_t;

typedef struct node_t {
	Key_t key;
	Value_t value;
	bool busy;
	char pad[3];
	struct hlist_node hnode; // hash list
	struct list_head list; // lru list
} node_t;

typedef struct rmem_cache_t {
	size_t capacity;
	size_t num_buckets;
	size_t size;

	struct hlist_head *hlistheads;
	struct list_head listhead;

	cache_lock_t lock;     
} rmem_cache_t;

rmem_cache_t *rmem_cache_init(size_t);
void rmem_cache_exit(rmem_cache_t *);

void rmem_cache_insert(rmem_cache_t *, Key_t, Value_t, 
		Key_t *evicted_key, Value_t *evicted_value);
Value_t rmem_cache_get(rmem_cache_t *, Key_t);
Value_t rmem_cache_get_with_flag(rmem_cache_t *, Key_t, bool);
Value_t rmem_cache_remove(rmem_cache_t *cache, Key_t);
Value_t rmem_cache_evict(rmem_cache_t *cache, Key_t *);

#endif // __RMEM_CACHE_H__
