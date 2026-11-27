// 主窗口的骨架：注册、创建、布局、绘制、消息分发。按键闸门与翻译流程在
// PanelActions.cpp。
#include <windows.h>

#include <commctrl.h>
#include <imm.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>

#include "PanelCore.h"
#include "PanelMessages.h"
#include "PanelStyle.h"
#include "PanelWindow.h"
#include "SettingsDialog.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")

namespace MoqiPanel {
namespace {

HFONT makeFont(const std::wstring& face, int px, int weight) {
  return ::CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face.c_str());
}

struct FontSearch {
  const wchar_t* family;
  bool found = false;
};

int CALLBACK fontEnumProc(const LOGFONTW* logFont, const TEXTMETRICW*, DWORD,
                          LPARAM param) {
  auto* search = reinterpret_cast<FontSearch*>(param);
  if (_wcsicmp(logFont->lfFaceName, search->family) == 0) {
    search->found = true;
    return 0;
  }
  return 1;
}

bool hasFontFamily(const wchar_t* family) {
  HDC dc = ::GetDC(nullptr);
  FontSearch search{family, false};
  LOGFONTW query = {};
  query.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(query.lfFaceName, family, _TRUNCATE);
  ::EnumFontFamiliesExW(dc, &query, &fontEnumProc, reinterpret_cast<LPARAM>(&search), 0);
  ::ReleaseDC(nullptr, dc);
  return search.found;
}

BOOL CALLBACK refontChildProc(HWND child, LPARAM param) {
  const auto* self = reinterpret_cast<const PanelWindow*>(param);
  ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(self->uiFont()), TRUE);
  return TRUE;
}

bool readFileBytes(const std::filesystem::path& file, std::string* out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

} // namespace

// ---- 创建 -----------------------------------------------------------------

ATOM PanelWindow::registerClass(HINSTANCE instance) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = &PanelWindow::wndProc;
  wc.hInstance = instance;
  wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // 自己涂底色（WM_ERASEBKGND 返回 1），避免闪
  wc.lpszClassName = kClassName;
  return ::RegisterClassExW(&wc);
}

PanelWindow* PanelWindow::create(HINSTANCE instance, const std::wstring& exeDir,
                                 std::wstring* why) {
  why->clear();
  // ① 路径必须纯 ASCII。引擎依赖链（LevelDB、marisa-trie）用的是 ANSI 文件
  // API：纯英文系统（代码页 1252）上，路径里只要有中文，用户词典和编译产物
  // 就打不开——表现是引擎起不来。这里直接拦住并说清楚。
  for (wchar_t c : exeDir) {
    if (c > 0x7F) {
      *why = L"程序所在的文件夹路径只能包含英文、数字和普通符号。\n\n当前路径：\n" +
             exeDir + L"\n\n请把整个文件夹移到（或改名成）纯英文路径后再运行。";
      return nullptr;
    }
  }
  // ② 自带文件齐全。
  const wchar_t* required[] = {
      L"\\MoqiDaemon.exe", L"\\rime.dll",
      L"\\moqi-ime\\input_methods\\rime\\data\\default.yaml"};
  for (const wchar_t* relative : required) {
    const std::wstring path = exeDir + relative;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      *why = L"文件夹不完整，缺少：\n" + path + L"\n\n请把压缩包完整解压后再运行。";
      return nullptr;
    }
  }
  // ③ 目录可写：用户词典、学习数据、日志都落在程序目录的 user\ 下。
  const std::filesystem::path userDir = std::filesystem::path(exeDir) / L"user";
  std::error_code ec;
  std::filesystem::create_directories(userDir, ec);
  bool writable = !ec;
  if (writable) {
    const std::filesystem::path probe = userDir / L".write-probe";
    {
      std::ofstream out(probe, std::ios::binary);
      writable = out.good();
    }
    std::error_code removeEc;
    std::filesystem::remove(probe, removeEc);
  }
  if (!writable) {
    *why = L"程序目录不可写，无法保存用户词典和学习数据：\n" + userDir.wstring() +
           L"\n\n请把整个文件夹放到可写的位置（例如 D:\\MoqiPanel 或 U 盘）再运行。";
    return nullptr;
  }

  auto* panel = new PanelWindow();
  panel->instance_ = instance;
  panel->exeDir_ = exeDir;
  panel->translateConfigPath_ = std::filesystem::path(exeDir) / L"translate.json";
  panel->geometryPath_ = userDir / L"panel.json";
  HDC screen = ::GetDC(nullptr);
  panel->dpi_ = static_cast<UINT>(::GetDeviceCaps(screen, LOGPIXELSX));
  ::ReleaseDC(nullptr, screen);
  panel->loadGeometry();
  ensureTranslateConfigTemplate(panel->translateConfigPath_);

  // 启动检查全过才弹窗口；窗口本身失败才需要回收（此后对象随进程存活）。
  HWND hwnd = ::CreateWindowExW(
      panel->topmost_ ? WS_EX_TOPMOST : 0, kClassName, kWindowTitle,
      WS_OVERLAPPEDWINDOW, panel->geometryX_, panel->geometryY_,
      panel->geometryW_, panel->geometryH_, nullptr, nullptr, instance, panel);
  if (hwnd == nullptr) {
    *why = L"创建窗口失败。";
    delete panel;
    return nullptr;
  }
  return panel;
}

