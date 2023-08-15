#ifndef __COUNTING_BLOOM_FILTER_HPP__
#define __COUNTING_BLOOM_FILTER_HPP__

#include <cstdlib>
#include <cstring>

#include "util/hash.h"

//#define DCC_CBF_DEBUG
#ifdef DCC_CBF_DEBUG
#define cbf_debug(str, ...) printf("%s: " str, __func__, __VA_ARGS__)
#else 
#define cbf_debug(str, ...) do {} while (0)  
#endif

#define BITS_PER_BYTE        (8)
#define DIV_ROUND_UP(n,d)    (((n) + (d) - 1) / (d))
#define BITS_TO_LONGS(nr)    DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

// fpp: 1% prime number: =>
// (2GB:    6, 700KB*8:  5734439)
// (4GB:    6, 2MB*8:    16777259)
// (6GB:    6, 2MB*8:    16777259)
// (8GB:    6, 3MB*8:    25165843)
// (16GB:   6, 8MB*8:    67108879)
// (32GB:   6, 10MB*8:   83886091)
// (64GB:   6, 20MB*8:   167772161)
// (128GB:  6, 40MB*8:   335544323)    
// (256GB:  6, 80MB*8:   671088667)    
// (512GB:  6, 160MB*8:  1342177283)    
// (1024GB: 6, 320MB*8:  2684354591)    

/* A counting Bloom filter. Instead of an array of bits, maintains an array of
 *  bytes. Each byte is incremented for an added item, or decremented for a
 *  deleted item, thereby supporting a delete operation.
 *  @param T Contained type being indexed
 */
template <typename T>
class CountingBloomFilter {
    public:
        /** Constructor: Creates a BloomFilter with the requested bit array size
         *  and number of hashes.
         *  @param num_hash The number of hashes which will be computed for each
         *                   added object.
         *  @param num_bits   The size of the bit array to allocate. If the
         *                   BloomFilter is not an OrdinaryBloomFilter, the actual
         *                   storage size will differ from this.
         */
        explicit CountingBloomFilter(uint8_t num_hash, uint32_t num_bits) {
            num_hash_  = num_hash;
            num_bits_  = num_bits;
            num_longs_ = BITS_TO_LONGS(num_bits);
            
            // alloc size: 1 B x num_bits
            cbf_byte_bitarr_ = (uint8_t *)aligned_alloc(8, sizeof(uint8_t) * 
                    num_bits); 
            memset(cbf_byte_bitarr_, 0x00, sizeof(uint8_t) * num_bits);
            // alloc size: 8 B x num_longs
            bf_long_bitarr_ = (unsigned long *)aligned_alloc(8, 
                    sizeof(unsigned long) * num_longs_); 
            memset(bf_long_bitarr_, 0x00, sizeof(uint64_t) * num_longs_);
        }

        uint8_t GetNumHash() const {
            return num_hash_;
        }

        uint64_t GetCBFAddr() const {
            return (uint64_t)cbf_byte_bitarr_;
        }

        uint32_t GetNumBits() const {
            return num_bits_;
        }

        uint64_t GetNumLongs() const {
            return num_longs_;
        }

        uint64_t GetBFAddr() const {
            return (uint64_t)bf_long_bitarr_;
        }

        uint64_t GetBFAddr(uint32_t idx) const {
            return (uint64_t)&bf_long_bitarr_[idx];
        }

        void Insert(T const& o) {
            for (uint8_t i = 0; i < num_hash_; i++) {
                uint32_t idx = computeHash(o, i);
                if (cbf_byte_bitarr_[idx] < 255) {
                    cbf_byte_bitarr_[idx]++;
                } else {
                    cbf_byte_bitarr_[idx] = 1;
                    fprintf(stderr, "idx: %u exceed 256\n", idx);
                    //GetUtilization();
                }
            }
        }

        bool MightContain(T const& o, uint32_t idx_res[]) const {
            bool ret = true;

            for (uint8_t i = 0; i < num_hash_; i++) {
                uint32_t idx = computeHash(o, i);
                idx_res[i] = idx;
                if (!cbf_byte_bitarr_[idx]) {
                    ret = false;
                    break;
                }
            }

            return ret;
        }

