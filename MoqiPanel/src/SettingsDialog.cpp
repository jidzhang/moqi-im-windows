// 翻译设置对话框（模态）：接口地址 / API 密钥 / 模型 / 代理 / 附加参数。
// 不依赖对话框资源模板，控件在代码里建——换高 DPI 时按当前比例算坐标。
#include <windows.h>

#include <memory>
#include <thread>

#include "PanelMessages.h"
#include "PanelCore.h"
#include "TranslateHttp.h"
#include "SettingsDialog.h"

namespace MoqiPanel {
namespace {

constexpr wchar_t kDialogClass[] = L"MoqiPanelSettings";
constexpr COLORREF kErrorText = RGB(196, 43, 28);
constexpr COLORREF kSuccessText = RGB(16, 124, 16);
constexpr COLORREF kMuted = RGB(128, 132, 138);
constexpr COLORREF kLabelText = RGB(24, 24, 24);

enum {
  kIdLoadErrorSlot = 2199,  // 只为在 WM_CTLCOLORSTATIC 里认出"读不出配置"那行
  kIdFieldLabel = 1,        // 字段名（深色）；说明文字用默认 id 0（灰色）
  kIdBaseUrl = 2101,
  kIdApiKey,
  kIdShowKey,
  kIdModel,
  kIdProxy,
  kIdExtra,
  kIdTest,
  kIdTestResult,
  kIdSave,
  kIdCancel,
};

struct Dialog {
  HWND window = nullptr;  // 非空 = 对话框还在；WM_DESTROY 置空，模态循环据此结束
  HFONT font = nullptr;
  std::wstring loadErrorText;
  TranslateConfig initial;
  TranslateConfig* saved = nullptr;
  bool accepted = false;
  bool testing = false;
  UINT dpi = 96;

  HWND baseUrl = nullptr;
  HWND apiKey = nullptr;
  HWND showKey = nullptr;
  HWND model = nullptr;
  HWND proxy = nullptr;
  HWND extra = nullptr;
  HWND testButton = nullptr;
  HWND testResult = nullptr;
  HWND loadError = nullptr;

  int scaled(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }
};

std::wstring controlText(HWND control) {
  const size_t length = static_cast<size_t>(::SendMessageW(control, WM_GETTEXTLENGTH, 0, 0));
  std::wstring out(length + 1, L'\0');
  const size_t copied = static_cast<size_t>(
      ::SendMessageW(control, WM_GETTEXT, static_cast<WPARAM>(out.size()),
                     reinterpret_cast<LPARAM>(&out[0])));
  out.resize(copied);
  return out;
}

HWND makeControl(Dialog& dialog, const wchar_t* className, const wchar_t* text,
                 DWORD style, int x, int y, int w, int h, int id) {
  HWND control = ::CreateWindowExW(
      0, className, text, WS_CHILD | WS_VISIBLE | style, dialog.scaled(x), dialog.scaled(y),
      dialog.scaled(w), dialog.scaled(h), dialog.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr),
      nullptr);
  ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog.font), FALSE);
  return control;
}

HWND label(Dialog& dialog, const wchar_t* text, int x, int y, int w, int id = 0) {
  return makeControl(dialog, L"STATIC", text, SS_NOPREFIX, x, y, w, 18, id);
}

