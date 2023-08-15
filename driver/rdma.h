#ifndef __RDMA_H__
#define __RDMA_H__

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/inet.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

#include "rdma_mem.h"
#include "hashtable.h"

#define MAX_NUM_SVRS 8

enum msg_type {
	MSG_WRITE_REQUEST = 1,
	MSG_WRITE_REQUEST_REPLY,
	MSG_WRITE,
	MSG_WRITE_REPLY,

	MSG_READ_REQUEST,
	MSG_READ_REQUEST_REPLY,
	MSG_READ,
	MSG_READ_REPLY,

	MSG_INV_PAGE,
	MSG_INV_PAGE_REPLY,
};

enum tx_state {				
	TX_WRITE_BEGIN = 1,
	TX_WRITE_READY,
	TX_WRITE_COMMITTED,		
	TX_WRITE_ABORTED,

	TX_READ_BEGIN,
	TX_READ_READY,
	TX_READ_COMMITTED,
	TX_READ_ABORTED,

	TX_INV_PAGE_BEGIN,
	TX_INV_PAGE_COMMITED,
	TX_INV_PAGE_ABORTED,
};

enum filter_state {
	FILTER_UPDATE_BEGIN = 100,
	FILTER_UPDATE_END
};

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
	int num_get_qps;
	int num_data_qps;
	int num_filter_qps;
	int num_qps;
	int num_msgs;
	int num_put_batch;
	size_t get_metadata_size;
	size_t inv_metadata_size;
	size_t metadata_size;
	size_t metadata_mr_size;
};
extern struct dcc_rdma_config_t rdma_config;

struct dcc_metadata {
	u64 key;
	u64 raddr;
};

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

struct work_data {
	struct work_struct work;
	struct rdma_queue *q;
	uint64_t key;
	int msg_id;
};

struct rdma_queue {
	int id;
	int msg_id;

	spinlock_t lock;
	struct mutex mtx;

	struct ib_cq *event_cq;
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

	unsigned long wait;
	
	unsigned long remote_enabled;
	unsigned long disabled_when;

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
		unsigned int i)
{
	return &ctrl->queues[i];
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

enum imm_bit_type {
	IMM_MID_BIT             = 12,
	IMM_MSG_BIT             = 4,
	IMM_TX_BIT              = 4,
	IMM_ETC_BIT             = 4,
	IMM_CRC_BIT             = 8,
	/* bit sum must be 32 */
	IMM_MID_SHIFT           = 32 - IMM_MID_BIT,
	IMM_MSG_SHIFT           = IMM_MID_SHIFT - IMM_MSG_BIT,
	IMM_TX_SHIFT            = IMM_MSG_SHIFT - IMM_TX_BIT,
	IMM_ETC_SHIFT           = IMM_TX_SHIFT - IMM_ETC_BIT,

	IMM_MID_MASK            = (1 << IMM_MID_BIT) - 1,
	IMM_MSG_MASK            = (1 << IMM_MSG_BIT) - 1,
	IMM_TX_MASK             = (1 << IMM_TX_BIT) - 1,
	IMM_ETC_MASK            = (1 << IMM_ETC_BIT) - 1,
	IMM_CRC_MASK            = (1 << IMM_CRC_BIT) - 1
};

static inline uint32_t bit_mask(int mid, int msg, int tx, int etc,
		int crc)
{
	uint32_t target = (
			((uint32_t) mid << IMM_MID_SHIFT) |
			((uint32_t) msg << IMM_MSG_SHIFT) |
			((uint32_t) tx  << IMM_TX_SHIFT)  |
			((uint32_t) etc << IMM_ETC_SHIFT) |
			((uint32_t) crc)
			);
	return target;
}

static inline void bit_unmask(uint32_t target, int *mid, int *msg, 
		int *tx, int *etc, int *crc)
{
	*mid    = (int) ((target >> IMM_MID_SHIFT)  & IMM_MID_MASK);
	*msg    = (int) ((target >> IMM_MSG_SHIFT)  & IMM_MSG_MASK);
	*tx     = (int) ((target >> IMM_TX_SHIFT)   & IMM_TX_MASK);
	if (etc)
		*etc    = (int) ((target >> IMM_ETC_SHIFT)  & IMM_ETC_MASK);
	if (crc)
		*crc    = (int) (target & IMM_CRC_MASK);
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

static inline void pre_post_recvs(struct dcc_rdma_ctrl *ctrl) 
{
	int i, j;

	for (i = 0; i < rdma_config.num_qps ; i++) {
		for (j = 0; j < MAX_QPS_RECVS; j++) {
			post_recv(&ctrl->queues[i], 0, 0);
		}
	}
}

static inline u64 GET_METADATA_OFFSET(int queue_id, int msg_id, int msg_type) 
{
	switch (msg_type) {
		case MSG_READ:
			return rdma_config.metadata_size * queue_id;
		case MSG_INV_PAGE:
			return rdma_config.metadata_size * queue_id + 
				rdma_config.get_metadata_size + 
				msg_id * sizeof(u64);
		default:
			pr_err("Unknown msg type: %d", msg_type);
	}
	return -1;
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
	if (q->id < rdma_config.num_data_qps) {
#if 1
		ib_free_cq(q->send_cq);	
		ib_free_cq(q->recv_cq);	
#else
		ib_destroy_cq(q->send_cq);	
		ib_destroy_cq(q->recv_cq);	
#endif
	} else {
#if 0
		ib_free_cq(q->event_cq);
#else
		ib_destroy_cq(q->event_cq);
#endif
	}
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
int dcc_rdma_send_page(struct dcc_rdma_ctrl *, struct page *, u64);
int dcc_rdma_write_msg(struct dcc_rdma_ctrl *, struct page *, u64);
int dcc_rdma_write_msg2(struct dcc_rdma_ctrl *, struct page *, u64);
int dcc_rdma_send_msg(struct rdma_queue *, u64, u32, u32);

int dcc_rdma_init(void); 
void dcc_rdma_exit(struct dcc_rdma_ctrl *, bool);

#endif // __RDMA_H__
