#ifndef __LINEAR_PROBING_APPROX_HPP__
#define __LINEAR_PROBING_APPROX_HPP__

#include <mutex>
#include <shared_mutex>
#include <atomic>

#include "../Ihash.hpp"

#pragma pack(push, 1) 
struct LPA_Bkt_t {
    Key_t key;          
    Value_t value;
    unsigned int timestamp;
    std::atomic<bool> busy;
    uint8_t padding[3];
    /* 24B = 8 + 8 + 4 + 4 */

    LPA_Bkt_t(void)
        : key{INVALID}, value{NONE}, timestamp{INVALID32}, busy{false} { }

    void *operator new[] (size_t size) {
        void *ret;
        if (posix_memalign(&ret, 8, size)) ret = NULL;
        return ret;
    }

    void *operator new(size_t size) {
        void *ret;
        if (posix_memalign(&ret, 8, size)) ret = NULL;
        return ret;
    }
};
#pragma pack(pop)

class LinearProbingApprox : public IHash {
    private:
        size_t capacity;
        LPA_Bkt_t *buckets;

        std::shared_mutex *mtxs;
        int num_locks;
        int lock_size;

    public:
        LinearProbingApprox(void): 
            capacity{0}, buckets{nullptr} {}
        LinearProbingApprox(size_t);
        ~LinearProbingApprox(void);

        int Insert(Key_t &, Value_t, Key_t &, Value_t &);
        Value_t Delete(Key_t &);
        Value_t Get(Key_t &);
        Value_t GetWithFlag(Key_t &, bool);

        Value_t Evict(Key_t &);
        Value_t ApproxLRUEvict(Key_t &);
        Value_t ApproxLRUEvict(Key_t &, int cid);

        double Utilization(void) {
            size_t valid = 0;
            for (size_t i = 0; i < capacity; ++i) {
                if (IsValidBucket(i))
                    valid++;
            }
            return 100.0 * valid / capacity;
        }

        size_t Capacity(void) {
            return capacity;
        }

        void PrintStats(void);

    private:
        void CollEvict(Key_t &key, Value_t value, 
                Key_t &ev_key, Value_t &ev_value, size_t victim_slot) 
        {
            ev_key = buckets[victim_slot].key;
            ev_value = buckets[victim_slot].value;

            SetBucket(victim_slot, key, value);

            if (ev_key == INVALID) {
                throw std::runtime_error("ev_key is INVALID wrong victim_slot");
            }
        } 
        
        void SetBucket(size_t slot, Key_t &key, Value_t value) {
            buckets[slot].key = key;
            buckets[slot].value = value;
            SetTimestamp(buckets[slot].timestamp);
        }

        bool IsVacantBucket(size_t slot) {
            return buckets[slot].key == INVALID || 
                buckets[slot].key == TOMBSTONE;
        }

        bool IsValidBucket(size_t slot) {
            return buckets[slot].key != INVALID && 
                buckets[slot].key != TOMBSTONE; 
        }

        Key_t GetHashedKey(Key_t key) {
            return hash_funcs[0](&key, sizeof(Key_t), f_seed) % capacity;
        }

        void SetTimestamp(unsigned int &timestamp) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
            timestamp = ts.tv_sec;
        }
};

#endif  // __LINEAR_PROBING_APPROX_HPP__