int PanelWindow::onCreate() {
  setupFonts();
  bgBrush_ = ::CreateSolidBrush(kWindowBg);
  whiteBrush_ = ::CreateSolidBrush(RGB(255, 255, 255));
  resultBrush_ = ::CreateSolidBrush(kResultBg);
  highlightBrush_ = ::CreateSolidBrush(kHighlight);
  separatorBrush_ = ::CreateSolidBrush(kSeparator);
  caretBrush_ = ::CreateSolidBrush(kAccent);
  createControls();
  layout();
  updateControls();
  setStatus(L"正在启动输入引擎…（第一次启动要部署词库，需要十几秒）",
            StatusKind::Neutral);
  // EngineClient 的生命周期 = 进程；停线程见 onDestroy，对象不 delete。
  engine_ = new EngineClient();
  engine_->start(hwnd_, exeDir_);
  return 0;
}

void PanelWindow::setupFonts() {
  // 系统有微软雅黑就用它（与墨奇候选窗同观感）；没有（纯英文系统常见）则
  // 私有加载随包的思源黑体 Noto Sans SC——只在本进程可见，不装字体、不写
  // 系统目录。
  fontFamily_ = L"Microsoft YaHei";
  const wchar_t* forced = _wgetenv(L"MOQI_PANEL_FONT");
  const bool wantBundled = forced != nullptr && _wcsicmp(forced, L"bundled") == 0;
  if (wantBundled || !hasFontFamily(fontFamily_.c_str())) {
    const std::wstring file = exeDir_ + L"\\fonts\\NotoSansSC-Regular.otf";
    if (::AddFontResourceExW(file.c_str(), FR_PRIVATE, nullptr) != 0) {
      fontFamily_ = L"Noto Sans SC";
    }
  }
  applyFonts();
}

void PanelWindow::applyFonts() {
  struct Spec {
    HFONT* slot;
    int px;
    int weight;
  };
  // 界面字体 = 系统 9pt（12px），与 MFC/comctl 对话框一致（2026-11-27 用户
  // 在 12/13/14px 间实测定稿 12px；13/14 奇偶不是关键，观感以 12 为准）。
  const Spec specs[] = {
      {&uiFont_, 12, FW_NORMAL},       {&editFont_, 16, FW_NORMAL},
      {&preeditFont_, 15, FW_NORMAL},  {&candidateFont_, 15, FW_NORMAL},
      {&numberFont_, 13, FW_SEMIBOLD}, {&commentFont_, 12, FW_NORMAL},
  };
  for (const Spec& spec : specs) {
    if (*spec.slot != nullptr) ::DeleteObject(*spec.slot);
    *spec.slot = makeFont(fontFamily_, scaled(spec.px), spec.weight);
  }
  if (hwnd_ != nullptr) {
    ::EnumChildWindows(hwnd_, &refontChildProc, reinterpret_cast<LPARAM>(this));
  }
}

