#pragma once

// author: Jon Maiga, 2020-08-03, jonkagstrom.com, @jonkagstrom
// license: CC0 license

static const uint64_t C = 0xbea225f9eb34556d;

inline uint64_t mx3_mix(uint64_t x) {
	x *= C;
	x ^= x >> 33;
	x *= C;
	x ^= x >> 29;
	x *= C;
	x ^= x >> 39;
	return x;
}

inline uint64_t mix_stream(uint64_t h, uint64_t x) {
	x *= C;
	x ^= (x >> 57) ^ (x >> 33);
	x *= C;
	h += x;
	h *= C;
	return h;
}

inline size_t mx3(const void* buf, size_t len, size_t seed) {
	const uint64_t* buf64 = (const uint64_t*)(buf);
	const uint8_t* const tail = (const uint8_t*)(buf64 + len/8);

	uint64_t h = seed ^ len;
	while (len >= 32) {
		len -= 32;
		h = mix_stream(h, *buf64++);
		h = mix_stream(h, *buf64++);
		h = mix_stream(h, *buf64++);
		h = mix_stream(h, *buf64++);
	}

	while (len >= 8) {
		len -= 8;
		h = mix_stream(h, *buf64++);
	}

	uint64_t v = 0;
	switch (len & 7) {
		case 7: v |= (uint64_t)(tail[6]) << 48;
		case 6: v |= (uint64_t)(tail[5]) << 40;
		case 5: v |= (uint64_t)(tail[4]) << 32;
		case 4: v |= (uint64_t)(tail[3]) << 24;
		case 3: v |= (uint64_t)(tail[2]) << 16;
		case 2: v |= (uint64_t)(tail[1]) << 8;
		case 1: h = mix_stream(h, v | tail[0]);
		default: ;
	}
	return mx3_mix(h);
}
