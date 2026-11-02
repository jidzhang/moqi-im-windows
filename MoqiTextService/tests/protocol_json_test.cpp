// Protocol JSON interpretation-layer regression tests (no TSF dependency).
//
// Locks the root causes of three production incidents on 2026-11-02:
//  1) leaf menu items arrive with "submenu": [] and were treated as real
//     submenus -> every menu item opened an empty flyout, clicks did nothing
//  2) changeButton zero-value fields (commandId:0, type:BUTTON_TYPE_UNSPECIFIED,
//     text:"", tooltip:"") were applied on top of button state -> the 中/英
//     toggle worked only once
//  3) separator forms: explicit "separator":true and the legacy id==0+empty text
//
// Every JSON shape below is taken from real wire captures (probe4/probe5 in
// F:\work\IME), not hand-written assumptions. Keep this file pure ASCII:
// Chinese text uses \u escapes so MSVC codepage settings cannot corrupt it.
#include "../JsonCompat.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_pass = 0;

static void check(bool cond, const char *what) {
  if (cond) {
    ++g_pass;
    std::printf("PASS %s\n", what);
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", what);
  }
}

static Json::Value parse(const char *json) {
  Json::CharReaderBuilder builder;
  Json::Value value;
  Json::String errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  reader->parse(json, json + std::strlen(json), &value, &errors);
  if (!errors.empty()) {
    std::printf("JSON ERROR: %s\nJSON: %s\n", errors.c_str(), json);
  }
  return value;
}

