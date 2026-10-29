#define _POSIX_C_SOURCE 200809L
#include "timeutil.h"

#include <time.h>

uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void sleep_us(uint32_t us)
{
    struct timespec ts;

    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    while (nanosleep(&ts, &ts) != 0)
        ;
}
