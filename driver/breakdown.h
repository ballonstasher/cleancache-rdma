#ifndef __BREAKDOWN_H__
#define __BREAKDOWN_H__

#include <linux/ktime.h>

//#define DCC_BREAKDOWN

enum {
	BR_PUTS,
	BR_PUTS_INDEX,
	BR_PUTS_POOL,
	BR_PUTS_COMM,
	
	BR_GETS,
	BR_GETS_INDEX,
	BR_GETS_POOL,
	BR_GETS_COMM,

	BR_INVS,
	BR_INVS_INDEX,
	BR_INVS_POOL,
	BR_INVS_COMM,

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

#ifdef DCC_BREAKDOWN_ETE

#define _(x)                   		dcc_time_##x
#define dcc_declare_ete_ts(x)      	struct timespec _(x) = {0, 0}
#define dcc_start_ete_ts(x)        	getrawmonotonic(&_(x))
#define dcc_end_ete_ts(name, x)    do {                                 \
        struct timespec end = {0, 0};                                   \
        getrawmonotonic(&end);                                          \
        dcc_breakdown.elapseds[name] +=                  		\
                        (end.tv_sec - _(x).tv_sec) * NSEC_PER_SEC +     \
                        (end.tv_nsec - _(x).tv_nsec);                   \
} while (0)

#else

#define dcc_declare_ete_ts(x)              do {} while (0)
#define dcc_start_ete_ts(x)                do {} while (0)
#define dcc_end_ete_ts(name, x)            do {} while (0)

#endif

#endif // __BREAKDOWN_H__
