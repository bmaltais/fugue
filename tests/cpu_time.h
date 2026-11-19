#pragma once

#include <QtGlobal>

#include <sys/resource.h>
#include <time.h>

// CPU time for budgets in tests. Wall-clock budgets fail on a busy machine (other tests, a
// build, parallel agents) though the code did the same work; CPU time counts only that work.
namespace test_support {

// This process's CPU time, all its threads (the render and run threads included), in ms.
inline qint64 cpuMs() {
    timespec now{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// The CPU time of this process's children that have ended and been waited for, in ms.
inline qint64 childrenCpuMs() {
    rusage usage{};
    getrusage(RUSAGE_CHILDREN, &usage);
    auto ms = [](const timeval &t) { return qint64(t.tv_sec) * 1000 + t.tv_usec / 1000; };
    return ms(usage.ru_utime) + ms(usage.ru_stime);
}

} // namespace test_support
