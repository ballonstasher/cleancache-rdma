#ifndef __RDMA_HPP__
#define __RDMA_HPP__

#include "rdma_mem.hpp"

#define PAGE_SIZE 4096

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

struct dcc_rdma_config_t {
    int num_data_qps;
    int num_qps;
    int num_msgs;
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


int run_rdma(int, int);
struct dcc_rdma_ctrl *init_rdma(int);
int exit_rdma();


#endif // __RDMA_HPP__
