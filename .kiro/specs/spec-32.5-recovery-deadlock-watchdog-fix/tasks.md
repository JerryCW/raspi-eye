# Implementation Plan

> Spec 32.5: 恢复流程死锁 + 看门狗失明修复 — 任务清单（bugfix）

## Overview

按 bugfix 工作流编排：探索性测试与取证先行（在未修复代码上确认缺陷）→ 缺陷 1 修复（liveness 门控）→ 缺陷 2 封堵（依赖取证结论 gate）→ macOS 检查点 → Pi 5 端到端实测。
Task 1（取证）/ 2（exploration test）/ 3（preservation tests）相互独立可并行；Task 4 依赖 2+3；Task 5 依赖 1+3+4（取证 gate + refresh_liveness API）；Task 6 依赖 4+5；Task 7 依赖 6。
统一验证命令：`ctest --test-dir device/build --output-on-failure`。
项目惯例：agent 不自动 git commit；每个 task 完成后 `git status` 确认无敏感文件。

## Tasks

- [x] 1. 缺陷 2 取证：定位主线程同步 KVS 拆除路径（先取证后修复）
  - 静态审阅 design "Hypothesized Root Cause" 的 S1–S6 清单：逐条过 `device/src/pipeline_health.cpp`（`try_state_reset` / `attempt_recovery` / `try_full_rebuild` / `run_bounded` / `default_teardown`）与 `device/src/app_context.cpp` rebuild 回调中每一处 `gst_element_set_state` / `gst_object_unref` / 对象析构链，标记执行线程与"是否可能进入 kvssink change_state / dispose / KVS stream free"
  - gdb 堆栈存档比对：`/tmp/hang_stacks_20260903.txt`（`std::call_once` → `KinesisVideoStream::free()` → `pthread_join`，本地已归档）与 journal 时间线（"Attempting state reset recovery" 后 bounded 完成/超时日志一条不出）交叉验证，判定最可能调用点（当前最强假设：S1）
  - S1 动态旁证（macOS 可跑）：用 fakesink 管道驱动 `try_state_reset` 走"set_null_bounded 成功 + PLAYING 拉起失败"分支，用临时线程 id 记录（fprintf/GST_DEBUG）确认尾部裸 `set_state(NULL)` 发生在主调线程；观察完成后移除临时诊断代码
  - 取证结论记录到 `docs/development-trace.md`：定案调用点、S6 等排除项的排除理由、S2/S3 保守封堵的判定依据
  - **Gate（硬约束）**：若取证推翻 S1 假设（主线程进入 KVS free 的路径不在 S1–S3，如经由 S5 消息引用），先回 design.md 更新可疑面定位（re-hypothesize）再继续，不凭猜测扩大修改面
  - SHALL NOT 在取证结论记录到 development-trace 之前修改缺陷 2 相关代码
  - 完成后 `git status` 确认无敏感文件
  - _Requirements: 2.7_

