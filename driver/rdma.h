#ifndef __RDMA_H__
#define __RDMA_H__

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/inet.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

#include "rdma_mem.h"
#include "hashtable.h"


enum qp_attr_config {
	QP_MAX_SEND_WR  = 4096,
	QP_MAX_RECV_WR  = QP_MAX_SEND_WR,
	CQ_NUM_CQES     = QP_MAX_SEND_WR,
	QP_MAX_SEND_SGE = 4,
	QP_MAX_RECV_SGE = QP_MAX_SEND_SGE,
	QP_MAX_INLINE   = 32,
	CONNECTION_TIMEOUT_MS = 60000,
	
	MAX_QPS_RECVS  = QP_MAX_RECV_WR - 1,
};

struct dcc_rdma_config_t {
	int num_data_qps;
	int num_qps;
	int num_msgs;
};
extern struct dcc_rdma_config_t rdma_config;

struct rdma_dev {
	struct ib_device *dev;
	struct ib_pd *pd;
};

struct rdma_req {
	struct completion done;
	struct ib_cqe cqe;
	u64 dma;
	struct page *page;
};

struct dcc_rdma_ctrl;

struct rdma_queue {
	int id;
	int msg_id;
	atomic_t at_msg_id;
 	atomic64_t key_in_process;

	spinlock_t lock;

	struct ib_cq *send_cq;
	struct ib_cq *recv_cq;

	struct ib_qp *qp;
	
	struct dcc_rdma_ctrl *ctrl;

	int cm_error;
	struct completion cm_done;
	struct rdma_cm_id *cm_id;
};

struct dcc_rdma_ctrl {
	int id;

	struct rdma_queue *queues;
	struct rdma_dev *rdev;
	struct rdma_mm *mm;
		
	hashtable_t *ht;	
	
	union {
		struct sockaddr addr;
		struct sockaddr_in addr_in;
	};

	union {
		struct sockaddr src_addr;
		struct sockaddr_in src_addr_in;
	};
};


static inline struct rdma_queue *get_rdma_queue(struct dcc_rdma_ctrl *ctrl,
		unsigned int cpu_id)
{
	return &ctrl->queues[cpu_id]; /* per-cpu qp */
}

extern struct kmem_cache *req_cache;
/* allocates a dcc rdma request, creates a dma mapping for it in
 * req->dma, and synchronizes the dma mapping in the direction of
 * the dma map.
 * Don't touch the page with cpu after creating the request for it!
 * Deallocates the request if there was an error */
static inline int get_req_for_page(struct rdma_req **req, struct ib_device *dev,
		struct page *page, enum dma_data_direction dir)
{
	int ret;

	ret = 0;
	*req = kmem_cache_alloc(req_cache, GFP_ATOMIC);
	if (unlikely(!req)) {
		pr_err("no memory for req");
		ret = -ENOMEM;
		goto out;
	}

	(*req)->page = page;
	init_completion(&(*req)->done);

	(*req)->dma = ib_dma_map_page(dev, page, 0, PAGE_SIZE, dir);
	if (unlikely(ib_dma_mapping_error(dev, (*req)->dma))) {
		pr_err("ib_dma_mapping_error");
		ret = -ENOMEM;
		kmem_cache_free(req_cache, req);
		goto out;
	}

	ib_dma_sync_single_for_device(dev, (*req)->dma, PAGE_SIZE, dir);
out:
	return ret;
}

/* the buffer needs to come from kernel (not high memory) */
inline static int get_req_for_buf(struct rdma_req **req, struct ib_device *dev,
		void *buf, size_t size, enum dma_data_direction dir)
{
	int ret;

	ret = 0;
	*req = kmem_cache_alloc(req_cache, GFP_ATOMIC);
	if (unlikely(!req)) {
		pr_err("no memory for req");
		ret = -ENOMEM;
		goto out;
	}

	init_completion(&(*req)->done);

	(*req)->dma = ib_dma_map_single(dev, buf, size, dir);
	if (unlikely(ib_dma_mapping_error(dev, (*req)->dma))) {
		pr_err("ib_dma_mapping_error");
		ret = -ENOMEM;
		kmem_cache_free(req_cache, req);
		goto out;
	}

	ib_dma_sync_single_for_device(dev, (*req)->dma, size, dir);
out:
	return ret;
}

static inline void __poll_cq(struct ib_wc *wc, struct ib_cq *cq) 
{
	int ne;
	
	do {
		ne = ib_poll_cq(cq, 1, wc);
		if (ne < 0) {
			pr_err("failed to poll cq");
		}
	} while (ne < 1);

	if (wc->status != IB_WC_SUCCESS) {
		pr_err("%s: failed status %s(%d) for wr_id %llu", 
				__func__, ib_wc_status_msg(wc->status), 
				wc->status, wc->wr_id);
	}
}

static inline int post_recv(struct rdma_queue *q, u64 dma_addr, size_t buf_size) 
{
	struct ib_sge sge = {};
	struct ib_recv_wr wr = {
		.next    = NULL,
		.wr_id   = 0,
		.sg_list = &sge,
		.num_sge = !dma_addr ? 0 : 1,
	};
	const struct ib_recv_wr* bad_wr;
	int ret;

	if (dma_addr) {
		sge.addr = dma_addr;
		sge.length = buf_size;
		sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;
	}
	
	ret = ib_post_recv(q->qp, &wr, &bad_wr);
	if (unlikely(ret)) {
		pr_err("ib_post_recv failed");
	}

	return ret;
}

static inline int dcc_rdma_parse_ipaddr(struct sockaddr_in *saddr, char *ip)
{
	u8 *addr = (u8 *)&saddr->sin_addr.s_addr;
	size_t buflen = strlen(ip);

	if (buflen > INET_ADDRSTRLEN)
		return -EINVAL;
	if (in4_pton(ip, buflen, addr, '\0', NULL) == 0)
		return -EINVAL;
	saddr->sin_family = AF_INET;
	return 0;
}

static inline void dcc_rdma_stop_queue(struct rdma_queue *q)
{
	if (rdma_disconnect(q->cm_id)) {
		pr_err("Q%d: failed to rdma_disconnect()", q->id);
	}
	msleep(1);
}

static inline void dcc_rdma_free_queue(struct rdma_queue *q)
{	
	rdma_destroy_qp(q->cm_id);
	ib_free_cq(q->send_cq);	
	ib_free_cq(q->recv_cq);	
	rdma_destroy_id(q->cm_id);
}

static inline void dcc_rdma_stopandfree_queues(struct dcc_rdma_ctrl *ctrl)
{
	int i;
	
	for (i = 0; i < rdma_config.num_qps; ++i) {
		dcc_rdma_stop_queue(&ctrl->queues[i]);
		dcc_rdma_free_queue(&ctrl->queues[i]);
	}
	pr_err("%s: id=%d", __func__, ctrl->id);
}


int dcc_rdma_write_page(struct dcc_rdma_ctrl *, struct page *, u64, u64);
int dcc_rdma_read_page(struct dcc_rdma_ctrl *, struct page *, u64, u64);

int dcc_rdma_init(void); 
void dcc_rdma_exit(struct dcc_rdma_ctrl *, bool);

#endif // __RDMA_H__
