//
// Created by htsong on 2023/12/06.
//

#include "base_timer.h"
#include <chrono>

namespace base_tools
{
    basetime BaseTimer::GetSecTime()
    {
        struct timeval tv;
        gettimeofday(&tv,NULL);
        return static_cast<long long>(tv.tv_sec);
    }

    basetime BaseTimer::GetMilliTime()
    {
        struct timeval tv;
        gettimeofday(&tv,NULL);
        return static_cast<long long>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    }

    basetime BaseTimer::GetMicroTime()
    {
        struct timeval tv;
        gettimeofday(&tv,NULL);
        return static_cast<long long>(tv.tv_sec) * 1000000 + tv.tv_usec;
    }

    basetime BaseTimer::GetNanoTime() {
        auto now = std::chrono::high_resolution_clock::now();
        auto nano_time_point =
                std::chrono::time_point_cast<std::chrono::nanoseconds>(now);
        auto epoch = nano_time_point.time_since_epoch();
        uint64_t now_nano =
                std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count();
        return now_nano;
    }
}
