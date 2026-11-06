// jsoncpp 安全取值层（2026-11-06 可执行实证）：get/operator[] 在数组、标量
// 容器上抛 Json::LogicError；asBool/asString/asInt 只对完全不可转换的
// 类型抛，数字/布尔之间会静默强转（asBool(1)=true、asString(5)="5"）——
// 本层一律严格判型：容器非对象/键缺失/类型不符都回默认值，绝不猜、绝不抛。
// 用户可手改的配置文件绝不允许把进程炸掉。所有读配置文件的取值必须走这里，
// 不裸用 get/asXxx。
// 独立成小头（只依赖 json/json.h 与 string，不含 windows.h）：让
// LocalBehaviorPatch.h 这类纯函数头与 daemon 单测共用同一套纪律，
// 不必各自手写"非对象回默认"守卫。
#ifndef MOQI_JSON_SAFE_READ_H_
#define MOQI_JSON_SAFE_READ_H_

#include <json/json.h>

#include <string>

namespace Moqi {

inline bool jsonBoolOf(const Json::Value& v, const char* key, bool def = false) {
  if (!v.isObject()) return def;
  const Json::Value& x = v[key];
  return x.isBool() ? x.asBool() : def;
}

inline std::string jsonStringOf(const Json::Value& v, const char* key,
                                const std::string& def = std::string()) {
  if (!v.isObject()) return def;
  const Json::Value& x = v[key];
  return x.isString() ? x.asString() : def;
}

inline int jsonIntOf(const Json::Value& v, const char* key, int def = 0) {
  if (!v.isObject()) return def;
  const Json::Value& x = v[key];
  return x.isInt() ? x.asInt() : def;
}

// 成员访问：容器非对象时返回 null 单例（绝不抛）；键存在返回原值不判型，
// 调用方自行 isInt()/isObject() 判断——替代"裸 [] 取成员再判型"的写法。
inline const Json::Value& jsonMemberOf(const Json::Value& v, const char* key) {
  static const Json::Value kNull;
  return v.isObject() ? v[key] : kNull;
}

}  // namespace Moqi

#endif  // MOQI_JSON_SAFE_READ_H_
