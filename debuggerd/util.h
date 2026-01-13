/*
 * Copyright 2016, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <sys/cdefs.h>
#include <sys/types.h>

std::vector<std::string> get_command_line(pid_t pid);
std::string get_process_name(pid_t pid);
std::string get_thread_name(pid_t tid);

std::string get_timestamp();
bool iterate_tids(pid_t, std::function<void(pid_t)>);

namespace Process {
enum class State : char {
  kRunning = 'R',
  kSleeping = 'S',
  kUninterruptibleSleep = 'D',
  kZombie = 'Z',
  kStopped = 'T',
  kDead = 'X',
  kTracingStop = 't',
  kIdle = 'I',
  kUnknown = '?',
};

std::istream& operator>>(std::istream& is, State& state);
std::ostream& operator<<(std::ostream& os, const State& state);

// Represents the data parsed from /proc/<pid>/stat
// Field names and types are based on man proc(5)
struct Stat {
  pid_t pid;                  // (1) The process ID.
  std::array<char, 16> comm;  // (2) The filename of the executable, in parentheses.
  State state;                // (3) One char from "RSDZTWIL"
  pid_t ppid;                 // (4) The PID of the parent of this process.
  int32_t pgrp;               // (5) The process group ID of the process.
  int32_t session;            // (6) The session ID of the process.
  int32_t tty_nr;             // (7) The controlling terminal of the process.
  int32_t tpgid;              // (8) The ID of the foreground process group of the controlling terminal.
  uint64_t flags;   // (9) The kernel flags word of the process.
  uint64_t minflt;  // (10) The number of minor faults the process has made which have not
                    // required loading a memory page from disk.
  uint64_t cminflt; // (11) The number of minor faults that the process's waited-for children have made.
  uint64_t majflt;  // (12) The number of major faults the process has made which have required
                    // loading a memory page from disk.
  uint64_t cmajflt; // (13) The number of major faults that the process's waited-for children have made.
  uint64_t utime;   // (14) Amount of time that this process has been scheduled in user mode,
                    // measured in clock ticks.
  uint64_t stime;       // (15) Amount of time that this process has been scheduled in kernel mode,
                        // measured in clock ticks.
  int64_t cutime;       // (16) Amount of time that this process's waited-for children have been scheduled
                        // in user mode, measured in clock ticks.
  int64_t cstime;       // (17) Amount of time that this process's waited-for children have been scheduled
                        // in kernel mode, measured in clock ticks.
  int64_t priority;     // (18) The standard priority number.
  int64_t nice;         // (19) The nice value, a value in the range 19 (low priority) to -20 (high priority).
  int64_t num_threads;  // (20) Number of threads in this process.
  int64_t itrealvalue;  // (21) The time in jiffies before the next SIGALRM is sent to the process due
                        // to an interval timer.
  uint64_t starttime;   // (22) The time the process started after system boot. In clock ticks.
  uint64_t vsize;       // (23) Virtual memory size in bytes.
  int64_t rss;          // (24) Resident Set Size: number of pages the process has in real memory.
  uint64_t rsslim;      // (25) Current soft limit in bytes on the rss of the process.
  uint64_t startcode;   // (26) The address above which program text can run.
  uint64_t endcode;     // (27) The address below which program text can run.
  uint64_t startstack;  // (28) The address of the start of the stack.
  uint64_t kstkesp;  // (29) The current value of ESP (stack pointer), as found in the kernel stack page for the process.
  uint64_t kstkeip;  // (30) The current EIP (instruction pointer).
  /*
  unsigned long signal;         // (31) The bitmap of pending signals, displayed as a decimal number. Obsolete, use /proc/[pid]/status.
  unsigned long blocked;        // (32) The bitmap of blocked signals, displayed as a decimal number. Obsolete, use /proc/[pid]/status.
  unsigned long sigignore;      // (33) The bitmap of ignored signals, displayed as a decimal number. Obsolete, use /proc/[pid]/status.
  unsigned long sigcatch;       // (34) The bitmap of caught signals, displayed as a decimal number. Obsolete, use /proc/[pid]/status.
  */
  uint64_t wchan;  // (35) This is the "channel" in which the process is waiting.
  /*
  unsigned long nswap;   // (36) Number of pages swapped (not maintained).
  unsigned long cnswap;  // (37) Cumulative nswap for child processes (not maintained).
  */
  int32_t exit_signal;           // (38) Signal to be sent to parent when we die.
  int32_t processor;             // (39) CPU number last executed on.
  uint32_t rt_priority;  // (40) Real-time scheduling priority, a number in the range 1 to 99
                         // for processes scheduled under a real-time policy.
  uint32_t policy;       // (41) Scheduling policy.
  uint64_t delayacct_blkio_ticks;  // (42) Aggregated block I/O delays, measured in clock
                                   // ticks (centiseconds).
  uint64_t guest_time;  // (43) Guest time of the process (time spent running a virtual CPU for
                        // a guest operating system), measured in clock ticks.
  int64_t cguest_time;  // (44) Guest time of the process's children, measured in clock ticks.
  uint64_t start_data;  // (45) Address above which program data+bss is placed.
  uint64_t end_data;    // (46) Address below which program data+bss is placed.
  uint64_t start_brk;   // (47) Address above which program heap can be expanded with brk(2).
  uint64_t arg_start;   // (48) Address of beginning of command-line arguments.
  uint64_t arg_end;     // (49) Address of end of command-line arguments.
  uint64_t env_start;   // (50) Address of beginning of environment.
  uint64_t env_end;     // (51) Address of end of environment.
  int32_t exit_code;    // (52) The thread's exit status in the form reported by waitpid(2).

  static std::optional<Stat> get_from_pid(pid_t pid);
  friend std::ostream& operator<<(std::ostream& os, const Stat& stat);
};

}  // namespace Process