// Power Governor - versioned, deterministic binary persistence.
//
// The on-disk container carries an explicit schema version, a bounded payload length, and a CRC-32.
// Corruption, truncation, trailing garbage, and unsupported schema versions are all rejected. The
// record bodies are fixed-point little-endian integers (no NaN/Inf can exist) and the higher-level
// decoder additionally rejects duplicate ids, invalid enums/generations, and impossible budget
// hierarchies. Writes are atomic: the bytes are flushed to a temporary file and then swapped in.
#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "util.hpp"

#ifdef _WIN32
#  define PG_WINDOWS 1
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace pg {

constexpr std::uint32_t PERSIST_MAGIC = 0x53504750u;  // 'PGPS' as 32-bit LE
constexpr std::uint32_t PERSIST_SCHEMA = 1;
constexpr std::size_t PERSIST_MAX = 256u * 1024u * 1024u;  // 256 MiB

// ---- Generic little-endian binary writer --------------------------------
class BinWriter {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); u8(static_cast<std::uint8_t>(v >> 8)); }
  void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (i * 8))); }
  void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (i * 8))); }
  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void bool_(bool b) { u8(b ? 1 : 0); }
  void str(std::string_view s) {
    if (s.size() > std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("pg persist: string too long");
    u32(static_cast<std::uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }
  void f64(double v) { i64(static_cast<std::int64_t>(v * 1000.0)); }
  const std::vector<std::uint8_t>& buffer() const noexcept { return buf_; }
  std::size_t size() const noexcept { return buf_.size(); }
  std::vector<std::uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<std::uint8_t> buf_;
};

class BinReader {
 public:
  BinReader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
  explicit BinReader(const std::vector<std::uint8_t>& v) : p_(v.data()), n_(v.size()) {}

  bool u8(std::uint8_t& v) { if (pos_ + 1 <= n_) { v = p_[pos_++]; return true; } return false; }
  bool u16(std::uint16_t& v) { std::uint8_t a, b; if (u8(a) && u8(b)) { v = static_cast<std::uint16_t>(a | (b << 8)); return true; } return false; }
  bool u32(std::uint32_t& v) { std::uint32_t r = 0; for (int i = 0; i < 4; ++i) { std::uint8_t b; if (!u8(b)) return false; r |= static_cast<std::uint32_t>(b) << (i * 8); } v = r; return true; }
  bool u64(std::uint64_t& v) { std::uint64_t r = 0; for (int i = 0; i < 8; ++i) { std::uint8_t b; if (!u8(b)) return false; r |= static_cast<std::uint64_t>(b) << (i * 8); } v = r; return true; }
  bool i32(std::int32_t& v) { std::uint32_t u; if (!u32(u)) return false; v = static_cast<std::int32_t>(u); return true; }
  bool i64(std::int64_t& v) { std::uint64_t u; if (!u64(u)) return false; v = static_cast<std::int64_t>(u); return true; }
  bool bool_(bool& b) { std::uint8_t v; if (!u8(v)) return false; b = v != 0; return true; }
  bool str(std::string& s) {
    std::uint32_t len; if (!u32(len)) return false;
    if (len > n_ - pos_) return false;
    s.assign(reinterpret_cast<const char*>(p_ + pos_), len);
    pos_ += len; return true;
  }
  bool f64(double& v) { std::int64_t x; if (!i64(x)) return false; v = static_cast<double>(x) / 1000.0; return true; }
  bool bytes(std::vector<std::uint8_t>& b) {
    std::uint32_t len; if (!u32(len)) return false;
    if (len > n_ - pos_) return false;
    b.assign(p_ + pos_, p_ + pos_ + len);
    pos_ += len; return true;
  }
  std::size_t remaining() const noexcept { return n_ - pos_; }
  bool at_end() const noexcept { return pos_ == n_; }

 private:
  const std::uint8_t* p_;
  std::size_t n_;
  std::size_t pos_ = 0;
};

// ---- Envelope encode / decode --------------------------------------------
struct PersistEnvelope {
  std::uint32_t schema = PERSIST_SCHEMA;
  std::vector<std::uint8_t> blob;
};

inline std::vector<std::uint8_t> encode_envelope(const PersistEnvelope& e) {
  if (e.blob.size() > PERSIST_MAX) throw std::length_error("pg persist: blob exceeds PERSIST_MAX");
  BinWriter w;
  w.u32(PERSIST_MAGIC);
  w.u32(e.schema);
  w.u32(static_cast<std::uint32_t>(e.blob.size()));
  std::vector<std::uint8_t> out = w.buffer();
  out.insert(out.end(), e.blob.begin(), e.blob.end());
  const std::uint32_t crc = crc32(out.data(), out.size());
  std::uint8_t tmp[4]; std::memcpy(tmp, &crc, 4);
  for (int i = 0; i < 4; ++i) out.push_back(tmp[i]);
  return out;
}

struct EnvelopeResult {
  bool ok = false;
  PersistEnvelope envelope;
  std::string error;
};

inline EnvelopeResult decode_envelope(const std::uint8_t* data, std::size_t n) {
  EnvelopeResult r;
  static constexpr std::size_t H = 4 + 4 + 4;  // magic + schema + len
  if (n < H + 4) { r.error = "truncated persistence record"; return r; }
  BinReader br(data, n);
  std::uint32_t magic, schema, len;
  if (!br.u32(magic) || !br.u32(schema) || !br.u32(len)) { r.error = "malformed header"; return r; }
  if (magic != PERSIST_MAGIC) { r.error = "bad persistence magic"; return r; }
  if (schema != PERSIST_SCHEMA) { r.error = "unsupported schema version"; return r; }
  if (len > PERSIST_MAX) { r.error = "oversized persistence record"; return r; }
  const std::size_t total = H + static_cast<std::size_t>(len) + 4;
  if (n < total) { r.error = "truncated persistence payload"; return r; }
  const std::uint32_t stored = *reinterpret_cast<const std::uint32_t*>(data + H + len);
  const std::uint32_t calc = crc32(data, H + len);
  if (stored != calc) { r.error = "crc mismatch"; return r; }
  if (n != total) { r.error = "trailing garbage after persistence record"; return r; }
  r.ok = true;
  r.envelope.schema = schema;
  r.envelope.blob.assign(data + H, data + H + len);
  return r;
}

// ---- Atomic durable file write / read ------------------------------------
inline bool persist_write(const std::string& path, const std::vector<std::uint8_t>& data, std::string& err) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { err = "open tmp failed"; return false; }
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) { err = "write failed"; return false; }
  }
  bool ok = false;
#ifdef PG_WINDOWS
  if (MoveFileExA(tmp.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) ok = true;
#else
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (!ec) ok = true;
#endif
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    err = "atomic replace failed";
  }
  return ok;
}

inline bool persist_read(const std::string& path, std::vector<std::uint8_t>& data, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "open failed"; return false; }
  f.seekg(0, std::ios::end);
  const auto sz = f.tellg();
  if (sz < 0) { err = "seek failed"; return false; }
  data.resize(static_cast<std::size_t>(sz));
  f.seekg(0, std::ios::beg);
  if (!data.empty()) f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!f) { err = "read short"; return false; }
  return true;
}

}  // namespace pg
