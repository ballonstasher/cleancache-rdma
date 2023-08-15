#include <linux/cleancache.h>
#include <linux/swap.h>

#include "backend.h"
#include "stats.h"
#include "breakdown.h"

//define DCC_ACCESS_FILTER

/* TODO: set default config */
struct dcc_config_t config;

char cli_ip[INET_ADDRSTRLEN];
char *svr_ips[MAX_NUM_SVRS];
int svr_port = 50000;
bool exclusive_policy = 1;

module_param_string(cip, cli_ip, INET_ADDRSTRLEN, 0644);
module_param_array(svr_ips, charp, NULL, 0644);
module_param(svr_port, int, 0644);
module_param(exclusive_policy, bool, 0644);

struct dcc_backend **backends;
bool dma_mem_for_filter = true;

int __handle_onesided_put_page(struct page *page, u64 key, 
		struct rmem_pool_t *pool, struct rmem_cache_t *cache, 
		struct dcc_rdma_ctrl *ctrl) 
{
	u64 roffset = -1;
	u64 ev_key, ev_value;
	int retry_cnt = 0;
	int ret;
	dcc_declare_ts(all);
	dcc_declare_ts(index);
	dcc_declare_ts(pool);

RETRY:
	ev_key = -1;
	dcc_start_ts(pool);
	roffset = rmem_pool_alloc(pool);
	if (roffset == -1) {
		/* give up allocation */
		if (retry_cnt++ > 0) {
			return retry_cnt;
		}

		ev_value = rmem_cache_evict(cache, &ev_key);
		if (ev_value != -1)
			rmem_pool_free(pool, ev_value);
		BUG_ON(ev_value == -1);

		goto RETRY;
	} else {
		dcc_end_ts(BR_PUTS_POOL, pool);
	}
	
	dcc_start_ts(all);
	ret = dcc_rdma_write_page(ctrl, page, key, roffset);
	if (!ret)
		dcc_end_ts(BR_PUTS_COMM, all);

	dcc_start_ts(index);
	rmem_cache_insert(cache, key, roffset, &ev_key, &ev_value);
	if (ev_value != -1) {
		if (rmem_pool_free(pool, ev_value)) {
			pr_err("pool size overflow");
			BUG();
		}
	} else {
		dcc_end_ts(BR_PUTS_INDEX, index);
	}

	return 0;
}

int dcc_handle_put_page(struct page *page, ino_t ino, pgoff_t index) 
{
	DECLARE_BACKEND_VAR(ino, index);

#ifdef DCC_ACCESS_FILTER
	if (!af_upsert(backend->af, ino, index))
		return -5;
#endif

	return __handle_onesided_put_page(page, key, pool, cache, ctrl);
}
EXPORT_SYMBOL_GPL(dcc_handle_put_page);

int __handle_onesided_get_page(struct page *page, u64 key, 
		struct rmem_pool_t *pool, struct rmem_cache_t *cache, 
		struct dcc_rdma_ctrl *ctrl) 
{
	u64 roffset;
	int ret;
	dcc_declare_ts(all);
	dcc_declare_ts(index);
	dcc_declare_ts(pool);

	dcc_start_ts(index);
	roffset = rmem_cache_remove(cache, key);
	if (roffset == -1) {
		ret = -1;
		goto out;
	} else {
		dcc_end_ts(BR_GETS_INDEX, index);
	}

	dcc_start_ts(all);
	ret = dcc_rdma_read_page(ctrl, page, key, roffset);
	if(!ret)
		dcc_end_ts(BR_GETS_COMM, all);

	BUG_ON(roffset == -1);

	if (roffset != -1) {
		dcc_start_ts(pool);
		rmem_pool_free(pool, roffset);
		dcc_end_ts(BR_GETS_POOL, pool);
	}
out:
	return ret;
}

