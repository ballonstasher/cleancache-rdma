#include <assert.h>
#include <stdint.h>
#include <stdlib.h>     /* malloc, free */
#include <algorithm>    //max
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#include "percpu_pool.hpp"

using namespace std; 

//#define DCC_MEMPOOL_DEBUG
#ifdef DCC_MEMPOOL_DEBUG
#define dcc_mempool_debug(str, ...) printf("%s: " str, __func__, __VA_ARGS__)
#else 
#define dcc_mempool_debug(str, ...) do {} while (0)  
#endif

static UniformDist pool_uni_dist(0, ULONG_MAX);

PerCPUPool::PerCPUPool(void *start_addr) {
    total_fl_ = config.num_clients * rdma_config.num_data_qps;
    per_cli_fl_ = rdma_config.num_data_qps;

    start_addr_ = (uintptr_t) start_addr;
    
    stat_start_time_ = 0;

    allocs_ = new atomic<size_t>[total_fl_]();
    useds_ = new atomic<size_t>[total_fl_]();
    
    // maximum freelist size
    const int64_t max_chunks = config.mem_pool_size / SLOT_SIZE; 
    const int64_t init_chunks = config.mem_pool_size / SLOT_SIZE / total_fl_;
    uintptr_t addr = start_addr_;
    
    freelist_ = new FreeList *[total_fl_];
    for (int i = 0; i < total_fl_; i++) {
        freelist_[i] = new FreeList(addr, max_chunks);

        for (int64_t j = init_chunks - 1; j >= 0; j--) {
            freelist_[i]->PutFreeAddr(addr + ((uintptr_t)j * SLOT_SIZE));
            allocs_[i] += SLOT_SIZE;
        }
        addr += (init_chunks * SLOT_SIZE);
    }

    interval_ = 100; // (ms)
    alloc_thold_perc_ = 10;
    free_thold_perc_ = 5;
    min_fl_alloc_ = (init_chunks * SLOT_SIZE) * alloc_thold_perc_ / 100; 
    min_fl_free_ = (init_chunks * SLOT_SIZE) * free_thold_perc_ / 100;

    printf("PerCPUPool: addr=%p, mem_pool_size(MB)=%lu\n"
            "\tinit_chunks=%ld, num_cpus=%d, num_clients=%d\n"
            "\tmin_percpu_alloc(MB)=%lu, min_percpu_free(MB)=%lu\n", 
            start_addr, config.mem_pool_size / (1 << 20), 
            init_chunks, total_fl_, config.num_clients, 
            min_fl_alloc_ / (1 << 20), min_fl_free_ / (1 << 20));

    const char *filename = "percpu_pool.log";
    char *log_file = (char *)malloc(strlen(_LOGDIR_) + strlen(filename));
    memset(log_file, 0x00, strlen(_LOGDIR_) + strlen(filename));
    strcat(log_file, _LOGDIR_);
    strcat(log_file, filename);

    logger_ = new Logger(LOG_LEVEL_FATAL, log_file, true);
    free(log_file);
}

PerCPUPool::~PerCPUPool() {
    /* TODO: */
}

// Find the client with the most free pages
int PerCPUPool::SelectByFreeMem(int cur_cli_id) {
    int tgt_cli_id = cur_cli_id;
    int next_cli_id = ++cur_cli_id % config.num_clients;
    size_t max_free = 0;
    size_t cli_alloc, cli_thld_alloc;
    size_t cli_free, cli_thld_free;
    size_t cli_avail_free;
    bool alloc_cond, free_cond;

    cli_thld_alloc = min_fl_alloc_ * per_cli_fl_;

    for (int i = 0; i < config.num_clients - 1; i++) {
        cli_alloc = GetClientAlloc(next_cli_id);
        cli_free = GetClientFree(next_cli_id);
        /* next client's 1st free list's thld x per_cli_fl_ */
        cli_thld_free = GetThldFree(next_cli_id * per_cli_fl_) * per_cli_fl_;
        
        alloc_cond =  cli_alloc / (1 << 20) > cli_thld_alloc / (1 << 20);
        free_cond = cli_free / (1 << 20) > cli_thld_free / (1 << 20);
        
        if (alloc_cond && free_cond) {
            cli_avail_free = cli_free - cli_thld_free;
            if (cli_avail_free / (1 << 20) > max_free / (1 << 20)) {
                max_free = cli_avail_free; 
                tgt_cli_id = next_cli_id;         
            }
        }
        next_cli_id = ++next_cli_id % config.num_clients;
    }

    return tgt_cli_id;
}

/* Greedy way to find victim peer */
int PerCPUPool::SelectByGreedy(int cur_cli_id) {
    int next_cli_id = ++cur_cli_id % config.num_clients;
    bool alloc_cond, free_cond;

    for (int i = 0; i < config.num_clients - 1; i++) {
        alloc_cond = GetClientAlloc(next_cli_id) / (1 << 20) > 
            min_fl_alloc_ * per_cli_fl_  / (1 << 20);
        free_cond = GetClientFree(next_cli_id)  / (1 << 20) > 
            min_fl_free_ * per_cli_fl_ / (1 << 20);

        if (alloc_cond && free_cond)
            break;

        next_cli_id = ++next_cli_id % config.num_clients;
    }

    return next_cli_id;
}