- [x] 2. Write bug condition exploration test（缺陷 1 看门狗失明）
  - **Property 1: Bug Condition** — 主循环停摆时喂狗必停
  - **CRITICAL**: This test MUST FAIL on unfixed code — failure confirms the bug exists
  - **DO NOT attempt to fix the test or the code when it fails**
  - **NOTE**: This test encodes the expected behavior — it will validate the fix when it passes after implementation
  - **GOAL**: Surface counterexamples that demonstrate the bug exists
  - **Scoped PBT Approach**: 确定性 bug，scope 到具体失败场景——任意"超过 stale 阈值（10s）未刷新 liveness"的时间点，喂狗门控都应关闭
  - 在 `device/tests/sd_notifier_test.cpp` 中新增测试：`set_health_check` 注册恒返回 true（模拟死锁时状态机停在 RECOVERING 非 FATAL），模拟主循环死锁场景（未修复代码上根本不存在 liveness 刷新机制——这正是缺陷本身），断言 `EXPECT_FALSE(SdNotifier::watchdog_gate_open())`（编码 Expected Behavior：门控必须反映主循环活性，对齐 design Property 1/2/3）
  - 在未修复代码上运行：`watchdog_gate_open()` 仅查询 health_check → 恒返回 true → 测试 FAIL（确认缺陷 1 存在：门控只反映状态机标记，不反映主循环活性）
  - **注意（哨兵语义）**：design 决策点 2 规定"从未刷新（哨兵 0）视为门控开"，因此本测试的死锁建模在修复后需补齐 arrange 步骤（见 task 4.4：refresh 一次模拟曾经存活 → 缩小阈值 → 等待越过阈值），断言本身不变
  - **EXPECTED OUTCOME**: Test FAILS (this is correct — it proves the bug exists)
  - Document counterexamples: "liveness 从不刷新、健康状态非 FATAL 时门控仍开，WATCHDOG=1 照发（对应事故中喂狗持续 2 天 8 小时）"
  - Mark task complete when test is written, run, and failure is documented
  - SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure`
  - _Requirements: 2.1, 2.2, 2.6_

- [x] 3. Write preservation property tests (BEFORE implementing fix)
  - **Property 2: Preservation** — 正常喂狗节律 / FATAL 门控 / 恢复编排不变
  - **IMPORTANT**: Follow observation-first methodology
  - Observe on UNFIXED code（`device/tests/sd_notifier_test.cpp`）：health_check 未注册 → `watchdog_gate_open()` 返回 true；注册返回 true → true；注册返回 false → false（design Property 7 + FATAL 门控快照）
  - 新增 RapidCheck PBT：随机 healthy 布尔序列，断言 `watchdog_gate_open() == healthy`——不触碰任何 liveness 接口，修复后依靠"未刷新哨兵 0 视为门控开"的向后兼容语义继续通过（design 决策点 2）
  - 既有测试作为行为快照确认在未修复代码上全绿：`sd_notifier_test`（门控语义、`stop_watchdog_thread` 幂等/快速唤醒）与 `device/tests/health_test.cpp`（状态机转移表、state reset、full rebuild、退避、FATAL、ErrorScope 分支分流、恢复成功回 HEALTHY 重置计数）——修复后不改断言必须仍然全绿（design Property 8/9/10 基线）
  - **EXPECTED OUTCOME**: Tests PASS (this confirms baseline behavior to preserve)
  - Mark task complete when tests are written, run, and passing on unfixed code
  - SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure`
  - 完成后 `git status` 确认无敏感文件
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

