#include <linux/ktime.h>
#include <linux/types.h>

#include "timer.h"

ktime_t start_ts[NUM_TS] = {};
u64 elapsed_ts[NUM_TS] = {};
EXPORT_SYMBOL_GPL(start_ts);
EXPORT_SYMBOL_GPL(elapsed_ts);
