# 实施计划：pi-deploy.sh 自动化部署脚本

## 概述

基于 `pi-build.sh` 的双模式架构，创建 `scripts/pi-deploy.sh` 部署脚本，实现 build + install + config deploy + service restart 的完整部署流程。脚本采用函数化组织，每个部署步骤封装为独立函数，支持 macOS SSH 远程和 Pi 5 本地两种执行模式。

验证方式：shellcheck 静态分析 + `bash -n` 语法检查 + Pi 5 手动集成测试。

## 任务

- [x] 1. 创建脚本骨架与基础设施
  - [x] 1.1 创建 `scripts/pi-deploy.sh` 文件，包含 shebang、`set -euo pipefail`、全局变量定义（PI_HOST / PI_USER / PI_REPO_DIR / 系统路径常量）、`log()` 日志函数、`parse_args()` 参数解析函数（--skip-build / --skip-test / --no-pull）
    - 参考 `scripts/pi-build.sh` 的变量命名和默认值
    - 系统路径常量：INSTALL_BIN、CONFIG_DIR、CERTS_DIR、PLUGINS_DIR、SERVICE_NAME
    - 部署摘要追踪变量：SUMMARY_BINARY / SUMMARY_CONFIG / SUMMARY_CERTS / SUMMARY_PLUGINS / SUMMARY_SERVICE
    - SHALL NOT 使用 `cat <<` heredoc 方式写入文件（用 fsWrite/fsAppend 工具创建脚本）
    - _需求: 8.4, 8.5, 1.6, 1.7, 1.8_

  - [x] 1.2 实现 `main()` 主流程函数，按顺序调用各步骤函数：parse_args → do_pull → do_build → do_test → install_binary → deploy_config → deploy_certs → install_plugins → restart_service → print_summary
    - 本地模式入口：检测 `uname -s` 为 Linux 时直接调用 main
    - _需求: 7.4, 8.4_

- [x] 2. 实现编译阶段函数
  - [x] 2.1 实现 `do_pull()` 函数：执行 `git pull`，受 `--no-pull` 和 `--skip-build` 控制
    - _需求: 1.1, 1.8_

  - [x] 2.2 实现 `do_build()` 函数：cmake configure（Release）+ cmake build，受 `--skip-build` 控制
    - `--skip-build` 时检查 `device/build/raspi-eye` 是否存在，不存在则报错退出
    - _需求: 1.2, 1.6, 1.9, 1.4_

  - [x] 2.3 实现 `do_test()` 函数：执行 ctest，受 `--skip-test` 和 `--skip-build` 控制
    - 测试失败时输出失败信息并 exit 1
    - _需求: 1.3, 1.5, 1.7_

- [x] 3. 实现部署阶段函数（二进制 + 配置文件）
  - [x] 3.1 实现 `install_binary()` 函数：sudo cp 二进制到 `/usr/local/bin/raspi-eye`，设置权限 755
    - 幂等：已存在时覆盖
    - _需求: 2.1, 2.2, 2.3, 8.2_

  - [x] 3.2 实现 `deploy_config()` 函数：创建 `/etc/raspi-eye/` 目录，按决策树部署 config.toml
    - 已存在 config.toml → 跳过，输出提示
    - 不存在 → 优先复制 `device/config/config.toml`，其次 `config.toml.example`
    - 首次部署时 sed 替换证书路径（`device/certs/` → `/etc/raspi-eye/certs/`）
    - 设置权限 640，所有者 pi:pi
    - 两个源文件都不存在 → 警告，跳过
    - _需求: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 10.1, 10.3_

- [x] 4. 实现部署阶段函数（证书 + 插件 + 服务）
  - [x] 4.1 实现 `deploy_certs()` 函数：创建 `/etc/raspi-eye/certs/` 目录，复制 .pem 和 .key 文件
    - .pem 权限 644，.key 权限 600，所有者 pi:pi
    - 目录不存在或为空 → 警告，跳过
    - _需求: 4.1, 4.2, 4.3, 4.4, 10.2, 10.3_

  - [x] 4.2 实现 `install_plugins()` 函数：创建 `/usr/local/lib/raspi-eye/plugins/` 目录，复制 .so 文件
    - 权限 755
    - 无 .so 文件 → 警告，跳过
    - _需求: 5.1, 5.2, 5.3, 5.4, 10.5_

  - [x] 4.3 实现 `restart_service()` 函数：daemon-reload → restart → sleep 3 → is-active 检查
    - 服务 active → 输出成功
    - 服务非 active → 输出 journalctl 最近 20 行，exit 1
    - service 文件不存在 → 警告提示 spec-20，跳过
    - _需求: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6_

  - [x] 4.4 实现 `print_summary()` 函数：输出部署摘要，包含各步骤状态
    - 使用 SUMMARY_* 变量输出每个步骤的结果
    - 被跳过的步骤标注原因
    - _需求: 9.1, 9.2_

- [x] 5. 检查点 - 本地模式验证
  - 运行 `shellcheck scripts/pi-deploy.sh` 确保无警告
  - 运行 `bash -n scripts/pi-deploy.sh` 确保语法正确
  - 确保所有测试通过，如有问题请询问用户

- [x] 6. 实现 macOS 远程模式
  - [x] 6.1 实现 macOS 远程模式入口：检测 `uname -s` 为 Darwin 时，SSH 连接检测 + 远程执行
    - SSH 连接检测：`ssh -o ConnectTimeout=5 -o BatchMode=yes` 测试连接
    - 连接失败 → 输出错误，exit 1
    - 连接成功 → 通过 `ssh ... bash -s` 传递完整部署逻辑到 Pi 5 执行
    - 命令行参数在 macOS 端解析后传递给远程脚本
    - 参考 `pi-build.sh` 的远程模式实现
    - _需求: 7.1, 7.2, 7.3_

- [x] 7. 最终检查点 - 完整验证
  - 运行 `shellcheck scripts/pi-deploy.sh` 确保无警告
  - 运行 `bash -n scripts/pi-deploy.sh` 确保语法正确
  - 确认脚本有执行权限（chmod +x）
  - 确保所有测试通过，如有问题请询问用户

## 备注

- 标记 `*` 的任务为可选，可跳过以加速 MVP
- 每个任务引用具体需求编号以确保可追溯性
- 检查点确保增量验证
- 本 Spec 为纯 bash 脚本，不涉及 C++ 代码和 ctest 测试
- 验证方式：shellcheck 静态分析 + bash -n 语法检查 + Pi 5 手动集成测试
- SHALL NOT 使用 `cat << heredoc` 方式写入文件，用 fsWrite/fsAppend 工具创建脚本内容
