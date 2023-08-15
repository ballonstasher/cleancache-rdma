#ifndef _MURMUR2_H_
#define _MURMUR2_H_

static inline size_t MurmurHash64A(const void * key, size_t len, size_t seed) {
	const uint64_t m = 0xc6a4a7935bd1e995;
	const int r = 47;
	uint64_t h = seed ^ (len * m);
	const uint64_t * data = (const uint64_t *)key;
	const uint64_t * end = data + (len/8);
	const unsigned char *data2;

	while (data != end) {
		uint64_t k = *data++;

		k *= m;
		k ^= k >> r;
		k *= m;

		h ^= k;
		h *= m;
	}

	data2 = (const unsigned char*)data;

	switch(len & 7) {
		case 7: h ^= ((uint64_t) data2[6]) << 48;
			// fallthrough
		case 6: h ^= ((uint64_t) data2[5]) << 40;
			// fallthrough
		case 5: h ^= ((uint64_t) data2[4]) << 32;
			// fallthrough
		case 4: h ^= ((uint64_t) data2[3]) << 24;
			// fallthrough
		case 3: h ^= ((uint64_t) data2[2]) << 16;
			// fallthrough
		case 2: h ^= ((uint64_t) data2[1]) << 8;
			// fallthrough
		case 1: h ^= ((uint64_t) data2[0]);
			// fallthrough
			h *= m;
	};

	h ^= h >> r;
	h *= m;
	h ^= h >> r;

	return (size_t)h;
}

//-----------------------------------------------------------------------------
// MurmurHash2, by Austin Appleby

// Note - This code makes a few assumptions about how your machine behaves -

// 1. We can read a 4-byte value from any address without crashing
// 2. sizeof(int) == 4

// And it has a few limitations -

// 1. It will not work incrementally.
// 2. It will not produce the same results on little-endian and big-endian
//    machines.
static inline size_t murmur2 ( const void * key, size_t len, size_t seed)
{
	// 'm' and 'r' are mixing constants generated offline.
	// They're not really 'magic', they just happen to work well.

	const unsigned int m = 0x5bd1e995;
	const int r = 24;

	// Initialize the hash to a 'random' value

	unsigned int h = seed ^ len;

	// Mix 4 bytes at a time into the hash

	const unsigned char * data = (const unsigned char *)key;

	while(len >= 4)
	{
		unsigned int k = *(unsigned int *)data;

		k *= m;
		k ^= k >> r;
		k *= m;

		h *= m;
		h ^= k;

		data += 4;
		len -= 4;
	}

	// Handle the last few bytes of the input array

	switch(len)
	{
		case 3: h ^= data[2] << 16;
			/* fall through */
		case 2: h ^= data[1] << 8;
			/* fall through */
		case 1: h ^= data[0];
			h *= m;
	};

	// Do a few final mixes of the hash to ensure the last few
	// bytes are well-incorporated.

	h ^= h >> 13;
	h *= m;
	h ^= h >> 15;

	return h;
}

#endif // _MURMUR2_H_ 
