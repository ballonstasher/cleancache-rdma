#ifndef __RDMA_HPP__
#define __RDMA_HPP__

#include <mutex>
#include <condition_variable>

#include "rdma_mem.hpp"

#define PAGE_SIZE 4096

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
    MAX_QPS_RECVS  = QP_MAX_RECV_WR - 1
};

#define MAX_POLL_WC (CQ_NUM_CQES / 2)

struct dcc_rdma_config_t {
    int num_get_qps;
    int num_data_qps;
    int num_filter_qps;
    int num_qps;
    int num_msgs;
    size_t get_metadata_size;
    size_t inv_metadata_size;
    size_t metadata_size;
    size_t metadata_mr_size;
};
extern struct dcc_rdma_config_t rdma_config;

struct rdma_dev {
    struct ibv_pd *pd;
    struct ibv_context *verbs;
};

struct dcc_rdma_ctrl;

struct rdma_queue {
    struct dcc_rdma_ctrl *ctrl;
    
    int id;
    struct rdma_cm_id *cm_id;
    struct ibv_comp_channel *channel;
    struct ibv_cq *event_cq;
    struct ibv_qp *qp;
    enum {
        INIT,
        CONNECTED
    } state;
};

struct dcc_rdma_ctrl {
    int id; 

    struct rdma_queue *queues;
    struct rdma_dev *rdev;
    
    RdmaMem *mm;
};

struct dcc_clients {
    struct dcc_rdma_ctrl *ctrl;
};

static inline void die(const char *reason) {
    fprintf(stderr, "%s - errno: %d\n", reason, errno);
    exit(EXIT_FAILURE);
}

#define TEST_NZ(x) 												\
    do { 														\
        if ((x))												\
        die("error: " #x " failed (returned non-zero)." ); 	    \
    } while (0)
#define TEST_Z(x)  												\
    do { 														\
        if (!(x)) 												\
        die("error: " #x " failed (returned zero/null)."); 	    \
    } while (0)


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

static inline uint32_t bit_mask(int mid, int msg, int tx, int etc, int crc)
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

static inline void bit_unmask(uint32_t target, int *mid, int *msg, int *tx, 
        int *etc, int *crc)
{
	*mid    = (int) ((target >> IMM_MID_SHIFT)  & IMM_MID_MASK);
	*msg    = (int) ((target >> IMM_MSG_SHIFT)  & IMM_MSG_MASK);
	*tx     = (int) ((target >> IMM_TX_SHIFT)   & IMM_TX_MASK);
	if (etc)
		*etc    = (int) ((target >> IMM_ETC_SHIFT)  & IMM_ETC_MASK);
	if (crc)
		*crc    = (int) (target & IMM_CRC_MASK);
}


static inline uint64_t KEY_META_REGION(uint64_t addr, int qid, int mid, 
        int type) {
    switch (type) {
        case MSG_READ:
            return addr + (rdma_config.metadata_size * qid);
        case MSG_INV_PAGE:
            return addr + (rdma_config.metadata_size * qid) + 
                rdma_config.get_metadata_size + mid * sizeof(uint64_t);
        default:
            fprintf(stderr, "Unknown msg type: %d\n", type);
    }
    return -1;
}

static inline uint64_t ADDR_META_REGION(uint64_t addr, int qid, int mid) {
    return KEY_META_REGION(addr, qid, mid, MSG_READ) + sizeof(uint64_t);
}


int run_rdma(int, int);
struct dcc_rdma_ctrl *init_rdma(int);
int exit_rdma();
void rdma_show_stats();


#endif // __RDMA_HPP__