HWND PanelWindow::createControl(const wchar_t* className, const wchar_t* text,
                                DWORD style, DWORD extendedStyle, int id) {
  HWND control = ::CreateWindowExW(extendedStyle, className, text,
                                   WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   instance_, nullptr);
  ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), FALSE);
  return control;
}

void PanelWindow::createControls() {
  modeButton_ = createControl(L"BUTTON", L"中", WS_TABSTOP, 0, kIdMode);
  edit_ = createControl(L"EDIT", L"",
                        WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                            ES_NOHIDESEL,
                        WS_EX_CLIENTEDGE, kIdEdit);
  // 本进程已整体关掉系统输入法（见入口的 ImmDisableIME）；编辑框再补一刀
  // 双保险——否则在装了墨奇/微软拼音的机器上，键会先被系统输入法截走。
  ::ImmAssociateContext(edit_, nullptr);
  ::SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(scaled(4), scaled(4)));
  translateButton_ = createControl(L"BUTTON", L"翻译并复制 (Ctrl+Enter)", WS_TABSTOP, 0,
                                   kIdTranslate);
  copySourceButton_ = createControl(L"BUTTON", L"复制原文", WS_TABSTOP, 0, kIdCopySource);
  restoreButton_ = createControl(L"BUTTON", L"恢复", WS_TABSTOP, 0, kIdRestore);
  recopyButton_ = createControl(L"BUTTON", L"重新复制", WS_TABSTOP, 0, kIdRecopy);
  topmostCheck_ = createControl(L"BUTTON", L"置顶", WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                                kIdTopmost);
  settingsButton_ = createControl(L"BUTTON", L"翻译设置…", WS_TABSTOP, 0, kIdSettings);
  const DWORD resultStyle =
      WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL;
  sourceLabel_ = createControl(L"STATIC", L"原文", SS_NOPREFIX, 0, kIdSourceLabel);
  resultSource_ = createControl(L"EDIT", L"", resultStyle, WS_EX_CLIENTEDGE, kIdResultSource);
  transLabel_ = createControl(L"STATIC", L"译文", SS_NOPREFIX, 0, kIdTransLabel);
  resultTrans_ = createControl(L"EDIT", L"", resultStyle, WS_EX_CLIENTEDGE, kIdResultTrans);
  statusText_ = createControl(L"STATIC", L"", SS_NOPREFIX, 0, kIdStatus);
  counterText_ = createControl(L"STATIC", L"0/256", SS_NOPREFIX | SS_RIGHT, 0, kIdCounter);

  // 三个编辑框挂同一个子类：按键闸门（输入区）、复制走标点转换、选中自动复制。
  for (HWND target : {edit_, resultSource_, resultTrans_}) {
    ::SetWindowSubclass(target, &PanelWindow::editSubclassProc, 1,
                        reinterpret_cast<DWORD_PTR>(this));
  }
  ::SendMessageW(topmostCheck_, BM_SETCHECK, topmost_ ? BST_CHECKED : BST_UNCHECKED, 0);
}

// ---- 布局 -----------------------------------------------------------------

int PanelWindow::minimumHeight() const {
  // 输入区最小 64px（约三行）——面板是短句输入板，不是长文编辑器
  //（2026-11-27 用户："不是让人在这里长篇大论的"）。
  return scaled(64 + 8 + 64 + 8 + 30 + 10 + 2 * (20 + 54 + 6) + 6 + 20 + 8);
}

