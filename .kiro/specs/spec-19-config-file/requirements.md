# 需求文档

## 简介

当前系统中配置加载方式分散：`credential_provider.cpp`、`kvs_sink_factory.cpp`、`webrtc_signaling.cpp` 各自调用 `parse_toml_section` 解析独立的 TOML section，`BitrateConfig` 和 `CameraConfig` 使用硬编码默认值或命令行参数传入，日志格式通过 `--log-json` 命令行参数控制。`app_context.cpp` 的 `init()` 中分别调用三次 `parse_toml_section` 解析 `[aws]`、`[kvs]`、`[webrtc]` 三个 section。

本 Spec 创建统一的 ConfigManager 模块，一次性加载 config.toml 的所有 section（包括新增的 `[camera]`、`[streaming]`、`[logging]`），并提供类型安全的配置结构体访问接口。各模块从 ConfigManager 获取配置，不再各自解析 TOML。配置优先级为：命令行参数 > 配置文件 > 平台默认值。

## 术语表

- **ConfigManager**: 统一配置加载模块，负责解析 config.toml 并提供各 section 的配置结构体
- **parse_toml_section**: 现有的 TOML section 解析函数，定义在 `credential_provider.h` 中，返回 `std::unordered_map<std::string, std::string>`
- **config.toml**: 应用配置文件，路径通过 `--config` 命令行参数指定，包含 `[aws]`、`[kvs]`、`[webrtc]` 等 section
- **CameraConfig**: 摄像头配置结构体，定义在 `camera_source.h` 中，包含 type 和 device 字段
- **BitrateConfig**: 码率配置结构体，定义在 `bitrate_adapter.h` 中，包含 min/max/step/default/eval_interval/rampup_interval 字段
- **StreamModeController**: 流模式控制器，定义在 `stream_mode_controller.h` 中，管理 debounce 时间
- **AppContext**: 应用上下文，定义在 `app_context.h` 中，负责三阶段生命周期管理（init/start/stop）

## 需求

### 需求 1：ConfigManager 统一加载

**用户故事：** 作为开发者，我希望通过一个统一的 ConfigManager 一次性加载 config.toml 的所有 section，以消除各模块分散解析 TOML 的重复代码。

#### 验收标准

1. WHEN ConfigManager 接收到有效的 config.toml 文件路径, THE ConfigManager SHALL 调用 parse_toml_section 解析文件中所有已知 section（aws、kvs、webrtc、camera、streaming、logging）并将结果存储在内部
2. WHEN ConfigManager 完成加载, THE ConfigManager SHALL 提供 `aws_config()`、`kvs_config()`、`webrtc_config()`、`camera_config()`、`streaming_config()`、`logging_config()` 六个访问方法，每个方法返回对应的 const 引用类型配置结构体
3. IF config.toml 文件无法打开, THEN THE ConfigManager SHALL 返回 false 并通过 error_msg 参数报告文件路径和错误原因
4. IF 已有的必填 section（aws、kvs、webrtc）中缺少必填字段, THEN THE ConfigManager SHALL 返回 false 并通过 error_msg 参数报告缺失的 section 名称和字段名称

### 需求 2：新增 camera section

**用户故事：** 作为开发者，我希望在 config.toml 中配置摄像头参数（类型、设备路径、分辨率、帧率），以替代命令行参数和硬编码默认值。

#### 验收标准

1. WHEN config.toml 包含 `[camera]` section, THE ConfigManager SHALL 解析以下字段并填充 CameraConfig 结构体：type（字符串，映射到 CameraType 枚举）、device（字符串）、width（整数）、height（整数）、framerate（整数）
2. WHEN config.toml 缺少 `[camera]` section, THE ConfigManager SHALL 使用平台默认值填充 CameraConfig：type 为 test（macOS）或 v4l2（Linux），device 为 "/dev/video0"，width 为 1280，height 为 720，framerate 为 15
3. WHEN config.toml 的 `[camera]` section 中缺少某个字段, THE ConfigManager SHALL 对该字段使用平台默认值，已存在的字段正常解析
4. IF `[camera]` section 的 type 字段值不是 test、v4l2、libcamera 之一, THEN THE ConfigManager SHALL 返回 false 并通过 error_msg 报告无效的 type 值

