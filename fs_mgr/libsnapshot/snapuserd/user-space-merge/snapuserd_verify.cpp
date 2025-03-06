/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "snapuserd_verify.h"

#include <android-base/chrono_utils.h>
#include <android-base/scopeguard.h>
#include <android-base/strings.h>

#include "android-base/properties.h"
#include "snapuserd_core.h"

namespace android {
namespace snapshot {

using namespace android;
using namespace android::dm;
using android::base::unique_fd;

UpdateVerify::UpdateVerify(const std::string& misc_name)
    : misc_name_(misc_name), state_(UpdateVerifyState::VERIFY_UNKNOWN) {}

bool UpdateVerify::CheckPartitionVerification() {
    auto now = std::chrono::system_clock::now();
    auto deadline = now + 10s;
    {
        std::unique_lock<std::mutex> cv_lock(m_lock_);
        while (state_ == UpdateVerifyState::VERIFY_UNKNOWN) {
            auto status = m_cv_.wait_until(cv_lock, deadline);
            if (status == std::cv_status::timeout) {
                return false;
            }
        }
    }

    return (state_ == UpdateVerifyState::VERIFY_SUCCESS);
}

void UpdateVerify::UpdatePartitionVerificationState(UpdateVerifyState state) {
    {
        std::lock_guard<std::mutex> lock(m_lock_);
        state_ = state;
    }
    m_cv_.notify_all();
}

void UpdateVerify::VerifyUpdatePartition() {
    bool succeeded = false;

    auto scope_guard = android::base::make_scope_guard([this, &succeeded]() -> void {
        if (!succeeded) {
            UpdatePartitionVerificationState(UpdateVerifyState::VERIFY_FAILED);
        }
    });

    auto& dm = DeviceMapper::Instance();
    auto dm_block_devices = dm.FindDmPartitions();
    if (dm_block_devices.empty()) {
        SNAP_LOG(ERROR) << "No dm-enabled block device is found.";
        return;
    }

    const auto parts = android::base::Split(misc_name_, "-");
    std::string partition_name = parts[0];

    constexpr auto&& suffix_b = "_b";
    constexpr auto&& suffix_a = "_a";

    partition_name.erase(partition_name.find_last_not_of(suffix_b) + 1);
    partition_name.erase(partition_name.find_last_not_of(suffix_a) + 1);

    if (dm_block_devices.find(partition_name) == dm_block_devices.end()) {
        SNAP_LOG(ERROR) << "Failed to find dm block device for " << partition_name;
        return;
    }

    if (!VerifyPartition(partition_name, dm_block_devices.at(partition_name))) {
        SNAP_LOG(ERROR) << "Partition: " << partition_name
                        << " Block-device: " << dm_block_devices.at(partition_name)
                        << " verification failed";
    }
    succeeded = true;
}

bool UpdateVerify::VerifyBlocks(const std::string& partition_name,
                                const std::string& dm_block_device, off_t offset, int,
                                uint64_t dev_sz) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(dm_block_device.c_str(), O_RDONLY | O_DIRECT)));
    if (fd < 0) {
        SNAP_LOG(ERROR) << "open failed: " << dm_block_device;
        return false;
    }

    ring_ = std::make_unique<struct io_uring>();
    int queue_depth = 1;
    int ret = io_uring_queue_init(queue_depth, ring_.get(), 0);
    if (ret) {
        LOG(ERROR) << "Verify: io_uring_queue_init failed with ret: " << ret;
        return false;
    }

    struct iovec* vecs;
    vecs = (struct iovec*)malloc(queue_depth * sizeof(struct iovec));

    loff_t file_offset = offset;
    // 1M block size
    int verify_block_size = 1024 * 1024L;

    for (int i = 0; i < queue_depth; i++) {
        void* addr;
        ssize_t page_size = getpagesize();
        if (posix_memalign(&addr, page_size, verify_block_size) < 0) {
            LOG(ERROR) << "posix_memalign failed";
            return false;
        }
        vecs[i].iov_base = addr;
        vecs[i].iov_len = verify_block_size;
    }

    ret = io_uring_register_buffers(ring_.get(), vecs, queue_depth);
    if (ret < 0) {
        SNAP_LOG(ERROR) << "io_uring_register_buffers failed";
        return false;
    }

    uint64_t bytes_read = 0;

    struct io_uring_sqe* sqe;
    struct io_uring_cqe* cqe;
    const uint64_t read_sz = verify_block_size;
    uint64_t total_read = 0;
    int num_submitted = 0;

    while (total_read < dev_sz) {
        for (int i = 0; i < queue_depth; i++) {
            uint64_t to_read = std::min((dev_sz - file_offset), read_sz);
            if (to_read <= 0) break;
            sqe = io_uring_get_sqe(ring_.get());
            if (!sqe) {
                SNAP_LOG(ERROR) << "io_uring_get_sqe failed";
                return false;
            }

            io_uring_prep_read_fixed(sqe, fd.get(), vecs[i].iov_base, to_read, file_offset, i);
            file_offset += to_read;
            total_read += to_read;
            num_submitted += 1;
        }

        ret = io_uring_submit(ring_.get());
        SNAP_LOG(DEBUG) << "io_uring_submit: " << total_read << "num_submitted: " << num_submitted
                        << "ret: " << ret;

        while (num_submitted) {
            ret = io_uring_wait_cqe(ring_.get(), &cqe);
            if (ret < 0) {
                SNAP_LOG(ERROR) << "io_uring_wait_sqe: " << ret;
                return false;
            }
            io_uring_cqe_seen(ring_.get(), cqe);
            num_submitted -= 1;
        }

        SNAP_LOG(DEBUG) << "io_uring_submit success: " << total_read;
    }

    io_uring_unregister_buffers(ring_.get());
    for (int i = 0; i < queue_depth; i++) {
        free(vecs[i].iov_base);
    }
    free(vecs);
    io_uring_queue_exit(ring_.get());

    SNAP_LOG(INFO) << "Verification success with io_uring: bytes-read: " << bytes_read
                   << " dev_sz: " << dev_sz << " partition_name: " << partition_name;

    return true;
}

