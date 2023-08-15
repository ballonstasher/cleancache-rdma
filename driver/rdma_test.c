#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/memory.h>
#include <linux/mm_types.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/delay.h>

#include "backend.h"

//#define MULTICLIENT_TEST

#define PUT_TEST
#if 1
#define GET_TEST
#else
#define INVALIDATE_PAGE_TEST
#endif
//#define HYBRID_TEST

/*********************************/
int num_threads = 1;
module_param(num_threads, int, 0644);
int input_size_mb = 1024;
module_param(input_size_mb, int, 0644);
size_t total_capacity;
unsigned int num_iters = 1;

struct page ***vpages;
struct completion *comp;
struct page **return_pages;
uint64_t **keys;
struct task_struct **put_threads, **get_threads, **inv_threads;
struct dentry *debug_dir, *debug_file;
int test_enable = 0;

int cli_id = 0;

struct thread_data {
	int tid;
	struct completion *comp;
	int ret;
	int ret2;
	bool dummy;
};

void check_debugfs(void) {
#ifdef CONFIG_DEBUG_FS
	int i;

	pr_info("[ INFO ] Wait 60 sec to get enable sign\n");
	for (i = 0; i < 60*20; i++) {
		if (test_enable > 0) {
			//pr_info("[ INFO ] Start test\n");
			break;
		}
		//ssleep(1);
		msleep(500);
		//udelay(1000);
	}
#endif
}


int rdpma_put_test(void *arg) 
{
	struct thread_data* my_data = (struct thread_data *)arg;
	int tid = my_data->tid;
	int i, ret = -1;
	uint32_t key;
	int num_repl = 0;
	
	for (i = 0; i < num_iters; i++) {
		if (my_data->dummy)
			key = (uint32_t)keys[tid][i] + (num_iters * num_threads);
		else
			key = (uint32_t)keys[tid][i];
		
		ret = dcc_handle_put_page(vpages[tid][i], 0, key);
		if (ret)
			num_repl++;
	} 
	complete(my_data->comp);

	if (!num_repl)
		ret = 0;
	else
		ret = num_repl;
	my_data->ret = ret;

	return ret;
}

int rdpma_get_test(void *arg) 
{
	struct thread_data* my_data = (struct thread_data *)arg;
	int tid = my_data->tid;
	int i, ret = -1;
	int value_failed = 0, not_found = 0;
	uint32_t key;

	for (i = 0; i < num_iters; i++) {
		key = keys[tid][i];
		
		ret = dcc_handle_get_page(return_pages[tid], 0, key);

		if (!ret) {
			if (memcmp(page_address(return_pages[tid]), 
						page_address(vpages[tid][i]),
						0)) {
				value_failed++;
			}
		} else {
			not_found++;
		}
	}

	complete(my_data->comp);
	my_data->ret = value_failed;
	my_data->ret2 = not_found;
	ret = value_failed + not_found;

	return ret;
}

int rdpma_inv_page_test(void *arg) 
{
	struct thread_data* my_data = (struct thread_data *)arg;
	int tid = my_data->tid;
	int i, ret = -1;
	int not_found = 0;
	uint32_t key;

	for (i = 0; i < num_iters; i++) {
		key = keys[tid][i];
		
		ret = dcc_handle_invalidate_page(0, key);
		if (ret)
			not_found++;
	}
	complete(my_data->comp);

	ret = not_found;
	my_data->ret = not_found;

	return ret;
}

void prepare_put_test(struct thread_data **args) 
{
	int i, ret = 0;
	u64 elapsed, per_req_lat;
	int elapsed_ms;
	ktime_t start; 

	for (i = 0; i < num_threads; i++) {
		args[i] = (struct thread_data *)
			kzalloc(sizeof(struct thread_data), GFP_KERNEL);
		args[i]->tid = i;
		args[i]->comp = &comp[i];
		args[i]->ret = 0;
	}

#ifdef MULTICLIENT_TEST
	check_debugfs();
#endif

	pr_info("Start running put thread functions...");
	
	start = ktime_get();
	for (i = 0; i < num_threads; i++) {
		put_threads[i] = kthread_create((void *)&rdpma_put_test, 
				(void *)args[i], "page_writer");
		kthread_bind(put_threads[i], i % num_online_cpus());
		wake_up_process(put_threads[i]);
	}

	for (i = 0; i < num_threads; i++)
		wait_for_completion(&comp[i]);
	elapsed = (u64)ktime_to_ns(ktime_sub(ktime_get(), start));

	for (i = 0; i < num_threads; i++)
		ret += args[i]->ret;

	per_req_lat = elapsed / num_threads / num_iters;
	pr_info("%s: remote put: per_req_lat: %llu (ns), %d failed", 
			!ret ? "PASS" : "FAIL", per_req_lat, ret);

	elapsed_ms = elapsed/1000/1000;
	if (elapsed_ms) {
		pr_info("remote put, elapsed: %d (ms), Throughput: %ld (MB/sec)", 
				elapsed_ms, 
				1000 * (total_capacity >> 20) / elapsed_ms);
	}
}

