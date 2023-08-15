#ifndef _JENKINS_H_
#define _JENKINS_H_

// JENKINS HASH FUNCTION
static inline size_t jenkins(const void* _ptr, size_t _len, size_t _seed) 
{
    size_t i = 0;
    size_t hash = 0;
    const char* key = (const char*)(_ptr);
    while (i != _len) {
        hash += key[i++];
        hash += hash << (10);
        hash ^= hash >> (6);
    }
    hash += hash << (3);
    hash ^= hash >> (11);
    hash += hash << (15);
    return hash;
}

#endif // _JENKINS_H_

