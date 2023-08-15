#include <linux/debugfs.h>

#include "rdma_stats.h"

static struct dentry *rdma_debugfs_dentry; 

void dcc_rdma_debugfs_init(void) 
{
#ifdef CONFIG_DEBUG_FS
	int i;

	rdma_debugfs_dentry = debugfs_create_dir("dcc-rdma", NULL);

        for (i = 0; i < RDMA_ST_MAX; i++) {
		debugfs_create_u64(dcc_rdma_stats_names[i], 0644, 
                                rdma_debugfs_dentry, 
				&dcc_rdma_stats.cnts[i]);	
	}
#endif
}

void dcc_rdma_debugfs_exit(void) 
{
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(rdma_debugfs_dentry);
#endif
}
