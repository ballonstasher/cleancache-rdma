#ifndef __BREAKDOWN_H__
#define __BREAKDOWN_H__

#include <linux/ktime.h>

//#define DCC_BREAKDOWN

enum {
	BR_PUTS,
	BR_GETS,
	BR_INVS,
#if 0
	BR_META_PUTS,
	BR_META_GETS,
	BR_META_INVS,
#endif
	BR_COMM_PUTS,
	BR_COMM_GETS,
	BR_COMM_INVS,

	BR_MAX,
};

struct dcc_breakdown_t {
	u64 elapseds[BR_MAX];		
};

extern struct dcc_breakdown_t dcc_breakdown;
extern const char *dcc_breakdown_names[];

#ifdef DCC_BREAKDOWN

#define _(x)                   dcc_time_##x
#define dcc_declare_ts(x)      struct timespec _(x) = {0, 0}
#define dcc_start_ts(x)        getrawmonotonic(&_(x))
#define dcc_end_ts(name, x)    do {                                    	\
        struct timespec end = {0, 0};                                   \
        getrawmonotonic(&end);                                          \
        dcc_breakdown.elapseds[name] +=                  		\
                        (end.tv_sec - _(x).tv_sec) * NSEC_PER_SEC +     \
                        (end.tv_nsec - _(x).tv_nsec);                   \
} while (0)

#else

#define dcc_declare_ts(x)              do {} while (0)
#define dcc_start_ts(x)                do {} while (0)
#define dcc_end_ts(name, x)            do {} while (0)

#endif

#endif // __BREAKDOWN_H__
