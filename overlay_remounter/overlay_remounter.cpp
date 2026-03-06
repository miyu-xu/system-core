/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

struct ChildMount {
    std::string mount_point;
    std::string temp_mount_point;
};

int main(int /*argc*/, char** argv) {
    android::base::InitLogging(argv, &android::base::KernelLogger);
    LOG(INFO) << "Overlay remounter will remount all overlay mount points in the overlay_remounter "
                 "domain";

    // Remount ouerlayfs
    std::string contents;
    auto result = android::base::ReadFileToString("/proc/mounts", &contents, true);

    auto lines = android::base::Split(contents, "\n");
    for (auto const& line : lines) {
        if (!android::base::StartsWith(line, "overlay")) {
            continue;
        }
        auto bits = android::base::Split(line, " ");

        // Find and preserve child mount points using bind mounts
        std::string parent_mount = bits[1];
        std::vector<ChildMount> child_mounts;
        std::string child_contents;
        if (android::base::ReadFileToString("/proc/mounts", &child_contents, true)) {
            auto child_lines = android::base::Split(child_contents, "\n");
            int child_idx = 0;
            for (auto const& child_line : child_lines) {
                if (child_line.empty()) continue;
                auto child_bits = android::base::Split(child_line, " ");
                if (child_bits.size() < 2) continue;

                // Check if this is a child mount point
                if (android::base::StartsWith(child_bits[1], parent_mount + "/")) {
                    std::string child_mount = child_bits[1];
                    std::string temp_mount = "/mnt/.overlay_remounter_tmp_" + std::to_string(child_idx);

                    LOG(INFO) << "Preserving child mount: " << child_mount
                             << " to temporary location: " << temp_mount;

                    // Create temporary mount point
                    mkdir(temp_mount.c_str(), 0755);

                    // Bind mount child to temporary location
                    if (mount(child_mount.c_str(), temp_mount.c_str(), nullptr,
                             MS_BIND | MS_REC, nullptr) == 0) {
                        LOG(INFO) << "Successfully preserved: " << child_mount;

                        // Now umount the original child mount to unblock parent umount
                        LOG(INFO) << "Umounting original child mount: " << child_mount;
                        if (umount(child_mount.c_str()) == 0) {
                            child_mounts.push_back({child_mount, temp_mount});
                            LOG(INFO) << "Successfully unmounted: " << child_mount;
                        } else {
                            PLOG(WARNING) << "Failed to umount child mount: " << child_mount;
                            // If umount failed, try to clean up temp mount
                            umount(temp_mount.c_str());
                        }
                    } else {
                        PLOG(WARNING) << "Failed to preserve child mount: " << child_mount;
                    }

                    child_idx++;
                }
            }
        }

        // Now umount the parent (child mounts already unmounted)
        if (int result = umount(bits[1].c_str()); result == -1) {
            PLOG(FATAL) << "umount FAILED: " << bits[1];
        }

        // Remount parent as overlay
        std::string options;
        for (auto const& option : android::base::Split(bits[3], ",")) {
            if (option == "ro" || option == "seclabel" || option == "noatime") continue;
            if (!options.empty()) options += ',';
            options += option;
        }
        result = mount("overlay", bits[1].c_str(), "overlay", MS_RDONLY | MS_NOATIME,
                       options.c_str());
        if (result == 0) {
            LOG(INFO) << "mount succeeded: " << bits[1] << " " << options;
        } else {
            PLOG(FATAL) << "mount FAILED: " << bits[1] << " " << bits[3];
        }

        // Move child mounts back from temporary locations to their original mount points
        for (auto const& child : child_mounts) {
            LOG(INFO) << "Restoring child mount: " << child.temp_mount_point
                     << " to " << child.mount_point;

            // Move mount from temp location to original location on the new overlay
            if (mount(child.temp_mount_point.c_str(), child.mount_point.c_str(),
                     nullptr, MS_MOVE, nullptr) == 0) {
                LOG(INFO) << "Successfully restored: " << child.mount_point;
            } else {
                PLOG(WARNING) << "Failed to restore child mount: " << child.mount_point;
            }

            // Clean up temporary mount point
            rmdir(child.temp_mount_point.c_str());
        }
    }

    const char* path = "/system/bin/init";
    const char* args[] = {path, "second_stage", nullptr};
    execv(path, const_cast<char**>(args));

    // execv() only returns if an error happened, in which case we
    // panic and never return from this function.
    PLOG(FATAL) << "execv(\"" << path << "\") failed";

    return 1;
}
