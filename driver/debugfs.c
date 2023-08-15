#include <linux/debugfs.h>

#include "stats.h"
#include "breakdown.h"

static struct dentry *debugfs_dentry; 

void dcc_debugfs_init(void) 
{
#ifdef CONFIG_DEBUG_FS
	int i;

	debugfs_dentry = debugfs_create_dir("dcc", NULL);

	for (i = 0; i < ST_MAX; i++) {
		debugfs_create_u64(dcc_stats_names[i], 0644, debugfs_dentry, 
				&dcc_stats.cnts[i]);	
	}

	for (i = 0; i < BR_MAX; i++) {
		debugfs_create_u64(dcc_breakdown_names[i], 0644, debugfs_dentry, 
				&dcc_breakdown.elapseds[i]);
	}
#endif
}

void dcc_debugfs_exit(void) 
{
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(debugfs_dentry);
#endif
}
