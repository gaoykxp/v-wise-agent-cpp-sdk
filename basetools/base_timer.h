//
// Created by htsong on 2023/12/06.
//
#ifndef BASE_TIMER_H_
#define BASE_TIMER_H_

#include <sys/time.h>
#include <unistd.h>

namespace base_tools {
    typedef long long basetime;

    class BaseTimer {
    public:
        BaseTimer();

        ~BaseTimer();

        static basetime GetSecTime();

        static basetime GetMilliTime();

        static basetime GetMicroTime();

        static basetime GetNanoTime();
    };

}

#endif //BASE_TIMER_H_