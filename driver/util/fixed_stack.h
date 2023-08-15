#ifndef __FIXED_STACK_H__
#define __FIXED_STACK_H__

#include <linux/slab.h>
#include <linux/spinlock.h>

typedef struct fixed_stack {
	u64 *nodes;
	size_t num_elems;
	int64_t top;
	spinlock_t lock;
} fstack_t;

static inline bool is_full(fstack_t *fstack)
{
	if (fstack->top >= (int64_t)fstack->num_elems) {
		//pr_err("fstack is_full...!"); 
		return true;
	}
	return false;
}

static inline bool is_empty(fstack_t *fstack)
{
	if (fstack->top == -1) {
		//pr_err("fstack is_empty...!"); 
		return true;
	}
	return false;
}

static inline int stack_push(fstack_t *fstack, u64 item) 
{
	int ret = -1;

	spin_lock(&fstack->lock);
	if (!is_full(fstack)) {
		fstack->nodes[++fstack->top] = item;
		ret = 0;
	} 
	spin_unlock(&fstack->lock);

	return ret; 
}

static inline u64 stack_pop(fstack_t *fstack)
{
	u64 res = -1;

	spin_lock(&fstack->lock);
	if (!is_empty(fstack))
		res = fstack->nodes[fstack->top--];
	spin_unlock(&fstack->lock);

	return res;
}

static inline fstack_t *stack_init(size_t num_elems) 
{
	fstack_t *fstack;
	
	fstack = (fstack_t *)kzalloc(sizeof(fstack_t), GFP_KERNEL);
	if (!fstack) {
		pr_err("failed to allocate fstack");
		goto out_error;
	}

	fstack->nodes = (u64 *)vzalloc(num_elems * sizeof(u64));
	if (!fstack->nodes) {
		pr_err("failed to allocate fstack->nodes");
		goto out_error2;
	}

	fstack->num_elems = num_elems;
	fstack->top = -1;
	spin_lock_init(&fstack->lock);

	return fstack;

out_error2:
	kfree(fstack);
out_error:
	return NULL;
}

static inline void stack_exit(fstack_t *fstack)
{
	vfree(fstack->nodes);
	kfree(fstack);
}

#endif // __FIXED_STACK_H__
