#ifndef _HASH_FUNCS_H_
#define _HASH_FUNCS_H_

#include <stddef.h>

#include "hash-funcs/fasthash.h"
#include "hash-funcs/komihash.h"
#include "hash-funcs/mx3.h"
#include "hash-funcs/standard.h"
#include "hash-funcs/xxhash.h"
#include "hash-funcs/jenkins.h"
#include "hash-funcs/pengyhash.h"
#include "hash-funcs/murmur2.h"

enum hash_seed {
    f_seed = 0x2B12647CD1C7F697UL,
    s_seed = 0x9549A2C424952156UL
};

static size_t
(*hash_funcs[8])(const void* key, size_t len, size_t seed) = {
    komihash,
    fasthash64,
    mx3,
    xxhash,
    jenkins,
    pengyhash,
    MurmurHash64A,
    standard,
};

static inline size_t h(const void* key, size_t len, size_t seed) {
  return hash_funcs[0](key, len, seed);
}


#endif  // _HASH_FUNCS_H_