void createControls(Dialog& dialog) {
  const int fieldX = 96;
  const int fieldW = 450;
  const int editH = 24;
  dialog.loadError = label(dialog, dialog.loadErrorText.c_str(), 14, 12, 532, kIdLoadErrorSlot);
  dialog.baseUrl = makeControl(dialog, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, fieldX, 50,
                               fieldW, editH, kIdBaseUrl);
  label(dialog, L"Base URL", 14, 55, 76, kIdFieldLabel);
  label(dialog, L"OpenAI-compatible endpoint, e.g. https://api.deepseek.com (/v1 suffix also works)",
        fieldX, 76, fieldW);
  dialog.apiKey = makeControl(dialog, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
                              fieldX, 100, 356, editH, kIdApiKey);
  label(dialog, L"API Key", 14, 105, 76, kIdFieldLabel);
  dialog.showKey = makeControl(dialog, L"BUTTON", L"Show Key", WS_TABSTOP | BS_AUTOCHECKBOX,
                               462, 103, 84, 20, kIdShowKey);
  dialog.model = makeControl(dialog, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, fieldX, 132,
                             240, editH, kIdModel);
  label(dialog, L"Model", 14, 137, 76, kIdFieldLabel);
  dialog.proxy = makeControl(dialog, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, fieldX, 164,
                             240, editH, kIdProxy);
  label(dialog, L"Proxy", 14, 169, 76, kIdFieldLabel);
  label(dialog, L"Empty = system proxy; direct = no proxy; or http://host:port", fieldX, 190, fieldW);
  dialog.extra = makeControl(
      dialog, L"EDIT", L"",
      WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, fieldX, 214,
      fieldW, 44, kIdExtra);
  label(dialog, L"Extra Body", 14, 219, 76, kIdFieldLabel);
  label(dialog,
        L"JSON object, optional. E.g. {\"thinking\":{\"type\":\"disabled\"}} disables thinking", fieldX, 262,
        fieldW);
  dialog.testButton = makeControl(dialog, L"BUTTON", L"Test Connection", WS_TABSTOP, fieldX, 288, 96,
                                  28, kIdTest);
  makeControl(dialog, L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 362, 288, 88, 28,
              kIdSave);
  makeControl(dialog, L"BUTTON", L"Cancel", WS_TABSTOP, 462, 288, 84, 28, kIdCancel);
  dialog.testResult = label(dialog, L"", 14, 322, 532, kIdTestResult);

  ::SetWindowTextW(dialog.baseUrl, utf8ToWide(dialog.initial.baseUrl).c_str());
  ::SetWindowTextW(dialog.apiKey, utf8ToWide(dialog.initial.apiKey).c_str());
  ::SetWindowTextW(dialog.model, utf8ToWide(dialog.initial.model).c_str());
  ::SetWindowTextW(dialog.proxy, utf8ToWide(dialog.initial.proxy).c_str());
  if (dialog.initial.extraBody.isObject() && !dialog.initial.extraBody.empty()) {
    Json::StreamWriterBuilder builder;
    builder["commentStyle"] = "None";
    builder["indentation"] = "";
    ::SetWindowTextW(dialog.extra,
                     utf8ToWide(Json::writeString(builder, dialog.initial.extraBody)).c_str());
  }
}

// 把对话框里的值收进 config。校验不过返回 false 并给出原因。
bool collect(Dialog& dialog, TranslateConfig* config, std::wstring* error) {
  *config = TranslateConfig();
  config->baseUrl = wideToUtf8(trimmed(controlText(dialog.baseUrl)));
  config->apiKey = wideToUtf8(trimmed(controlText(dialog.apiKey)));
  config->model = wideToUtf8(trimmed(controlText(dialog.model)));
  config->proxy = wideToUtf8(trimmed(controlText(dialog.proxy)));
  // 提示词与 max_tokens 不在对话框里：原样保留配置文件里的值。
  config->systemPrompt = dialog.initial.systemPrompt;
  config->maxTokens = dialog.initial.maxTokens;
  const std::string extra = wideToUtf8(trimmed(controlText(dialog.extra)));
  if (!extra.empty()) {
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string parseError;
    if (!reader->parse(extra.data(), extra.data() + extra.size(), &parsed, &parseError) ||
        !parsed.isObject()) {
      *error =
          L"Extra Body must be a valid JSON object, e.g. {\"thinking\":{\"type\":\"disabled\"}}; "
          L"leave it empty if not needed.";
      return false;
    }
    config->extraBody = parsed;
  }
  if (config->baseUrl.empty()) {
    *error = L"Base URL cannot be empty.";
    return false;
  }
  const std::wstring baseUrl = utf8ToWide(config->baseUrl);
  if (_wcsnicmp(baseUrl.c_str(), L"http://", 7) != 0 &&
      _wcsnicmp(baseUrl.c_str(), L"https://", 8) != 0) {
    *error = L"Base URL must start with http:// or https://.";
    return false;
  }
  if (config->model.empty()) {
    *error = L"Model cannot be empty.";
    return false;
  }
  return true;
}

void setTestResult(Dialog& dialog, const std::wstring& text, bool error) {
  ::SetWindowTextW(dialog.testResult, text.c_str());
  ::SetWindowTextW(dialog.testButton, dialog.testing ? L"Testing…" : L"Test Connection");
  ::EnableWindow(dialog.testButton, !dialog.testing);
  ::SetPropW(dialog.testResult, L"MoqiPanelTestFailed", reinterpret_cast<HANDLE>(error ? 1 : 0));
  ::InvalidateRect(dialog.testResult, nullptr, TRUE);
}

void startTest(Dialog& dialog) {
  TranslateConfig config;
  std::wstring error;
  if (!collect(dialog, &config, &error)) {
    setTestResult(dialog, error, true);
    return;
  }
  dialog.testing = true;
  setTestResult(dialog, L"Connecting…", false);
  const HWND target = dialog.window;
  std::thread([target, config]() {
    auto* outcome = new TranslateOutcome(translateText(config, wideToUtf8(L"你好，世界。")));
    if (!::PostMessageW(target, WM_APP_TRANSLATE_DONE, 0,
                        reinterpret_cast<LPARAM>(outcome))) {
      delete outcome;
    }
  }).detach();
}

