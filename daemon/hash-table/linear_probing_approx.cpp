#include <climits>
#include <cassert>

#include "linear_probing_approx.hpp"

#if 1
#define COLLISION_EVICT /* on: coll evict, off : just evict cur key */
#define MAX_PROBING_LEN (16 - 1) // (16 - 1)
#else
#define MAX_PROBING_LEN (size_t)-1
#endif 

using namespace std;

UniformDist lpa_uni_dist(0UL, ULONG_MAX);

LinearProbingApprox::LinearProbingApprox(size_t _capacity)
    : capacity{_capacity}, buckets{new LPA_Bkt_t[capacity]}
{
#if MAX_PROBING_LEN == (size_t)-1
    lock_size = 16; // we only consider adjacent lock range
#else
    lock_size = MAX_PROBING_LEN + 1; // we only consider adjacent lock range
#endif
    num_locks = capacity/lock_size + 1;
    mtxs = new std::shared_mutex[num_locks];

    printf("%s: buckets, num=%lu, size: %lu (KB)\n", __func__,
            capacity, sizeof(Key_t) * capacity / 1024);
}

LinearProbingApprox::~LinearProbingApprox(void) {
    if (buckets != nullptr)
        delete[] buckets;
    delete mtxs;
}

/* ret = 0, successfully insert key or dup key , -1: failed to insert key */
int LinearProbingApprox::Insert(Key_t& key, Value_t value, 
        Key_t &ev_key, Value_t &ev_value) {
    unsigned int min_timestamp = -1, prev_min_timestamp = -1;
    size_t victim_slot = -1, prev_victim_slot = -1;
    int ret = 0;
    size_t i = 0;
    size_t prev_lock_idx = -1, lock_idx;
    
    /* check duplicated key => remove and retry */
    ev_value = Delete(key);
    if (ev_value)
        return 0;

    size_t loc = GetHashedKey(key);

    while (i < capacity) {
        size_t slot = (loc + i) % capacity;
        lock_idx = slot / lock_size;
        
        unique_lock<shared_mutex> cur_lock(mtxs[lock_idx]);

        do {
            /* insert succeed */
            if (IsVacantBucket(slot)) {
                if (buckets[slot].key == key)
                    throw std::runtime_error("LPA::key is duplicated");
                
                SetBucket(slot, key, value);
                goto out;
            }
            /* probe next bucket */
#ifdef COLLISION_EVICT
            /* find victim which has the smallest timestamp */
            if (buckets[slot].timestamp <= min_timestamp) {
                min_timestamp = buckets[slot].timestamp;
                victim_slot = slot;
            }
#endif
            if (i >= MAX_PROBING_LEN) {
#ifdef COLLISION_EVICT
                if (prev_victim_slot != -1 && 
                        prev_min_timestamp < min_timestamp) {
                    /* prev range */
                    unique_lock<shared_mutex> prev_lock(mtxs[prev_lock_idx],
                            defer_lock);

                    if (prev_lock.try_lock()) {
                        /* prev_victim_slot chagnes to vacant => insert */
                        victim_slot = prev_victim_slot;
                        if (IsVacantBucket(victim_slot)) {
                            SetBucket(victim_slot, key, value);
                            prev_lock.unlock();
                            goto out; 
                        }

                        if (buckets[victim_slot].busy) {
                            ev_key = key;
                            ev_value = value;
                            ret = -1; 
                        } else {
                            /* valid: swap old bucket */
                            CollEvict(key, value, ev_key, ev_value, victim_slot);
                        }

                        prev_lock.unlock();
                        goto out;
                    } else {
                        /* prev lock is already locked => try cur range */ 
                    }
                }

                /* cur range */
                /* XXX: still not found victim => failed to insert */
                if (victim_slot == -1) {
                    ev_key = key;
                    ev_value = value;
                    ret = -1;
                    goto out;
                }

                if (buckets[victim_slot].busy) {
                    ev_key = key;
                    ev_value = value;
                    ret = -1;
                } else {
                    CollEvict(key, value, ev_key, ev_value, victim_slot);
                }
#else 
                ev_key = key; 
                ev_value = value;
                ret = -1;
#endif
                goto out;
            }

            i++;
            slot = (loc + i) % capacity;
            if (!(i < capacity)) 
                break;
        } while (slot % lock_size);
        /* probe next lock range */
        prev_lock_idx = lock_idx;
        prev_min_timestamp = min_timestamp;
        prev_victim_slot = victim_slot;
        victim_slot = -1;
        min_timestamp = -1;
    }

out:
    return ret;
}

Value_t LinearProbingApprox::Delete(Key_t &key) {
    Value_t value = NONE;
    size_t loc = GetHashedKey(key);
    size_t i = 0;

    while (i < capacity) {
        size_t slot = (loc + i) % capacity;

        unique_lock<shared_mutex> lock(mtxs[slot / lock_size]);
        
        do {
            if (buckets[slot].key == key) {
                value = buckets[slot].value;
                buckets[slot].key = TOMBSTONE;
                goto out;
            } else if (buckets[slot].key == INVALID) {
                goto out;
            } else {
                /* keep probing even meet TOMBSTONE */
            }

            if (i >= MAX_PROBING_LEN)
                goto out;

            i++;
            slot = (loc + i) % capacity;
            if (!(i < capacity)) 
                break;
        } while (slot % lock_size);
    }

out:
    return value;
}

