#ifndef __METADATA_H__
#define __METADATA_H__

#include "filter/bloom_filter.h"
#include "filter/counting_bloom_filter.h"
#include "hashtable.h"

enum metadata_type_t {
	FILTER_OFF = 0,
	COOP_BF,
	ORDINARY_BF,
	COUNTING_BF,
	HASH_TABLE,
};

typedef struct metadata_t {
	bf_t *bf;
	cbf_t *cbf;
	hashtable_t *ht;
	int type;
} metadata_t;

static inline metadata_t *metadata_init(u8 num_hash, unsigned int num_bits, 
		void *ptr, u32 num_rmem_pages, int type) 
{
	metadata_t *meta;
	
	meta = (metadata_t *) kzalloc(sizeof(metadata_t), GFP_KERNEL);
	if (!meta) {
		pr_err("failed to allocate meta");
		return NULL;
	}

	meta->type = type;

	switch (meta->type) {
		case COOP_BF:
		case ORDINARY_BF:
			meta->bf = bf_init(num_hash, num_bits, ptr);
			break;
		case COUNTING_BF:
			meta->cbf = cbf_init(num_hash, num_bits);
			break;
		case HASH_TABLE:
			meta->ht = hashtable_init(num_rmem_pages);
			break;
		default:
			pr_err("Unknown meta type: %d", meta->type);
	}

	return meta;
}

static inline void metadata_exit(metadata_t *meta) 
{
	switch (meta->type) {
		case COOP_BF:
		case ORDINARY_BF:
			bf_exit(meta->bf);
			break;
		case COUNTING_BF:
			cbf_exit(meta->cbf);
			break;
		case HASH_TABLE:
			hashtable_exit(meta->ht);
			break;
		default:
			pr_err("Unknown meta type: %d", meta->type);
	}

	kfree(meta);
}

static inline void metadata_add(metadata_t *meta, const void *data, 
		unsigned int data_len) 
{
	switch (meta->type) {
		case COOP_BF:
		case ORDINARY_BF:
			bf_add(meta->bf, data, data_len); 
			break;
		case COUNTING_BF:
			cbf_add(meta->cbf, data, data_len);
			break;
		case HASH_TABLE:
			hashtable_insert(meta->ht, *(u64 *) data, 0);
			break;
		default:
			pr_err("Unknown meta type: %d", meta->type);
	}
}

static inline bool metadata_contain(metadata_t *meta, const void *data, 
		unsigned int data_len) 
{
	switch (meta->type) {
		case COOP_BF:
		case ORDINARY_BF:
			return bf_might_contain(meta->bf, data, data_len); 
		case COUNTING_BF:
			return cbf_might_contain(meta->cbf, data, data_len); 
		case HASH_TABLE:
			if (hashtable_get(meta->ht, *(u64 *) data) == -1)
				return 0;
			else
				return 1;
		default:
			pr_err("Unknown meta type: %d", meta->type);
	}
	return 0;
}

static inline bool metadata_remove(metadata_t *meta, const void *data, 
		unsigned int data_len) 
{
	switch (meta->type) {
		case COOP_BF:
		case ORDINARY_BF:
			return 1; 
		case COUNTING_BF:
			return cbf_remove(meta->cbf, data, data_len); 
		case HASH_TABLE:
			if (hashtable_remove(meta->ht, *(u64 *) data) == -1)
				return 0;
			else 
				return 1;
		default:
			pr_err("Unknown meta type: %d", meta->type);
	}
	return 0;
}

static inline bool metadata_is_merging(metadata_t *meta) 
{
	if (meta->type != COOP_BF)
		BUG();
	return check_merging(meta->bf); 
}

static inline void metadata_blocking(metadata_t *meta) 
{
	if (meta->type == COOP_BF)
		bf_blocking(meta->bf);
}

static inline void metadata_unblocking(metadata_t *meta) 
{
	if (meta->type == COOP_BF)
		bf_unblocking(meta->bf);
}

#endif // __METADATA_H__
