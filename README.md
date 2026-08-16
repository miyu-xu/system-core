# Android system core subset

[简体中文](README.zh-CN.md) | English

This repository is the manifest-pinned Android system/core baseline used by BSCP guest and host
compatibility work. Changes on the BSCP branch are limited to reviewed cross-platform integration
above the Android baseline. Build and test it as part of the synchronized workspace; do not copy
files into the root orchestration repository.

Security-sensitive init, property, storage, and policy behavior must retain Android's fail-closed
defaults. Development-only boot experiments and policy bypasses are not accepted on the release
branch.
