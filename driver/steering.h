#ifndef __STEERING_H__
#define __STEERING_H__

#include <linux/sort.h>

//#define EXTENT_SIZE (32 * 1024) /* XXX: this is hardcoded extent size */
//#define BLOCKS_PER_EXTENT (EXTENT_SIZE / 4096)
extern size_t extent_size;
extern size_t blocks_per_extent;

enum steering_policy {
    STEERING_OFF = 0,
    STEERING_BY_PATTERN,
    STEERING_BY_RANDOM,
    STEERING_BY_NETWORK
};

static inline u64 PG_INDEX_TO_EXTENT_ID(u64 pg_index) 
{
    //return pg_index / BLOCKS_PER_EXTENT;
    return pg_index / blocks_per_extent;
}

static inline u64 STEERING_INODE_FROM_KEY(u64 key)
{
    return key >> 32;
}

static inline u64 STEERING_PG_INDEX_FROM_KEY(u64 key)
{
    return (key & ((1UL << 32) - 1));
}

static inline u64 MAKE_STEERING_KEY(u64 inode, u64 pg_index)
{
    u64 key;
    key = inode << 32;
    key |= pg_index;
    return key;
}

static inline int steering_compare_key(const void *a, const void *b)
{
	if (*(const u64 *)a < *(const u64 *)b)
		return -1;
	else if (*(const u64 *)a > *(const u64 *)b)
		return 1;
	else
		return 0;
}

static inline void steering_sort(u64 *arr, size_t arr_size) 
{
    sort(arr, arr_size, sizeof(*arr), steering_compare_key, NULL);
}

static inline void steering_classify(hashtable_t *ht, u64 *arr, size_t arr_size)
{
    u64 cur_inode = STEERING_INODE_FROM_KEY(arr[0]);
    u64 cur_pg_index = STEERING_PG_INDEX_FROM_KEY(arr[0]);
    u64 cur_extent_id = PG_INDEX_TO_EXTENT_ID(cur_pg_index);
    int count = 1;
    int i, j;
    int adjacent = 0;
    u64 inode, pg_index, extent_id, x;
    u64 keys[32] = {}; // FIXME : same as num_put_batch
    u64 keys_idx = 0;
    //int count_thld = BLOCKS_PER_EXTENT / 3;
    int count_thld = blocks_per_extent / 3;

    for (i = 1; i < arr_size; i++) {
        inode = STEERING_INODE_FROM_KEY(arr[i]);
        pg_index = STEERING_PG_INDEX_FROM_KEY(arr[i]);
        extent_id = PG_INDEX_TO_EXTENT_ID(pg_index);

        if (inode == cur_inode && extent_id == cur_extent_id) {
            x = MAKE_STEERING_KEY(inode, pg_index);
            hashtable_insert(ht, x, 1);
            count += 1;
            keys[keys_idx++] = x;
        } else {
            if (count > 1) {
                if (count > count_thld) {
                    adjacent = 1;
                } else {
                    for (j = 0; j < keys_idx; j++) {
                        hashtable_update(ht, x, 0);
                    }
                    adjacent = 0;
                }

                for (j = 0; j < keys_idx; j++) {
                    keys[j] = 0;
                }
            } else {
                adjacent = 0;
            }
            keys_idx = 0;

            x = MAKE_STEERING_KEY(cur_inode, cur_pg_index);
            hashtable_insert(ht, x, adjacent);

            cur_inode = STEERING_INODE_FROM_KEY(arr[i]);
            cur_pg_index = STEERING_PG_INDEX_FROM_KEY(arr[i]);
            cur_extent_id = PG_INDEX_TO_EXTENT_ID(cur_pg_index);
            count = 1;
        }
    }

    if (count > 1) {
        if (count > count_thld) {
            adjacent = 1;
        } else {
            for (j = 0; j < keys_idx; j++) {
                hashtable_update(ht, x, 0); 
            }
            adjacent = 0;
        }
    } else {
        adjacent = 0;
    }

    x = MAKE_STEERING_KEY(cur_inode, cur_pg_index);
    hashtable_insert(ht, x, adjacent);
}

static inline void steering_process(hashtable_t *ht, u64 *arr, size_t arr_size) 
{
    steering_sort(arr, arr_size);

    steering_classify(ht, arr, arr_size);
}

#endif // __STEERING_H__
