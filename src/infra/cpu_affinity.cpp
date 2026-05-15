#include "infra/cpu_affinity.hpp"

#include <pthread.h>
#include <sched.h>

namespace spreadara::infra {

bool pin_current_thread_to_core(int core) {
    if (core < 0) {
        return false;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    return rc == 0;
}

}
