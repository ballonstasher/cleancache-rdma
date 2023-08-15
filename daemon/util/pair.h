#ifndef UTIL_PAIR_H_
#define UTIL_PAIR_H_

#include <cstdlib>
#include <stdint.h>

typedef size_t Key_t;
typedef void *Value_t;

enum key_const {
    INVALID    = 0xffffffffffffffff, // 11..11111111
    SENTINEL   = 0xfffffffffffffffe, // 11..11111110 
    TOMBSTONE  = 0xfffffffffffffffc, // 11..11111100

    INVALID32    = 0xffffffff, // 11..11111111 (32bit)
};

const Value_t NONE = 0x0;

struct Pair {
    Key_t key;
    Value_t value;

    Pair(void)
        : key{INVALID}, value{NONE} { }

    Pair(Key_t _key, Value_t _value)
        : key{_key}, value{_value} { }

    Pair& operator=(const Pair& other) {
        key = other.key;
        value = other.value;
        return *this;
    }

    void* operator new(size_t size) {
        void *ret;
        if (posix_memalign(&ret, 64, size) ) ret=NULL;
        return ret;
    }

    void* operator new[](size_t size) {
        void *ret;
        if (posix_memalign(&ret, 64, size) ) ret=NULL;
        return ret;
    }
};


#endif  // UTIL_PAIR_H_
