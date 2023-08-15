#include <iostream>
#include <thread>

#include <unistd.h>
#include <cstring>

#include "kv.hpp"
#include "rdma.hpp"
//#include "hash-table/chaining.hpp"
//#include "hash-table/chaining_approx.hpp"
//#include "hash-table/linear_probing.hpp"
#include "hash-table/linear_probing_approx.hpp"
#include "config.hpp"

using namespace std;

//#define DCC_KV_DEBUG
#ifdef DCC_KV_DEBUG
#define dcc_kv_debug(str, ...) printf("%s: " str, __func__, __VA_ARGS__)
#else 
#define dcc_kv_debug(str, ...) do {} while (0)  
#endif

static inline int get_cpu_id(int cid, int qid) {
    int cpu_id; 
    if (config.poller_type == PER_QP_POLLER) {
        cpu_id = cid * rdma_config.num_data_qps + qid; 
    } else {
        cpu_id = qid; 
    }
    return cpu_id;
}

static inline int get_ht_shard_id(Key_t key) {
    if (config.num_ht_shards > 1)
        return hash_funcs[0](&key, sizeof(Key_t), f_seed) % config.num_ht_shards;
    else 
        return 0;
}

KV::KV(void *ptr) {
    /* Hash table init */
    size_t cap = config.mem_pool_size / 4096 * 110 / 100; /* prevent resizing */
    if (cap % 8)
        cap = cap + (8 - (cap % 8));
    assert(!(cap % 8) && "cap is not multiple of 8\n");

    hashes = new IHash *[config.num_ht_shards];
    for (int i = 0; i < config.num_ht_shards; i++) {
        switch (config.ht_type) {
            case HT_CH:
                //hashes[i] = new Chaining(cap);
                break;
            case HT_CH_APPROX:
                //hashes[i] = new ChainingApprox(cap);
                break;
            case HT_LP:
                //hashes[i] = new LinearProbing(cap);
                break;
            case HT_LP_APPROX:
                hashes[i] = new LinearProbingApprox(cap);
                break;
            default:
                printf("Unknown hash table type: %d\n", config.ht_type);
        }
    }

    /* Pool init */
    pool = new PerCPUPool(ptr);
    for (int i = 0; i < config.num_clients; i++) {
        for (int j = 0; j < rdma_config.num_data_qps; j++) {
            std::thread evictor(&KV::Evictor, this, i, j);
            evictor.detach();
        }       
    }

    /* CBF init */
    if (config.cbf_on) {
        unsigned int num_hash, num_bits;
        double fpp = calc_filter_param(cap, num_hash, num_bits);

        cbfs = new CountingBloomFilter<Key_t> *[config.num_clients];
        for (int i = 0; i < config.num_clients; i++)
            cbfs[i] = new CountingBloomFilter<Key_t>(num_hash, num_bits);

        printf("Per-client CBF: num_hash=%d, num_bits=%u\n"
                "cbf size(KB)=%u, bf size(KB)=%lu, fpp:%.3f)\n", 
                cbfs[0]->GetNumHash(), cbfs[0]->GetNumBits(), 
                cbfs[0]->GetNumBits() * 1 / (1 << 10), 
                cbfs[0]->GetNumLongs() * 8 / (1 << 10), 
                fpp); 
    }

    /* Logfile Init */
    const char *filename = "kv.log";
    char *log_file = (char *) malloc(strlen(_LOGDIR_) + strlen(filename));
    memset(log_file, 0x00, strlen(_LOGDIR_) + strlen(filename));
    strcat(log_file, _LOGDIR_);
    strcat(log_file, filename);

    logger = new Logger(LOG_LEVEL_FATAL, log_file, true);
    free(log_file);

    stats = new Stats(KV_STATS);
}

void KV::Insert(Key_t &key, Value_t value, int cid, int qid) {
    int shard_id = get_ht_shard_id(key);
    int cpu_id = get_cpu_id(cid, qid);
    int ret; 

RETRY:
    Key_t ev_key = INVALID;
    Value_t ev_value = NONE;

    ret = hashes[shard_id]->Insert(key, value, ev_key, ev_value);

    if (ev_key != INVALID && ev_value) {
        /* case1: eviction + failed insert */
        int ev_cid = longkey_to_cid(ev_key);
        pool->Free(ev_value, ev_cid, -1);

        if (!ret) {
            if (config.cbf_on) 
                cbfs[ev_cid]->Remove(ev_key);
            stats->Inc(KV_EVICTED_PUT, cpu_id);
        } else {
            stats->Inc(KV_FAILED_PUT, cpu_id);
        }
    } else if (ev_value) {
        if (config.exclusive_policy) {
            //fprintf(stderr, "%s: key is duplicated, "
            //        "key=%lu, value=%p, ev_key=%lu, ev_value=%p\n", 
            //        __func__, key, value, ev_key, ev_value);
            //throw std::runtime_error("KV::key is duplicated");
        }
        /* case2: duplicated key */
        if (ret)
            throw std::runtime_error("dup key but ret > 0");

        pool->Free(ev_value, cid, -1);
        stats->Inc(KV_UPDATED_PUT, cpu_id);
        goto RETRY;
    }

    if (!ret) {
        if (config.cbf_on)
            cbfs[cid]->Insert(key);
        stats->Inc(KV_SUCC_PUT, cpu_id);
    }
}

