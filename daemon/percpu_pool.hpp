#ifndef __PERCPU_POOL_HPP__ 
#define __PERCPU_POOL_HPP__

#include <climits>
#include <cstring>

#include "freelist.hpp"
#include "logger.hpp"
#include "util/random_dist.hpp"
#include "rdma.hpp"
#include "config.hpp"

struct slot {
    char data[4096]; 
    uint64_t key;
};

#define SLOT_SIZE (sizeof(struct slot))

class PerCPUPool {
    private:
        int total_fl_;
        int per_cli_fl_;
        uintptr_t start_addr_;
        std::atomic<size_t> *allocs_;
        std::atomic<size_t> *useds_;
    
        int interval_;
        int alloc_thold_perc_;
        int free_thold_perc_;
        size_t min_fl_alloc_;
        size_t min_fl_free_;

        uint64_t stat_start_time_; 
 
        FreeList **freelist_;
        Logger *logger_;

    public:
        PerCPUPool(void *);
        ~PerCPUPool();

        int SelectByFreeMem(int);
        int SelectByGreedy(int);
        void *AllocateFromPeer(int);

        void *Allocate(int, int);
        void Free(void *, int, int);
        
        void ShowStats(); 
    
        int GetEvictInterval() const {
            return interval_;
        }

        size_t GetThldFree(int fl_id) const {
#if 1
            int cur_thold = allocs_[fl_id] * free_thold_perc_ / 100;
            if (cur_thold > min_fl_free_)
                return cur_thold;
            else
                return min_fl_free_;
#else 
            return min_fl_free_;
#endif
        }

        uintptr_t GetStartAddr() const {
            return start_addr_;
        }
        
        size_t GetAlloc(int fl_id) const {
            return allocs_[fl_id].load();
        }

        size_t GetClientAlloc(int cli_id) {
            size_t alloc_sum = 0;

            for (int i = cli_id * per_cli_fl_; 
                    i < cli_id * per_cli_fl_ + per_cli_fl_; i++)
                alloc_sum += allocs_[i].load();
            return alloc_sum;
        }

        size_t GetUsed(int fl_id) const {
            return useds_[fl_id].load();
        }

        size_t GetClientUsed(int cli_id) {
            size_t used_sum = 0;

            for (int i = cli_id * per_cli_fl_; 
                    i < cli_id * per_cli_fl_ + per_cli_fl_; i++)
                used_sum += useds_[i].load();
            return used_sum;
        }

        size_t GetUsed() {
            size_t used_mem = 0;
            for (int i = 0; i < total_fl_; i++)
                used_mem += useds_[i].load();
            return used_mem;
        }

        size_t GetFree(int fl_id) {
            return allocs_[fl_id] - GetUsed(fl_id);
        }

        size_t GetClientFree(int cli_id) {
            return GetClientAlloc(cli_id) - GetClientUsed(cli_id);
        }

        size_t GetFree() {
            return config.mem_pool_size - GetUsed();
        }

        double GetClientUtil(int cli_id) {
            return 100.0 * GetClientUsed(cli_id) / GetClientAlloc(cli_id);
        }

        double GetUtil() {
            return 100.0 * GetUsed() / config.mem_pool_size;
        }

    private:
        PerCPUPool(PerCPUPool &allocator);
};

#endif // __PERCPU_POOL_HPP__
