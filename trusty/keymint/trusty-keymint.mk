#
# Copyright (C) 2024 The Android Open-Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

#
# This makefile should be included by devices that use Trusty TEE
# to pull in a set of Trusty KeyMint specific modules.
#
# When Keymint HAL is selected by vendor apex (KEYMINT_HAL_VENDOR_APEX_SELECT=true)
# - both the Rust and legacy CPP service are built but disabled by default
# - when TRUSTY_SYSTEM_VM is defined, the system keymint service is also built
#
# Otherwise (KEYMINT_HAL_VENDOR_APEX_SELECT=false):
# a single KeyMint HAL service implementation is selected at build time.
# This must be synchronized with the TA implementation included in Trusty TEE.
# Possible values:
# - Rust implementation for Trusty TEE (no Trusty VM support):
#   export TRUSTY_KEYMINT_IMPL=rust
# - C++ implementation (default): (any other value or unset TRUSTY_KEYMINT_IMPL)

ifeq ($(KEYMINT_HAL_VENDOR_APEX_SELECT),true)
    PRODUCT_PACKAGES += \
        android.hardware.security.keymint-service.trusty_tee.cpp \
        android.hardware.security.keymint-service.trusty_tee \

    ifeq ($(findstring $(TRUSTY_SYSTEM_VM),secure nonsecure),$(TRUSTY_SYSTEM_VM))
        PRODUCT_PACKAGES += \
            android.hardware.security.keymint-service.trusty_system_vm \

    endif

else
ifeq ($(TRUSTY_KEYMINT_IMPL),rust)
    PRODUCT_PACKAGES += \
        android.hardware.security.keymint-service.rust.trusty
else
    # Default to the C++ implementation
    PRODUCT_PACKAGES += \
        android.hardware.security.keymint-service.trusty \

endif

endif