void PanelWindow::layout() {
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  const int width = client.right;
  const int margin = scaled(10);
  auto place = [&](HWND control, int left, int top, int w, int h) {
    ::MoveWindow(control, left, top, w, h, TRUE);
    return left + w;
  };

  // 组字/候选带（固定高度，空闲时只留模式按钮：布局不随打字跳动）。
  const int stripHeight = scaled(64);
  stripRect_ = {0, 0, width, stripHeight};
  place(modeButton_, margin, (stripHeight - scaled(26)) / 2, scaled(38), scaled(26));
  int y = stripHeight + scaled(8);

  // 输入区吃掉剩余高度。
  const int actionHeight = scaled(30);
  const int labelHeight = scaled(20);
  const int resultEditHeight = scaled(54);
  const int gap = scaled(6);
  const int statusHeight = scaled(20);
  const int belowEdit = scaled(8) + actionHeight + scaled(10) +
                        2 * (labelHeight + resultEditHeight + gap) + gap + statusHeight +
                        scaled(8);
  int editHeight =
      (std::max)(static_cast<int>(client.bottom) - y - belowEdit, scaled(96));
  place(edit_, margin, y, width - margin * 2, editHeight);
  y += editHeight + scaled(8);

  // 动作行。
  int x = margin;
  x = place(translateButton_, x, y, scaled(200), actionHeight) + gap;
  x = place(copySourceButton_, x, y, scaled(92), actionHeight) + gap;
  x = place(restoreButton_, x, y, scaled(64), actionHeight) + gap;
  place(recopyButton_, x, y, scaled(92), actionHeight);
  const int settingsWidth = scaled(104);
  const int topmostWidth = scaled(62);
  place(settingsButton_, width - margin - settingsWidth, y, settingsWidth, actionHeight);
  place(topmostCheck_, width - margin - settingsWidth - gap - topmostWidth, y, topmostWidth,
        actionHeight);
  y += actionHeight + scaled(10);

  // 结果区：原文、译文各一格（只读，可选中）。
  const int innerWidth = width - margin * 2;
  place(sourceLabel_, margin, y, scaled(60), labelHeight);
  y += labelHeight;
  place(resultSource_, margin, y, innerWidth, resultEditHeight);
  y += resultEditHeight + gap;
  place(transLabel_, margin, y, scaled(60), labelHeight);
  y += labelHeight;
  place(resultTrans_, margin, y, innerWidth, resultEditHeight);
  y += resultEditHeight + gap;

  // 状态行。
  const int counterWidth = scaled(90);
  place(statusText_, margin, y, (std::max)(innerWidth - counterWidth - gap, scaled(100)),
        statusHeight);
  place(counterText_, width - margin - counterWidth, y, counterWidth, statusHeight);
}

// ---- 绘制 -----------------------------------------------------------------

void PanelWindow::paint(HDC dc, const RECT& paintRect) {
  ::FillRect(dc, &paintRect, bgBrush_);
  RECT dirtyStrip = {};
  if (::IntersectRect(&dirtyStrip, &paintRect, &stripRect_)) {
    (void)dirtyStrip;
    paintStrip(dc);
  }
}