- [x] 4. Fix 缺陷 1：喂狗与主循环活性绑定（liveness 门控）

  - [x] 4.1 sd_notifier 新增 liveness 原语与组合门控
    - `device/src/sd_notifier.{h,cpp}`：新增静态原子 `s_liveness_ms`（`std::atomic<int64_t>`，初始 0 = 未刷新哨兵，static_assert lock-free）与 `s_stale_threshold_ms`（默认 10000）
    - 新增 `static void refresh_liveness()`：写入 steady_clock 当前毫秒
    - 新增纯函数 `static bool should_feed_watchdog(int64_t now_ms, int64_t last_liveness_ms, int64_t stale_threshold_ms, bool healthy)`：`!healthy` 短路 false；`last==0` 哨兵返回 true（向后兼容）；否则 `(now - last) <= threshold`（时钟全部参数注入，不依赖 systemd 与真实时钟）
    - `watchdog_gate_open()` 改为组合门控：既有 health_check 逻辑不动 + 读 `s_liveness_ms` + 取 now → 调 `should_feed_watchdog`（health AND liveness-fresh，2.3）
    - 跳过喂狗时的 warn 日志区分原因（health gate / liveness stale），日志一律 ASCII
    - 新增 `static void set_liveness_stale_threshold_ms(int64_t ms)`（测试用，生产用默认 10000——阈值链：相邻检查点最大间隔 5s+ε < T_stale=10s < 喂狗间隔 15s < WatchdogSec=30s，改任何参数必须重推）
    - _Bug_Condition: isBugCondition C1 — 主循环停摆 + 状态非 FATAL + 喂狗继续_
    - _Expected_Behavior: 陈旧度 > T_stale 后所有喂狗时机跳过，systemd 在 WatchdogSec 窗口内兜底（design Property 1/2/3）_
    - _Preservation: set_health_check 语义不变、哨兵 0 保证既有测试与 macOS 行为零变化_
    - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5_

  - [x] 4.2 main.cpp 接线 CP0 主循环 liveness timer
    - `device/src/main.cpp` Phase 5.5 中、`start_watchdog_thread()` 之前：显式调用 `SdNotifier::refresh_liveness()`（消除哨兵态）+ `g_timeout_add(2000, liveness_tick, nullptr)` 注册 2s timer（CP0，回调仅调 `refresh_liveness()` 并返回 `G_SOURCE_CONTINUE`，开销一次原子写）
    - _Expected_Behavior: 主循环正常运转时 liveness 持续刷新，停摆时刷新必然停止_
    - _Requirements: 2.1_

  - [x] 4.3 缺陷 1 单测 + RapidCheck PBT
    - `should_feed_watchdog` 边界用例：恰好等于阈值（fresh）、超出 1ms（stale）、哨兵 0（视为开）、healthy=false 短路
    - 门控四象限枚举（design Property 3）：(healthy, fresh) ∈ {true,false}²，喂狗 ⇔ healthy AND fresh
    - PBT-1（design Property 2）：随机生成单调时钟序列 + 刷新事件序列，验证 fresh→stale 单向不可逆、任意 refresh 后立即 fresh、阈值边界精确
    - PBT-2（design Property 3/7）：随机 (healthy, now−last, threshold) 向量，验证 feed ⇔ healthy ∧ (now−last ≤ threshold ∨ last==0)
    - 测试超时分级：纯逻辑 ≤1s
    - SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure`
    - _Requirements: 2.2, 2.3, 2.4, 2.5_

  - [x] 4.4 Verify bug condition exploration test now passes
    - **Property 1: Expected Behavior** — 主循环停摆时喂狗必停
    - **IMPORTANT**: Re-run the SAME test from task 2 — do NOT write a new test
    - The test from task 2 encodes the expected behavior（断言 `watchdog_gate_open() == false` 不变）
    - 补齐 arrange 步骤使死锁场景在新 API 下成立（哨兵 0 语义使"从未刷新"不再代表死锁）：`refresh_liveness()` 一次（模拟主循环曾经存活）→ `set_liveness_stale_threshold_ms` 缩小阈值（如 100ms，避免测试真实等待 10s）→ 等待越过阈值（模拟刷新停止 = 主循环死锁）→ 原断言判定门控关闭
    - Run bug condition exploration test from task 2
    - **EXPECTED OUTCOME**: Test PASSES (confirms 缺陷 1 fixed：stale 判定生效、门控关闭)
    - SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure`
    - _Requirements: 2.1, 2.2, 2.6_

  - [x] 4.5 Verify preservation tests still pass（sd_notifier 部分）
    - **Property 2: Preservation** — 正常喂狗节律 / FATAL 门控不变
    - **IMPORTANT**: Re-run the SAME tests from task 3 — do NOT write new tests
    - Run preservation property tests from task 3（sd_notifier_test 全部 + 新增 PBT）
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions：哨兵语义保证既有断言不改全绿)
    - 完成后 `git status` 确认无敏感文件
    - _Requirements: 3.1, 3.2, 3.3_

