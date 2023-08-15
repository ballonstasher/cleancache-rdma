#ifndef __FAST_MONO_CLOCK_HPP__
#define __FAST_MONO_CLOCK_HPP__

#include <chrono>
#include <cassert>

class FastMonoClock {
public:
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<FastMonoClock>;

    static constexpr bool is_steady = true;

    static time_point now() noexcept;

    static duration get_resolution() noexcept;

private:
    static clockid_t clock_id();
    static clockid_t test_coarse_clock();
    static duration convert(const timespec&);
};

inline clockid_t FastMonoClock::test_coarse_clock() {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &t) == 0) {
        return CLOCK_MONOTONIC_COARSE;
    } else {
        return CLOCK_MONOTONIC;
    }
}

clockid_t FastMonoClock::clock_id() {
    static clockid_t the_clock = test_coarse_clock();
    return the_clock;
}

inline auto FastMonoClock::convert(const timespec& t) -> duration {
    return std::chrono::seconds(t.tv_sec) + std::chrono::nanoseconds(t.tv_nsec);
}

auto FastMonoClock::now() noexcept -> time_point {
    struct timespec t;
    const auto result = clock_gettime(clock_id(), &t);
    assert(result == 0);
    return time_point{convert(t)};
}

auto FastMonoClock::get_resolution() noexcept -> duration {
    struct timespec t;
    const auto result = clock_getres(clock_id(), &t);
    assert(result == 0);
    return convert(t);
}

#endif // __FAST_MONO_CLOCK_HPP__