### 需求 3：新增 streaming section

**用户故事：** 作为开发者，我希望在 config.toml 中配置码率控制和流模式参数，以替代 BitrateConfig 和 StreamModeController 中的硬编码默认值。

#### 验收标准

1. WHEN config.toml 包含 `[streaming]` section, THE ConfigManager SHALL 解析以下字段并填充 StreamingConfig 结构体：bitrate_min_kbps（整数）、bitrate_max_kbps（整数）、bitrate_step_kbps（整数）、bitrate_default_kbps（整数）、bitrate_eval_interval_sec（整数）、bitrate_rampup_interval_sec（整数）、debounce_sec（整数）
2. WHEN config.toml 缺少 `[streaming]` section, THE ConfigManager SHALL 使用以下默认值填充 StreamingConfig：bitrate_min_kbps=1000、bitrate_max_kbps=4000、bitrate_step_kbps=500、bitrate_default_kbps=2500、bitrate_eval_interval_sec=5、bitrate_rampup_interval_sec=30、debounce_sec=3
3. WHEN config.toml 的 `[streaming]` section 中缺少某个字段, THE ConfigManager SHALL 对该字段使用默认值，已存在的字段正常解析
4. IF `[streaming]` section 中 bitrate_min_kbps 大于 bitrate_max_kbps, THEN THE ConfigManager SHALL 返回 false 并通过 error_msg 报告码率范围无效
5. IF `[streaming]` section 中 bitrate_default_kbps 不在 bitrate_min_kbps 和 bitrate_max_kbps 范围内, THEN THE ConfigManager SHALL 返回 false 并通过 error_msg 报告默认码率超出范围

### 需求 4：新增 logging section

**用户故事：** 作为开发者，我希望在 config.toml 中配置日志级别和格式，以替代仅通过 `--log-json` 命令行参数控制日志格式的方式。

#### 验收标准

1. WHEN config.toml 包含 `[logging]` section, THE ConfigManager SHALL 解析以下字段并填充 LoggingConfig 结构体：level（字符串，映射到 trace/debug/info/warn/error）、format（字符串，映射到 text/json）
2. WHEN config.toml 缺少 `[logging]` section, THE ConfigManager SHALL 使用默认值填充 LoggingConfig：level="info"、format="text"
3. WHEN config.toml 的 `[logging]` section 中缺少某个字段, THE ConfigManager SHALL 对该字段使用默认值，已存在的字段正常解析
4. IF `[logging]` section 的 level 字段值不是 trace、debug、info、warn、error 之一, THEN THE ConfigManager SHALL 返回 false 并通过 error_msg 报告无效的日志级别值
5. IF `[logging]` section 的 format 字段值不是 text、json 之一, THEN THE ConfigManager SHALL 返回 false 并通过 error_msg 报告无效的日志格式值

### 需求 5：命令行参数覆盖配置文件

**用户故事：** 作为运维人员，我希望通过命令行参数覆盖配置文件中的值，以便在不修改配置文件的情况下临时调整运行参数。

#### 验收标准

1. WHEN 命令行参数 `--camera` 指定了摄像头类型, THE ConfigManager SHALL 使用命令行参数值覆盖 config.toml 中 `[camera]` section 的 type 字段
2. WHEN 命令行参数 `--device` 指定了设备路径, THE ConfigManager SHALL 使用命令行参数值覆盖 config.toml 中 `[camera]` section 的 device 字段
3. WHEN 命令行参数 `--log-json` 存在, THE ConfigManager SHALL 将 LoggingConfig 的 format 字段覆盖为 "json"
4. THE ConfigManager SHALL 保持三层优先级顺序：命令行参数 > 配置文件 > 平台默认值

### 需求 6：AppContext 集成

