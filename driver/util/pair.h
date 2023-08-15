#ifndef _PAIR_H_
#define _PAIR_H_

#include <linux/types.h>

typedef uint64_t Key_t;
typedef uint64_t Value_t;

enum key_const {
    INVALID  = 0xffffffffffffffff,
    SENTINEL = 0xfffffffffffffffe,
    UPDATED  = 0xfffffffffffffffc,
    NONE = 0x00,
};

#endif // _PAIR_H_
