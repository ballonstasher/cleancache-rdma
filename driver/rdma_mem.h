#ifndef __RDMA_MM_H__
#define __RDMA_MM_H__

#include <linux/types.h>

struct dcc_rdma_ctrl;

// 32 B
struct mm_info {
	u64 baseaddr;
	u32 key;
	u32 len;
	u32 cli_id;     // 20 B, order registered on the server
	u32 info[3];	// 32 B
};

struct mm_pool {
	void *ptr;
	size_t size;
	dma_addr_t dma_addr;
};

struct rdma_mm {
	struct mm_info server_mm_info;
	struct mm_info filter_mm_info;

	struct mm_pool filter_mm_pool; 
	struct mm_pool key_mm_pool;
	struct mm_pool pagebuf_mm_pool;
	struct mm_pool keypage_mm_pool;

	struct ib_mr *mr;
};

#define SVR_MM_INFO(ctrl) 	(ctrl->mm->server_mm_info)
#define FILTER_MM_INFO(ctrl) 	(ctrl->mm->filter_mm_info)
#define FILTER_MM_POOL(ctrl)	(ctrl->mm->filter_mm_pool)
#define KEY_MM_POOL(ctrl) 	(ctrl->mm->key_mm_pool)
#define PAGEBUF_MM_POOL(ctrl) 	(ctrl->mm->pagebuf_mm_pool)
#define KEYPAGE_MM_POOL(ctrl) 	(ctrl->mm->keypage_mm_pool)
#define MM_MR(ctrl)		(ctrl->mm->mr)

struct rdma_mm *rdma_mem_init(void);
void rdma_mem_exit(struct rdma_mm *);
int rdma_mem_register_region(struct dcc_rdma_ctrl *);
int rdma_mem_alloc_region(struct dcc_rdma_ctrl *);
void rdma_mem_free_region(struct dcc_rdma_ctrl *);

#endif // __RDMA_MM_H__