Value_t LinearProbingApprox::Get(Key_t &key) {
    Key_t key_hash = GetHashedKey(key);
    Value_t value = NONE;
    size_t loc = key_hash;

    for (size_t i = 0; i < capacity; i++) {
        size_t slot = (loc + i) % capacity;

        shared_lock<shared_mutex> lock(mtxs[slot / lock_size]);
        
        if (buckets[slot].key == key) {
            value = buckets[slot].value;
            SetTimestamp(buckets[slot].timestamp);
            break;
        } else if (buckets[slot].key == INVALID) {
            break;
        } else {
            /* keep probing even meet TOMBSTONE */
        }

        if (i >= MAX_PROBING_LEN)
            break;
    }

    return value;
}

Value_t LinearProbingApprox::GetWithFlag(Key_t &key, bool busy) {
    Key_t key_hash = GetHashedKey(key);
    Value_t value = NONE;
    size_t loc = key_hash;
    size_t slot;

    for (size_t i = 0; i < capacity; i++) {
        slot = (loc + i) % capacity;

        /* shared lock with small range is better */
        shared_lock<shared_mutex> lock(mtxs[slot / lock_size]);
        if (buckets[slot].key == key) {
            buckets[slot].busy.store(busy);

            if (busy) {
                value = buckets[slot].value;
                SetTimestamp(buckets[slot].timestamp);
            }
            break;
        }

        if (buckets[slot].key == INVALID)
            break;

        if (i >= MAX_PROBING_LEN)
            break;
    }

    return value;
}

Value_t LinearProbingApprox::Evict(Key_t &key) {
    return ApproxLRUEvict(key);
}

#define MAX_EVICT_RETRY 2
Value_t LinearProbingApprox::ApproxLRUEvict(Key_t &key) {
    Value_t value = NONE;
    unsigned int min_timestamp = -1, prev_min_timestamp;
    size_t victim_slot = -1, prev_victim_slot;
    size_t lock_idx, prev_lock_idx;
    int retry_cnt = -1;

RETRY:
    retry_cnt++;
    prev_min_timestamp = -1;
    prev_lock_idx = -1;
    prev_victim_slot = -1;

    size_t loc = lpa_uni_dist.GetRand() % capacity;
    size_t i = 0;
    while (i < capacity) {
        size_t slot = (loc + i) % capacity;

        lock_idx = slot / lock_size;
        unique_lock<shared_mutex> cur_lock(mtxs[lock_idx], std::defer_lock);
        /* already locked => better to try other range */
        if (!cur_lock.try_lock())
            goto RETRY;

        do {
            if (IsValidBucket(slot)) {
                /* selected bucket is now used */
                if (buckets[slot].busy) {
                    cur_lock.unlock();
                    goto RETRY;
                }

                if (retry_cnt > MAX_EVICT_RETRY) {
                    /* Random replacement: find victim immediately */
                    prev_victim_slot = -1;
                    victim_slot = slot;
                    goto out_direct;
                } else {
                    if (buckets[slot].timestamp <= min_timestamp) {
                        min_timestamp = buckets[slot].timestamp;
                        victim_slot = slot;
                    }
                }
            }

            if (i >= MAX_PROBING_LEN) {
out_direct:
                /* unlikely all adjacent slots are not valid */
                if (victim_slot == -1) {
                    cur_lock.unlock();
                    goto RETRY; 
                }

                if (prev_victim_slot != -1 && 
                        prev_min_timestamp < min_timestamp) {
                    /* prev range */
                    unique_lock<shared_mutex> prev_lock(mtxs[prev_lock_idx], 
                            defer_lock);
                    if (prev_lock.try_lock()) {
                        /* prev_victim_slot chagnes to vacant => evict cur range */
                        if (IsVacantBucket(prev_victim_slot)) {
                            prev_lock.unlock(); 
                            goto out_cur;
                        }

                        victim_slot = prev_victim_slot;
                        key = buckets[victim_slot].key;
                        value = buckets[victim_slot].value;
                        buckets[victim_slot].key = TOMBSTONE; 

                        prev_lock.unlock();
                        cur_lock.unlock();
                        goto out;
                    } else {
                        /* prev is already locked => try cur range */ 
                    }
                }

out_cur: 
                /* cur range */
                key = buckets[victim_slot].key;
                value = buckets[victim_slot].value;
                buckets[victim_slot].key = TOMBSTONE; 
                cur_lock.unlock();
                goto out;
            }

            i++;
            slot = (loc + i) % capacity;
            if (!(i < capacity))
                break;
        } while (slot % lock_size);
        /* probe next lock range */
        prev_lock_idx = lock_idx;
        prev_min_timestamp = min_timestamp;
        prev_victim_slot = victim_slot;
        victim_slot = -1;
        min_timestamp = -1;
    }

out:
    //printf("cid=%d, key=%lu\n", longkey_to_cid(key), key);
    if (key == INVALID || key == TOMBSTONE)
        throw std::runtime_error("Evict: key is INVALID or TOMBSTONE");
    if (value == NONE)
        throw std::runtime_error("Evict: value is NONE");

    return value;
}

void LinearProbingApprox::PrintStats(void) {
    //printf("%lu, %lu", total_insert_probe_cnt, total_get_probe_cnt);
}
