#pragma once
// 墨奇输入板主窗口：输入区（上屏落这里）+ 组字/候选带 + 动作行 + 结果区 + 状态行。
// 实现分两个文件：PanelWindow.cpp = 窗口与布局绘制；PanelActions.cpp = 按键
// 闸门、引擎回包、复制与翻译流程。
// include 纪律：windows.h 开篇（见 PanelCore.cpp）。
#include <windows.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "EngineClient.h"
#include "TranslateHttp.h"

namespace MoqiPanel {

class PanelWindow {
public:
  static ATOM registerClass(HINSTANCE instance);

  // 启动检查（路径纯 ASCII、文件齐全、目录可写）不过时返回 nullptr，why 给出
  // 给用户看的中文原因。
  static PanelWindow* create(HINSTANCE instance, const std::wstring& exeDir,
                             std::wstring* why);

  HWND hwnd() const { return hwnd_; }
  HFONT uiFont() const { return uiFont_; }

  // 消息循环调用：这条消息要不要先扣住、不调用 TranslateMessage（按键闸门）。
  bool holdKeyMessage(const MSG& msg) const;
  void handleStatus(EngineStatus status);
  void handleResult(EngineResult* result);
  void handleTranslateDone(TranslateOutcome* outcome);

  // 关窗：存窗口位置、停引擎（最多等 waitMs）。
  void shutdown();

private:
  enum ControlId {
    kIdMode = 1001,
    kIdEdit,
    kIdTranslate,
    kIdCopySource,
    kIdRestore,
    kIdRecopy,
    kIdTopmost,
    kIdSettings,
    kIdSourceLabel,
    kIdResultSource,
    kIdTransLabel,
    kIdResultTrans,
    kIdStatus,
    kIdCounter,
  };
  enum TimerId { kTimerSelection = 1, kTimerGeometry };
  enum class StatusKind { Neutral, Success, Warn, Error };

  // 被扣住的按键：引擎说"没吃"才用扣住时的键盘状态原样回放给编辑框。
  struct HeldKey {
    uint64_t id = 0;
    MSG msg = {};
    BYTE states[256] = {};
    bool resolved = false;
    bool eaten = false;
  };
  struct CandidateItem {
    std::wstring text;
    std::wstring comment;
  };

  static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK editSubclassProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR subclassId, DWORD_PTR refData);

  LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);
  LRESULT onCommand(WPARAM wp, LPARAM lp);
  int onCreate();
  int onDestroy();
  void createControls();
  void setupFonts();
  void applyFonts();
  void layout();
  int minimumHeight() const;
  void paint(HDC dc, const RECT& paintRect);
  void paintStrip(HDC dc);
  void onMouseDown(LPARAM lp);

  // 按键闸门
  LRESULT onEditKeyDown(HWND edit, UINT message, WPARAM wp, LPARAM lp);
  LRESULT onEditKeyUp(HWND edit, UINT message, WPARAM wp, LPARAM lp);
  void holdKey(HWND edit, UINT message, WPARAM wp, LPARAM lp, const BYTE states[256]);
  void replayKey(const HeldKey& held);
  void resolveHeld(uint64_t id, bool ok, bool eaten);
  void drainHeld();
  void checkSelectionAutoCopy(HWND edit);

  // 引擎回包
  void applyFrame(const Json::Value& frame);
  void setAsciiMode(bool ascii);
  void clearComposition();
  void insertCommit(const std::wstring& text);

  // 动作
  void doTranslate();
  void startTranslateRequest();
  void doCopySource();
  void doRestore();
  void doRecopy();
  void toggleTopMost(bool on);
  void openSettings();
  bool copySelection(HWND edit);
  void autoCopySelections();
  void clearTranslatedSpan();

  // 界面杂项
  void setStatus(const std::wstring& text, StatusKind kind);
  void updateControls();
  HWND createControl(const wchar_t* className, const wchar_t* text, DWORD style,
                     DWORD extendedStyle, int id);
  void loadGeometry();
  void saveGeometry() const;
  std::wstring controlText(HWND control) const;
  // 进剪贴板的唯一出口。调用方负责先过 convertPunctuation()。
  static bool copyText(HWND owner, const std::wstring& text);
  int scaled(int px) const { return MulDiv(px, static_cast<int>(dpi_), 96); }

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  std::wstring exeDir_;
  std::filesystem::path translateConfigPath_;
  std::filesystem::path geometryPath_;
  std::wstring fontFamily_;  // 系统微软雅黑，或随包的 Noto Sans SC
  EngineClient* engine_ = nullptr;

  HWND modeButton_ = nullptr;
  HWND edit_ = nullptr;
  HWND translateButton_ = nullptr;
  HWND copySourceButton_ = nullptr;
  HWND restoreButton_ = nullptr;
  HWND recopyButton_ = nullptr;
  HWND topmostCheck_ = nullptr;
  HWND settingsButton_ = nullptr;
  HWND sourceLabel_ = nullptr;
  HWND resultSource_ = nullptr;
  HWND transLabel_ = nullptr;
  HWND resultTrans_ = nullptr;
  HWND statusText_ = nullptr;
  HWND counterText_ = nullptr;

  UINT dpi_ = 96;
  bool topmost_ = false;
  int geometryX_ = CW_USEDEFAULT;  // 窗口几何记忆（user\panel.json，可丢）
  int geometryY_ = CW_USEDEFAULT;
  int geometryW_ = 740;
  int geometryH_ = 560;
  HFONT uiFont_ = nullptr;
  HFONT editFont_ = nullptr;
  HFONT preeditFont_ = nullptr;
  HFONT candidateFont_ = nullptr;
  HFONT numberFont_ = nullptr;
  HFONT commentFont_ = nullptr;
  HBRUSH bgBrush_ = nullptr;
  HBRUSH whiteBrush_ = nullptr;
  HBRUSH resultBrush_ = nullptr;
  HBRUSH highlightBrush_ = nullptr;
  HBRUSH separatorBrush_ = nullptr;
  HBRUSH caretBrush_ = nullptr;
  COLORREF statusColor_ = 0;
  bool overLimit_ = false;

  RECT stripRect_ = {};
  std::vector<RECT> candidateRects_;

  // 引擎状态（只由界面线程读写）
  EngineStatus engineStatus_ = EngineStatus::Connecting;
  bool asciiMode_ = false;
  std::wstring preedit_;
  int preeditCaret16_ = 0;  // 组字光标，UTF-16 下标
  std::vector<CandidateItem> candidates_;
  bool showCandidates_ = false;
  int highlighted_ = 0;
  std::wstring selKeys_ = L"1234567890";

  std::deque<HeldKey> held_;
  uint64_t nextKeyId_ = 1;
  std::set<UINT> swallowedUps_;  // 面板快捷键自己的键松，不再送引擎

  // 翻译
  bool translating_ = false;
  bool translatePending_ = false;
  std::wstring pendingSource_;   // 发出去的原文（翻译期间输入区可能继续变）
  size_t pendingLead_ = 0;       // 原文在输入区里的起始位置（UTF-16），回来时只清这一段
  std::wstring lastSource_;      // 最近一次清掉的原文，给【恢复】用；用过即清
  std::wstring lastTranslation_;

  static constexpr UINT kMsgRefocusEdit = WM_APP + 20;
};

} // namespace MoqiPanel
