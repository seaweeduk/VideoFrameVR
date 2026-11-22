#include "thread_priority.h"

#include "platform.h"

#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

void runInBackground(Background how) {
  const auto tid = static_cast<pid_t>(syscall(SYS_gettid));
  setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 19);
  if (how == Background::Cpu) return;
  syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, tid, 3 << 13 /* IOPRIO_CLASS_IDLE */);
  if (how == Background::Decoding && kStandaloneHeadset) {
    cpu_set_t cores;
    CPU_ZERO(&cores);
    for (const int core : kFrameBackgroundCores) CPU_SET(core, &cores);
    sched_setaffinity(tid, sizeof(cores), &cores);
  }
}
