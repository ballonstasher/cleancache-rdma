#include <linux/types.h>

#include "stats.h"

struct dcc_stats_t dcc_stats = {};

const char *dcc_stats_names[] = {
	"blocked_puts",
	"rejected_puts",
	"clean_puts",
	"redirtied_puts",
	"access_filtered_puts",
	"false_positive_gets",
	"crc_error_gets",
	"valid_invalidates",
};
