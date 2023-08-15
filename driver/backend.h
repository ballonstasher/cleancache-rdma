#ifndef __BACKEND_H__
#define __BACKEND_H__

#include "config.h"
#include "rdma.h"
#include "rmem_pool.h"
#include "rmem_cache.h"
#include "access_filter.h"

struct dcc_backend {
	int id;

	struct dcc_rdma_ctrl *ctrl;
	
	rmem_pool_t *pool;
	rmem_cache_t *cache;
	access_filter_t *af;
};

/* longkey: 64bit = 4(cid) + 32(ino) + 28(page offset: enough file size) */
static inline u64 make_longkey(u64 cli_id, u64 inode, u64 index) 
{
        u64 longkey;

        longkey = inode << (32 - 4);
        longkey |= index;
        longkey |= cli_id << 60;

        return longkey;
}

static inline u32 longkey_to_cid(u64 longkey)
{
        return (longkey >> 60);
}

static inline u32 longkey_to_inode(u64 longkey)
{
        return (longkey >> (32 - 4)) & ((1UL << 32) - 1);
}

static inline u32 longkey_to_offset(u64 longkey)
{
        return longkey & ((1 << (28 + 1)) - 1);
}

static inline int get_backend_id(ino_t ino, pgoff_t index) 
{
	u64 key;

	if (config.num_svrs == 1)
		return 0;

	key = ino << 32;
	key |= index;

	return (key * GOLDEN_RATIO_64) % config.num_svrs;
}

extern struct dcc_backend **backends;
#define DECLARE_BACKEND_VAR(ino, index) 			\
	int be_id = get_backend_id(ino, index); 		\
	struct dcc_backend *backend = backends[be_id];		\
	int cli_id = backend->ctrl->mm->server_mm_info.cli_id; 	\
	u64 key = make_longkey(cli_id, ino, index); 		\
	struct rmem_pool_t *pool = backend->pool; 		\
	struct rmem_cache_t *cache = backend->cache; 		\
	struct dcc_rdma_ctrl *ctrl = backend->ctrl; 		\


int dcc_handle_put_page(struct page *, ino_t, pgoff_t);
int dcc_handle_get_page(struct page *, ino_t, pgoff_t);
int dcc_handle_invalidate_page(ino_t, pgoff_t);


#endif // __BACKEND_H__