LRESULT CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Dialog* dialog = reinterpret_cast<Dialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
    dialog = static_cast<Dialog*>(create->lpCreateParams);
    dialog->window = hwnd;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    return TRUE;
  }
  if (dialog == nullptr || dialog->window != hwnd) return FALSE;
  switch (msg) {
    case WM_CREATE:
      createControls(*dialog);
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case kIdShowKey:
          ::SendMessageW(dialog->apiKey, EM_SETPASSWORDCHAR,
                         ::SendMessageW(dialog->showKey, BM_GETCHECK, 0, 0) == BST_CHECKED
                             ? 0
                             : L'\x25CF',
                         0);
          ::InvalidateRect(dialog->apiKey, nullptr, TRUE);
          return TRUE;
        case kIdTest:
          if (!dialog->testing) startTest(*dialog);
          return TRUE;
        case 2:  // IDCANCEL：IsDialogMessage 把 Esc 转成它（Esc 本身到不了 WM_KEYDOWN）
        case kIdCancel:
          ::DestroyWindow(hwnd);
          return TRUE;
        case kIdSave: {
          TranslateConfig config;
          std::wstring error;
          if (collect(*dialog, &config, &error)) {
            *dialog->saved = config;
            dialog->accepted = true;
            ::DestroyWindow(hwnd);
          } else {
            ::MessageBoxW(hwnd, error.c_str(), L"Translation Settings", MB_OK | MB_ICONWARNING);
          }
          return TRUE;
        }
        default:
          break;
      }
      break;
    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      dialog->window = nullptr;  // 模态循环看到它就结束
      return 0;
    case WM_APP_TRANSLATE_DONE: {
      auto* raw = reinterpret_cast<TranslateOutcome*>(lp);
      std::unique_ptr<TranslateOutcome> outcome(raw);
      dialog->testing = false;
      if (outcome != nullptr && outcome->ok) {
        setTestResult(*dialog, L"Endpoint works. \"你好，世界。\" -> " + utf8ToWide(outcome->translation),
                      false);
      } else {
        setTestResult(*dialog,
                      L"Connection failed: " + utf8ToWide(outcome ? outcome->error : "unknown error"), true);
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wp);
      HWND control = reinterpret_cast<HWND>(lp);
      // 必须 OPAQUE：透明底的 GDI 文本不出 ClearType（灰阶，发虚）。
      // 静态控件的底色与对话框刷子同色（BTNFACE），字形格看不出来。
      ::SetBkMode(dc, OPAQUE);
      ::SetBkColor(dc, ::GetSysColor(COLOR_BTNFACE));
      if (control == dialog->testResult) {
        ::SetTextColor(dc, ::GetPropW(control, L"MoqiPanelTestFailed") != nullptr ? kErrorText
                                                                                 : kSuccessText);
      } else if (control == dialog->loadError) {
        ::SetTextColor(dc, kErrorText);
      } else {
        ::SetTextColor(dc,
                       ::GetWindowLongPtrW(control, GWLP_ID) == 0 ? kMuted : kLabelText);
      }
      return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_BTNFACE));
    }
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool showSettingsDialog(HWND owner, HFONT font, const std::wstring& loadError,
                        const TranslateConfig& initial, TranslateConfig* saved) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &dialogProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hIcon = ::LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kDialogClass;
    if (::RegisterClassExW(&wc) == 0) return false;
    registered = true;
  }

  Dialog dialog;
  dialog.font = font;
  dialog.loadErrorText = loadError;
  dialog.initial = initial;
  dialog.saved = saved;
  HDC screen = ::GetDC(nullptr);
  dialog.dpi = static_cast<UINT>(::GetDeviceCaps(screen, LOGPIXELSX));
  ::ReleaseDC(nullptr, screen);

  // 控件坐标是按客户区排的：窗口尺寸要由客户区反推（加上标题栏和边框），
  // 否则最底下的测试结果那一行被标题栏挤出去。
  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  const DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  RECT frame = {0, 0, dialog.scaled(560), dialog.scaled(346)};
  ::AdjustWindowRectEx(&frame, style, FALSE, extendedStyle);
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;

  RECT ownerRect = {};
  ::GetWindowRect(owner, &ownerRect);
  HWND window = ::CreateWindowExW(
      extendedStyle, kDialogClass, L"Translation Settings", style,
      ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2,
      ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2, width, height, owner,
      nullptr, ::GetModuleHandleW(nullptr), &dialog);
  if (window == nullptr) return false;

  ::EnableWindow(owner, FALSE);
  ::ShowWindow(window, SW_SHOW);
  ::UpdateWindow(window);
  ::SetFocus(dialog.baseUrl);

  // 模态循环：主窗口此时不可点，但投给它的引擎回包照常分发。
  while (dialog.window != nullptr) {
    MSG msg = {};
    const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
    if (got <= 0) {
      if (got == 0) ::PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    if (::IsDialogMessageW(window, &msg)) continue;
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
  if (::IsWindow(owner)) {
    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
  }
  return dialog.accepted;
}

} // namespace MoqiPanel
