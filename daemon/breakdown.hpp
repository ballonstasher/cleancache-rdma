#ifndef __BREAKDOWN_HPP__
#define __BREAKDOWN_HPP__

#include <cinttypes>
#include <time.h>

//#define DCC_BREAKDOWN

enum {
        BR_PUTS,
        BR_GETS,
        BR_INVS,

        BR_MAX,
};

struct dcc_breakdown_t {
        uint64_t elapseds[BR_MAX];
};

extern struct dcc_breakdown_t dcc_breakdown;
extern const char *dcc_breakdown_names[];

#ifdef DCC_BREAKDOWN

#define _(x)                    dcc_time_##x
#define dcc_declare_ts(x)       struct timespec _(x) = {0, 0}
#define dcc_start_ts(x)         clock_gettime(CLOCK_MONOTONIC, &_(x))
#define dcc_end_ts(name, x)     do {                                    \
        struct timespec end = {0, 0};                                   \
		clock_gettime(CLOCK_MONOTONIC, &end);                           \
        dcc_breakdown.elapseds[name] +=									\
                        (end.tv_sec - _(x).tv_sec) * (size_t) 1e9 +     \
                        (end.tv_nsec - _(x).tv_nsec);                   \
} while (0)

#else

#define dcc_declare_ts(x)              do {} while (0)
#define dcc_start_ts(x)                do {} while (0)
#define dcc_end_ts(name, x)            do {} while (0)

#endif


#endif // __BREAKDOWN_HPP__