bool UpdateVerify::VerifyPartition(const std::string& partition_name,
                                   const std::string& dm_block_device) {
    android::base::Timer timer;

    SNAP_LOG(INFO) << "VerifyPartition: " << partition_name << " Block-device: " << dm_block_device;

    bool succeeded = false;
    auto scope_guard = android::base::make_scope_guard([this, &succeeded]() -> void {
        if (!succeeded) {
            UpdatePartitionVerificationState(UpdateVerifyState::VERIFY_FAILED);
        }
    });

    unique_fd fd(TEMP_FAILURE_RETRY(open(dm_block_device.c_str(), O_RDONLY | O_DIRECT)));
    if (fd < 0) {
        SNAP_LOG(ERROR) << "open failed: " << dm_block_device;
        return false;
    }

    uint64_t dev_sz = get_block_device_size(fd.get());
    if (!dev_sz) {
        SNAP_PLOG(ERROR) << "Could not determine block device size: " << dm_block_device;
        return false;
    }

    if (!IsBlockAligned(dev_sz)) {
        SNAP_LOG(ERROR) << "dev_sz: " << dev_sz << " is not block aligned";
        return false;
    }

    /*
     * Not all partitions are of same size. Some partitions are as small as
     * 100Mb. We can just finish them in a single thread. For bigger partitions
     * such as product, 4 threads are sufficient enough.
     *
     * TODO: With io_uring SQ_POLL support, we can completely cut this
     * down to just single thread for all partitions and potentially verify all
     * the partitions with zero syscalls. Additionally, since block layer
     * supports polling, IO_POLL could be used which will further cut down
     * latency.
     */
    int num_threads = kMinThreadsToVerify;

    std::vector<std::future<bool>> threads;
    off_t start_offset = 0;
    const int skip_blocks = num_threads;

    auto verify_block_size =
            android::base::GetUintProperty("ro.virtual_ab.verify_block_size", kBlockSizeVerify);
    while (num_threads) {
        threads.emplace_back(std::async(std::launch::async, &UpdateVerify::VerifyBlocks, this,
                                        partition_name, dm_block_device, start_offset, skip_blocks,
                                        dev_sz));
        start_offset += verify_block_size;
        num_threads -= 1;
        if (start_offset >= dev_sz) {
            break;
        }
    }

    bool ret = true;
    for (auto& t : threads) {
        ret = t.get() && ret;
    }

    if (ret) {
        succeeded = true;
        UpdatePartitionVerificationState(UpdateVerifyState::VERIFY_SUCCESS);
        SNAP_LOG(INFO) << "Partition: " << partition_name << " Block-device: " << dm_block_device
                       << " Size: " << dev_sz
                       << " verification success. Duration : " << timer.duration().count() << " ms";
        return true;
    }

    return false;
}

}  // namespace snapshot
}  // namespace android
