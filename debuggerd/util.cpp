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

#include "util.h"

#include <time.h>

#include <functional>
#include <string>
#include <utility>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/logging.h>
#include "protocol.h"

std::vector<std::string> get_command_line(pid_t pid) {
  std::vector<std::string> result;

  std::string cmdline;
  android::base::ReadFileToString(android::base::StringPrintf("/proc/%d/cmdline", pid), &cmdline);

  auto it = cmdline.cbegin();
  while (it != cmdline.cend()) {
    // string::iterator is a wrapped type, not a raw char*.
    auto terminator = std::find(it, cmdline.cend(), '\0');
    result.emplace_back(it, terminator);
    it = std::find_if(terminator, cmdline.cend(), [](char c) { return c != '\0'; });
  }
  if (result.empty()) {
    result.emplace_back("<unknown>");
  }

  return result;
}

std::string get_process_name(pid_t pid) {
  std::string result = "<unknown>";
  android::base::ReadFileToString(android::base::StringPrintf("/proc/%d/cmdline", pid), &result);
  // We only want the name, not the whole command line, so truncate at the first NUL.
  return result.c_str();
}

std::string get_thread_name(pid_t tid) {
  std::string result = "<unknown>";
  android::base::ReadFileToString(android::base::StringPrintf("/proc/%d/comm", tid), &result);
  return android::base::Trim(result);
}

std::string get_timestamp() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  tm tm;
  localtime_r(&ts.tv_sec, &tm);

  char buf[strlen("1970-01-01 00:00:00.123456789+0830") + 1];
  char* s = buf;
  size_t sz = sizeof(buf), n;
  n = strftime(s, sz, "%F %H:%M", &tm), s += n, sz -= n;
  n = snprintf(s, sz, ":%02d.%09ld", tm.tm_sec, ts.tv_nsec), s += n, sz -= n;
  n = strftime(s, sz, "%z", &tm), s += n, sz -= n;
  return buf;
}

bool iterate_tids(pid_t pid, std::function<void(pid_t)> callback) {
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), "/proc/%d/task", pid);
  std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(buf), closedir);
  if (dir == nullptr) {
    return false;
  }

  struct dirent* entry;
  while ((entry = readdir(dir.get())) != nullptr) {
    pid_t tid = atoi(entry->d_name);
    if (tid == 0) {
      continue;
    }
    callback(tid);
  }
  return true;
}

namespace Process {
std::istream& operator>>(std::istream& is, State& state) {
  char state_char;
  is >> state_char;
  switch (state_char) {
    case 'R':
      state = State::kRunning;
      break;
    case 'S':
      state = State::kSleeping;
      break;
    case 'D':
      state = State::kUninterruptibleSleep;
      break;
    case 'Z':
      state = State::kZombie;
      break;
    case 'T':
      state = State::kStopped;
      break;
    case 't':
      state = State::kTracingStop;
      break;
    case 'X':
      state = State::kDead;
      break;
    case 'I':
      state = State::kIdle;
      break;
    default:
      state = State::kUnknown;
      LOG(ERROR) << "Unknown state: " << state_char;
      break;
  }
  return is;
}

std::ostream& operator<<(std::ostream& os, const State& state) {
  os << static_cast<char>(state);
  return os;
}
// Updated operator<<
std::ostream& operator<<(std::ostream& os, const Stat& stat) {
  os << stat.pid << " " << "(" << std::string_view(stat.comm.data()) << ") " << stat.state << " " << stat.ppid << " "
     << stat.pgrp << " " << stat.session << " " << stat.tty_nr << " " << stat.tpgid << " "
     << stat.flags << " " << stat.minflt << " " << stat.cminflt << " " << stat.majflt << " "
     << stat.cmajflt << " " << stat.utime << " " << stat.stime << " " << stat.cutime << " "
     << stat.cstime << " " << stat.priority << " " << stat.nice << " " << stat.num_threads << " "
     << 0 /* itrealvalue */ << " " << stat.starttime << " " << stat.vsize << " "
     << stat.rss << " " << stat.rsslim << " " << stat.startcode
     << " " << stat.endcode << " " << stat.startstack << " " << stat.kstkesp << " "
     << stat.kstkeip << " " << 0 /* signal */ << " " << 0 /* blocked */ << " "
     << 0 /* sigignore */ << " " << 0 /* sigcatch */ << " " << stat.wchan << " " << 0 /* nswap */
     << " " << 0 /* cnswap */ << " " << stat.exit_signal << " " << stat.processor << " "
     << stat.rt_priority << " " << stat.policy << " " << stat.delayacct_blkio_ticks << " "
     << stat.guest_time << " " << stat.cguest_time << " " << stat.start_data << " "
     << stat.end_data << " " << stat.start_brk << " " << stat.arg_start << " " << stat.arg_end
     << " " << stat.env_start << " " << stat.env_end << " " << stat.exit_code;

  if (os.fail()) {
    LOG(ERROR) << "Failed to print stat struct for pid: " << stat.pid;
  }
  return os;
}

std::optional<Stat> Stat::get_from_pid(pid_t pid) {
  auto stat_path = android::base::StringPrintf("/proc/%d/stat", pid);
  std::string stat_str;
  if (!android::base::ReadFileToString(stat_path, &stat_str)) {
    // Process might have died, which is fine.
    return std::nullopt;
  }

  int64_t obsolete_field = 0;
  Stat stat;
  std::memset(stat.comm.data(), '\0', stat.comm.size());

  std::stringstream stream(stat_str);

  stream >> stat.pid;

  // Command name max size is 16 bytes including the NULL terminator and the parentheses.
  // Start `i` at 1 to skip the leading (.
  for (size_t i = 1; i < stat.comm.size() - 1; ++i) {
    if (stream.peek() == ')') {
      // We found the end of the command name.
      break;
    }

    stream >> stat.comm[i];
  }

  stream >> stat.state >> stat.ppid >> stat.pgrp >> stat.session >> stat.tty_nr >> stat.tpgid >>
        stat.flags >> stat.minflt >> stat.cminflt >> stat.majflt >> stat.cmajflt >>
        stat.utime >> stat.stime >> stat.cutime >> stat.cstime >> stat.priority >> stat.nice >>
        stat.num_threads >> obsolete_field /* itrealvalue */ >> stat.starttime >> stat.vsize >>
        stat.rss >> stat.rsslim >> stat.startcode >>
        stat.endcode >> stat.startstack >> stat.kstkesp >> stat.kstkeip >>
        obsolete_field /* signal */ >> obsolete_field /* blocked */ >>
        obsolete_field /* sigignore */ >> obsolete_field /* sigcatch */ >> stat.wchan >>
        obsolete_field /* nswap */ >> obsolete_field /* cnswap */ >> stat.exit_signal >>
        stat.processor >> stat.rt_priority >> stat.policy >> stat.delayacct_blkio_ticks >>
        stat.guest_time >> stat.cguest_time >> stat.start_data >> stat.end_data >>
        stat.start_brk >> stat.arg_start >> stat.arg_end >> stat.env_start >> stat.env_end >>
        stat.exit_code;


  if (stream.fail()) {
    LOG(ERROR) << "Failed to parse stat string: " << stat_str;
    return std::nullopt;
  }

  return stat;
}
}  // namespace Process
