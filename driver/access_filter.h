#ifndef __ACCESS_FILTER_H__
#define __ACCESS_FILTER_H__

#include <linux/spinlock.h>
#include <linux/hash.h>

struct af_bucket {
	ino_t ino;
	pgoff_t index;
};

typedef struct access_filter_t {
	size_t num_buckets;
	size_t lock_size;
	int num_locks;
	spinlock_t *locks;
	struct af_bucket *buckets;
} access_filter_t; 

static inline access_filter_t *af_init(void) 
{
	int i;
	access_filter_t *af;

	af = kzalloc(sizeof(access_filter_t), GFP_KERNEL);
	if (!af) {
		pr_err("failed to allocate access_filter_t");
		goto out_err;
	}

	af->num_buckets = 4096;
	af->buckets = kzalloc(sizeof(struct af_bucket) * af->num_buckets,
			GFP_KERNEL);
	if (!af->buckets) {
		pr_err("%s: failed to allocate buckets", __func__);
		goto out_err2;
	}

	af->lock_size = 16;
	af->num_locks = af->num_buckets / af->lock_size + 1;
	af->locks = kzalloc(sizeof(spinlock_t) * af->num_locks, GFP_KERNEL);
	if (!af->locks) {
		pr_err("%s: failed to allocate locks", __func__);
		goto out_err3;
	}

	for (i = 0; i < af->num_locks; i++)
		spin_lock_init(&af->locks[i]);

	pr_info("%s: num_locks=%u, lock_size=%lu, num_buckets=%lu", 
			__func__, af->num_locks, af->lock_size, af->num_buckets);

	return af;

out_err3:
	kfree(af->buckets);
out_err2:
	kfree(af);
out_err:
	return NULL;
}

static inline void af_exit(access_filter_t *af) 
{
	kfree(af->buckets);
	kvfree(af->locks);
	kfree(af);
}

#define DIFF_ABS(X, Y) ((X) > (Y) ? (X) - (Y) : (Y) - (X))
static inline bool 
af_upsert(access_filter_t *af, ino_t ino, pgoff_t index)
{
	u32 bkt_id = (ino * GOLDEN_RATIO_64) % af->num_buckets;
	spinlock_t *lock = &af->locks[bkt_id / af->lock_size];
	struct af_bucket *bucket = &af->buckets[bkt_id];
	bool ret;
	
	spin_lock(lock);
	
	if (bucket->ino == ino) {
		/* cur access is very close before access, 32 (128 KB) */
		if (DIFF_ABS(bucket->index, index) < 32)
			ret = 0;
		else
			ret = 1;
	} else {
		/* collision evict */
		bucket->ino = ino;
		ret = 1;

	}
	bucket->index = index;
	
	spin_unlock(lock);

	return ret;
}

#endif // __ACCESS_FILTER_H__
