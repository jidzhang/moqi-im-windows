#include <windows.h>

#include <winhttp.h>

#include <cstdlib>
#include <string>

#include "PanelCore.h"
#include "TranslateHttp.h"

#pragma comment(lib, "winhttp.lib")

namespace MoqiPanel {
namespace {

std::wstring utf8(const std::string& s) { return utf8ToWide(s); }

// "系统代理"：读当前用户的系统代理设置（WinINET 那份）。WinHTTP 的
// DEFAULT_PROXY 只认 netsh 的机器级代理，不认用户在系统设置里配的那个，
// 所以这里取出来按"指定代理"用；没有配就直连。
bool systemProxy(std::wstring* proxy, std::wstring* bypass) {
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config = {};
  if (!::WinHttpGetIEProxyConfigForCurrentUser(&config)) return false;
  const bool manual = config.lpszProxy != nullptr && config.lpszProxy[0] != L'\0';
  if (manual) {
    proxy->assign(config.lpszProxy);
    if (config.lpszProxyBypass != nullptr) bypass->assign(config.lpszProxyBypass);
  }
  if (config.lpszProxy != nullptr) ::GlobalFree(config.lpszProxy);
  if (config.lpszProxyBypass != nullptr) ::GlobalFree(config.lpszProxyBypass);
  if (config.lpszAutoConfigUrl != nullptr) ::GlobalFree(config.lpszAutoConfigUrl);
  return manual;
}

// 解析 proxy 字段。失败时 error 填可读原因并返回 false。
bool resolveProxy(const std::string& setting, DWORD* access, std::wstring* proxyName,
                  std::wstring* bypass, std::string* error) {
  *access = WINHTTP_ACCESS_TYPE_NO_PROXY;
  proxyName->clear();
  bypass->clear();
  if (setting.empty()) {
    if (systemProxy(proxyName, bypass)) *access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    return true;
  }
  std::wstring value = utf8(setting);
  for (wchar_t& c : value) c = static_cast<wchar_t>(towlower(c));
  if (value == L"direct") return true;
  const bool hasScheme = value.rfind(L"http://", 0) == 0 || value.rfind(L"https://", 0) == 0;
  if (hasScheme || value.find(L':') != std::wstring::npos) {
    if (hasScheme) value.erase(0, value.find(L"://") + 3);
    while (!value.empty() && value.back() == L'/') value.pop_back();
    if (value.find(L':') == std::wstring::npos) value += L":80";
    *proxyName = value;
    *bypass = L"<local>";
    *access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    return true;
  }
  *error = "代理地址格式不对：留空用系统代理，填 direct 直连，或填 http://主机:端口。";
  return false;
}

// 同步 POST。传输层失败时 error 形如 "winhttp <错误码>"。
bool httpPostJson(const TranslateConfig& config, const std::string& url,
                  const std::string& apiKey, const std::string& body, int* status,
                  std::string* responseBody, std::string* error) {
  const std::wstring wideUrl = utf8(url);
  URL_COMPONENTS comps = {};
  comps.dwStructSize = sizeof(comps);
  comps.dwSchemeLength = static_cast<DWORD>(-1);
  comps.dwHostNameLength = static_cast<DWORD>(-1);
  comps.dwUrlPathLength = static_cast<DWORD>(-1);
  comps.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0,
                         &comps)) {
    *error = "接口地址不是合法的 URL（应以 http:// 或 https:// 开头）。";
    return false;
  }
  DWORD access = WINHTTP_ACCESS_TYPE_NO_PROXY;
  std::wstring proxyName;
  std::wstring bypass;
  if (!resolveProxy(config.proxy, &access, &proxyName, &bypass, error)) return false;