int main() {
  using namespace Moqi::ProtocolJson;

  // 1) Real onMenu leaf item (probe5 shape). "submenu": [] must NOT create
  //    children -- this is incident 1.
  {
    const Json::Value items = parse(
        "[{\"id\":2000,\"text\":\"\\u4e2d\\u6587 \\u2192 \\u82f1\\u6587\","
        "\"checked\":false,\"enabled\":true,\"submenu\":[],\"separator\":false}]");
    std::vector<MenuNode> nodes;
    check(parseMenuNodes(items, nodes), "menu: array parses");
    check(nodes.size() == 1, "menu: one node");
    check(!nodes[0].separator, "menu: leaf is not separator");
    check(nodes[0].children.empty(),
          "menu: empty submenu [] means NO children (incident 1)");
    check(nodes[0].id == 2000, "menu: id preserved");
    check(nodes[0].enabled, "menu: enabled true preserved");
  }

  // 2) Real submenu (the "维护" node from probe5) keeps its children.
  {
    const Json::Value items = parse(
        "[{\"id\":0,\"text\":\"\\u7ef4\\u62a4\",\"checked\":false,"
        "\"enabled\":true,\"submenu\":["
        "{\"id\":30,\"text\":\"\\u5f00\\u542f\\u8c03\\u8bd5\",\"checked\":false,"
        "\"enabled\":true,\"submenu\":[],\"separator\":false},"
        "{\"id\":32,\"text\":\"\\u91cd\\u542f\\u5f15\\u64ce\",\"checked\":true,"
        "\"enabled\":true,\"submenu\":[],\"separator\":false}],"
        "\"separator\":false}]");
    std::vector<MenuNode> nodes;
    check(parseMenuNodes(items, nodes), "menu: submenu array parses");
    check(nodes.size() == 1, "menu: submenu parent is one node");
    check(nodes[0].children.size() == 2, "menu: real submenu keeps 2 children");
    check(!nodes[0].separator,
          "menu: submenu parent (id=0 with text) is not separator");
    check(nodes[0].children[0].id == 30, "menu: child id preserved");
    check(nodes[0].children[1].checked, "menu: child checked preserved");
    check(nodes[0].children[0].children.empty(),
          "menu: grandchild leaf has no children");
  }

  // 3) Both separator forms (incident 3).
  {
    const Json::Value items = parse(
        "[{\"id\":0,\"text\":\"\",\"checked\":false,\"enabled\":true,"
        "\"submenu\":[],\"separator\":true},"
        "{\"id\":0,\"text\":\"\",\"checked\":false,\"enabled\":true,"
        "\"submenu\":[],\"separator\":false}]");
    std::vector<MenuNode> nodes;
    check(parseMenuNodes(items, nodes), "menu: separators parse");
    check(nodes[0].separator, "menu: explicit separator flag respected");
    check(nodes[1].separator,
          "menu: id==0 + empty text is separator even without flag (legacy)");
  }

  // 4) changeButton carries only id+icon; the rest are EmitUnpopulated zeros.
  //    Zeros must not override button state -- this is incident 2.
  {
    const Json::Value patchJson = parse(
        "{\"id\":\"windows-mode-icon\",\"icon\":\"C:\\\\x\\\\chi_half.ico\","
        "\"text\":\"\",\"tooltip\":\"\",\"commandId\":0,"
        "\"type\":\"BUTTON_TYPE_UNSPECIFIED\",\"enable\":true,\"toggled\":false}");
    const ButtonPatch patch = parseButtonPatch(patchJson);
    check(patch.hasIcon, "button: icon applied");
    check(!patch.hasCommandId,
          "button: commandId:0 is not an instruction (incident 2)");
    check(!patch.hasText, "button: empty text does not wipe text (incident 2)");
    check(!patch.hasTooltip, "button: empty tooltip does not wipe tooltip");
    check(patch.kind == ButtonPatch::Kind::None,
          "button: BUTTON_TYPE_UNSPECIFIED keeps style bits (incident 2)");
    check(patch.hasEnable && patch.enable, "button: enable:true parsed");
    check(patch.hasToggled && !patch.toggled, "button: toggled:false parsed");
  }

  // 5) addButton carries the full values (text is legitimately empty for the
  //    icon-only mode button; real commandId/tooltip must apply).
  {
    const Json::Value patchJson = parse(
        "{\"id\":\"windows-mode-icon\",\"icon\":\"C:\\\\x\\\\chi.ico\","
        "\"text\":\"\",\"tooltip\":\"\\u4e2d\\u82f1\\u6587\\u5207\\u6362\","
        "\"commandId\":1,\"type\":\"BUTTON_TYPE_UNSPECIFIED\","
        "\"enable\":true,\"toggled\":false}");
    const ButtonPatch patch = parseButtonPatch(patchJson);
    check(patch.hasCommandId && patch.commandId == 1,
          "button: real commandId:1 applied");
    check(!patch.hasText, "button: empty text stays unset (icon-only button)");
    check(patch.hasTooltip, "button: real tooltip applied");
  }

  // 6) Type-name mapping, including the symbolic proto enum names.
  {
    const ButtonPatch menuPatch = parseButtonPatch(
        parse("{\"id\":\"settings\",\"type\":\"BUTTON_TYPE_MENU\"}"));
    check(menuPatch.kind == ButtonPatch::Kind::Menu,
          "button: BUTTON_TYPE_MENU maps to menu style");
    const ButtonPatch togglePatch = parseButtonPatch(
        parse("{\"id\":\"x\",\"type\":\"BUTTON_TYPE_TOGGLE\"}"));
    check(togglePatch.kind == ButtonPatch::Kind::Toggle,
          "button: BUTTON_TYPE_TOGGLE maps to toggle style");
    const ButtonPatch buttonPatch = parseButtonPatch(
        parse("{\"id\":\"x\",\"type\":\"BUTTON_TYPE_BUTTON\"}"));
    check(buttonPatch.kind == ButtonPatch::Kind::Button,
          "button: BUTTON_TYPE_BUTTON maps to button style");
    const ButtonPatch legacyPatch =
        parseButtonPatch(parse("{\"id\":\"x\",\"type\":\"button\"}"));
    check(legacyPatch.kind == ButtonPatch::Kind::Button,
          "button: legacy plain type name still works");
  }

  // 7) Robustness on empty input.
  {
    std::vector<MenuNode> nodes;
    check(!parseMenuNodes(Json::Value(), nodes), "menu: non-array rejected");
    const ButtonPatch patch = parseButtonPatch(Json::Value());
    check(!patch.hasIcon && !patch.hasCommandId && !patch.hasText,
          "button: non-object yields empty patch");
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