int dcc_handle_get_page(struct page *page, ino_t ino, pgoff_t index) 
{
	DECLARE_BACKEND_VAR(ino, index);

	return __handle_onesided_get_page(page, key, pool, cache, ctrl);
}
EXPORT_SYMBOL_GPL(dcc_handle_get_page);

int __handle_onesided_invalidate_page(u64 key, struct rmem_pool_t *pool, 
		struct rmem_cache_t *cache) 
{
	u64 roffset;
	dcc_declare_ts(index);
	dcc_declare_ts(pool);

	dcc_start_ts(index);
	roffset = rmem_cache_remove(cache, key);
	if (roffset != -1) {
		dcc_end_ts(BR_INVS_INDEX, index);

		dcc_start_ts(pool);
		rmem_pool_free(pool, roffset);
		dcc_end_ts(BR_INVS_POOL, pool);
	}

	return roffset != -1 ? 0 : -1;
}

int dcc_handle_invalidate_page(ino_t ino, pgoff_t index) 
{
	DECLARE_BACKEND_VAR(ino, index);

	return __handle_onesided_invalidate_page(key, pool, cache);
}
EXPORT_SYMBOL_GPL(dcc_handle_invalidate_page);

void dcc_set_default_config(void) 
{
	int i;

	strcpy(config.cli_ip, cli_ip);
	for (i = 0; i < MAX_NUM_SVRS; i++) {
		if (!svr_ips[i])
			break;
		strcpy(config.svr_ips[i], svr_ips[i]);
		config.num_svrs++;
	}
	
	config.svr_port = svr_port;
	config.exclusive_policy = exclusive_policy;
}

int dcc_backend_init(int id, struct dcc_rdma_ctrl *ctrl) 
{
	u32 num_rmem_pages = ctrl->mm->server_mm_info.len;
	struct dcc_backend *backend = backends[id];
	
	backend->id = id;
	backend->ctrl = ctrl;
	
	/* TODO: error handling */
	backend->cache = rmem_cache_init(num_rmem_pages);
	if (!backend->cache)
		return -ENOMEM;
	backend->pool =  rmem_pool_init(num_rmem_pages);
	if (!backend->pool) {
		rmem_cache_exit(backend->cache);
		return -ENOMEM;
	}

	backend->af = af_init();

	return 0;
}

static int __init dcc_backend_init_module(void)
{
	int i;
	
	dcc_set_default_config();

	backends = (struct dcc_backend **) kzalloc(
			sizeof(struct dcc_backend *) * MAX_NUM_SVRS, GFP_KERNEL);
	for (i = 0; i < MAX_NUM_SVRS; i++) {
		backends[i] = (struct dcc_backend *) 
			kzalloc(sizeof(struct dcc_backend), GFP_KERNEL);
		if (!backends[i]) {
			pr_err("failed to allocate backends");
			goto out_err;
		}
	}

	dcc_rdma_init();

	/* backend is initiated by connection */
	return 0;

out_err:
	for (i = 0; i < MAX_NUM_SVRS; i++)
		if (backends[i])
			kfree(backends[i]);
	kfree(backends);

	return -1;
}

void dcc_backend_exit(void) 
{
	int i;
	bool last_flag = false;

	for (i = 0; i < config.num_svrs; i++) {
		if (i == config.num_svrs - 1)
			last_flag = true;
		dcc_rdma_exit(backends[i]->ctrl, last_flag);

		rmem_cache_exit(backends[i]->cache);
		rmem_pool_exit(backends[i]->pool);

		af_exit(backends[i]->af);
	}

	for (i = 0; i < MAX_NUM_SVRS; i++) {
		if (backends[i])
			kfree(backends[i]);
	}
	kfree(backends);
	//pr_err("%s", __func__);
}

static void __exit dcc_backend_exit_module(void)
{
	dcc_backend_exit();
	ssleep(1);
}


module_init(dcc_backend_init_module);
module_exit(dcc_backend_exit_module);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Backend for DCC");