void PanelWindow::paintStrip(HDC dc) {
  // 组字带每键都重画。**直接画在屏幕 DC 上**：GDI 的 ClearType 只对画到
  // 屏幕 DC 的文字生效，先画内存位图再 BitBlt 会回落成灰阶抗锯齿，文字发虚
  // （2026-11-27 用户实测，像素级确认）。防闪靠只重画条带自身的小区域，
  // 底色和文字在同一遍 WM_PAINT 里完成。坐标从条带局部换回窗口绝对坐标；
  // BeginPaint 给的 DC 已按失效区域裁剪，这里不会画出条带外。
  const int width = stripRect_.right - stripRect_.left;
  const int height = stripRect_.bottom - stripRect_.top;
  RECT whole = {stripRect_.left, stripRect_.top, stripRect_.right, stripRect_.bottom};
  ::FillRect(dc, &whole, whiteBrush_);
  RECT line = {stripRect_.left, stripRect_.bottom - 1, stripRect_.right, stripRect_.bottom};
  ::FillRect(dc, &line, separatorBrush_);

  const int left = stripRect_.left + scaled(10) + scaled(38) + scaled(10);
  const int rowHeight = scaled(24);
  // 保持原行为：Transparent（本轮不动，见下方直画说明之外的取舍）。
  ::SetBkMode(dc, TRANSPARENT);

  // 第一行：组字串 + 组字光标。
  if (!preedit_.empty()) {
    ::SelectObject(dc, preeditFont_);
    ::SetTextColor(dc, kText);
    const int top = stripRect_.top + (scaled(32) - rowHeight) / 2 + scaled(4);
    SIZE prefixSize = {};
    const std::wstring prefix = preedit_.substr(0, static_cast<size_t>(preeditCaret16_));
    ::GetTextExtentPoint32W(dc, prefix.data(), static_cast<int>(prefix.size()),
                            &prefixSize);
    ::TextOutW(dc, left, top, preedit_.data(), static_cast<int>(preedit_.size()));
    RECT caret = {left + prefixSize.cx, top, left + prefixSize.cx + scaled(2),
                  top + rowHeight};
    ::FillRect(dc, &caret, caretBrush_);
  }

  // 第二行：候选。序号 + 词文 + （有则）提示；高亮项铺底色。
  candidateRects_.clear();
  if (showCandidates_) {
    const int top = stripRect_.top + scaled(36);
    int x = left;
    for (size_t i = 0; i < candidates_.size(); ++i) {
      const bool highlighted = static_cast<int>(i) == highlighted_;
      const CandidateItem& item = candidates_[i];
      const std::wstring number =
          i < selKeys_.size() ? selKeys_.substr(i, 1) : std::to_wstring(i + 1);
      const std::wstring numberText = number + L" ";
      ::SelectObject(dc, numberFont_);
      SIZE numberSize = {};
      ::GetTextExtentPoint32W(dc, numberText.data(),
                              static_cast<int>(numberText.size()), &numberSize);
      ::SelectObject(dc, candidateFont_);
      SIZE textSize = {};
      ::GetTextExtentPoint32W(dc, item.text.data(), static_cast<int>(item.text.size()),
                              &textSize);
      SIZE commentSize = {};
      if (!item.comment.empty()) {
        ::SelectObject(dc, commentFont_);
        ::GetTextExtentPoint32W(dc, item.comment.data(),
                                static_cast<int>(item.comment.size()), &commentSize);
      }
      const int textTop = top + (rowHeight - textSize.cy) / 2;
      const int total = scaled(8) + numberSize.cx + textSize.cx +
                        (commentSize.cx > 0 ? scaled(4) + commentSize.cx : 0) + scaled(8);
      if (x + total > stripRect_.left + width - scaled(8)) {
        break;  // 放不下的不画（翻页键可看其余）
      }
      if (highlighted) {
        RECT box = {x, top, x + total, top + rowHeight};
        HBRUSH oldBrush = static_cast<HBRUSH>(::SelectObject(dc, highlightBrush_));
        HPEN pen = ::CreatePen(PS_SOLID, 1, kHighlightBorder);
        HPEN oldPen = static_cast<HPEN>(::SelectObject(dc, pen));
        ::RoundRect(dc, box.left, box.top, box.right, box.bottom, scaled(6), scaled(6));
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);
      }
      int cursor = x + scaled(8);
      ::SelectObject(dc, numberFont_);
      ::SetTextColor(dc, kAccent);
      ::TextOutW(dc, cursor, top + (rowHeight - numberSize.cy) / 2, numberText.data(),
                 static_cast<int>(numberText.size()));
      cursor += numberSize.cx;
      ::SelectObject(dc, candidateFont_);
      ::SetTextColor(dc, kText);
      ::TextOutW(dc, cursor, textTop, item.text.data(), static_cast<int>(item.text.size()));
      cursor += textSize.cx;
      if (commentSize.cx > 0) {
        cursor += scaled(4);
        ::SelectObject(dc, commentFont_);
        ::SetTextColor(dc, kMutedText);
        ::TextOutW(dc, cursor, top + (rowHeight - commentSize.cy) / 2,
                   item.comment.data(), static_cast<int>(item.comment.size()));
      }
      candidateRects_.push_back({x - stripRect_.left, top - stripRect_.top,
                                 x + total - stripRect_.left, top + rowHeight - stripRect_.top});
      x += total + scaled(6);
    }
  }
}