        bool Remove(T const& o) {
            bool ret = false;
            uint32_t idx; 
            uint32_t idx_res[num_hash_];
            
            for (int i = 0; i < num_hash_; i++)
                idx_res[i] = -1;

            if (MightContain(o, idx_res)) {
                for (uint8_t i = 0; i < num_hash_; i++) {
                    if (idx_res[i] != -1)
                        idx = idx_res[i]; 
                    else
                        idx = computeHash(o, i);

                    cbf_byte_bitarr_[idx]--;
                }

                ret = true;
            }
            return ret;
        }

        bool MightContain(T const& o) const {
            bool ret = true;

            for (uint8_t i = 0; i < num_hash_; i++) {
                uint32_t idx = computeHash(o, i);
                if (!cbf_byte_bitarr_[idx]) {
                    /* o might not be contained */
                    ret = false;
                    break;
                }
            }
            return ret;
        }

        bool BFInsert(T const& o) {
            for(uint8_t i = 0; i < num_hash_; i++){
                uint32_t idx = computeHash(o, i);
                uint32_t j = idx / 64; 
                uint8_t bit_shift = idx % 64;
                uint64_t check_bit = (1UL << bit_shift);
                bf_long_bitarr_[j] |= check_bit;
                cbf_debug("bf_long_bitarr_[%lu] = %lu\n", j, bf_long_bitarr_[j]);
            }
        }

        bool BFMightContain(T const& o) const {
            for(uint8_t i = 0; i < num_hash_; i++){
                auto idx = computeHash(o, i);
                uint8_t bit_shift = idx % 64;
                uint64_t check_bit = (1UL << bit_shift);
                if (!(bf_long_bitarr_[idx/64] & check_bit)) {
                    return false;
                }
            }
            return true;
        }

        bool ToOrdinaryBF() {
            /* reset bf */
            for (uint32_t i = 0; i < num_longs_; i++) {
                bf_long_bitarr_[i] = 0;
            }

            for (uint32_t i = 0; i < num_bits_; i++) {
                uint32_t j = i / 64; 
                if (cbf_byte_bitarr_[i] > 0) {
                    uint8_t bit_shift = i % 64;
                    uint64_t check_bit = (1UL << bit_shift);
                    bf_long_bitarr_[j] |= check_bit;

                    cbf_debug("bf_long_bitarr[%u] = %lu\n", j, bf_long_bitarr_[j]);
                }
            }

            return true;
        }

        void GetUtilization() {
            double util;
            size_t valid = 0;
            for (uint64_t i = 0; i < num_bits_; i++) {
                if (!cbf_byte_bitarr_[i])
                    valid++;
            }

            util = 100.0 * valid / num_bits_;
            printf("CBF Utilization: %.2f (%%)\n", util);
        }

        uint32_t computeHash(T const& key, uint8_t salt) const {
            return hash_funcs[salt](&key, sizeof(key), f_seed) % num_bits_;
        }

    private:
        uint8_t num_hash_;
        /* Number of bits for bit arr */
        uint32_t num_bits_;
        uint64_t num_longs_;

        uint8_t *cbf_byte_bitarr_;
        unsigned long *bf_long_bitarr_;

}; // class CountingBloomFilter

static inline unsigned int generate_prime(unsigned int low, unsigned int high) {
    unsigned int i;
    bool is_prime;
    //vector<size_t> vec;

    while (low < high) {
        is_prime = true;

        // 0 and 1 are not prime numbers
        if (low == 0 || low == 1) {
            is_prime = false;
        }

        for (i = 2; i <= low/2; ++i) {
            if (low % i == 0) {
                is_prime = false;
                break;
            }
        }

        if (is_prime) {
            //vec.push_back(low);
            break;
        }

        ++low;
    }

    return low; //vec.front();
}

static inline double calc_filter_param(unsigned int num_elems, 
        unsigned int &num_hash, unsigned int &num_bits) {
    double fpp = 0.05; // TODO: expected fpp, 0.01: 1%, 0.001: 0.1%

    num_bits = ceil((num_elems * log(fpp)) / log(1 / pow(2, log(2))));
    num_bits = generate_prime(num_bits, num_bits + 50);
    num_hash = round((num_bits / num_elems) * log(2));

    return pow(1 - exp((-1.0 * num_hash * num_elems/num_bits)), num_hash);
}


#endif // __COUNTING_BLOOM_FILTER_HPP__
