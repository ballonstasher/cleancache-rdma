#ifndef _STANDARD_H_
#define _STANDARD_H_ 

#if 0
static inline size_t standard(const void* _ptr, size_t _len, size_t _seed) {
	size_t hash = *(size_t *)_ptr;

	/*  Sigh, gcc can't optimise this alone like it does for 32 bits. */
	size_t n = hash;
	n <<= 18;
	hash -= n;
	n <<= 33;
	hash -= n;
	n <<= 3;
	hash += n;
	n <<= 3;
	hash -= n;
	n <<= 4;
	hash += n;
	n <<= 2;
	hash += n;
	
    return hash;
}
#else
static inline size_t standard(const void* _ptr, size_t _len, size_t _seed) {
    /* 
       size_t hash = _seed;
       const char* cptr = (const char*)(_ptr);
       for (; _len; --_len)
       hash = (hash * 131) + *cptr++;
       return hash;
       */
    const char* cptr = (const char*)(_ptr);
    for (; _len; --_len) {
        _seed ^= (size_t)(*cptr++);
        _seed *= (size_t)(1099511628211UL);
    }
    return _seed;
}
#endif

#endif // _STANDARD_H_
