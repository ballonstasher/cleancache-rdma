#include <linux/slab.h>

#include "counting_bloom_filter.h"
#include "../util/hash.h"

cbf_t *cbf_init(u8 num_hash, unsigned int num_bits) 
{
	cbf_t *filter;
	
	filter = (cbf_t *)kzalloc(sizeof(*filter), GFP_KERNEL);
	if (!filter) {
		pr_err("failed to alocate cbf");
		goto out_err;
	}

	filter->num_hash    = num_hash;
	filter->num_bits    = num_bits;
	filter->bitmap_size = num_bits * sizeof(unsigned long);
	// alloc size: 1 B x numBits
	filter->byte_bitmap = (u8 *)vzalloc(filter->bitmap_size);
	if (!filter->byte_bitmap) {
		pr_err("failed to allocate cbf's byte bitmap");
		goto out_err2;
	}
	rwlock_init(&filter->rw_lock);

	pr_info("Counting BF, nr_hash=%u, num_bits=%u, bitmap_size(KB)=%lu", 
			num_hash, num_bits, filter->bitmap_size/1024);

	return filter;

out_err2:
	kfree(filter);
out_err:
	return NULL;
}

void cbf_exit(cbf_t *filter) 
{
	vfree(filter->byte_bitmap);
	kfree(filter);
}

static inline unsigned int compute_hash(cbf_t *filter, unsigned int i, 
		const void *data, unsigned int data_len) 
{
	return hash_funcs[i](data, data_len, f_seed) % filter->bitmap_size;
}

void cbf_add(cbf_t *filter, const void *data, unsigned int data_len) 
{
	unsigned int i, index;
	unsigned long flags;

	write_lock_irqsave(&filter->rw_lock, flags);
	for (i = 0 ; i < filter->num_hash; i++) {
		index = compute_hash(filter, i, data, data_len);
		if (filter->byte_bitmap[index] < 255) {
			filter->byte_bitmap[index]++;
		} else {
			//pr_err("index: %u exceeds 255 count", index);
		}
	}
	write_unlock_irqrestore(&filter->rw_lock, flags);
}

bool __cbf_might_contain(cbf_t *filter, const void *data, unsigned int data_len) {
	unsigned int i, index;
	int ret = 1;

	for (i = 0; i < filter->num_hash; i++) {
		index = compute_hash(filter, i, data, data_len);
		if (!filter->byte_bitmap[index]) {
			ret = 0;
			break;
		}
	}
	return ret;
}

bool cbf_might_contain(cbf_t *filter, const void *data, unsigned int data_len) {
	int ret;
	unsigned long flags;

	read_lock_irqsave(&filter->rw_lock, flags);
	ret = __cbf_might_contain(filter, data, data_len);
	read_unlock_irqrestore(&filter->rw_lock, flags);
	
	return ret;
}

bool cbf_remove(cbf_t *filter, const void *data, unsigned int data_len) {
	unsigned int i, index;
	int ret = 0;
	unsigned long flags;

	write_lock_irqsave(&filter->rw_lock, flags);
	if (__cbf_might_contain(filter, data, data_len)) {
		for (i = 0; i < filter->num_hash; i++) {
			index = compute_hash(filter, i, data, data_len);
			if (filter->byte_bitmap[index] >= 0)
				filter->byte_bitmap[index]--;
		}
		ret = 1;
	}
	write_unlock_irqrestore(&filter->rw_lock, flags);

	return ret;
}
