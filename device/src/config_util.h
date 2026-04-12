// config_util.h
// 轻量配置解析工具函数（无外部依赖，可被 kvs_module / webrtc_module 直接链接）
#pragma once

#include <string>
#include <unordered_map>

// 从 kv map 中解析布尔字段。
// 接受 "true"/"false"（大小写不敏感）。字段不存在时保持 out 不变。
// 非法值返回 false 并设置 error_msg。
bool parse_bool_field(
    const std::unordered_map<std::string, std::string>& kv,
    const std::string& field_name,
    bool& out,
    std::string* error_msg = nullptr);
