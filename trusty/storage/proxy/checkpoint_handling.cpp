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

#include <fstab/fstab.h>
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

bool checkpointingDoneForever = false;
std::atomic<bool> voldPossibleCheckpointing = true;

class VoldListener : public BnVoldCheckpointListener {
  public:
    ScopedAStatus onCheckpointingComplete() final {
        assert(voldPossibleCheckpointing);
        voldPossibleCheckpointing.store(false);
        return ScopedAStatus::ok();
    }
};

}  // namespace

int is_data_checkpoint_active(bool* active) {
    if (!active) {
        ALOGE("active out parameter is null");
        return 0;
    }

    *active = false;

    if (checkpointingDoneForever) {
        return 0;
    }

    android::fs_mgr::Fstab procMounts;
    bool success = android::fs_mgr::ReadFstabFromFile("/proc/mounts", &procMounts);
    if (!success) {
        ALOGE("Could not parse /proc/mounts\n");
        /* Really bad. Tell the caller to abort the write. */
        return -1;
    }

    android::fs_mgr::FstabEntry* dataEntry =
            android::fs_mgr::GetEntryForMountPoint(&procMounts, "/data");
    if (dataEntry == NULL) {
        ALOGE("/data is not mounted yet\n");
        return 0;
    }

    /* We can't handle e.g., ext4. Nothing we can do about it for now. */
    if (dataEntry->fs_type != "f2fs") {
        ALOGW("Checkpoint status not supported for filesystem %s\n", dataEntry->fs_type.c_str());
        checkpointingDoneForever = true;
        return 0;
    }

    /*
     * The data entry looks like "... blah,checkpoint=disable:0,blah ...".
     * checkpoint=disable means checkpointing is on (yes, arguably reversed).
     */
    size_t checkpointPos = dataEntry->fs_options.find("checkpoint=disable");
    if (checkpointPos == std::string::npos) {
        /* Assumption is that once checkpointing turns off, it stays off */
        checkpointingDoneForever = true;
    } else {
        *active = true;
    }

    return 0;
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
    auto binder =
            ndk::SpAIBinder(AServiceManager_waitForService("android.system.vold.IVold/default"));
    auto vold = IVold::fromBinder(binder);

    ALOGW("wcarv: storageproxyd registering vold listener\n");
    CheckpointingState state;
    ScopedAStatus ret =
            vold->registerCheckpointListener(SharedRefBase::make<VoldListener>(), &state);
    if (!ret.isOk()) {
        ALOGE("Could not register VoldCheckpointListener: %s\n", ret.getDescription().c_str());
        return ret.getExceptionCode();
    }
    ALOGW("wcarv: storageproxyd registered vold listener in %s\n", toString(state).c_str());

    if (state == CheckpointingState::CHECKPOINTING_COMPLETE) {
        voldPossibleCheckpointing.store(false);
    }
    return 0;
}

int checkpointing_get_state(struct storage_msg* msg, const void*, size_t req_len, struct watcher*) {
    if (req_len != 0) {
        ALOGW("malformed rpmb request: invalid length (%zu < %zu)\n", req_len,
              static_cast<size_t>(0));
        msg->result = STORAGE_ERR_NOT_VALID;
        goto err_response;
    }

    msg->result = STORAGE_NO_ERROR;
    struct storage_checkpointing_state_resp resp;
    resp.data = voldPossibleCheckpointing.load() ? 1 : 0;
    ALOGW("wcarv: storageproxyd read checkpointing state %d\n", resp.data);
    return ipc_respond(msg, &resp, sizeof(resp));

err_response:
    return ipc_respond(msg, NULL, 0);
}