#ifndef _PAIR_H_
#define _PAIR_H_

#include <linux/types.h>

typedef uint64_t Key_t;
typedef uint64_t Value_t;

enum key_const {
    INVALID    = (uint64_t) -1,
    SENTINEL   = (uint64_t) -2,
    TOMBSTONE  = (uint64_t) -3,
    BYPASS     = (uint64_t) -4,
};

#endif // _PAIR_H_
