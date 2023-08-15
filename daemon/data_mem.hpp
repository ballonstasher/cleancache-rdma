#ifndef __DATAMEM_HPP__
#define __DATAMEM_HPP__

struct memblock {
    void *ptr;
    struct ibv_mr *mr;
    bool is_registered;
};

class DataMem {
    public:
        DataMem();
        ~DataMem();
        
        struct memblock *Register(struct ibv_pd *, bool);
        int Deregister(struct memblock *);
        
        struct ibv_mr *GetMBMR(void *ptr) {
            struct memblock *mb;

            return mb->mr;
        }

    private:
        size_t max_mem_size;
        int memblock_size;
        int max_num_memblocks;
        int num_registered;
        
        struct memblock **mbs;
};

inline DataMem::DataMem() {
    max_mem_size = 4UL * (1 << 30);
    memblock_size = 16 * (1 << 20);
    max_num_memblocks = max_mem_size / memblock_size;
    num_registered = 0;

    mbs = new struct memblock *[max_num_memblocks]; 
    for (int i = 0; i < max_num_memblocks; i++) {
        mbs[i] = new struct memblock();
    }
}

inline DataMem::~DataMem() {
    for (int i = 0; i < max_num_memblocks; i++) {
        if (mbs[i]->is_registered) {
            Deregister(mbs[i]); 
        }
        delete mbs[i];
    } 
    delete[] mbs;
}

inline struct memblock *DataMem::Register(struct ibv_pd *pd, bool prealloc) {
    struct memblock *mb = NULL;
    int access_flag = IBV_ACCESS_LOCAL_WRITE |
        IBV_ACCESS_REMOTE_WRITE |
        IBV_ACCESS_REMOTE_READ;

    /* TODO: find not registered mb O(1)? */
    for (int i = 0; i < max_num_memblocks; i++) {
        if (!mbs[i]->is_registered) {
            mb = mbs[i];
            mb->mr = ibv_reg_mr(pd, mb->ptr, memblock_size, access_flag);
            if (!mb->mr) {
                fprintf(stderr, "failed to ibv_reg_mr()\n");
                goto out;
            }
            mb->is_registered = true;
            if (!prealloc)
                break;
        }
    }

out:
    return mb;
}

inline int DataMem::Deregister(struct memblock *mb) {
    int ret = 0;

    if (ibv_dereg_mr(mb->mr)) {
        fprintf(stderr, "failed to ibv_dereg_mr()\n");
        ret = -1;
    }

    free(mb->ptr);
    mb->is_registered = false;

    return ret;
}

#endif // __DATAMEM_HPP__
