/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "checkpoint_handling.h"
#include "ipc.h"
#include "log.h"

#include <unistd.h>
#include <cstring>
#include <string>

#include <aidl/android/system/vold/BnVoldCheckpointListener.h>
#include <aidl/android/system/vold/IVold.h>
#include <android/binder_auto_utils.h>
#include <android/binder_interface_utils.h>
#include <android/binder_manager.h>
#include <libgsi/libgsi.h>

namespace {

using ::aidl::android::system::vold::BnVoldCheckpointListener;
using ::aidl::android::system::vold::CheckpointingState;
using ::aidl::android::system::vold::IVold;
using ::aidl::android::system::vold::toString;
using ::ndk::ScopedAStatus;
using ::ndk::SharedRefBase;

const char kVoldService[] = "android.system.vold.IVold/default";

std::atomic<bool> voldPossibleCheckpointing = true;

class VoldListener : public BnVoldCheckpointListener {
  public:
    ScopedAStatus onCheckpointingComplete() final {
        voldPossibleCheckpointing.store(false);
        ALOGD("VoldCheckpointListener reported checkpointing complete\n");
        return ScopedAStatus::ok();
    }
};

}  // namespace

bool is_data_checkpoint_active() {
    return voldPossibleCheckpointing.load();
}

/**
 * is_gsi_running() - Check if a GSI image is running via DSU.
 *
 * This function is equivalent to android::gsi::IsGsiRunning(), but this API is
 * not yet vendor-accessible although the underlying metadata file is.
 *
 */
bool is_gsi_running() {
    /* TODO(b/210501710): Expose GSI image running state to vendor storageproxyd */
    return !access(android::gsi::kGsiBootedIndicatorFile, F_OK);
}

int vold_connect() {
    auto binder = ndk::SpAIBinder(AServiceManager_waitForService(kVoldService));
    if (binder == nullptr) {
        ALOGD("Got null binder for %s (was %sdeclared)\n", kVoldService,
              AServiceManager_isDeclared(kVoldService) ? "" : "not ");
        return -1;
    }

    auto vold = IVold::fromBinder(binder);
    if (vold == nullptr) {
        ALOGI("Could not convert binder to android::system::vold::IVold\n");
        return -1;
    }

    CheckpointingState state;
    ScopedAStatus ret =
            vold->registerCheckpointListener(SharedRefBase::make<VoldListener>(), &state);
    if (!ret.isOk()) {
        ALOGE("Could not register VoldCheckpointListener: %s\n", ret.getDescription().c_str());
        return -1;
    }
    ALOGI("Registered VoldCheckpointListener in %s\n", toString(state).c_str());

    if (state == CheckpointingState::CHECKPOINTING_COMPLETE) {
        voldPossibleCheckpointing.store(false);
    }

    return 0;
}

static storage_checkpoint_state checkpointing_state() {
    if (voldPossibleCheckpointing.load()) {
        return STORAGE_CHECKPOINT_STATE_POSSIBLE;
    }
    return STORAGE_CHECKPOINT_STATE_DONE;
}

int checkpointing_get_state(struct storage_msg* msg, const void*, size_t req_len, struct watcher*) {
    if (req_len != 0) {
        ALOGE("%s: invalid request length (%zu != %d)\n", __func__, req_len, 0);
        msg->result = STORAGE_ERR_NOT_VALID;
        goto err_response;
    }

    msg->result = STORAGE_NO_ERROR;
    struct storage_checkpointing_state_resp resp;
    resp.state = checkpointing_state();
    ALOGD("Read checkpointing state %d\n", resp.data);
    return ipc_respond(msg, &resp, sizeof(resp));

err_response:
    return ipc_respond(msg, NULL, 0);
}