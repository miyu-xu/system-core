/*
 * Copyright (C) 2024 The Android Open Source Project
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

#define LOG_TAG "LibUringUtils"

#include <limits.h>
#include <linux/time_types.h>
#include <sys/cdefs.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include <cutils/sockets.h>
#include <private/android_logger.h>

#include <android-base/logging.h>
#include <android-base/scopeguard.h>

#include <sysutils/LibUringUtils.h>

bool LibUringUtils::isIouringEnabled() {
    // TODO: b/385143770 - Change this behavior to check the OS version and Liburing version.
    return false;
}

LibUringUtils::LibUringUtils(int socket_fd) : socket_(socket_fd) {}

LibUringUtils::~LibUringUtils() {
    if (registered_buffers_) {
        io_uring_free_buf_ring(&mCtx->ring, br_, num_buffers_, bgid_);
    }
    if (ring_setup_) {
        io_uring_queue_exit(&mCtx->ring);
    }
}

bool LibUringUtils::ArmRecvMsg() {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&mCtx->ring);
    memset(&msg, 0, sizeof(msg));
    msg.msg_controllen = control_len_;
    io_uring_prep_recvmsg_multishot(sqe, socket_, &msg, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = bgid_;
    io_uring_sqe_set_data64(sqe, 1234);
    int ret = io_uring_submit(&mCtx->ring);
    if (ret < 0) {
        LOG(ERROR) << "ArmRecvMsg failed: ret: " << ret;
        return false;
    }
    return true;
}

void LibUringUtils::AllocateBuffers(size_t num_buffers, size_t buf_size) {
    num_buffers_ = num_buffers;
    control_len_ = CMSG_ALIGN(sizeof(struct ucred)) + sizeof(struct cmsghdr);

    buffer_size_ = sizeof(struct io_uring_recvmsg_out) + control_len_ + buf_size;

    for (size_t i = 0; i < num_buffers_; i++) {
        std::unique_ptr<uint8_t[]> buffer = std::make_unique<uint8_t[]>(buffer_size_);
        buffers_.push_back(std::move(buffer));
    }
}

bool LibUringUtils::RegisterBuffers() {
    int ret = 0;
    br_ = io_uring_setup_buf_ring(&mCtx->ring, num_buffers_, bgid_, 0, &ret);
    if (!br_) {
        LOG(ERROR) << "io_uring_setup_buf_ring failed with error: " << ret;
        return false;
    }
    for (size_t i = 0; i < num_buffers_; i++) {
        void* buffer = buffers_[i].get();
        io_uring_buf_ring_add(br_, buffer, buffer_size_, i, io_uring_buf_ring_mask(num_buffers_),
                              i);
    }
    io_uring_buf_ring_advance(br_, num_buffers_);
    LOG(INFO) << "RegisterBuffers success: " << num_buffers_;
    registered_buffers_ = true;
    return true;
}

bool LibUringUtils::SetupIoUring(int queue_size) {
    mCtx = std::unique_ptr<uring_context>(new uring_context());
    struct io_uring_params params = {};

    // COOP_TASKRUN - No IPI to logd
    // SINGLE_ISSUER - Only one thread is doing the work on the ring
    // TASKRUN_FLAG - we use peek_cqe - Hence, trigger task work if required
    // DEFER_TASKRUN - trigger task work when CQE is explicitly polled
    params.flags |= (IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER |
                     IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_DEFER_TASKRUN);

    int ret = io_uring_queue_init_params(queue_size + 1, &mCtx->ring, &params);
    if (ret) {
        LOG(ERROR) << "io_uring_queue_init_params failed with ret: " << ret;
        return false;
    } else {
        LOG(INFO) << "io_uring_queue_init_params success";
    }

    ring_setup_ = true;
    return true;
}

void LibUringUtils::ReleaseBuffer() {
    if (active_buffer_id_ == -1) {
        return;
    }

    // Put the buffer back to the pool
    io_uring_buf_ring_add(br_, buffers_[active_buffer_id_].get(), buffer_size_, active_buffer_id_,
                          io_uring_buf_ring_mask(num_buffers_), 0);
    io_uring_buf_ring_cq_advance(&mCtx->ring, br_, 1);
    active_buffer_id_ = -1;

    // If there are no more CQE data, re-arm the SQE
    bool is_more_cqe = (cqe->flags & IORING_CQE_F_MORE);
    if (!is_more_cqe) {
        ArmRecvMsg();
    }
}

void LibUringUtils::ReceiveData(void** payload, size_t& payload_len, struct ucred** cred) {
    if (io_uring_peek_cqe(&mCtx->ring, &cqe) < 0) {
        int ret = io_uring_wait_cqe(&mCtx->ring, &cqe);
        if (ret) {
            LOG(ERROR) << "WaitCqe failed: " << ret;
            ArmRecvMsg();
            return;
        }
    }

    if (cqe->res < 0) {
        io_uring_cqe_seen(&mCtx->ring, cqe);
        ArmRecvMsg();
        return;
    }

    active_buffer_id_ = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

    void* this_recv = buffers_[active_buffer_id_].get();
    struct io_uring_recvmsg_out* o = io_uring_recvmsg_validate(this_recv, cqe->res, &msg);

    if (!o) {
        return;
    }

    struct cmsghdr* cmsg;
    cmsg = io_uring_recvmsg_cmsg_firsthdr(o, &msg);

    struct ucred* cr = nullptr;
    while (cmsg != nullptr) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
            cr = (struct ucred*)CMSG_DATA(cmsg);
            break;
        }
        cmsg = io_uring_recvmsg_cmsg_nexthdr(o, &msg, cmsg);
    }

    if (cr == nullptr) {
        return;
    }

    *payload = io_uring_recvmsg_payload(o, &msg);
    payload_len = io_uring_recvmsg_payload_length(o, cqe->res, &msg);
    *cred = cr;
}