void PanelWindow::onMouseDown(LPARAM lp) {
  POINT point = {static_cast<SHORT>(LOWORD(lp)), static_cast<SHORT>(HIWORD(lp))};
  for (size_t i = 0; i < candidateRects_.size(); ++i) {
    RECT rect = candidateRects_[i];
    if (::PtInRect(&rect, point)) {
      engine_->postSelectCandidate(static_cast<int>(i));
      break;
    }
  }
  ::SetFocus(edit_);
}

// ---- 杂项 -----------------------------------------------------------------

std::wstring PanelWindow::controlText(HWND control) const {
  const size_t length = static_cast<size_t>(::SendMessageW(control, WM_GETTEXTLENGTH, 0, 0));
  std::wstring out(length + 1, L'\0');
  const size_t copied = static_cast<size_t>(
      ::SendMessageW(control, WM_GETTEXT, static_cast<WPARAM>(out.size()),
                     reinterpret_cast<LPARAM>(&out[0])));
  out.resize(copied);
  return out;
}

void PanelWindow::setStatus(const std::wstring& text, StatusKind kind) {
  ::SetWindowTextW(statusText_, text.c_str());
  statusColor_ = kind == StatusKind::Success   ? kStatusSuccess
                 : kind == StatusKind::Warn   ? kStatusWarn
                 : kind == StatusKind::Error  ? kStatusError
                                              : kStatusNeutral;
  ::InvalidateRect(statusText_, nullptr, FALSE);
}

void PanelWindow::updateControls() {
  const bool busy = translating_ || translatePending_;
  ::EnableWindow(translateButton_, !busy);
  ::SetWindowTextW(translateButton_, translating_ ? L"正在翻译…" : L"翻译并复制 (Ctrl+Enter)");
  ::EnableWindow(restoreButton_, !lastSource_.empty());
  ::EnableWindow(recopyButton_, !lastTranslation_.empty());
  ::EnableWindow(modeButton_, engineStatus_ != EngineStatus::Failed);
  // 字数护栏：按去首尾空白后的码点数计。
  const size_t count = codePointCount(trimmed(controlText(edit_)));
  overLimit_ = count > kMaxTranslateCodePoints;
  ::SetWindowTextW(counterText_,
                   (std::to_wstring(count) + L"/" + std::to_wstring(kMaxTranslateCodePoints))
                       .c_str());
  ::InvalidateRect(counterText_, nullptr, FALSE);
  ::InvalidateRect(statusText_, nullptr, FALSE);
}

