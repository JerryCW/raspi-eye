# 需求文档

## 简介

当前 Pi 5 上的部署流程完全手动：SSH 登录 → git pull → cmake configure + build → 手动运行二进制。缺少安装到系统路径、配置文件部署、证书部署、GStreamer 插件路径设置、systemd 服务重启等生产环境必需的步骤。

本 Spec 创建自动化部署脚本 `scripts/pi-deploy.sh`，实现一键 build + install + deploy + restart：
- 编译 Release 版本（不带 ASan）
- 安装二进制到 `/usr/local/bin/raspi-eye`
- 部署配置文件到 `/etc/raspi-eye/config.toml`（首次从 `config.toml.example` 生成，后续保留用户修改）
- 部署证书到 `/etc/raspi-eye/certs/`
- 安装 GStreamer 插件到 `/usr/local/lib/raspi-eye/plugins/`
- 重启 `raspi-eye.service` systemd 服务
- 支持 macOS 远程触发（SSH）和 Pi 5 本地执行两种模式（复用 `pi-build.sh` 的双模式架构）

本 Spec 为纯脚本 Spec，不修改 C++ 源码。systemd service 文件由 spec-20 创建，本 Spec 假设 service 名为 `raspi-eye.service`。

## 术语表

- **Deploy_Script**: 部署脚本 `scripts/pi-deploy.sh`，负责 build + install + config deploy + service restart 的完整部署流程
- **pi-build.sh**: 现有构建脚本，仅做 git pull + cmake + build + ctest，不做 install/deploy
- **provision-device.sh**: 现有 AWS IoT 设备注册脚本，在开发机上运行（需 AWS AKSK），负责证书生成和 `device/config/config.toml` 生成。Pi 5 上不调用，部署脚本只复制仓库中已有的文件
- **config.toml**: 应用配置文件，由 `provision-device.sh` 生成到 `device/config/config.toml`，部署时复制到 `/etc/raspi-eye/config.toml`
- **config.toml.example**: 配置文件模板，位于 `device/config/config.toml.example`，首次部署时作为默认配置源
- **raspi-eye.service**: systemd 服务单元文件，由 spec-20 创建，本 Spec 假设已存在
- **System_Install_Path**: 二进制安装路径 `/usr/local/bin/raspi-eye`
- **System_Config_Dir**: 系统配置目录 `/etc/raspi-eye/`
- **System_Certs_Dir**: 系统证书目录 `/etc/raspi-eye/certs/`
- **System_Plugins_Dir**: GStreamer 插件安装目录 `/usr/local/lib/raspi-eye/plugins/`

## 需求

### 需求 1：Release 编译

**用户故事：** 作为开发者，我希望部署脚本自动编译 Release 版本（不带 ASan），以确保部署到 Pi 5 的二进制是生产优化版本。

#### 验收标准

1. WHEN Deploy_Script 在 Pi 5 上执行, THE Deploy_Script SHALL 执行 git pull 拉取最新代码
2. WHEN Deploy_Script 执行编译步骤, THE Deploy_Script SHALL 使用 `CMAKE_BUILD_TYPE=Release` 配置 cmake 并编译项目
3. WHEN 编译完成, THE Deploy_Script SHALL 执行 ctest 运行所有测试，确保测试通过后再继续部署步骤
4. IF 编译失败, THEN THE Deploy_Script SHALL 输出错误信息并以非零退出码终止，不执行后续部署步骤
5. IF 测试失败, THEN THE Deploy_Script SHALL 输出失败的测试信息并以非零退出码终止，不执行后续部署步骤
6. WHEN Deploy_Script 接收到 `--skip-build` 参数, THE Deploy_Script SHALL 跳过 git pull、cmake configure、编译和测试步骤，直接使用已有的 `device/build/raspi-eye` 二进制执行部署
7. WHEN Deploy_Script 接收到 `--skip-test` 参数, THE Deploy_Script SHALL 跳过 ctest 测试步骤，编译完成后直接执行部署
8. WHEN Deploy_Script 接收到 `--no-pull` 参数, THE Deploy_Script SHALL 跳过 git pull 步骤，使用当前工作区代码编译
9. IF `--skip-build` 被指定但 `device/build/raspi-eye` 不存在, THEN THE Deploy_Script SHALL 输出错误信息并以非零退出码终止

### 需求 2：二进制安装

**用户故事：** 作为运维人员，我希望部署脚本将编译好的二进制安装到系统路径，以便 systemd 服务能通过标准路径启动应用。

#### 验收标准

1. WHEN 编译和测试通过, THE Deploy_Script SHALL 使用 sudo 将 `device/build/raspi-eye` 复制到 `/usr/local/bin/raspi-eye`
2. WHEN 安装二进制文件, THE Deploy_Script SHALL 设置文件权限为 755（所有者可读写执行，其他用户可读执行）
3. IF `/usr/local/bin/raspi-eye` 已存在, THEN THE Deploy_Script SHALL 覆盖现有文件（幂等操作）