- [x] 5. Fix 缺陷 2：封堵同步 KVS 拆除路径 + 检查点接线 + 日志增强
  - **前置 Gate（已消解）**：Task 1 取证结论已记录 `docs/development-trace.md`（2026-09-04 条目），定案 S6——`try_state_reset` 上行 `set_state(PLAYING)` → kvssink NULL_TO_READY 的 `kinesis_video_producer_init` 末行 unique_ptr 赋值同步析构旧 producer → KVS free 链持锁 join，推翻原 S1 最强假设；design.md 已同步更新（Hypothesized Root Cause + Fix Implementation + 检查点表），以下按"S6 定案封堵 + S1/S2/S3 保守封堵"编排

  - [x] 5.1 pipeline_health：S1/S4/S5 封堵 + op_tag 日志 + 检查点 CP1–CP3/CP6
    - **S6 封堵（取证定案调用点，_Requirements: 2.8_）**：新增 `bool set_playing_bounded(GstElement* pipeline, int budget_ms, std::function<void(GstElement*)> set_playing_fn = nullptr)`，与 `set_null_bounded` 同模式（复用 `run_bounded`：不转移所有权、fn 可注入供测试捕获执行线程、默认实现为 worker 内 `gst_element_set_state(pipeline, GST_STATE_PLAYING)`，op_tag 日志与 S4 worker ref/unref 自动覆盖）；`try_state_reset` 的 PLAYING 拉起改为 `set_playing_bounded(pipeline_, config_.state_reset_timeout_ms)`：预算内完成 → 主线程继续 `get_state` 确认 PLAYING（get_state 只等待不 free，取证维持排除）；超时 → 视为 reset 失败走既有失败分支（NULL 推进已由 S1 封堵改为 set_null_bounded）；`set_state(PLAYING)` 返回 FAILURE 的判定由后续 get_state 承接（既有 ok 判定不变）
    - **S1 封堵（保守）**：`try_state_reset` 失败分支的同步 `gst_element_set_state(pipeline_, GST_STATE_NULL)` 替换为 `set_null_bounded(pipeline_, config_.state_reset_timeout_ms)`（结果忽略，照常 return false；design 决策点 4；取证已排除其为本次事故调用点，保留为同类危险面封堵）
    - **S4 加固**：`run_bounded` 的 worker 进入 work_fn 前 `gst_object_ref(pipeline)`、work_fn 返回后 unref（防两 worker 竞态 UAF；该额外引用的归零点也在 worker 线程）
    - **S5 加固**：`default_teardown` 扩展为 worker 内先排空 bus（`gst_element_get_bus` → `gst_bus_set_flushing(bus, TRUE)` → unref bus）再 set NULL + get_state + unref pipeline，防队列消息引用使 kvssink 归零点漂移到不确定线程
    - **日志增强（2.9）**：`run_bounded` 增加 `op_tag` 参数；worker 侧打 "worker [tag] start" / "worker [tag] done, elapsed=Xms"（detach 后后台完成同样补一条完成日志）；caller 侧 completed/timeout 分支日志补充实际等待耗时；`try_state_reset` / `attempt_recovery` 阶段边界补 info 日志（与检查点同点位），日志一律 ASCII
    - **检查点**：新增 `void set_liveness_callback(std::function<void()>)`；在 CP1（`attempt_recovery` 入口）、CP2（`set_null_bounded` 返回后）、CP2.5（`set_playing_bounded` 返回后，S6 封堵新增——否则 set_playing(≤5s)+get_state(≤5s) 串行超 T_stale=10s）、CP3（PLAYING `get_state` 返回后）、CP3.5（失败分支 `set_null_bounded` 返回后——否则失败路径 set_null(≤5s)+teardown(≤5s) 串行超 T_stale）、CP6（`attempt_recovery` 出口）调用（若注册；未注册时行为与现状完全一致）；不变式：try_state_reset 内每个有界等待原语返回后必有检查点（design 决策点 1 间隔论证表）
    - _Bug_Condition: isBugCondition C2 — 恢复路径操作在主循环线程同步触碰 KVS 拆除链_
    - _Expected_Behavior: KVS 拆除链只在 worker 线程，主循环只做有界等待 ≤5s（design Property 4/5）_
    - _Preservation: 恢复编排结构与状态机转移表不变（design Property 9）_
    - SHALL NOT 在 GLib 主循环线程执行任何可能进入 kvssink dispose / KVS stream free 的同步调用——含上行 set_state(PLAYING) 再初始化（S6 取证定案）与引用计数归零的 unref（归零点所在线程即 dispose 执行线程）
    - _Requirements: 2.4, 2.8, 2.9_

  - [x] 5.2 app_context：S2/S3 封堵 + CP4/CP5 接线
    - **S2 封堵**：rebuild 回调中 `PipelineManager::create` 失败分支的 `gst_object_unref(p)` → `teardown_pipeline_bounded(p, 5000)`（未启动管道 NULL 转换为 no-op，统一走 worker 保证归零点在 worker 线程）
    - **S3 封堵**：`new_pm->start()` 失败分支改为 `GstElement* hp = new_pm->release(); new_pm.reset(); teardown_pipeline_bounded(hp, 5000);`，消除半启动管道在主线程的同步析构
    - **CP4/CP5**：rebuild 回调内 `teardown_pipeline_bounded` 返回后、`open_with_retry` 每次 try_open lambda 尾部，直接调 `SdNotifier::refresh_liveness()`
    - 接线：`health_monitor->set_liveness_callback([]{ SdNotifier::refresh_liveness(); });`
    - _Bug_Condition: isBugCondition C2 — S2/S3 主线程 unref/析构_
    - _Expected_Behavior: 恢复不可完成时收敛到三种确定性终态之一（HEALTHY / FATAL 优雅退出 / liveness 兜底重启），无"永久 RECOVERING 假活"（design Property 6）_
    - _Preservation: rebuild 编排顺序（detach → teardown → open_with_retry）与对外接口不变_
    - _Requirements: 2.4, 2.8, 2.10_

  - [x] 5.3 缺陷 2 单测：线程断言 + 检查点触发 + 超时分支可观测
    - **线程断言（design Property 4 双轨之 b，覆盖 S1/S2/S3/S6 改造点）**：注入 teardown_fn/set_null_fn/set_playing_fn 捕获 `std::this_thread::get_id()`，断言 ≠ 调用者线程 id；S6 通过驱动 `try_state_reset` 的 PLAYING 拉起路径断言 `set_state(PLAYING)` 执行线程 ≠ 主调线程；S1 通过驱动"set_null_bounded 成功 + PLAYING 拉起失败"分支验证尾部 NULL 也走 worker
    - **检查点触发**：注入计数回调，驱动 `try_state_reset` 成功/失败路径，断言各路径对应检查点触发（成功路径：CP1/CP2/CP2.5/CP3/CP6 各一次；PLAYING 拉起失败路径：额外 CP3.5）
    - **超时分支可观测**：注入慢 fn（> budget）驱动超时分支，断言返回 false 且 worker 后台完成标志最终置位、op_tag 日志成对
    - **PBT-3（design Property 9）**：复用/扩展既有 health_test 恢复序列 PBT，断言检查点注入不改变状态机轨迹
    - 测试超时分级：多轮恢复/PBT ≤15s
    - SHALL NOT 直接运行测试可执行文件，必须通过 `ctest --test-dir device/build --output-on-failure`
    - _Requirements: 2.8, 2.9, 2.10, 3.4_

  - [x] 5.4 Verify preservation tests still pass（恢复编排部分）
    - **Property 2: Preservation** — 恢复编排、分支错误语义与对外接口不变
    - **IMPORTANT**: Re-run the SAME tests from task 3 — do NOT write new tests
    - Run preservation tests from task 3（health_test 既有断言不改全绿：状态机转移、退避、FATAL、ErrorScope 分流、恢复成功回 HEALTHY）
    - **EXPECTED OUTCOME**: Tests PASS (confirms no regressions)
    - 完成后 `git status` 确认无敏感文件
    - _Requirements: 3.4, 3.5, 3.6_

