#ifndef __LFSTACK_HPP__
#define __LFSTACK_HPP__

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cassert>
#ifndef __cplusplus
#include <stdatomic.h>
#define ATOMIC_VAR_INIT(value) (value)
#else
#include <atomic>
#define _Atomic(X) std::atomic< X >
#endif

typedef struct lfstack_node {
    uintptr_t value;
    struct lfstack_node *next;
} lfstack_node;

typedef struct lfstack_head {
    uintptr_t aba;
    struct lfstack_node *node;
} lfstack_head;


class LockFreeStack {
    private:
        struct lfstack_node *node_buffer;
        _Atomic(struct lfstack_head) head, free;
        _Atomic(size_t) size;

    public:
        LockFreeStack(size_t num_elems);
        ~LockFreeStack();

        void push(_Atomic(struct lfstack_head) *, struct lfstack_node *);
        struct lfstack_node *pop(_Atomic(struct lfstack_head) *);

        int stack_push(uintptr_t);
        uintptr_t stack_pop();

    private:
        size_t stack_size() {
            return atomic_load(&size);
        }
};

#if 0
LockFreeStack::LockFreeStack(size_t num_elems) {
    struct lfstack_head head_init = {0, NULL};
    struct lfstack_head free_init = {};

    head = ATOMIC_VAR_INIT(head_init);
    size = ATOMIC_VAR_INIT(0);

    /* Pre-allocate all nodes. */
    node_buffer = (lfstack_node *)aligned_alloc(8, num_elems * 
            sizeof(struct lfstack_node));
    if (!node_buffer)
        assert(0);

    // Fill freelist
    for (size_t i = 0; i < num_elems - 1; i++) {
        node_buffer[i].next = node_buffer + i + 1;
    }

    node_buffer[num_elems - 1].next = NULL;
    
    free_init = {0, node_buffer};
    free = ATOMIC_VAR_INIT(free_init);
}

LockFreeStack::~LockFreeStack() {
    delete node_buffer;
}

void LockFreeStack::push(_Atomic(struct lfstack_head) *head, 
        struct lfstack_node *node) {
    struct lfstack_head next, orig = atomic_load(head);
    
    do {
        node->next = orig.node;
        next.aba = orig.aba + 1;
        next.node = node;
    } while (!atomic_compare_exchange_weak(head, &orig, next));
}

inline struct lfstack_node *
LockFreeStack::pop(_Atomic(struct lfstack_head) *head) {
    struct lfstack_head next, orig = atomic_load(head);

    do {
        if (!orig.node)
            return NULL;
        next.aba = orig.aba + 1;
        next.node = orig.node->next;
    } while (!atomic_compare_exchange_weak(head, &orig, next));

    return orig.node;
}

int LockFreeStack::stack_push(uintptr_t value) {
    struct lfstack_node *node = pop(&free);
    
    if (!node)
        return -1;
     
    node->value = value;
    push(&head, node);
    //atomic_fetch_add(&lfstack->size, 1UL);

    return 0;
}

uintptr_t LockFreeStack::stack_pop() {
    struct lfstack_node *node = pop(&head);
    
    /* no freespace */
    if (!node)
        return -1;
    
    uintptr_t value = node->value;
    push(&free, node);
    //atomic_fetch_sub(&lfstack->size, 1UL);

    return value;
}
#endif 

#endif // __LFSTACK_HPP__