void *PerCPUPool::AllocateFromPeer(int fl_id) {
    uintptr_t free_addr = -1; 
    int cur_fl_id = fl_id;
    int cur_cli_id = fl_id / per_cli_fl_;
    int next_cli_id;
    bool alloc_cond, free_cond;
    int num_iter = per_cli_fl_ == 1 ? 1 : per_cli_fl_ / 2;

    if (1) {
        next_cli_id = SelectByFreeMem(cur_cli_id);
    } else {
        next_cli_id = SelectByGreedy(cur_cli_id);
    }

    /* 
     * Cannot found peer, peer also does not have sufficient free pages
     * Let's evict cache
     */
    if (cur_cli_id == next_cli_id) {
        return (void *) -1;
    }

    int fl_offset = cur_fl_id % per_cli_fl_;
    int peer_fl_id = (next_cli_id * per_cli_fl_) + fl_offset;
    int max_fl_id = (next_cli_id * per_cli_fl_) + per_cli_fl_;

    /* Try to borrow free page from peer */
    for (int i = 0; i < num_iter; i++) {
        /* Check condition of per-cpu free list */
        alloc_cond = GetAlloc(peer_fl_id) / (1 << 20) >
            min_fl_alloc_ / (1 << 20);
        free_cond = GetFree(peer_fl_id) / (1 << 20) > 
            min_fl_free_ / (1 << 20);

        if (alloc_cond && free_cond) {
            free_addr = freelist_[peer_fl_id]->GetFreeAddr();
            if (free_addr != -1) {
                allocs_[peer_fl_id] -= SLOT_SIZE;
                allocs_[cur_fl_id] += SLOT_SIZE;
                freelist_[cur_fl_id]->PutFreeAddr(free_addr);
                break;
            }
            
            /* first qid of client */
            if (peer_fl_id == (max_fl_id - 1))
                peer_fl_id = next_cli_id * per_cli_fl_;
            else
                peer_fl_id++;
        }
    }

    return (void *) free_addr;
}

/* Client-local allocation, 1. current cpu, 2. other cpu */ 
void *PerCPUPool::Allocate(int cid, int qid) {
    int fl_id;
    int max_fl_id = (cid * per_cli_fl_) + per_cli_fl_;
    uintptr_t free_addr = -1; 
    int num_iter = per_cli_fl_ == 1 ? 1 : per_cli_fl_ / 2;
    
    if (qid == -1)
        fl_id = (cid * per_cli_fl_) + (pool_uni_dist.GetRand() % per_cli_fl_);
    else 
        fl_id = cid * per_cli_fl_ + qid;

    /* try to allocate next freelist in local (lookup next 2 freelists) */
    for (int i = 0; i < num_iter; i++) {
        free_addr = freelist_[fl_id]->GetFreeAddr();
        if (free_addr != -1) {
            useds_[fl_id] += SLOT_SIZE;
            break;
        }
        
        /* next fl_id */
        if (fl_id == (max_fl_id - 1))
            fl_id = cid * per_cli_fl_;
        else
            fl_id++;
    }

#if 0
    if (free_addr == -1) {
        printf("%d:%d, orig:%d,cur:%d free_addr=-1\n", cli_id, qid, 
                (cli_id * rdma_config.num_data_qps) + qid, fl_id);
    }
#endif

    return (void *) free_addr;
}

void PerCPUPool::Free(void *addr, int cid, int qid) {
    int fl_id; 
    
    if (qid == -1)
        fl_id = (cid * per_cli_fl_) + (pool_uni_dist.GetRand() % per_cli_fl_);
    else
        fl_id = cid * per_cli_fl_ + qid; 

    freelist_[fl_id]->PutFreeAddr((uintptr_t)addr);

    useds_[fl_id] -= SLOT_SIZE;
}

void PerCPUPool::ShowStats() {
    uint64_t elapsed;
    struct timespec ts;
    
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (stat_start_time_ == 0) {
        elapsed = 0;
        stat_start_time_ = ts.tv_nsec + ts.tv_sec * 1e9; 
    } else {
        uint64_t cur_time = ts.tv_nsec + ts.tv_sec * 1e9; 
        elapsed = (cur_time - stat_start_time_) / 1e9;
    }

    for (int i = 0; i < config.num_clients; i++) {
        logger_->info("[Cli%d] Util(%%)=%.1f, Used(MB)=%lu, "
                "Free(MB)=%lu, Alloc(MB)=%lu", 
                i, GetClientUtil(i), GetClientUsed(i) / (1 << 20), 
                GetClientFree(i) / (1 << 20), GetClientAlloc(i) / (1 << 20));
#if 0
        for (int j = 0; j < per_cli_fl_; j++) {
            int fl_id = (i * per_cli_fl_) + j;
            logger_->info("[Q%d] Used(MB)=%lu, Free(MB)=%lu, Alloc(MB)=%lu", 
                    j, GetUsed(fl_id) / (1 << 20), 
                    GetFree(fl_id) / (1 << 20),
                    GetAlloc(fl_id) / (1 << 20));
        }
#endif
    }
    logger_->info("[Total] Util(%%)=%.1f, Used(MB)=%lu, Free(MB)=%lu\n", 
            GetUtil(), GetUsed() / (1 << 20), GetFree() / (1 << 20));
}