- [x] 6. Checkpoint — macOS 全量构建 + 测试全绿
  - 执行完整验证：`cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Debug && cmake --build device/build && ctest --test-dir device/build --output-on-failure`
  - 确认所有测试通过：exploration test（已转 PASS）、preservation tests、缺陷 1/2 新增单测与 PBT、全部既有测试（macOS stub 路径不变，design Property 10）
  - 确认无 ASan 报告（重点：run_bounded worker ref/unref 配对、S5 bus flush 路径）
  - `git status` 确认无敏感文件；Ensure all tests pass, ask the user if questions arise
  - SHALL NOT 在子代理最终检查点任务中执行 git commit
  - _Requirements: 3.7_

- [~] 7. Pi 5 端到端验证 + trace 归档 + shall-not 反哺
  - 部署：`PI_HOST=192.168.2.100 PI_REPO_DIR='~/Workspace/raspi-eye' scripts/pi-deploy.sh`；构建后先跑 Pi 上 `ctest --test-dir device/build --output-on-failure` 确认全绿
  - SHALL NOT 在无法本地复现的远程平台问题上凭猜测修复：任何异常先收集信息（journalctl、systemctl status、GST_DEBUG、线程堆栈）综合分析后再做一次性修改，严禁"改一行推一次"试错循环
  - **实测 1（主循环停摆兜底，design Property 1/6）**：`kill -STOP <pid>`（或 gdb attach 挂起主线程）模拟停摆 → journal 观察 liveness stale 跳喂 warn 日志 → systemd watchdog kill → `Restart=on-failure` 自动重启，自最后一次成功喂狗起总窗口 ≤ ~35s（对比事故 2 天 8 小时）
  - **实测 2（不误杀，design Property 7/8）**：正常运行 ≥ 1 个喂狗周期（15s）无跳喂日志；`systemctl stop` 走优雅关闭（notify_stopping → stop_watchdog_thread → 逆序清理）无 liveness 误杀
  - **实测 3（恢复期，design Property 5 + 三阶段不误杀论证实测）**：拔插 USB 摄像头触发 TRUNK 恢复，确认恢复期无 stale 跳喂、teardown worker 日志成对出现（start/done 含耗时，或 timeout+后台完成补日志），事故再发生时可从日志直接定位恢复流程走到哪一步
  - **实测 4（CP5 假设量化确认）**：从日志测量单次 build+start 实际耗时，确认 < 3s（阈值链推导前提）；若超出则重推"相邻检查点最大间隔 < T_stale"约束链并更新 design
  - 结论归档 `docs/development-trace.md`（含 4 项实测数据与结论）；反哺 `.kiro/steering/shall-not.md`（如"SHALL NOT 让看门狗喂狗与主循环活性解耦"、取证定案的同步拆除模式）
  - `git status` 确认无敏感文件（尤其 Pi 侧日志/堆栈文件不入库）
  - SHALL NOT 在子代理最终检查点任务中执行 git commit
  - _Requirements: 2.2, 2.6, 2.9, 2.10, 3.1, 3.2, 3.3_

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": ["1", "2", "3"], "note": "取证（缺陷 2）/ exploration test（缺陷 1）/ preservation tests 相互独立可并行" },
    { "wave": 2, "tasks": ["4"], "note": "缺陷 1 修复，依赖 2（exploration test 先行）+ 3（preservation 基线先行）" },
    { "wave": 3, "tasks": ["5"], "note": "缺陷 2 封堵，依赖 1（取证结论 gate）+ 3（preservation 基线）+ 4（CP4/CP5 需要 refresh_liveness API）" },
    { "wave": 4, "tasks": ["6"], "note": "macOS 检查点，依赖 4+5 全部实现" },
    { "wave": 5, "tasks": ["7"], "note": "Pi 5 端到端实测 + 归档，依赖 6" }
  ],
  "dependencies": {
    "1": [],
    "2": [],
    "3": [],
    "4": ["2", "3"],
    "5": ["1", "3", "4"],
    "6": ["4", "5"],
    "7": ["6"]
  }
}
```

## Notes

- **取证 gate（已消解）**：Task 1 取证已定案 S6（推翻原 S1 最强假设）并回写 design.md（Hypothesized Root Cause + Fix Implementation + 决策点 1 检查点表），Task 5 按"S6 定案封堵 + S1/S2/S3 保守封堵"执行；SHALL NOT 约束（取证结论记录前不改缺陷 2 代码）已满足。
- **文件预算例外**：生产文件 6 个（sd_notifier.{h,cpp}、main.cpp、pipeline_health.{h,cpp}、app_context.cpp），超常规 2–5 预算 1 个，design 已声明显式例外（两缺陷分属看门狗层与恢复层，物理上无法再收敛）；测试复用既有 sd_notifier_test.cpp / health_test.cpp，不新建测试文件。
- **不自动 commit**：每个 task 完成 `git status` 确认无敏感文件，由用户测试确认后自行提交。
- **systemd 单元不改**：`WatchdogSec=30`、`StartLimitBurst=5/60s`、`TimeoutStopSec=35` 保持现值，阈值量化推导以其为前提。
- 统一验证：`ctest --test-dir device/build --output-on-failure`（macOS 与 Pi 5 均适用）。
