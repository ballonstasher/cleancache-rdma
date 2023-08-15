#include <infiniband/verbs.h>
#include <iostream>

#include "rdma_mem.hpp"
#include "rdma.hpp"
#include "config.hpp"

extern struct dcc_clients **clients;

int RdmaMem::Allocate(int cid, struct dcc_rdma_ctrl *ctrl) {
    ctrl->mm->GetDataMMPool()->ptr = aligned_alloc(8, config.mem_pool_size);
    ctrl->mm->GetDataMMPool()->size = config.mem_pool_size;
   
    return 0;
}

int RdmaMem::Register(struct rdma_queue *q) {
    struct dcc_rdma_ctrl *ctrl = q->ctrl;
    struct ibv_pd *pd = ctrl->rdev->pd;
    int access_flag = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_REMOTE_READ;
    struct mm_pool *data_mm_pool = ctrl->mm->GetDataMMPool();

    TEST_Z(ctrl->mm->GetDataMMPool()->mr = ibv_reg_mr(pd, data_mm_pool->ptr, 
                data_mm_pool->size, access_flag));

    printf("Data MR(GB): %lu\n", 
            data_mm_pool->size >> 30);

    return 0;
}

int RdmaMem::Deregister(struct dcc_rdma_ctrl *ctrl, bool last_flag) {
    if (last_flag) {
        ibv_dereg_mr(data_mm_pool.mr);
        free(data_mm_pool.ptr);
    } 
}
