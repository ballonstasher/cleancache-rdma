#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cleancache.h>
#include <linux/uuid.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/utsname.h>
#include <asm/delay.h>

#include "backend.h"
#include "stats.h"
#include "breakdown.h"

extern void dcc_debugfs_init(void);
extern void dcc_debugfs_exit(void);

/* Cleancache operations implementation */
static void dcc_put_page(int pool_id, struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
	int ret;
	dcc_declare_ts(all);

	/* not coming from shrink_page_list */
	if (page_count(page) != 0 || !PageLocked(page) ||
			page->mapping->host->i_ino < 2) {
		return;
	}
	
	dcc_start_ts(all);
	ret = dcc_handle_put_page(page, key.u.ino, index);
	if (!ret)
		dcc_end_ts(BR_PUTS, all);

	switch (ret) {
		case -3:
			dcc_stats_inc(CLEAN_PUTS);
			break;
		case -4:
			dcc_stats_inc(REDIRTIED_PUTS);
			break;
		case -5:
			dcc_stats_inc(ACCESS_FILTERED_PUTS);
			break;
		case -6:
			dcc_stats_inc(BLOCKED_PUTS);
			break;
		case -7:
			dcc_stats_inc(REJECTED_PUTS);
			break;
	}
	/* acutal puts: puts - clean_puts */
}

static int dcc_get_page(int pool_id, struct cleancache_filekey key,
		pgoff_t index, struct page *page)
{
	int ret; 
	dcc_declare_ts(all);

	dcc_start_ts(all);
	ret = dcc_handle_get_page(page, key.u.ino, index);
	if (!ret) 
		dcc_end_ts(BR_GETS, all);

	if (ret > 0)
		dcc_stats_inc(FALSE_POSITIVE_GETS);
	else if (ret == -4)
		dcc_stats_inc(CRC_ERROR_GETS);

	return ret;
}

static void dcc_invalidate_page(int pool_id,
		struct cleancache_filekey key,
		pgoff_t index)
{
	int ret;
	dcc_declare_ts(all);

	dcc_start_ts(all);
	ret = dcc_handle_invalidate_page(key.u.ino, index);
	dcc_end_ts(BR_INVS, all);

	if (!ret)
		dcc_stats_inc(VALID_INVALIDATES);
}

static void dcc_invalidate_inode(int pool_id, struct cleancache_filekey key) 
{
	// TODO: handeled by inv page
}

static void dcc_invalidate_fs(int pool_id) 
{
	// TODO: 
}

static int dcc_init_fs(size_t pagesize) 
{
	static atomic_t pool_id = ATOMIC_INIT(0);

	atomic_inc(&pool_id);
	
	pr_info("dcc_init_fs (pool_id=%d)\n", atomic_read(&pool_id));

	return atomic_read(&pool_id);
}

static int dcc_init_shared_fs(uuid_t *uuid, size_t pagesize) 
{
	pr_info("FLUSH INIT SHARED\n");
	return -1;
}

static const struct cleancache_ops dcc_cleancache_ops = {
	.put_page = dcc_put_page,
	.get_page = dcc_get_page,
	.invalidate_page = dcc_invalidate_page,
	.invalidate_inode = dcc_invalidate_inode,
	.invalidate_fs = dcc_invalidate_fs,
	.init_shared_fs = dcc_init_shared_fs,
	.init_fs = dcc_init_fs
};

static int __init dcc_init(void) 
{
	int ret;

	pr_info("Hello... DCC: Disaggregated CleanCache");
	//pr_info("+-- Get: on, Put: on, inv. page: on");
	
	dcc_debugfs_init();

	ret = cleancache_register_ops(&dcc_cleancache_ops);
	if (!ret)
		pr_info("cleancache_register_ops success");
	else
		pr_err("cleancache_register_ops fail=%d", ret);

	if (cleancache_enabled)
		pr_info("cleancache_enabled");
	else
		pr_err("cleancache_disabled");

	return 0;
}

static void dcc_exit(void) 
{
	dcc_debugfs_exit();
#if 0
	ret = cleancache_deregister_ops(&dcc_cleancache_ops);
	if (!ret)
		pr_info("cleancache_deregister_ops success");
	else
		pr_err("cleancache_deregister_ops fail=%d", ret);
#endif
	pr_info("Bye... DCC: Disaggregated CleanCache");
}

module_init(dcc_init);
module_exit(dcc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("");
MODULE_DESCRIPTION("Cleancache backend driver");