void prepare_get_test(struct thread_data **args) 
{
	int i, ret = 0, ret2 = 0;
	u64 elapsed, per_req_lat;
	int elapsed_ms;
	ktime_t start; 

	for (i = 0; i < num_threads; i++) {
		reinit_completion(&comp[i]);
		args[i]->comp = &comp[i];
		args[i]->ret = 0;
		args[i]->ret2 = 0;
	}

#ifdef MULTICLIENT_TEST
	check_debugfs();
#endif

	pr_info("Start running get thread functions...");

	start = ktime_get();
	for (i = 0; i < num_threads; i++) {
		get_threads[i] = kthread_create((void *)&rdpma_get_test, 
				(void *)args[i], "page_reader");
		kthread_bind(get_threads[i], i % num_online_cpus());
		wake_up_process(get_threads[i]);
	}

	for (i = 0; i < num_threads; i++)
		wait_for_completion(&comp[i]);
	elapsed = (u64)ktime_to_ns(ktime_sub(ktime_get(), start));

	for (i = 0; i < num_threads; i++) {
		ret += args[i]->ret;
		ret2 += args[i]->ret2;
	}

	per_req_lat = elapsed / num_threads / num_iters;
	pr_info("%s: remote get, per_req_lat: %llu (ns)\n"
			"(K:o, V:x): %d, (K:x, v:x): %d\n", 
			(!ret && !ret2) ? "PASS" : "FAIL", per_req_lat, 
			ret, ret2);

	elapsed_ms = elapsed/1000/1000;
	if (elapsed_ms) {
		pr_info("remote get, elapsed: %d (ms), Throughput: %ld (MB/sec)", 
				elapsed_ms, 
				1000 * (total_capacity >> 20) / elapsed_ms);
	}
}

// FIXME
void prepare_hybrid_test(struct thread_data **args) {
	int i, ret = 0;
	u64 elapsed, per_req_lat;
	int elapsed_ms;
	ktime_t start; 

	for (i = 0; i < num_threads; i++) {
		reinit_completion(&comp[i]);
		args[i]->comp = &comp[i];
		args[i]->ret = 0;
		args[i]->ret2 = 0;
		args[i]->dummy = true;
	}

	pr_info("Start running hybrid thread functions...");
	
	for (i = 0; i < num_threads / 2; i++) {
		put_threads[i] = kthread_create((void *)&rdpma_put_test, 
				(void *)args[i], "page_writer");
		wake_up_process(put_threads[i]);
	}

	start = ktime_get();
	for (i = num_threads / 2; i < num_threads; i++) {
		get_threads[i] = kthread_create((void*)&rdpma_get_test, 
				(void*)args[i], "page_reader");
		wake_up_process(get_threads[i]);
	}
	elapsed = (u64)ktime_to_ns(ktime_sub(ktime_get(), start));

	for (i = 0; i < num_threads; i++)
		wait_for_completion(&comp[i]);

	for (i = 0; i < num_threads; i++)
		ret += args[i]->ret;

	per_req_lat = elapsed / num_threads / num_iters;
	pr_info("%s: remote put: per_req_lat: %llu (ns), %d failed", 
			!ret ? "PASS" : "FAIL", per_req_lat, ret);

	elapsed_ms = elapsed/1000/1000;
	if (elapsed_ms) {
		pr_info("remote put, elapsed: %d (ms), Throughput: %ld (MB/sec)", 
				elapsed_ms, 
				1000 * (total_capacity >> 20) / elapsed_ms);
	}
}

