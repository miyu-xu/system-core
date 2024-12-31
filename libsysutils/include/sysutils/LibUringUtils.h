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
#pragma once

#include <memory>
#include <vector>

#include <liburing.h>

class LibUringUtils {
  public:
    LibUringUtils(int socket_fd);
    ~LibUringUtils();

    /* Setup io_uring ring buffer */
    bool SetupIoUring(int queue_size);

    /* Allocate 'num_buffers' of size 'buf_size' */
    void AllocateBuffers(size_t num_buffers, size_t buf_size);

    /* Register buffers with io_uring */
    bool RegisterBuffers();

    /* ARM io_uring recvmsg opcode */
    bool ArmRecvMsg();

    /* Release the buffer to io_uring */
    void ReleaseBuffer();

    /* Receive payload data of size payload_len. Additionally, receive
     * credential data */
    void ReceiveData(void** payload, size_t& payload_len, struct ucred** cred);

    /* check if io_uring is supported */
    static bool isIouringEnabled();

  private:
    struct uring_context {
        struct io_uring ring;
    };
    int socket_;
    std::unique_ptr<uring_context> mCtx;
    std::vector<std::unique_ptr<uint8_t[]>> buffers_;
    struct msghdr msg;
    int control_len_;
    size_t num_buffers_ = 0;
    int buffer_size_;
    int active_buffer_id_ = -1;
    struct io_uring_cqe* cqe;
    const int bgid_ = 7;

    struct io_uring_buf_ring* br_;
    bool registered_buffers_ = false;
    bool ring_setup_ = false;
};
