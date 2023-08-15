#ifndef __IHASH_HPP__
#define __IHASH_HPP__

#include "util/pair.h"
#include "util/hash.h"
#include "util/random_dist.hpp"

//#define DCC_HT_DEBUG
#ifdef DCC_HT_DEBUG
#define dcc_ht_debug(str, ...) printf("%s: " str, __func__, __VA_ARGS__)
#else 
#define dcc_ht_debug(str, ...) do {} while (0)  
#endif

class IHash {
    public:
        IHash(void) = default;
        ~IHash(void) = default;

        virtual int Insert(Key_t&, Value_t, Key_t &, Value_t &) { }
        virtual Value_t Delete(Key_t &) { return NONE; }
        virtual Value_t Get(Key_t &) { return NULL; }
        virtual Value_t GetWithFlag(Key_t &, bool) { return NULL; }
        
        virtual Value_t Evict(Key_t &) { return NONE; }

        virtual double Utilization(void) { return 0; }
        virtual size_t Capacity(void) { return 0; }

        virtual void PrintStats(void) {}
};

/* longkey: 64bit = 4(cid) + 32(ino) + 28(page offset: enough file size) */
static inline uint64_t make_longkey(uint64_t cli_id, uint64_t inode,
		uint32_t index) {
	uint64_t longkey;

	longkey = inode << (32 - 4);
	longkey |= index;
	longkey |= cli_id << 60;

	return longkey;
}

static inline uint32_t longkey_to_cid(uint64_t longkey)
{
	return (longkey >> 60);
}

static inline uint32_t longkey_to_inode(uint64_t longkey)
{
	return (longkey >> (32 - 4)) & ((1UL << 32) - 1);
}

static inline uint32_t longkey_to_offset(uint64_t longkey)
{
	return longkey & ((1 << (28 + 1)) - 1);
}

#endif  // __IHASH_HPP__
