#ifndef __RAND_DIST_H__
#define __RAND_DIST_H__

#include <random>

class UniformDist {
    private :
        std::default_random_engine re;
        std::uniform_int_distribution<unsigned long> uniform_dist;

    public :
        UniformDist(unsigned long low, unsigned long high)
            : uniform_dist {low, high}
        {
            re.seed(time(NULL));
        }

        const unsigned long GetRand() { return uniform_dist(re); }
};

#endif // __RAND_DIST_H__
