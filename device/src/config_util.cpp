// config_util.cpp
// 轻量配置解析工具函数实现。

#include "config_util.h"
#include <algorithm>

namespace {

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

}  // namespace

bool parse_bool_field(
    const std::unordered_map<std::string, std::string>& kv,
    const std::string& field_name,
    bool& out,
    std::string* error_msg) {

    auto it = kv.find(field_name);
    if (it == kv.end()) {
        // 字段不存在，保持 out 不变（调用方设置默认值）
        return true;
    }

    std::string lower = to_lower(it->second);
    if (lower == "true") {
        out = true;
        return true;
    }
    if (lower == "false") {
        out = false;
        return true;
    }

    if (error_msg) {
        *error_msg = "Invalid boolean value for '" + field_name + "': " + it->second;
    }
    return false;
}
