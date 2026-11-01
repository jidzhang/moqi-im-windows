#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace Moqi {
namespace Proto {

class FrameBuffer {
public:
  void append(const char *data, size_t len) { buffer_.append(data, len); }

  bool nextFrame(std::string &payload) {
    if (buffer_.size() < sizeof(std::uint32_t)) {
      return false;
    }

    std::uint32_t payloadSize = 0;
    std::memcpy(&payloadSize, buffer_.data(), sizeof(payloadSize));
    if (buffer_.size() < sizeof(payloadSize) + payloadSize) {
      return false;
    }

    payload.assign(buffer_.data() + sizeof(payloadSize), payloadSize);
    buffer_.erase(0, sizeof(payloadSize) + payloadSize);
    return true;
  }

  void clear() { buffer_.clear(); }

private:
  std::string buffer_;
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
