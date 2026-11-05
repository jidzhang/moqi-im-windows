#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace Moqi {
namespace Proto {

class FrameBuffer {
public:
  // 单帧上限 16MB：JSON 响应最大的是 onMenu 全菜单（~15KB），AI 推送也远
  // 小于此值。损坏/恶意长度前缀（例如 4GB）以前会让 nextFrame 永远返回
  // false、缓冲区无限增长直到进程被 OOM 杀掉——2026-11-05 代码通读发现。
  static constexpr std::uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

  void append(const char *data, size_t len) { buffer_.append(data, len); }

  bool nextFrame(std::string &payload) {
    if (buffer_.size() < sizeof(std::uint32_t)) {
      return false;
    }

    std::uint32_t payloadSize = 0;
    std::memcpy(&payloadSize, buffer_.data(), sizeof(payloadSize));
    if (payloadSize > kMaxFrameBytes) {
      // 流已失步，不可能再恢复：置位 overflow 让上层断连重连（协议语义=
      // 单连接内帧序不可信时必须重来，不能靠猜边界）。
      overflow_ = true;
      return false;
    }
    if (buffer_.size() < sizeof(payloadSize) + payloadSize) {
      return false;
    }

    payload.assign(buffer_.data() + sizeof(payloadSize), payloadSize);
    buffer_.erase(0, sizeof(payloadSize) + payloadSize);
    return true;
  }

  bool overflowed() const { return overflow_; }

  void clear() {
    buffer_.clear();
    overflow_ = false;
  }

private:
  std::string buffer_;
  bool overflow_ = false;
};

inline std::string framePayload(const std::string &payload) {
  std::string framed(sizeof(std::uint32_t), '\0');
  const auto payloadSize = static_cast<std::uint32_t>(payload.size());
  std::memcpy(&framed[0], &payloadSize, sizeof(payloadSize));
  framed.append(payload);
  return framed;
}

// The wire payload is JSON (protojson-compatible naming); framing stays a
// 4-byte little-endian length prefix. Kept header-only for pipe framing.
#include <json/json.h>
#include <memory>

inline bool serializeJsonMessage(const Json::Value &message,
                                 std::string &framedPayload) {
  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = "";
  framedPayload = framePayload(Json::writeString(builder, message));
  return true;
}

inline bool parseJsonPayload(const std::string &payload, Json::Value &message) {
  Json::CharReaderBuilder builder;
  Json::String errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  return reader->parse(payload.data(), payload.data() + payload.size(),
                       &message, &errors);
}

} // namespace Proto
} // namespace Moqi
