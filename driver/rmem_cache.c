#include <linux/slab.h>

#include "rmem_cache.h"

static struct kmem_cache *node_cache;

inline uint32_t get_bkt_id(rmem_cache_t *cache, Key_t key) {
	return hash_funcs[0](&key, sizeof(Key_t), f_seed) % cache->num_buckets;
}

rmem_cache_t *rmem_cache_init(size_t num_pages) {
	rmem_cache_t *cache;

	node_cache = kmem_cache_create("node_cache", sizeof(node_t), 0,
			SLAB_TEMPORARY | SLAB_HWCACHE_ALIGN, NULL);
	if (!node_cache) {
		pr_err("no memory for node_t cache allocation");
		goto error;
	}

	cache = (rmem_cache_t *)kzalloc(sizeof(rmem_cache_t), GFP_KERNEL);
	if (!cache) {
		pr_err("no memory for rmem cache");
		goto error2;
	}

	cache->size = 0;
	cache->capacity = num_pages;
	cache->num_buckets = num_pages * 70 / 100;

	cache->hlistheads = (struct hlist_head *)vzalloc(
			sizeof(struct hlist_head) * cache->num_buckets);
	if (!cache->hlistheads) {
		pr_err("no memory for rmem cache's hlistheads");
		goto error3;
	}

	/* rmem hash: initialize buckets by the list head */
	INIT_LIST_HEAD(&cache->listhead);
	spin_lock_init(&cache->lock);

	pr_info("%s: capacity=%lu, num_buckets=%lu", __func__, cache->capacity,
			cache->num_buckets);

	return cache;

error3:
	kfree(cache);
error2:
	kmem_cache_destroy(node_cache);
error:
	return NULL;
}

void rmem_cache_exit(rmem_cache_t *cache) 
{
	vfree(cache->hlistheads);
	kfree(cache);
	kmem_cache_destroy(node_cache);
}

void __evict(rmem_cache_t *cache, Key_t *key, Value_t *value) {
	node_t *victim_node = list_entry(cache->listhead.next, node_t, list); 

	BUG_ON(victim_node->key == INVALID);

	*value = victim_node->value; 

	list_del(&victim_node->list);

	/* delete hashtable node */
	hlist_del(&victim_node->hnode);
	cache->size--;

	kmem_cache_free(node_cache, victim_node);
}

static inline node_t *allocate_node(Key_t key, Value_t value) {
	node_t *new_node = kmem_cache_alloc(node_cache, GFP_ATOMIC);
	new_node->key = key;
	new_node->value = value;
	new_node->busy = false;
	return new_node;
}

void rmem_cache_insert(rmem_cache_t *cache, Key_t key, Value_t value, 
		Key_t *evicted_key, Value_t *evicted_value)
{
	node_t *new_node;
	uint32_t bkt_id;
	unsigned long flags;

	// remove duplicated key
	*evicted_value = rmem_cache_remove(cache, key);
	if (*evicted_value != INVALID) {
		pr_warn("key: %llu already exist", key);
		return;
	}

	spin_lock_irqsave(&cache->lock, flags);
	if (cache->size >= cache->capacity) {
		__evict(cache, evicted_key, evicted_value);
		spin_unlock_irqrestore(&cache->lock, flags);
		return;
	}
	/* add to the list tail: mru pos */
	new_node = allocate_node(key, value);
	list_add_tail(&new_node->list, &cache->listhead);

	/* add to hashtable */
	bkt_id = get_bkt_id(cache, key);
	hlist_add_head(&new_node->hnode, &cache->hlistheads[bkt_id]);
	cache->size++;

	spin_unlock_irqrestore(&cache->lock, flags);
}

Value_t rmem_cache_get(rmem_cache_t *cache, Key_t key) {
	node_t *target;
	struct hlist_node *cur;
	uint32_t bkt_id = get_bkt_id(cache, key);
	unsigned long flags;
	Value_t value = INVALID;

	spin_lock_irqsave(&cache->lock, flags);

	for (cur = cache->hlistheads[bkt_id].first; cur != NULL; 
			cur = cur->next) {
		target = container_of(cur, node_t, hnode);
		if (target->key == key) {
			value = target->value;
			break;
		}
	}

	if (value != INVALID) {
		// move_node_to_mru
		list_del(&target->list);
		list_add_tail(&target->list, &cache->listhead);
	}

	spin_unlock_irqrestore(&cache->lock, flags);

	return value;
}

Value_t rmem_cache_get_with_flag(rmem_cache_t *cache, Key_t key, bool busy) 
{
	node_t *target;
	struct hlist_node *cur;
	uint32_t bkt_id = get_bkt_id(cache, key);
	unsigned long flags;
	Value_t value = INVALID;

	spin_lock_irqsave(&cache->lock, flags);
	for (cur = cache->hlistheads[bkt_id].first; cur != NULL; 
			cur = cur->next) {
		target = container_of(cur, node_t, hnode);
		if (target->key == key) {
			if (busy) {
				target->busy = true;
				value = target->value;
			} else {
				target->busy = false;
			}
			break;
		}
	}

	if (target->busy && value != INVALID) {
		// move_node_to_mru
		list_del(&target->list);
		list_add_tail(&target->list, &cache->listhead);
	}
	spin_unlock_irqrestore(&cache->lock, flags);

	return value;
}

Value_t rmem_cache_remove(rmem_cache_t *cache, Key_t key) 
{
	node_t *target;
	struct hlist_node *cur;
	uint32_t bkt_id;
	unsigned long flags;
	Value_t value = INVALID;

RETRY: 
	spin_lock_irqsave(&cache->lock, flags);

	bkt_id = get_bkt_id(cache, key);

	for (cur = cache->hlistheads[bkt_id].first; cur != NULL; 
			cur = cur->next) {
		target = container_of(cur, node_t, hnode);
		if (target->key == key) {
			/* XXX: for inclusive policy */
			if (target->busy) {
				spin_unlock_irqrestore(&cache->lock, flags);
				goto RETRY;
			}
			value = target->value;
			target->key = INVALID;
			break;
		}
	}

	if (value != INVALID) {
		list_del(&target->list);
		hlist_del(&target->hnode);
		kmem_cache_free(node_cache, target);
		cache->size--;
	}

	spin_unlock_irqrestore(&cache->lock, flags);

	return value;
}

Value_t rmem_cache_evict(rmem_cache_t *cache, Key_t *key) {
	node_t *victim;
	Value_t value = INVALID;
	unsigned long flags;

RETRY:
	spin_lock_irqsave(&cache->lock, flags);

	victim = list_entry(cache->listhead.next, node_t, list); 

	/* XXX: for inclusive policy */
	if (victim->busy) {
		spin_unlock_irqrestore(&cache->lock, flags);
		goto RETRY; 
	}

	BUG_ON(victim->key == INVALID);

	value = victim->value;
	victim->key = INVALID;

	list_del(&victim->list);
	hlist_del(&victim->hnode);
	kmem_cache_free(node_cache, victim);
	cache->size--;

	spin_unlock_irqrestore(&cache->lock, flags);

	return value;   
}
