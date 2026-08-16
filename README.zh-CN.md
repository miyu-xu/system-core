# Android system core 子集

简体中文 | [English](README.md)

本仓库是 manifest 固定的 Android system/core 基线，供 BSCP Guest 与主机兼容工作使用。
BSCP 分支只保留 Android 基线之上经过审查的跨平台集成修改。请在同步工作区内构建和测试，
不要把文件复制到根编排仓库重复提交。

涉及 init、property、存储和策略的安全敏感行为必须保持 Android 的关闭失败默认值。仅用于
开发的启动试验和策略绕过不能进入发布分支。