**用户故事：** 作为开发者，我希望 AppContext 使用 ConfigManager 替代分散的 parse_toml_section 调用，以简化初始化流程。

#### 验收标准

1. WHEN AppContext::init() 被调用, THE AppContext SHALL 创建 ConfigManager 实例并调用其 load 方法一次性加载所有配置
2. WHEN ConfigManager 加载成功, THE AppContext SHALL 从 ConfigManager 获取各模块配置结构体（AwsConfig、KvsConfig、WebRtcConfig、CameraConfig），替代原有的三次 parse_toml_section 调用和手动 build_*_config 调用
3. WHEN BitrateAdapter 被创建, THE AppContext SHALL 从 ConfigManager 的 StreamingConfig 构造 BitrateConfig 并传入 BitrateAdapter 构造函数
4. WHEN StreamModeController 被创建, THE AppContext SHALL 从 ConfigManager 的 StreamingConfig 获取 debounce_sec 并传入 StreamModeController

### 需求 7：向后兼容

**用户故事：** 作为现有用户，我希望现有的 config.toml 文件无需修改即可继续使用，新增的 section 全部可选。

#### 验收标准

1. WHEN 现有 config.toml 仅包含 `[aws]`、`[kvs]`、`[webrtc]` 三个 section, THE ConfigManager SHALL 成功加载并对缺失的 `[camera]`、`[streaming]`、`[logging]` section 使用默认值
2. THE ConfigManager SHALL 保持现有 `[aws]`、`[kvs]`、`[webrtc]` section 的字段名称和格式不变
3. WHEN config.toml 包含 ConfigManager 未识别的 section, THE ConfigManager SHALL 忽略该 section 并继续正常加载

## 配置文件示例

```toml
# =============================================================
# raspi-eye 配置文件
# 路径：device/config/config.toml
# 用法：raspi-eye --config device/config/config.toml
# =============================================================

# --- AWS IoT 凭证（必填，由 provision-device.sh 自动生成） ---
[aws]
thing_name = "RaspiEyeAlpha"
credential_endpoint = "c19imbvbcm8u20.credentials.iot.ap-southeast-1.amazonaws.com"
role_alias = "RaspiEyeRoleAlias"
cert_path = "device/certs/device-cert.pem"
key_path = "device/certs/device-private.key"
ca_path = "device/certs/root-ca.pem"

# --- KVS 录制（必填） ---
[kvs]
stream_name = "RaspiEyeAlphaStream"
aws_region = "ap-southeast-1"

# --- WebRTC 实时观看（必填） ---
[webrtc]
channel_name = "RaspiEyeAlphaChannel"
aws_region = "ap-southeast-1"

# --- 摄像头（可选，缺失时使用平台默认值） ---
[camera]
type = "v4l2"           # test | v4l2 | libcamera
device = "/dev/IMX678"  # 仅 v4l2 使用
width = 1280
height = 720
framerate = 15

# --- 自适应码率 + 流模式（可选，缺失时使用默认值） ---
[streaming]
bitrate_min_kbps = 1000
bitrate_max_kbps = 4000
bitrate_step_kbps = 500
bitrate_default_kbps = 2500
bitrate_eval_interval_sec = 5
bitrate_rampup_interval_sec = 30
debounce_sec = 3

# --- 日志（可选，缺失时 level=info, format=text） ---
[logging]
level = "info"          # trace | debug | info | warn | error
format = "text"         # text | json
```

## 约束

- C++17，复用现有 `parse_toml_section` 函数，不引入新的 TOML 解析库
- config.toml 路径通过 `--config` 命令行参数指定（已有机制）
- 新增 section（camera、streaming、logging）全部可选，缺失时使用默认值
- 不修改现有测试文件
- 不包含配置文件热重载、加密、远程配置下发、schema 校验、迁移工具

## 禁止项

- SHALL NOT 在代码中硬编码 AWS 凭证、密钥、证书路径或任何 secret（来源：安全基线）
- SHALL NOT 在日志或错误输出中打印密钥、证书内容、token 等敏感信息（来源：安全基线）
