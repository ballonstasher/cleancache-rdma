#ifndef __BLOOM_FILTER_H__
#define __BLOOM_FILTER_H__

#include <linux/seqlock.h>
#include <linux/spinlock.h>
#include "../util/hash.h"

typedef struct bloom_filter {
	bool is_ordinary_bf;
	u8 num_hash;
	unsigned int num_bits;
	size_t bitmap_size;
	unsigned long *bitmap;
	unsigned long *tmp_bitmap;

	atomic_t merging;
} bf_t;


bf_t *bf_init(u8, unsigned int, void *);
void bf_exit(bf_t *);
void bf_add(bf_t *, const void *, unsigned int);
bool bf_might_contain(bf_t *, const void *, unsigned int);
bool bf_remove(bf_t *, const void *, unsigned int);


static inline uint32_t 
compute_hash(bf_t *filter, uint8_t i, const void *data, unsigned int len) 
{
	return hash_funcs[i](data, len, f_seed) % filter->num_bits;
}

static inline void 
calc_bitidx(uint32_t key_hash, uint32_t *long_idx, uint64_t *check_bit) 
{
	*long_idx = key_hash / 64;
	*check_bit = 1UL << (key_hash % 64);
}

static inline bool check_merging(bf_t *filter) 
{
	if (atomic_read(&filter->merging) > 0)
		return true;
	else
		return false;
}

static inline void bf_merge(bf_t *filter) 
{
	unsigned int i;

	for (i = 0; i < BITS_TO_LONGS(filter->num_bits); i++)
		filter->bitmap[i] |= filter->tmp_bitmap[i];
}

static inline void bf_blocking(bf_t *filter) 
{
	unsigned int i;
	
	for (i = 0; i < BITS_TO_LONGS(filter->num_bits); i++)
		filter->tmp_bitmap[i] = 0;

	atomic_set(&filter->merging, 1);
	//pr_err("%s", __func__);
}

static inline void bf_unblocking(bf_t *filter) 
{
	atomic_set(&filter->merging, 0);
	bf_merge(filter);
}

#endif /* __BLOOM_FILTER_H__ */
