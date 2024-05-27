/*
 * Copyright (C) 2012-2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "DEBUG"

#include "libdebuggerd/tombstone.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <string>

#include <android-base/file.h>
#include <android-base/unique_fd.h>
#include <android/log.h>
#include <async_safe/log.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>
#include <unwindstack/AndroidUnwinder.h>
#include <unwindstack/Error.h>
#include <unwindstack/Regs.h>

#include "libdebuggerd/backtrace.h"
#include "libdebuggerd/open_files_list.h"
#include "libdebuggerd/utility.h"
#include "util.h"

#include "tombstone.pb.h"

using android::base::unique_fd;

using namespace std::literals::string_literals;

void engrave_tombstone_ucontext(int tombstone_fd, int proto_fd, uint64_t abort_msg_address,
                                siginfo_t* siginfo, ucontext_t* ucontext) {
  pid_t uid = getuid();
  pid_t pid = getpid();
  pid_t target_tid = gettid();

  log_t log;
  log.current_tid = target_tid;
  log.crashed_tid = target_tid;
  log.tfd = tombstone_fd;
  log.amfd_data = nullptr;

  std::string thread_name = get_thread_name(target_tid);
  std::vector<std::string> command_line = get_command_line(pid);

  std::unique_ptr<unwindstack::Regs> regs(
      unwindstack::Regs::CreateFromUcontext(unwindstack::Regs::CurrentArch(), ucontext));

  std::string selinux_label;
  android::base::ReadFileToString("/proc/self/attr/current", &selinux_label);

  std::map<pid_t, ThreadInfo> threads;
  threads[target_tid] = ThreadInfo {
    .registers = std::move(regs), .uid = uid, .tid = target_tid,
    .thread_name = std::move(thread_name), .pid = pid, .command_line = std::move(command_line),
    .selinux_label = std::move(selinux_label), .siginfo = siginfo, .signo = siginfo->si_signo,
    // Only supported on aarch64 for now.
#if defined(__aarch64__)
    .tagged_addr_ctrl = prctl(PR_GET_TAGGED_ADDR_CTRL, 0, 0, 0, 0),
    .pac_enabled_keys = prctl(PR_PAC_GET_ENABLED_KEYS, 0, 0, 0, 0),
#endif
  };
  const ThreadInfo& thread = threads[pid];
  if (!iterate_tids(pid, [&threads, &thread, &target_tid](pid_t tid) {
        if (target_tid == tid) {
          return;
        }
        threads[tid] = ThreadInfo{
            .uid = thread.uid,
            .tid = tid,
            .pid = thread.pid,
            .command_line = thread.command_line,
            .thread_name = get_thread_name(tid),
            .tagged_addr_ctrl = thread.tagged_addr_ctrl,
            .pac_enabled_keys = thread.pac_enabled_keys,
        };
      })) {
    async_safe_format_log(ANDROID_LOG_ERROR, LOG_TAG, "failed to open /proc/%d/task: %s", pid,
                          strerror(errno));
  }

  // Do not use the thread cache here because it will call pthread_key_create
  // which doesn't work in linker code. See b/189803009.
  // Use a normal cached object because the thread is stopped, and there
  // is no chance of data changing between reads.
  auto process_memory = unwindstack::Memory::CreateProcessMemoryCached(getpid());
  unwindstack::AndroidLocalUnwinder unwinder(process_memory);
  unwindstack::ErrorData error;
  if (!unwinder.Initialize(error)) {
    async_safe_format_log(ANDROID_LOG_ERROR, LOG_TAG, "failed to init unwinder object: %s",
                          unwindstack::GetErrorCodeString(error.code));
    return;
  }

  ProcessInfo process_info;
  process_info.abort_msg_address = abort_msg_address;
  engrave_tombstone(unique_fd(dup(tombstone_fd)), unique_fd(dup(proto_fd)), &unwinder, threads,
                    target_tid, process_info, nullptr, nullptr);
}

void engrave_tombstone(unique_fd output_fd, unique_fd proto_fd,
                       unwindstack::AndroidUnwinder* unwinder,
                       const std::map<pid_t, ThreadInfo>& threads, pid_t target_thread,
                       const ProcessInfo& process_info, OpenFilesList* open_files,
                       std::string* amfd_data, const Architecture* guest_arch,
                       unwindstack::AndroidUnwinder* guest_unwinder) {
  // Don't copy log messages to tombstone unless this is a development device.
  Tombstone tombstone;
  engrave_tombstone_proto(&tombstone, unwinder, threads, target_thread, process_info, open_files,
                          guest_arch, guest_unwinder);

  if (proto_fd != -1) {
    if (!tombstone.SerializeToFileDescriptor(proto_fd.get())) {
      async_safe_format_log(ANDROID_LOG_ERROR, LOG_TAG, "failed to write proto tombstone: %s",
                            strerror(errno));
    }
  }

  log_t log;
  log.current_tid = target_thread;
  log.crashed_tid = target_thread;
  log.tfd = output_fd.get();
  log.amfd_data = amfd_data;

  tombstone_proto_to_text(tombstone, [&log](const std::string& line, bool should_log) {
    _LOG(&log, should_log ? logtype::HEADER : logtype::LOGS, "%s\n", line.c_str());
  });
}