void PanelWindow::loadGeometry() {
  geometryX_ = CW_USEDEFAULT;
  geometryY_ = CW_USEDEFAULT;
  geometryW_ = MulDiv(640, static_cast<int>(dpi_), 96);
  geometryH_ = MulDiv(420, static_cast<int>(dpi_), 96);
  topmost_ = false;
  std::string bytes;
  if (!readFileBytes(geometryPath_, &bytes)) return;
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string parseError;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &root, &parseError)) return;
  const Json::Value& window = root["window"];
  if (!window.isObject() || !window["x"].isInt() || !window["y"].isInt() ||
      !window["w"].isInt() || !window["h"].isInt()) {
    return;
  }
  RECT saved = {window["x"].asInt(), window["y"].asInt(),
                window["x"].asInt() + window["w"].asInt(),
                window["y"].asInt() + window["h"].asInt()};
  // 窗口至少要有一角还落在某块屏幕的工作区里（拔了显示器也不至于找不到）。
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (::GetMonitorInfoW(::MonitorFromRect(&saved, MONITOR_DEFAULTTONULL), &info)) {
    RECT overlap = {};
    if (::IntersectRect(&overlap, &saved, &info.rcWork) &&
        overlap.right - overlap.left >= scaled(160) &&
        overlap.bottom - overlap.top >= scaled(120)) {
      geometryX_ = saved.left;
      geometryY_ = saved.top;
      geometryW_ = (std::max)(window["w"].asInt(), scaled(620));
      geometryH_ = (std::max)(window["h"].asInt(), minimumHeight());
    }
  }
  topmost_ = root["topmost"].isBool() && root["topmost"].asBool();
}

void PanelWindow::saveGeometry() const {
  if (hwnd_ == nullptr || ::IsIconic(hwnd_)) return;
  WINDOWPLACEMENT placement = {};
  placement.length = sizeof(placement);
  if (!::GetWindowPlacement(hwnd_, &placement)) return;
  Json::Value root(Json::objectValue);
  Json::Value window(Json::objectValue);
  const RECT& normal = placement.rcNormalPosition;
  window["x"] = static_cast<int>(normal.left);
  window["y"] = static_cast<int>(normal.top);
  window["w"] = static_cast<int>(normal.right - normal.left);
  window["h"] = static_cast<int>(normal.bottom - normal.top);
  root["window"] = window;
  root["topmost"] = topmost_;
  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = "  ";
  std::ofstream out(geometryPath_, std::ios::binary | std::ios::trunc);
  if (!out) return;
  out << Json::writeString(builder, root) << "\n";
}

// ---- 窗口过程 -------------------------------------------------------------

LRESULT CALLBACK PanelWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  PanelWindow* self = reinterpret_cast<PanelWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
    self = static_cast<PanelWindow*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (self == nullptr || self->hwnd_ != hwnd) {
    return ::DefWindowProcW(hwnd, msg, wp, lp);
  }
  return self->handleMessage(msg, wp, lp);
}

