#include <infiniband/verbs.h>

#include "rdma_mem.hpp"
#include "rdma.hpp"
#include "kv.hpp"
#include "config.hpp"

extern struct dcc_clients **clients;
extern KV *kv;

int RdmaMem::Allocate(int cid, struct dcc_rdma_ctrl *ctrl) {
    // twosided type shares data mem pool
    if (cid > 0) {
        ctrl->mm->GetDataMMPool()->ptr = 
            clients[0]->ctrl->mm->GetDataMMPool()->ptr; 
        ctrl->mm->GetDataMMPool()->size = 
            clients[0]->ctrl->mm->GetDataMMPool()->size; 
    } else {
        ctrl->mm->GetDataMMPool()->ptr = aligned_alloc(8, config.mem_pool_size);
        ctrl->mm->GetDataMMPool()->size = config.mem_pool_size;
    }
    
    ctrl->mm->GetMetaMMPool()->ptr = aligned_alloc(8, 
            rdma_config.metadata_mr_size);
    ctrl->mm->GetMetaMMPool()->size = rdma_config.metadata_mr_size;

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

    struct mm_region *client_mr = ctrl->mm->GetClientMR();
    struct mm_pool *meta_mm_pool = ctrl->mm->GetMetaMMPool();
    struct mm_pool *filter_mm_pool = ctrl->mm->GetFilterMMPool();

    TEST_Z(ctrl->mm->GetMetaMMPool()->mr = ibv_reg_mr(pd, 
                meta_mm_pool->ptr, meta_mm_pool->size, access_flag));

    /* XXX: only used once for get client info */
    TEST_Z(ctrl->mm->GetClientMR()->mr = ibv_reg_mr(pd, 
                (void *) &client_mr->info, sizeof(struct mm_info),
                access_flag)); 

    // XXX: move initialization
    ctrl->mm->GetFilterMMPool()->ptr = 
        (void *) kv->GetCBF(ctrl->id)->GetBFAddr();
    ctrl->mm->GetFilterMMPool()->size = 
        kv->GetCBF(ctrl->id)->GetNumLongs() * sizeof(unsigned long);
    TEST_Z(ctrl->mm->GetFilterMMPool()->mr = ibv_reg_mr(pd, 
                filter_mm_pool->ptr, filter_mm_pool->size, access_flag));

    printf("Data MR(GB): %lu, Meta MR(B): %lu, Client MR(B): %lu\n", 
            data_mm_pool->size >> 30, ctrl->mm->GetMetaMMPool()->size,
            sizeof(struct mm_info));

    return 0;
}

int RdmaMem::Deregister(struct dcc_rdma_ctrl *ctrl, bool last_flag) {
    ibv_dereg_mr(meta_mm_pool.mr);
    free(meta_mm_pool.ptr);

    if (last_flag) {
        ibv_dereg_mr(data_mm_pool.mr);
        free(data_mm_pool.ptr);
    } 
}

