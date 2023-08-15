#ifndef __TIMER_H__
#define __TIMER_H__

enum timer_name_t {
	dcc_put_page_t,
	dcc_get_page_t,
	dcc_inv_page_t,

	rdma_put_t,
	rdma_put_alloc_t,
	rdma_put_cache_t,
	rdma_put_index_t,
	rdma_put_comm_t,

	rdma_get_t,
	rdma_get_alloc_t,
	rdma_get_cache_t,
	rdma_get_index_t,
	rdma_get_index2_t,
	rdma_get_comm_t,

	rdma_inv_page_t,
	rdma_inv_page_index_t,
	rdma_inv_page_comm_t,

	NUM_TS
};

extern ktime_t start_ts[NUM_TS];
extern u64 elapsed_ts[NUM_TS];

#ifdef DCC_TIME_CHECK
static inline void DCC_SST(int num) 
{
	start_ts[num] = ktime_get();
} 

static inline void DCC_EST(int num) 
{
	elapsed_ts[num] += 
		(u64)ktime_to_ns(ktime_sub(ktime_get(), start_ts[num]));
}
#else 
static inline void DCC_SST(int num) {} 
static inline void DCC_EST(int num) {} 
#endif

#if defined RDMA_TIME_CHECK
static inline void RDMA_SST(int num) 
{
	start_ts[num] = ktime_get();
} 

static inline void RDMA_EST(int num) 
{
	elapsed_ts[num] += 
		(u64)ktime_to_ns(ktime_sub(ktime_get(), start_ts[num]));
}
#else 
static inline void RDMA_SST(int num) {} 
static inline void RDMA_EST(int num) {} 
#endif 

static inline u64 get_elapsed(int num) 
{
	return elapsed_ts[num];
}

static inline u64 get_elapsed_us(int num) 
{
	return elapsed_ts[num]/1000;
}

#ifdef COUNT_CHECK
static inline void COUNT(u64 *cnt) 
{
	++(*cnt);
}
#else 
static inline void COUNT(u64 *cnt) {} 
#endif

#endif // __TIMER_H__
