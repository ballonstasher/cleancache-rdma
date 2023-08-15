#ifndef __RDMA_MEM_HPP__
#define __RDMA_MEM_HPP__

#include <cinttypes>
#include <unistd.h>

struct dcc_rdma_ctrl;
struct rdma_queue;

// 32 B
struct mm_info {
    uint64_t baseaddr;
    uint32_t key;
    uint32_t len;       // 16 B 
    uint32_t cli_id;    // 20 B
};

struct mm_region {
    struct mm_info info;
    struct ibv_mr *mr;
};

struct mm_pool {
    void *ptr;
    size_t size;
    struct ibv_mr *mr;
};

class RdmaMem {
    public:
        RdmaMem() {};
        ~RdmaMem() {};
   
        int Allocate(int, struct dcc_rdma_ctrl *);
        int Register(struct rdma_queue *q);
        int Deregister(struct dcc_rdma_ctrl *, bool);

        struct mm_pool *GetDataMMPool() {
            return &data_mm_pool;
        }
        
    private:
        struct mm_pool data_mm_pool;
};

#endif // __RDMA_MEM_HPP__
