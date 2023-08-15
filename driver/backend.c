#include <linux/cleancache.h>
#include <linux/swap.h>

#include "backend.h"
#include "stats.h"
#include "breakdown.h"

/* TODO: set default config */
struct dcc_config_t config;

char cli_ip[INET_ADDRSTRLEN];
char *svr_ips[MAX_NUM_SVRS];
int svr_port = 50000;
bool exclusive_policy = 1;
int metadata_type = COOP_BF;
int filter_update_interval = 5;
bool af_enable = false;

module_param_string(cip, cli_ip, INET_ADDRSTRLEN, 0644);
module_param_array(svr_ips, charp, NULL, 0644);
module_param(svr_port, int, 0644);
module_param(exclusive_policy, bool, 0644);
module_param(metadata_type, int, 0644);
module_param(filter_update_interval, int, 0644);
module_param(af_enable, bool, 0644);

struct dcc_backend **backends;
bool dma_mem_for_filter = true;

int __handle_twosided_put_page(struct page *page, u64 key, 
		struct metadata_t *meta, struct dcc_rdma_ctrl *ctrl) 
{
	int ret = 0, ret2;
	dcc_declare_ts(all);

	if (!exclusive_policy) {
		if (metadata_type)
			ret = metadata_contain(meta, (void *) &key, sizeof(key));
		else 
			ret = 1;
		/* 1. already exists and clean => don't have to put */
		if (ret) {
			if (!xa_get_mark(&page->mapping->i_pages, 
						page_index(page), 
						PAGECACHE_TAG_DIRTIED)) {
				ret = -3;
				goto out;
			}
			/* 2. but it is dirty => put to remote to update old */
			ret = -4;
		}
		/* 3. put page which does not exist in remote */
		ret = 0;
	}
	
	dcc_start_ts(all);
	ret2 = dcc_rdma_send_page(ctrl, page, key);
	if (!ret2) 
		dcc_end_ts(BR_COMM_PUTS, all);
	
	if (metadata_type && !ret2) { 
		metadata_add(meta, (void *) &key, sizeof(key));
	}

	if (ret2)
		ret = ret2;
out:
	return ret;
}

int dcc_handle_put_page(struct page *page, ino_t ino, pgoff_t index) 
{
	DECLARE_BACKEND_VAR(ino, index);

	if (config.af_enable && !af_upsert(backend->af, ino, index))
		return -5;

	return __handle_twosided_put_page(page, key, meta, ctrl);
}
EXPORT_SYMBOL_GPL(dcc_handle_put_page);

int __handle_twosided_get_page(struct page *page, u64 key, 
		struct metadata_t *meta, struct dcc_rdma_ctrl *ctrl)
{
	int ret;
	dcc_declare_ts(all);

	if (metadata_type && !metadata_contain(meta, (void *) &key, sizeof(key))) {
		return -1;
	}
	
	dcc_start_ts(all);
	ret = dcc_rdma_write_msg(ctrl, page, key);
	if (!ret)
		dcc_end_ts(BR_COMM_GETS, all);

	/* don't have to remove for inclusive */
	if (exclusive_policy) {
		if (metadata_type)
			metadata_remove(meta, (void *) &key, sizeof(key));
	} else {
		if (ret && metadata_type)
			metadata_remove(meta, (void *) &key, sizeof(key));
	}	
	return ret;
}

int dcc_handle_get_page(struct page *page, ino_t ino, pgoff_t index) 
{
	DECLARE_BACKEND_VAR(ino, index);
	
	return __handle_twosided_get_page(page, key, meta, ctrl);
}
EXPORT_SYMBOL_GPL(dcc_handle_get_page);

int __handle_twosided_invalidate_page(u64 key, struct metadata_t *meta, 
		struct dcc_rdma_ctrl *ctrl) 
{
	int ret;
	dcc_declare_ts(all);
#if 1
	/* XXX: multithread error? */
	if (metadata_type && !metadata_contain(meta, (void *) &key, sizeof(key)))
		return -1;
#endif
	dcc_start_ts(all);
	ret = dcc_rdma_write_msg2(ctrl, NULL, key);
	dcc_end_ts(BR_COMM_INVS, all);
	if (metadata_type)
		metadata_remove(meta, (void *) &key, sizeof(key));

	return ret;
}

int dcc_handle_invalidate_page(ino_t ino, pgoff_t index) 
{
	DECLARE_BACKEND_VAR(ino, index);

	return __handle_twosided_invalidate_page(key, meta, ctrl);
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
	config.metadata_type = metadata_type;
	config.filter_update_interval = filter_update_interval;
	config.af_enable = af_enable;
}

int dcc_backend_init(int id, struct dcc_rdma_ctrl *ctrl) 
{
	int i, j;
	u8 num_hash = ctrl->mm->server_mm_info.info[0];
	u32 num_bits = ctrl->mm->server_mm_info.info[1];
	u32 num_rmem_pages = ctrl->mm->server_mm_info.info[2];
	struct dcc_backend *backend = backends[id];

	backend->id = id;
	backend->ctrl = ctrl;

	if (metadata_type) {
		backend->meta = metadata_init(num_hash, num_bits,
				ctrl->mm->filter_mm_pool.ptr,
				num_rmem_pages, metadata_type);
	}

	if (metadata_type == COOP_BF) {
		if (filter_update_interval <= 0) {
			pr_err("filter_update_interval=%d", 
					filter_update_interval);
		}

		backend->worker[0] = 
			dcc_start_worker(backend, ctrl,
					(void *) &do_update_filter,
					&backend->worker_arg[0], 
					"filter_worker");
		if (IS_ERR(backend->worker[0]))
			goto out_err;
	}

	if (config.af_enable)
		backend->af = af_init();

	return 0;

out_err:
	for (i = 0; i < config.num_svrs; i++) {
		for (j = 0; j < 2; j++) {
			if (backend->worker[j])
				dcc_backend_stop_worker(backend->worker[j]);
		}
	}
	return -1;
}

static int __init dcc_backend_init_module(void)
{
	int i;
	
	dcc_set_default_config();

	backends = (struct dcc_backend **) kzalloc(
			sizeof(struct dcc_backend *) * MAX_NUM_SVRS, 
			GFP_KERNEL);
	for (i = 0; i < MAX_NUM_SVRS; i++) {
		backends[i] = (struct dcc_backend *) 
			kzalloc(sizeof(struct dcc_backend), GFP_KERNEL);
		if (!backends[i]) {
			pr_err("failed to allocate backends");
			goto out_err;
		}
	}

	if (metadata_type != COOP_BF)
		dma_mem_for_filter = false;

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

	if (metadata_type == COOP_BF) {
		for (i = 0; i < config.num_svrs; i++) {
			dcc_backend_stop_worker(
					backends[i]->worker[0]);
		}
	}

	for (i = 0; i < config.num_svrs; i++) {
		if (i == config.num_svrs - 1)
			last_flag = true;
		dcc_rdma_exit(backends[i]->ctrl, last_flag);
		
		if (metadata_type)
			metadata_exit(backends[i]->meta);
		
		if (config.af_enable)
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