  HINTERNET session = ::WinHttpOpen(
      L"MoqiPanel/1.0", access,
      access == WINHTTP_ACCESS_TYPE_NAMED_PROXY ? proxyName.c_str() : WINHTTP_NO_PROXY_NAME,
      access == WINHTTP_ACCESS_TYPE_NAMED_PROXY ? bypass.c_str() : WINHTTP_NO_PROXY_BYPASS,
      0);
  if (session == nullptr) {
    *error = "winhttp " + std::to_string(::GetLastError());
    return false;
  }
  // 解析 / 连接 / 发送 / 接收。接收给得宽：推理模型可能先想一会儿。
  ::WinHttpSetTimeouts(session, 10000, 10000, 30000, 90000);
  const std::wstring host(comps.lpszHostName, comps.dwHostNameLength);
  std::wstring path(comps.lpszUrlPath, comps.dwUrlPathLength);
  if (comps.dwExtraInfoLength > 0) {
    path.append(comps.lpszExtraInfo, comps.dwExtraInfoLength);
  }
  const bool https = comps.nScheme == INTERNET_SCHEME_HTTPS;
  HINTERNET connect = ::WinHttpConnect(session, host.c_str(), comps.nPort, 0);
  HINTERNET request = nullptr;
  if (connect != nullptr) {
    request = ::WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   https ? WINHTTP_FLAG_SECURE : 0);
  }
  bool ok = request != nullptr;
  if (ok) {
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!apiKey.empty()) {
      headers += L"Authorization: Bearer " + utf8(apiKey) + L"\r\n";
    }
    ok = ::WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                              const_cast<char*>(body.data()),
                              static_cast<DWORD>(body.size()),
                              static_cast<DWORD>(body.size()), 0) != FALSE;
    if (ok) ok = ::WinHttpReceiveResponse(request, nullptr) != FALSE;
  }
  if (!ok) {
    *error = "winhttp " + std::to_string(::GetLastError());
  } else {
    DWORD code = 0;
    DWORD size = sizeof(code);
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                          WINHTTP_NO_HEADER_INDEX);
    *status = static_cast<int>(code);
    std::string received;
    DWORD available = 0;
    do {
      available = 0;
      if (!::WinHttpQueryDataAvailable(request, &available)) break;
      if (available == 0) break;
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (!::WinHttpReadData(request, &chunk[0], available, &read)) break;
      received.append(chunk.data(), read);
    } while (available > 0);
    *responseBody = std::move(received);
  }
  if (request != nullptr) ::WinHttpCloseHandle(request);
  if (connect != nullptr) ::WinHttpCloseHandle(connect);
  ::WinHttpCloseHandle(session);
  return ok;
}

std::string friendlyTransportError(const std::string& error) {
  const std::string prefix = "winhttp ";
  if (error.rfind(prefix, 0) != 0) return error;
  const int code = std::atoi(error.c_str() + prefix.size());
  switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
      return "Connection to the translation API timed out.";
    case ERROR_WINHTTP_CANNOT_CONNECT:
      return "Cannot reach the translation API server - check the network or the proxy setting in translate.json.";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
      return "DNS lookup for the translation API failed - check the network or the proxy setting in translate.json.";
    case ERROR_WINHTTP_SECURE_FAILURE:
      return "TLS handshake failed (proxy or certificate problem).";
    default:
      return "Request to the translation API failed (WinHTTP error " + std::to_string(code) + ").";
  }
}

std::string friendlyHttpError(int status, const std::string& body) {
  const std::string detail = describeHttpError(status, body);
  switch (status) {
    case 401:
    case 403:
      return "Invalid key or no permission (" + detail + ")";
    case 404:
      return "Wrong endpoint: /chat/completions not found (" + detail + ")";
    case 429:
      return "Rate limited or quota exhausted (" + detail + ")";
    default:
      return "The translation API returned an error: " + detail;
  }
}

} // namespace

TranslateOutcome translateText(const TranslateConfig& config,
                               const std::string& utf8Text) {
  TranslateOutcome outcome;
  const std::string url = chatCompletionsUrl(config.baseUrl);
  const std::string body = buildTranslateRequest(config, utf8Text);
  int status = 0;
  std::string response;
  std::string error;
  if (!httpPostJson(config, url, config.apiKey, body, &status, &response, &error)) {
    outcome.error = friendlyTransportError(error);
    return outcome;
  }
  if (status < 200 || status >= 300) {
    outcome.error = friendlyHttpError(status, response);
    return outcome;
  }
  std::string text;
  if (!parseChatText(response, &text, &error)) {
    outcome.error = error;
    return outcome;
  }
  outcome.ok = true;
  outcome.translation = std::move(text);
  return outcome;
}

} // namespace MoqiPanel