### 需求 3：配置文件部署

**用户故事：** 作为运维人员，我希望部署脚本将配置文件部署到系统路径，首次部署自动生成默认配置，后续部署保留用户修改。

#### 验收标准

1. WHEN Deploy_Script 执行配置部署步骤, THE Deploy_Script SHALL 创建 `/etc/raspi-eye/` 目录（如不存在）
2. WHEN `/etc/raspi-eye/config.toml` 不存在且 `device/config/config.toml` 存在, THE Deploy_Script SHALL 将 `device/config/config.toml` 复制到 `/etc/raspi-eye/config.toml`
3. WHEN `/etc/raspi-eye/config.toml` 不存在且 `device/config/config.toml` 不存在但 `device/config/config.toml.example` 存在, THE Deploy_Script SHALL 将 `device/config/config.toml.example` 复制到 `/etc/raspi-eye/config.toml`
4. WHEN `/etc/raspi-eye/config.toml` 已存在, THE Deploy_Script SHALL 跳过配置文件复制并输出提示信息，保留用户已有的修改
5. IF `device/config/config.toml` 和 `device/config/config.toml.example` 均不存在, THEN THE Deploy_Script SHALL 输出警告信息并跳过配置文件部署步骤，不终止脚本执行
6. WHEN 配置文件被复制, THE Deploy_Script SHALL 设置文件权限为 640（所有者可读写，组可读）
7. WHEN 部署配置文件, THE Deploy_Script SHALL 仅在首次部署（新复制的文件）时更新配置文件中的证书路径字段（cert_path、key_path、ca_path）为系统路径 `/etc/raspi-eye/certs/` 下的对应文件名；已存在的 config.toml 不做路径替换

### 需求 4：证书文件部署

**用户故事：** 作为运维人员，我希望部署脚本将证书文件部署到系统路径，以便应用通过标准路径访问 AWS IoT 证书。

#### 验收标准

1. WHEN Deploy_Script 执行证书部署步骤, THE Deploy_Script SHALL 创建 `/etc/raspi-eye/certs/` 目录（如不存在）
2. WHEN `device/certs/` 目录包含 `.pem` 或 `.key` 文件, THE Deploy_Script SHALL 将所有 `.pem` 和 `.key` 文件复制到 `/etc/raspi-eye/certs/`
3. WHEN 证书文件被复制, THE Deploy_Script SHALL 设置 `.pem` 文件权限为 644，`.key` 文件权限为 600
4. IF `device/certs/` 目录不存在或为空, THEN THE Deploy_Script SHALL 输出警告信息并跳过证书部署步骤，不终止脚本执行

### 需求 5：GStreamer 插件安装

**用户故事：** 作为运维人员，我希望部署脚本将 GStreamer 插件安装到系统路径，以便 systemd 服务通过 `GST_PLUGIN_PATH` 环境变量加载插件。

#### 验收标准

1. WHEN Deploy_Script 执行插件安装步骤, THE Deploy_Script SHALL 创建 `/usr/local/lib/raspi-eye/plugins/` 目录（如不存在）
2. WHEN `device/plugins/` 目录包含 `.so` 文件, THE Deploy_Script SHALL 将所有 `.so` 文件复制到 `/usr/local/lib/raspi-eye/plugins/`
3. WHEN 插件文件被复制, THE Deploy_Script SHALL 设置文件权限为 755
4. IF `device/plugins/` 目录不存在或不包含 `.so` 文件, THEN THE Deploy_Script SHALL 输出警告信息并跳过插件安装步骤，不终止脚本执行

### 需求 6：systemd 服务重启

**用户故事：** 作为运维人员，我希望部署脚本在安装完成后自动重启 systemd 服务，以使新版本立即生效。

#### 验收标准

1. WHEN 所有安装步骤完成, THE Deploy_Script SHALL 执行 `sudo systemctl daemon-reload` 重新加载 systemd 配置
2. WHEN daemon-reload 完成, THE Deploy_Script SHALL 执行 `sudo systemctl restart raspi-eye.service` 重启服务
3. WHEN 服务重启后, THE Deploy_Script SHALL 等待 3 秒后执行 `systemctl is-active raspi-eye.service` 检查服务状态
4. IF 服务状态为 active, THEN THE Deploy_Script SHALL 输出部署成功信息
5. IF 服务状态不为 active, THEN THE Deploy_Script SHALL 输出最近 20 行 journalctl 日志并以非零退出码终止
6. IF `raspi-eye.service` 服务单元文件不存在, THEN THE Deploy_Script SHALL 输出警告信息提示用户需要先创建 service 文件（spec-20），跳过服务重启步骤但不终止脚本

### 需求 7：macOS 远程触发模式

**用户故事：** 作为开发者，我希望从 macOS 开发机远程触发 Pi 5 上的部署流程，以避免手动 SSH 登录操作。

#### 验收标准

