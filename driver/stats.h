#ifndef __STATS_H__
#define __STATS_H__

#include <linux/types.h>

enum dcc_stats_type {
	BLOCKED_PUTS,
	REJECTED_PUTS,
	CLEAN_PUTS,
	REDIRTIED_PUTS,
	ACCESS_FILTERED_PUTS,
	FALSE_POSITIVE_GETS,
	CRC_ERROR_GETS,
	VALID_INVALIDATES,

	ST_MAX
};

struct dcc_stats_t {
	u64 cnts[ST_MAX];
};

extern struct dcc_stats_t dcc_stats;
extern const char *dcc_stats_names[];

static inline void dcc_stats_inc(int name) 
{
	dcc_stats.cnts[name]++;
}

#endif // __STATS_H__
