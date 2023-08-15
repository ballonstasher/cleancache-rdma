#include <linux/slab.h>

#include "bloom_filter.h"

bf_t *bf_init(u8 num_hash, unsigned int num_bits, void *ptr)
{
	bf_t *filter;
	
	filter = (bf_t *)kzalloc(sizeof(bf_t), GFP_KERNEL);
	if (!filter) {
		pr_err("failed to allocate bloom_filter");
		goto out_err;
	}

	filter->num_hash = num_hash;
	filter->num_bits = num_bits;
	filter->bitmap_size = BITS_TO_LONGS(num_bits) * sizeof(unsigned long);
	
	if (ptr) {
		filter->is_ordinary_bf = false;
		filter->bitmap = (unsigned long *) ptr;
		memset((void *)filter->bitmap, 0x00, filter->bitmap_size);
	} else {
		filter->is_ordinary_bf = true;
		filter->bitmap = vzalloc(filter->bitmap_size);
	}

	pr_info("%s bf: num_hash=%u, num_bits=%u, bitmap(KB)=%lu", 
			filter->is_ordinary_bf ? "Oridary" : "COOP",
			filter->num_hash, filter->num_bits, 
			filter->bitmap_size/1024);

	filter->tmp_bitmap = vzalloc(filter->bitmap_size);
	if (!filter->tmp_bitmap) {
		pr_err("failed to alloc tmp_bitmap");
		goto out_err2;
	}
	
	atomic_set(&filter->merging, 0);

	return filter;

out_err2:
	if (filter->is_ordinary_bf)
		vfree(filter->bitmap);
	kfree(filter);
out_err:	
	return NULL;
}

void bf_exit(bf_t *filter) 
{
	// coop bf's bitmap mem will be freed at rdma.c
	if (filter->is_ordinary_bf)
		vfree(filter->bitmap);
	if (!filter->is_ordinary_bf)
		vfree(filter->tmp_bitmap);

	kfree(filter);
	pr_err("%s", __func__);
}

void bf_add(bf_t *filter, const void *data, unsigned int len) 
{
	uint8_t i;
	uint32_t key_hash, long_idx;
	uint64_t check_bit;

	if (check_merging(filter)) {
		for (i = 0 ; i < filter->num_hash; i++) {
			key_hash = compute_hash(filter, i, data, len);
			calc_bitidx(key_hash, &long_idx, &check_bit);

			filter->tmp_bitmap[long_idx] |= check_bit;
		}
	} else {
		for (i = 0 ; i < filter->num_hash; i++) {
			key_hash = compute_hash(filter, i, data, len);
			calc_bitidx(key_hash, &long_idx, &check_bit);

			filter->bitmap[long_idx] |= check_bit;
		}
	}
}

bool bf_might_contain(bf_t *filter, const void *data, unsigned int len) 
{
	uint8_t i;
	uint32_t key_hash, long_idx;
	uint64_t check_bit;
	bool ret = true;

	if (check_merging(filter)) {
		/* lookup tmp bitmap */
		for (i = 0; i < filter->num_hash; i++) {
			key_hash = compute_hash(filter, i, data, len);
			calc_bitidx(key_hash, &long_idx, &check_bit);

			if (!(filter->tmp_bitmap[long_idx] & check_bit))
				goto orig;
		}
		goto out; /* exist at tmp buf */
	}

orig:
	for (i = 0; i < filter->num_hash; i++) {
		key_hash = compute_hash(filter, i, data, len);
		calc_bitidx(key_hash, &long_idx, &check_bit);

		if (!(filter->bitmap[long_idx] & check_bit)) {
			ret = false;
			break;
		}
	}

out: 
	return ret;
}