Value_t KV::Remove(Key_t &key, int cid, int qid) {
    Value_t value;
    int shard_id = get_ht_shard_id(key); 
    int cpu_id = get_cpu_id(cid, qid);

    value = hashes[shard_id]->Delete(key);
    if (value) {
        if (config.cbf_on) {
            cbfs[cid]->Remove(key);
        }
        stats->Inc(KV_SUCC_INV, cpu_id); 
    } else {
        stats->Inc(KV_FAILED_INV, cpu_id); 
    }
    return value;
}

Value_t KV::Get(Key_t &key, int cid, int qid) {
    int shard_id = get_ht_shard_id(key);

    return hashes[shard_id]->Get(key);
}

// inclusive
Value_t KV::GetWithFlag(Key_t &key, int cid, int qid, bool busy) {
    Value_t value;
    int shard_id = get_ht_shard_id(key);
    int cpu_id = get_cpu_id(cid, qid);

    value = hashes[shard_id]->GetWithFlag(key, busy);
    if (value) {
        stats->Inc(KV_SUCC_GET, cpu_id); 
    } else {
        stats->Inc(KV_FAILED_GET, cpu_id); 
    }
    return value; 
}

void KV::Evict(int cid, int qid) {
    Key_t key = INVALID;
    Value_t value;

    if (config.num_ht_shards == 1)
        value = hashes[0]->Evict(key);
    else
        value = hashes[victim_shard_id++ % config.num_ht_shards]->Evict(key);

    int ev_cid = longkey_to_cid(key); 
    /* fill free page to requester(client)*/
    pool->Free(value, ev_cid, -1);

    if (config.cbf_on)
        cbfs[ev_cid]->Remove(key);

    stats->Inc(KV_EVICTED_BG, get_cpu_id(ev_cid, qid));
}

void KV::Evictor(int cid, int qid) {
    int per_cli_fl = rdma_config.num_data_qps;
    int fl_id = (cid * per_cli_fl) + qid; 
    int interval = pool->GetEvictInterval();
    size_t thld_free = pool->GetThldFree(fl_id);
    int thld_evict_cnt, evict_cnt;

    if (qid == 0)
        printf("Evictor(%d:%d:%d) thld_free(MB)=%lu, interval(ms)=%d\n",
                cid, qid, fl_id, thld_free / (1 << 20), interval);

    while (true) {
        thld_free = pool->GetThldFree(fl_id);
        thld_evict_cnt = thld_free / 4096;
        evict_cnt = 0;

        if (!(thld_free / (1 << 20) > pool->GetFree(fl_id) / (1 << 20)))
            usleep(interval * 1000);

        /* MB unit */
        while (thld_free / (1 << 20) > pool->GetFree(fl_id) / (1 << 20)) {
            if (pool->AllocateFromPeer(fl_id) == (void *) -1) {
                Evict(cid, qid);
                evict_cnt++;
            }

            if (evict_cnt > thld_evict_cnt)
                break;
        }
    }
}

void KV::ShowStats(void) {
#ifdef DCC_COUNT_CHECK
    uint64_t succ_puts;
    uint64_t failed_puts;
    uint64_t updated_puts;
    uint64_t evicted_puts;
    uint64_t succ_gets;
    uint64_t failed_gets;
    uint64_t succ_invs;
    uint64_t failed_invs;

    logger->info("succ_puts=%lu, failed_puts=%lu, updated_puts=%lu "
            "evicted_puts=%lu",
            stats->Sum(KV_SUCC_PUT), stats->Sum(KV_FAILED_PUT), 
            stats->Sum(KV_UPDATED_PUT), stats->Sum(KV_EVICTED_PUT));
    logger->info("succ_gets=%lu, failed_gets=%lu", 
            stats->Sum(KV_SUCC_GET), stats->Sum(KV_FAILED_GET));
    logger->info("succ_invs=%lu, failed_invs=%lu", 
            stats->Sum(KV_SUCC_INV), stats->Sum(KV_FAILED_INV));
#endif
    pool->ShowStats();
}
