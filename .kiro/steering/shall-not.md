---
inclusion: always
---

# SHALL NOT — 禁止项池

从开发 trace 中提炼的已知失败模式，按 Spec 三层分类。
编写 Spec 时必须检查此列表，将相关项复制到对应层级的文件中。

> 维护规则：同类问题在 `docs/development-trace.md` 中出现 ≥ 3 次时，提炼到此文件对应分类下。

---

## Requirements 层

_功能边界、用户交互、场景覆盖相关的禁止项。→ 补充到 requirements.md_

暂无。

---

## Design 层

_架构模式、API 选择、依赖兼容、接口契约相关的禁止项。→ 补充到 design.md_

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
  - 原因：泄露风险，一旦提交到 git 无法撤回
  - 建议：通过环境变量、配置文件（已加入 .gitignore）或 AWS IoT 证书机制获取

- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
  - 原因：日志可能被收集到云端或共享给他人
  - 建议：日志中只输出资源标识（如 thing-name），不输出凭证内容

- SHALL NOT 在 macOS 上直接在 main() 中运行含 autovideosink 的 GStreamer 管道（来源：spec-0 手动验证）
  - 原因：macOS 要求 NSApplication 在主线程运行，否则 GStreamer-GL 报警告且可能崩溃
  - 建议：用 `gst_macos_main()` 包装管道运行逻辑

---

## Tasks 层

_具体实现写法、性能预算、安全校验、边界处理相关的禁止项。→ 补充到 tasks.md 对应任务_

- SHALL NOT 直接使用系统 `python` 或 `python3` 执行项目 Python 代码或测试（来源：历史经验）
  - 原因：未激活 venv 导致依赖找不到，反复出现
  - 建议：执行任何 Python 命令前必须先 `source .venv-raspi-eye/bin/activate`

- SHALL NOT 直接运行测试可执行文件（如 `./build/log_test`、`device/build/smoke_test --gtest_print_time=1`），必须通过 `ctest --test-dir device/build --output-on-failure` 统一运行（来源：spec-0, spec-1, spec-1 共 3 次违规）
  - 原因：直接运行单个测试可执行文件容易遗漏其他测试、运行方式不统一、与 CI 集成不一致
  - 建议：所有测试验证统一使用 `ctest --test-dir device/build --output-on-failure`，禁止在子代理 prompt 中使用任何 `./build/<test_name>` 形式的命令

- SHALL NOT 将 .pem、.key、.env、证书文件提交到 git（来源：安全基线）
  - 原因：密钥泄露不可逆
  - 建议：已在 .gitignore 中排除，提交前用 `git status` 确认

- SHALL NOT 在 `g_print`/`g_printerr` 中使用非 ASCII 字符（来源：历史经验）
  - 原因：GLib 输出函数在部分终端环境下不正确处理 UTF-8，显示为乱码
  - 建议：日志和用户提示信息统一使用英文

- SHALL NOT 在子代理（subagent）的最终检查点任务中执行 git commit（来源：历史经验）
  - 原因：commit 后编排层还需更新 tasks.md 状态和写入 trace 记录，导致变更遗漏
  - 建议：git commit 统一由编排层在所有 post-task hook 完成后执行

- SHALL NOT 将新建文件直接放到项目根目录（来源：历史经验）
  - 原因：根目录应保持干净，文件散落在根目录会导致项目结构混乱
  - 建议：根据文件功能放到对应模块目录下（如 device/、docs/、.kiro/ 等），参考 `.kiro/steering/structure.md` 中的项目结构
