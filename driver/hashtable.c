#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/hash.h>

#include "util/hash.h"
#include "util/pair.h"
#include "hashtable.h"

hashtable_t *hashtable_init(size_t num_entries) 
{
	int i;
	hashtable_t *ht;

	ht = kzalloc(sizeof(hashtable_t), GFP_KERNEL);
	if (!ht) {
		pr_err("failed to allocate hashtable");
		goto out_err;
	}

	ht->num_buckets = num_entries;
	ht->hlistheads = vzalloc(sizeof(struct hlist_head) * ht->num_buckets);
	if (!ht->hlistheads) {
		pr_err("%s: failed to allocate hlistheads", __func__);
		goto out_err2;
	}

	ht->rw_locks = kvzalloc(sizeof(rwlock_t) * ht->num_buckets, GFP_KERNEL);
	if (!ht->rw_locks) {
		pr_err("%s: failed to allocate rw_locks", __func__);
		goto out_err3;
	}

	for (i = 0; i < ht->num_buckets; i++) {
		rwlock_init(&ht->rw_locks[i]);
        }

	pr_info("%s: num_buckets=%lu", __func__, ht->num_buckets);

	return ht;

out_err3:
	vfree(ht->hlistheads);
out_err2:
	kfree(ht);
out_err:
	return NULL;
}

void hashtable_exit(hashtable_t *ht) 
{
	vfree(ht->hlistheads);
	kvfree(ht->rw_locks);
	kfree(ht);
}

static inline u32 get_bkt_id(hashtable_t *ht, u64 key) 
{
	return hash_funcs[0](&key, sizeof(u64), f_seed) % ht->num_buckets;
}

// XXX: failure handling
inline ht_node_t *allocate_node(u64 key, u64 value) 
{
	ht_node_t *item = (struct ht_node_t *) kzalloc(sizeof(struct ht_node_t),
			GFP_ATOMIC);
	item->key = key;
	item->value = value; 
	return item;
}

bool hashtable_update(hashtable_t *ht, u64 key, u64 value) 
{
	struct ht_node_t *cur;
	u32 bkt_id;
	unsigned long flags;
        bool ret = false;

	bkt_id = get_bkt_id(ht, key);
	
	write_lock_irqsave(&ht->rw_locks[bkt_id], flags);
        hlist_for_each_entry(cur, &ht->hlistheads[bkt_id], hnode) {
                if (cur->key == key) {
                        /* prevent to overwrite bypass entry */
                        if (cur->value != BYPASS) {
                                cur->value = value;
                        }
                        ret = true;
                        break;
                }
        }
	write_unlock_irqrestore(&ht->rw_locks[bkt_id], flags);

        return ret;
}

void hashtable_insert(hashtable_t *ht, u64 key, u64 value) 
{
	struct ht_node_t *item;
	u32 bkt_id;
	unsigned long flags;

        if (hashtable_update(ht, key, BYPASS)) {
                value = BYPASS;
        }

	bkt_id = get_bkt_id(ht, key);
	
	item = allocate_node(key, value);
 
	write_lock_irqsave(&ht->rw_locks[bkt_id], flags);
	hlist_add_head(&item->hnode, &ht->hlistheads[bkt_id]);
	write_unlock_irqrestore(&ht->rw_locks[bkt_id], flags);
}

u64 hashtable_get(hashtable_t *ht, u64 key) 
{
	struct ht_node_t *cur;
	u32 bkt_id;
	u64 value = -1;
	unsigned long flags;

	bkt_id = get_bkt_id(ht, key);
	
	read_lock_irqsave(&ht->rw_locks[bkt_id], flags);
	hlist_for_each_entry(cur, &ht->hlistheads[bkt_id], hnode) {
		if (cur->key == key) {
			value = cur->value;
			break;
		}
	}
	read_unlock_irqrestore(&ht->rw_locks[bkt_id], flags);

	return value;
}

u64 hashtable_remove(hashtable_t *ht, u64 key) {
	struct ht_node_t *cur;
	struct hlist_node *h_tmp;
	u32 bkt_id;
	u64 value = -1;
	unsigned long flags;

	bkt_id = get_bkt_id(ht, key);

	write_lock_irqsave(&ht->rw_locks[bkt_id], flags);
	hlist_for_each_entry_safe(cur, h_tmp, &ht->hlistheads[bkt_id], hnode) {
		if (cur->key == key) {
			value = cur->value;
			cur->key = -1;
			hlist_del(&cur->hnode);
			kfree(cur);
			break;
		}
	}
	write_unlock_irqrestore(&ht->rw_locks[bkt_id], flags);

	return value;
}

void hashtable_print(hashtable_t *ht) 
{
	int i;
	struct ht_node_t *cur;
	struct hlist_node *h_tmp;

	pr_info("%s", __func__);

	for (i = 0; i < ht->num_buckets; i++) {
		pr_info("[ Bkt%d ]: ", i);
		hlist_for_each_entry_safe(cur, h_tmp, &ht->hlistheads[i], 
				hnode) {
			printk(KERN_CONT "%llu ", cur->key);
		}
		pr_info("");
	}
}
