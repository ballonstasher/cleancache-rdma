#ifndef __HASHTABLE_H__
#define __HASHTABLE_H__

#include <linux/rwlock.h>
#include <linux/list.h>

typedef struct ht_node_t {
	u64 key;
	u64 value;
	struct hlist_node hnode;
} ht_node_t;

typedef struct hashtable_t {
	size_t num_buckets;
	size_t lock_size;
	int num_locks;
	rwlock_t *rw_locks;
	struct hlist_head *hlistheads;
} hashtable_t;

/*****************************************************************/

hashtable_t *hashtable_init(size_t);
void hashtable_exit(hashtable_t *);
void hashtable_insert(hashtable_t *, u64, u64);
u64 hashtable_get(hashtable_t *, u64);
u64 hashtable_remove(hashtable_t *, u64);
void hashtable_print(hashtable_t *);

#endif // __HASHTABLE_H__