LRESULT PanelWindow::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      return onCreate();
    case WM_SIZE:
      layout();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lp);
      info->ptMinTrackSize = {scaled(640), minimumHeight()};
      return 0;
    }
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wp);
      applyFonts();
      const RECT* suggested = reinterpret_cast<const RECT*>(lp);
      ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    // 工作线程投来的三样东西：引擎状态、引擎回包、翻译结果（见 PanelMessages.h）。
    case WM_APP_ENGINE_STATUS:
      handleStatus(static_cast<EngineStatus>(wp));
      return 0;
    case WM_APP_ENGINE_RESULT:
      handleResult(reinterpret_cast<EngineResult*>(lp));
      return 0;
    case WM_APP_TRANSLATE_DONE:
      handleTranslateDone(reinterpret_cast<TranslateOutcome*>(lp));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd_, &ps);
      paint(dc, ps.rcPaint);
      ::EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_COMMAND:
      return onCommand(wp, lp);
    case WM_TIMER:
      if (wp == kTimerSelection) {
        ::KillTimer(hwnd_, kTimerSelection);
        autoCopySelections();
      } else if (wp == kTimerGeometry) {
        ::KillTimer(hwnd_, kTimerGeometry);
        saveGeometry();
      }
      return 0;
    case WM_EXITSIZEMOVE:
      ::SetTimer(hwnd_, kTimerGeometry, 400, nullptr);
      return 0;
    case WM_LBUTTONDOWN:
      onMouseDown(lp);
      return 0;
    case WM_MOUSEWHEEL:
      // 组字时滚轮翻页候选（往前滚 = 上一页），与候选窗的滚轮习惯一致。
      if (showCandidates_ || !preedit_.empty()) {
        engine_->postChangePage(GET_WHEEL_DELTA_WPARAM(wp) > 0);
        return 0;
      }
      break;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        POINT point = {};
        ::GetCursorPos(&point);
        ::ScreenToClient(hwnd_, &point);
        for (const RECT& rect : candidateRects_) {
          if (::PtInRect(&rect, point)) {
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
          }
        }
      }
      break;
    case WM_CTLCOLOREDIT:
      ::SetTextColor(reinterpret_cast<HDC>(wp), kText);
      ::SetBkColor(reinterpret_cast<HDC>(wp), RGB(255, 255, 255));
      return reinterpret_cast<LRESULT>(whiteBrush_);
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wp);
      HWND control = reinterpret_cast<HWND>(lp);
      ::SetBkMode(dc, TRANSPARENT);
      if (control == resultSource_ || control == resultTrans_) {
        ::SetTextColor(dc, kText);
        ::SetBkColor(dc, kResultBg);
        return reinterpret_cast<LRESULT>(resultBrush_);
      }
      if (control == statusText_) {
        ::SetTextColor(dc, statusColor_);
      } else if (control == counterText_) {
        ::SetTextColor(dc, overLimit_ ? kStatusError : kMutedText);
      } else {
        ::SetTextColor(dc, kMutedText);
      }
      return reinterpret_cast<LRESULT>(bgBrush_);
    }
    case WM_SETFOCUS:
      ::SetFocus(edit_);
      return 0;
    case kMsgRefocusEdit:
      if (::GetFocus() != edit_ && ::GetFocus() != nullptr) ::SetFocus(edit_);
      return 0;
    case WM_DESTROY:
      return onDestroy();
    default:
      break;
  }
  return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT PanelWindow::onCommand(WPARAM wp, LPARAM lp) {
  const int id = LOWORD(wp);
  const int code = HIWORD(wp);
  if (lp == 0) return 0;  // 不是控件发来的
  switch (id) {
    case kIdMode:
      if (code == BN_CLICKED) {
        engine_->postShiftTap();
        ::SetFocus(edit_);
      }
      return 0;
    case kIdTranslate:
      if (code == BN_CLICKED) {
        doTranslate();
        ::SetFocus(edit_);
      }
      return 0;
    case kIdCopySource:
      if (code == BN_CLICKED) {
        doCopySource();
        ::SetFocus(edit_);
      }
      return 0;
    case kIdRestore:
      if (code == BN_CLICKED) {
        doRestore();
        ::SetFocus(edit_);
      }
      return 0;
    case kIdRecopy:
      if (code == BN_CLICKED) {
        doRecopy();
        ::SetFocus(edit_);
      }
      return 0;
    case kIdTopmost:
      if (code == BN_CLICKED) {
        toggleTopMost(::SendMessageW(topmostCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        ::SetFocus(edit_);
      }
      return 0;
    case kIdSettings:
      if (code == BN_CLICKED) {
        openSettings();
        ::SetFocus(edit_);
      }
      return 0;
    case kIdEdit:
      if (code == EN_CHANGE) updateControls();
      return 0;
    default:
      return 0;
  }
}

int PanelWindow::onDestroy() {
  ::KillTimer(hwnd_, kTimerSelection);
  ::KillTimer(hwnd_, kTimerGeometry);
  saveGeometry();
  // 停引擎最多等 2 秒。没等到也不阻塞退出：进程一走管道就断，便携 daemon
  // 随之自退。
  shutdown();
  ::PostQuitMessage(0);
  return 0;
}

} // namespace MoqiPanel
