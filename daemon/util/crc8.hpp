#ifndef __CRC8_HPP__
#define __CRC8_HPP__

#include <cinttypes>

//#define DCC_CRC_CHECK

#define CRC8_NUM_CHECK 16

extern uint8_t crc8tab[];

/*
 * check all data is too expensive, instead check data sparsely
 * in our work, using crc to check data is corrupted while transferred
 */
#ifdef DCC_CRC_CHECK
static inline uint8_t CRC8(const void *buff)
{
	const char *buf = (char *) buff;
	int i, stride = 4096 / CRC8_NUM_CHECK;
	uint8_t crc = 0;

    for (i = 0; i < CRC8_NUM_CHECK; i++) {
		crc = crc8tab[(crc ^ *buf) & 0xff];
		buf += stride;
	}
	return crc;
}
#else
static inline uint8_t CRC8(const void *buf) { return 0; }
#endif


#endif // __CRC8_HPP__
