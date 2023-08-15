#include "lfstack.hpp"

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
