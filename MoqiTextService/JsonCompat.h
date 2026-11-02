#pragma once
// 纯 JSON 解释层：不依赖 TSF/Win32，单元测试见 tests/protocol_json_test.cpp。
//
// 契约来自后端 protojson(EmitUnpopulated) 的实测线上形状（2026-11-02 probe5）：
//  - 隐式存在字段在每条响应里都会出现，未设置时是零值：
//      commandId:0 / type:"BUTTON_TYPE_UNSPECIFIED" / text:"" / tooltip:"" /
//      submenu:[] / showCandidates:false / compositionString:"" …
//    零值 = "后端未指定、不要动"，不是指令。把它当指令会洗掉按钮 commandId、
//    把叶子菜单项变成空子菜单、清掉组字（三次线上事故的共同根因）。
//  - optional 字段（style/candidateCursor/customizeUi/showMessage…）未设置时
//    直接不出现，缺省即"无此信息"。
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include <json/json.h>

namespace Moqi {
namespace ProtocolJson {

// ---- 菜单 ----
struct MenuNode {
  uint32_t id = 0;
  std::string text; // UTF-8
  bool checked = false;
  bool enabled = true;
  bool separator = false;
  std::vector<MenuNode> children; // 只有非空才是真子菜单（[] 不是）
};

inline bool parseMenuNodes(const Json::Value &items,
                           std::vector<MenuNode> &out) {
  if (!items.isArray()) {
    return false;
  }
  out.clear();
  out.reserve(items.size());
  for (const auto &item : items) {
    MenuNode node;
    node.id = item.get("id", 0).asUInt();
    node.text = item.get("text", "").asString();
    node.checked = item.get("checked", false).asBool();
    node.enabled = item.get("enabled", true).asBool();
    // 显式 separator 字段优先；老的 id==0+空文本启发式保留兼容。
    node.separator =
        item.get("separator", false).asBool() || (node.id == 0 && node.text.empty());
    const Json::Value &sub = item["submenu"];
    if (sub.isArray() && !sub.empty()) {
      parseMenuNodes(sub, node.children);
    }
    out.push_back(std::move(node));
  }
  return true;
}

// ---- 语言栏按钮 ----
// ButtonInfo 的增量补丁。addButton 携带完整字段，changeButton 只带
// id+icon，其余是 EmitUnpopulated 补的零值 —— 后者必须视为"不更新"。
struct ButtonPatch {
  enum class Kind { None, Button, Toggle, Menu };

  bool hasIcon = false;
  std::string icon;
  bool hasText = false;
  std::string text;
  bool hasTooltip = false;
  std::string tooltip;
  bool hasCommandId = false;
  uint32_t commandId = 0;
  Kind kind = Kind::None; // None = 未指定，保持现有类型位
  bool hasEnable = false;
  bool enable = false;
  bool hasToggled = false;
  bool toggled = false;
};

inline ButtonPatch parseButtonPatch(const Json::Value &info) {
  ButtonPatch patch;
  if (!info.isObject()) {
    return patch;
  }
  const Json::Value &icon = info["icon"];
  if (icon.isString() && !icon.asString().empty()) {
    patch.hasIcon = true;
    patch.icon = icon.asString();
  }
  const Json::Value &text = info["text"];
  if (text.isString() && !text.asString().empty()) {
    patch.hasText = true;
    patch.text = text.asString();
  }
  const Json::Value &tooltip = info["tooltip"];
  if (tooltip.isString() && !tooltip.asString().empty()) {
    patch.hasTooltip = true;
    patch.tooltip = tooltip.asString();
  }
  const Json::Value &cmd = info["commandId"];
  if (cmd.isUInt() && cmd.asUInt() > 0) {
    patch.hasCommandId = true;
    patch.commandId = cmd.asUInt();
  }
  const Json::Value &type = info["type"];
  if (type.isString()) {
    std::string kind = type.asString();
    if (kind.rfind("BUTTON_TYPE_", 0) == 0) {
      kind = kind.substr(12);
      // enum 名是全大写（BUTTON_TYPE_MENU → MENU），必须整体降小写；
      // 只降首字母会得到 "mENU"，三类分支永不命中（2026-11-02 单测抓出）。
      for (char &c : kind) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      }
    }
    if (kind == "button") {
      patch.kind = ButtonPatch::Kind::Button;
    } else if (kind == "toggle") {
      patch.kind = ButtonPatch::Kind::Toggle;
    } else if (kind == "menu") {
      patch.kind = ButtonPatch::Kind::Menu;
    }
  }
  const Json::Value &enable = info["enable"];
  if (enable.isBool()) {
    patch.hasEnable = true;
    patch.enable = enable.asBool();
  }
  const Json::Value &toggled = info["toggled"];
  if (toggled.isBool()) {
    patch.hasToggled = true;
    patch.toggled = toggled.asBool();
  }
  return patch;
}

} // namespace ProtocolJson
} // namespace Moqi
