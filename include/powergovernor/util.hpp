// Power Governor - deterministic serialization utilities.
//
// A small, dependency-free JSON writer (fields appended in caller order, so output is always
// stable) and a FNV-1a digest used to fingerprint decisions and persisted state.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pg {

inline std::uint64_t fnv1a_64(std::string_view s) noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  for (char c : s) { h ^= static_cast<std::uint8_t>(c); h *= 1099511628211ULL; }
  return h;
}

inline std::string to_hex(std::uint64_t v) {
  static const char* hex = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) { out[static_cast<std::size_t>(i)] = hex[v & 0xF]; v >>= 4; }
  return out;
}

inline std::string digest_hex(std::string_view s) { return to_hex(fnv1a_64(s)); }

// CRC-32 (IEEE 802.3 polynomial).
inline std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t n) noexcept {
  return crc32_update(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
}
inline std::uint32_t crc32(std::string_view s) noexcept {
  return crc32(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

inline std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\""; break;
      case '\\': out += "\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

// Minimal append-style JSON object writer. Field order is exactly the order append() is called.
class JsonWriter {
 public:
  JsonWriter() = default;

  void begin_object() { open('{'); }
  void end_object() { close('}'); }
  void begin_array() { open('['); }
  void end_array() { close(']'); }

  // key is emitted when we are inside an object and expecting a key.
  template <class T>
  void key(const std::string& k, const T& value) {
    comma();
    append_string(k);
    buf_ += ':';
    append_value(value);
  }

  template <class T>
  void value(const T& v) {
    comma();
    append_value(v);
  }

  void raw_key(const std::string& k) {
    comma();
    append_string(k);
    buf_ += ':';
    value_pending_ = true;
  }

  void raw_value(const std::string& raw) {
    // raw already-JSON value, no escaping. After a raw_key it directly follows ':'.
    if (value_pending_) { buf_ += raw; value_pending_ = false; }
    else { comma(); buf_ += raw; }
  }

  void raw(const std::string& s) { buf_ += s; }

  const std::string& str() const noexcept { return buf_; }
  std::string take() { return std::move(buf_); }
  void clear() { buf_.clear(); }

 private:
  void comma() {
    if (!first_) buf_ += ',';
    first_ = false;
  }

  void open(char c) { buf_ += c; first_ = true; value_pending_ = false; }
  void close(char c) { buf_ += c; value_pending_ = false; }

  void append_string(const std::string& s) {
    buf_ += '"'; buf_ += json_escape(s); buf_ += '"';
  }

  template <class T>
  void append_value(const T& v) {
    if constexpr (std::is_same_v<std::decay_t<T>, std::string> ||
                  std::is_same_v<std::decay_t<T>, std::string_view>) {
      append_string(std::string(v));
    } else if constexpr (std::is_same_v<std::decay_t<T>, const char*>) {
      append_string(std::string(v));
    } else if constexpr (std::is_integral_v<std::decay_t<T>> && !std::is_same_v<std::decay_t<T>, bool>) {
      buf_ += std::to_string(v);
    } else if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
      buf_ += v ? "true" : "false";
    } else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
      char tmp[40];
      std::snprintf(tmp, sizeof(tmp), "%.9g", v);
      buf_ += tmp;
    } else if constexpr (std::is_same_v<std::decay_t<T>, const char* const>) {
      append_string(v);
    } else {
      // fallback: textual
      append_string(std::to_string(v));
    }
  }

  std::string buf_;
  bool first_ = true;
  bool value_pending_ = false;
};

}  // namespace pg
