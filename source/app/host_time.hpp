#ifndef HC_APP_HOST_TIME_HPP
#define HC_APP_HOST_TIME_HPP

/* host_time — one tiny shared helper: a realtime (CLOCK_REALTIME) millisecond stamp. The project index
 * (hc_projects) PERSISTS its created/touched timestamps and orders the list by them, so the monotonic clock
 * (which resets per boot, and is used elsewhere for the activity timeline) would be wrong for those. Inline so
 * main.cpp + host_services.cpp share ONE definition rather than each keeping an identical file-static copy. */

#include <cstdint>
#include <ctime>

namespace hcapp {

inline int64_t realtime_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

} // namespace hcapp

#endif /* HC_APP_HOST_TIME_HPP */