1. WHEN Deploy_Script 在 macOS 上执行, THE Deploy_Script SHALL 通过 SSH 连接到 Pi 5（使用 `PI_HOST`、`PI_USER`、`PI_REPO_DIR` 环境变量，默认值与 `pi-build.sh` 一致）
2. WHEN SSH 连接成功, THE Deploy_Script SHALL 在 Pi 5 上远程执行完整的部署流程（build + install + deploy + restart）
3. IF SSH 连接失败, THEN THE Deploy_Script SHALL 输出连接错误信息并以非零退出码终止
4. WHEN Deploy_Script 在 Linux（Pi 5）上执行, THE Deploy_Script SHALL 直接在本地执行部署流程，不使用 SSH

### 需求 8：幂等性与安全性

**用户故事：** 作为运维人员，我希望部署脚本可以安全地重复执行，不会因为重复运行而产生错误或破坏现有配置。

#### 验收标准

1. THE Deploy_Script SHALL 在创建目录前检查目录是否已存在，已存在时跳过创建
2. THE Deploy_Script SHALL 在复制二进制和插件文件时覆盖已有文件（幂等更新）
3. THE Deploy_Script SHALL 在复制配置文件时保留已有的用户修改（不覆盖已存在的 config.toml）
4. THE Deploy_Script SHALL 使用 `set -euo pipefail` 确保任何命令失败时立即终止
5. THE Deploy_Script SHALL 在每个关键步骤输出带 `[pi-deploy]` 前缀的日志信息，便于排查问题

### 需求 9：部署摘要

**用户故事：** 作为运维人员，我希望部署完成后看到清晰的摘要信息，以确认所有步骤的执行结果。

#### 验收标准

1. WHEN 部署流程完成, THE Deploy_Script SHALL 输出部署摘要，包含以下信息：二进制安装路径、配置文件路径（及是否为新部署或保留已有）、证书部署状态、插件安装状态、服务重启状态
2. WHEN 部署流程中有步骤被跳过（如证书目录为空、service 文件不存在）, THE Deploy_Script SHALL 在摘要中标注被跳过的步骤及原因

### 需求 10：运行权限

**用户故事：** 作为运维人员，我希望 raspi-eye 服务以 pi 用户运行而非 root，遵循最小权限原则。

#### 验收标准

1. THE Deploy_Script SHALL 将 `/etc/raspi-eye/` 目录及其内容的所有者设置为 `pi:pi`
2. THE Deploy_Script SHALL 将 `/etc/raspi-eye/certs/` 目录及其内容的所有者设置为 `pi:pi`
3. THE Deploy_Script SHALL 确保 config.toml 权限为 640（pi 用户可读写，组可读），私钥文件权限为 600（仅 pi 用户可读写）
4. THE Deploy_Script SHALL 确保 `/usr/local/bin/raspi-eye` 权限为 755（所有用户可执行）
5. THE Deploy_Script SHALL 确保 `/usr/local/lib/raspi-eye/plugins/` 目录及 `.so` 文件对 pi 用户可读可执行

- 脚本放在 `scripts/` 目录下，命名为 `pi-deploy.sh`
- 不修改 C++ 源码（纯脚本 Spec）
- systemd service 文件由 spec-20 创建，本 Spec 假设 service 名为 `raspi-eye.service`
- systemd service 的 `Environment=GST_PLUGIN_PATH=/usr/local/lib/raspi-eye/plugins/` 由 spec-20 负责配置，本 Spec 只负责将插件文件复制到该路径
- config.toml 系统路径为 `/etc/raspi-eye/config.toml`
- 证书系统路径为 `/etc/raspi-eye/certs/`
- 二进制安装路径为 `/usr/local/bin/raspi-eye`
- GStreamer 插件目录为 `/usr/local/lib/raspi-eye/plugins/`
- 服务以 `pi` 用户运行（非 root），systemd service 中 `User=pi`（由 spec-20 配置）
- 部署的配置文件和证书所有者为 `pi:pi`，确保 pi 用户有读取权限
- 需要 sudo 权限执行安装步骤（安装到系统路径），但服务本身不以 root 运行
- 脚本需要幂等（重复执行不出错）
- 环境变量 `PI_HOST`（默认 raspberrypi.local）、`PI_USER`（默认 pi）、`PI_REPO_DIR`（默认 ~/raspi-eye）与 `pi-build.sh` 保持一致
- 支持命令行参数：`--skip-build`（跳过编译）、`--skip-test`（跳过测试）、`--no-pull`（跳过 git pull）
- 不包含回滚机制、蓝绿部署、版本管理、远程日志收集

## 禁止项

- SHALL NOT 在脚本中硬编码 AWS 凭证、密钥内容或任何 secret
- SHALL NOT 在日志输出中打印证书内容、密钥内容或 token
- SHALL NOT 使用 `cat <<` heredoc 方式写入文件
- SHALL NOT 覆盖已存在的 `/etc/raspi-eye/config.toml`（保留用户修改）
- SHALL NOT 在测试未通过的情况下继续执行部署步骤
- SHALL NOT 以 root 用户运行 raspi-eye 服务（服务必须以 pi 用户运行）
