#ifndef __RDMA_MEM_H__
#define __RDMA_MEM_H__

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

struct rdma_mm {
	struct mm_info server_mm_info;
	struct ib_mr *mr;
};

#define SVR_MM_INFO(ctrl) 	(ctrl->mm->server_mm_info)

struct rdma_mm *rdma_mem_init(void);
void rdma_mem_exit(struct rdma_mm *);

#endif // __RDMA_MEM_H__