void prepare_inv_page_test(struct thread_data **args) 
{
	int i, ret = 0;
	u64 elapsed, per_req_lat;
	int elapsed_ms;
	ktime_t start; 

	for (i = 0; i < num_threads; i++) {
		reinit_completion(&comp[i]);
		args[i]->comp = &comp[i];
		args[i]->ret = 0;
	}

	pr_info("Start running inv. thread functions...");
	start = ktime_get();
	for (i = 0; i < num_threads; i++) {
		inv_threads[i] = kthread_create((void *)&rdpma_inv_page_test, 
				(void *)args[i], "page_inv");
		kthread_bind(inv_threads[i], i % num_online_cpus());
		wake_up_process(inv_threads[i]);
	}

	for (i = 0; i < num_threads; i++)
		wait_for_completion(&comp[i]);
	elapsed = (u64)ktime_to_ns(ktime_sub(ktime_get(), start));

	for (i = 0; i < num_threads; i++)
		ret += args[i]->ret;

	per_req_lat = elapsed / num_threads / num_iters;
	pr_info("%s:remote inv. page, per_req_lat: %llu (ns),\n(K:x, v:x): %d", 
			(!ret) ? "PASS" : "FAIL", per_req_lat, ret);

	elapsed_ms = elapsed/1000/1000;
	if (elapsed_ms) {
		pr_info("remote inv. page, elapsed: %d (ms), "
				"Throughput: %ld (MB/sec)", 
				elapsed_ms, 
				1000 * (total_capacity >> 20) / elapsed_ms);
	}
}

int main(void) 
{
	int i;
	struct thread_data **args = (struct thread_data**)
		kzalloc(sizeof(struct thread_data *) * num_threads, GFP_KERNEL);
#ifdef PUT_TEST
	prepare_put_test(args);
#endif 

#ifdef GET_TEST
	ssleep(2);
	prepare_get_test(args);
#elif defined HYBRID_TEST
	ssleep(2);
	prepare_hybrid_test(args);
#elif defined INVALIDATE_PAGE_TEST
	ssleep(2);
	prepare_inv_page_test(args);
#endif

	for (i = 0; i < num_threads; i++)
		kfree(args[i]);
	kfree(args);

	return 0;
}

void exit_pages(void);

int init_pages(void) {
	int i, j;
	uint64_t key = 0;
	char str[8];

	put_threads = (struct task_struct **)
		kzalloc(sizeof(struct task_struct *) * num_threads, GFP_KERNEL);
	if(!put_threads){
		pr_err("put_threads allocation failed");
		goto ALLOC_ERR;
	}

	get_threads = (struct task_struct **)
		kzalloc(sizeof(struct task_struct *) * num_threads, GFP_KERNEL);
	if (!get_threads) {
		pr_err("get_threads allocation failed");
		goto ALLOC_ERR;
	}

	inv_threads = (struct task_struct **)
		kzalloc(sizeof(struct task_struct *) * num_threads, GFP_KERNEL);
	if (!inv_threads) {
		pr_err("inv_threads allocation failed");
		goto ALLOC_ERR;
	}

	return_pages = (struct page **) kzalloc(sizeof(void *) * num_threads, 
			GFP_KERNEL);
	if (!return_pages) {
		pr_err("return page allocation failed");
		goto ALLOC_ERR;
	}

	for (i = 0; i < num_threads; i++) {
		return_pages[i] = alloc_pages(GFP_KERNEL, 0);
		if (!return_pages[i]) {
			pr_err("return page[%d] allocation failed", i);
			goto ALLOC_ERR;
		}
		memset(page_address(return_pages[i]), 0x00, PAGE_SIZE);
	}

	vpages = (struct page ***)kzalloc(sizeof(struct page **) * num_threads, 
			GFP_KERNEL);
	if (!vpages) {
		pr_err("vpages allocation failed");
		goto ALLOC_ERR;
	}

	for (i = 0; i < num_threads; i++) {
		vpages[i] = (struct page **) vmalloc(sizeof(void *) * 
				num_iters * num_threads);
		if (!vpages[i]) {
			pr_err("vpages[%d] allocation failed\n", i);
			goto ALLOC_ERR;
		}
		for (j = 0; j < num_iters; j++) {
			vpages[i][j] = alloc_pages(GFP_KERNEL, 0);
			if (!vpages[i][j]) {
				pr_err("vpages[%d][%d] allocation failed", 
						i, j);
				goto ALLOC_ERR;
			}
			memset(page_address(vpages[i][j]), 0x00, PAGE_SIZE);

			sprintf(str, "%lld", ++key);
			strcpy(page_address(vpages[i][j]), str);
		}
	}

	keys = (uint64_t **) kzalloc(sizeof(uint64_t *) * num_threads, 
			GFP_KERNEL);
	key = 0;
	if (!keys) {
		printk(KERN_ALERT "keys allocation failed\n");
		goto ALLOC_ERR;
	}
	for (i = 0; i < num_threads; i++) {
		keys[i] = (uint64_t *) vzalloc(sizeof(uint64_t) * num_iters);
		for (j = 0; j < num_iters; j++) {
			keys[i][j] = ++key;
		}
	}

	return 0;

ALLOC_ERR:
	exit_pages();

	return -ENOMEM;
}

