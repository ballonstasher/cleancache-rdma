#ifndef __COUNTING_BLOOM_FILTER_H__
#define __COUNTING_BLOOM_FILTER_H__

typedef struct counting_bloom_filter {
    u8            num_hash;
    unsigned int  num_bits;
    size_t        bitmap_size;
    u8           *byte_bitmap;
    rwlock_t 	 rw_lock;
} cbf_t;

cbf_t *cbf_init(u8, unsigned int);
void cbf_exit(cbf_t *);
void cbf_add(cbf_t *, const void *, unsigned int);
bool cbf_might_contain(cbf_t *, const void *, unsigned int);
bool cbf_remove(cbf_t *, const void *, unsigned int);

#endif // __COUNTING_BLOOM_FILTER_H__
