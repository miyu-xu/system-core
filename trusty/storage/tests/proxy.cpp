

#include <gtest/gtest.h>

TEST(StorageServiceTest, GetCheckpointState) {
    // struct storage_msg msg = { .cmd = STORAGE_CHECKPOINTING_STATE, .flags = 0};
    // struct iovec tx[1] = {{&msg, sizeof(msg)}};
    // struct storage_checkpointing_state_resp rsp;
    // struct iovec rx[2] = {{&msg, sizeof(msg)}, {&rsp, sizeof(rsp)}};

    // ssize_t rc = send_reqv(session, tx, 1, rx, 1);
    // ASSERT_EQ(sizeof(msg) + sizeof(rsp), rc);

    // ASSERT_EQ(0, rsp.data);
}