void exit_pages(void) 
{
	int i, j;
	if (put_threads) 
		kfree(put_threads);
	if (get_threads)
		kfree(get_threads);
	if (inv_threads)
		kfree(inv_threads);

	for (i = 0; i < num_threads; i++) {		
		if (return_pages[i]) 
			__free_pages(return_pages[i], 0);

		for (j = 0; j < num_iters; j++) {
			if (vpages[i][j]) 
				__free_pages(vpages[i][j], 0);
		}

		if (vpages[i]) 
			vfree(vpages[i]);

		if (keys[i])
			vfree(keys[i]);
	}

	if (return_pages) 
		kfree(return_pages);
	if (vpages) 
		kfree(vpages);
	if (keys)
		kfree(keys);
}

void show_test_info(void) 
{
	pr_info("+------------------ RDMA Page test ------------------+");
#if defined GET_TEST
	pr_info("Put => Get");
#elif defined HYBRID_TEST
	pr_info("Put => Hybrid(Put/Get)");
#elif defined INVALIDATE_PAGE_TEST
	pr_info("Put => Inv. page\n");
#endif
	pr_info("Number of threads       : %d", num_threads);
	pr_info("Total capacity          : %ld (MB)", total_capacity >> 20);
	pr_info("Per-thread iterations   : %d", num_iters);
	pr_info("Total iterations        : %d", num_iters * num_threads);
	pr_info("+----------------------------------------------------+");
}

int create_debugfs(void) {
#ifdef CONFIG_DEBUG_FS
	debug_dir = debugfs_create_dir("rdma_test", 0);
	if (!debug_dir) {
		pr_err("failed to create /sys/kernel/debug/rdma_test");
		return -1;
	}

	debug_file = debugfs_create_u32("enable", 0666, debug_dir, 
			&test_enable);
	if (!debug_file) {
		pr_err("failed to create /sys/kernel/debug/rdma_test/enable");
		return -1;
	}
#endif
	return 0;
}

static int __init init_rdma_test_module(void) 
{
	int ret = 0;
	int i;

	total_capacity = (size_t) input_size_mb << 20;
	//total_capacity = PAGE_SIZE * 8;
	num_iters = total_capacity / PAGE_SIZE / num_threads;

	show_test_info();
	
	ret = init_pages();
	if (ret) {
		pr_err("failed to init pages");
		ret = -1;
		goto out_err;
	}

	comp = (struct completion *) kzalloc(sizeof(struct completion) * 
			num_threads, GFP_KERNEL);
	if (!comp) {
		pr_err("failed to init comp");
		ret = -1;
		goto out_err;

	}
	for (i = 0; i < num_threads; i++)
		init_completion(&comp[i]);
	
	create_debugfs();

	ret = main();
	if (ret) {
		pr_err("failed to init main");
		ret = -1;
		goto out_err2;
	}
	
	pr_info("run all the module functions");

out_err2:
	kfree(comp);
out_err:	
	exit_pages();
	
	return ret;
}

static void __exit exit_rdma_test_module(void) 
{
	pr_info("Bye... rdma_test");
}

module_init(init_rdma_test_module);
module_exit(exit_rdma_test_module);

MODULE_LICENSE("GPL");
