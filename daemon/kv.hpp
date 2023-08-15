#ifndef __KV_HPP__
#define __KV_HPP__

#include <iostream>
#include <mutex>
#include <atomic>
#include <thread>

#include "Ihash.hpp"
#include "percpu_pool.hpp"
#include "counting_bloom_filter.hpp"
#include "worker_thread.hpp"
#include "logger.hpp"
#include "cpu_stats.hpp"
#include "stats.hpp"

class KV {
    public:
        KV(void *);
        ~KV();

        void Insert(Key_t &, Value_t, int, int);
        Value_t Remove(Key_t &, int, int);
        Value_t Get(Key_t &, int, int);
        Value_t GetWithFlag(Key_t &, int, int, bool);

        void Evict(int, int); 
        void Evictor(int, int);

        void ShowStats(void);

        double Utilization(void) {
            return hashes[0]->Utilization();
        }

        size_t Capacity(void) {
            return hashes[0]->Capacity();
        }

        CountingBloomFilter<Key_t> *GetCBF(int cid) {
            return cbfs[cid];
        }

        Value_t Allocate(int cid, int qid) {
            return pool->Allocate(cid, qid);
        }

        void Free(Value_t value, int cid, int qid) {
            pool->Free(value, cid, qid);
        }

    private:
        std::atomic<size_t> victim_shard_id{0};
        std::atomic<size_t> victim_qid{0};

        IHash **hashes;
        CountingBloomFilter<Key_t> **cbfs;
        PerCPUPool *pool;

        WorkerThread **workers;

        Logger *logger;
        CPUStats *cpu_stats;
        Stats *stats;
};

#endif // __KV_HPP__
