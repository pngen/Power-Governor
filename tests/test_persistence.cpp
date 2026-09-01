#include "test_framework.hpp"
#include "powergovernor/persistence.hpp"
#include <cstring>
using namespace pg;

TEST(persistence_envelope_roundtrip) {
  PersistEnvelope e; e.blob = {1,2,3,4,5};
  auto b = encode_envelope(e);
  auto r = decode_envelope(b.data(), b.size());
  CHECK(r.ok && r.envelope.blob.size() == 5 && r.envelope.schema == PERSIST_SCHEMA);
}

TEST(persistence_rejects_corruption) {
  PersistEnvelope e; e.blob = {1,2,3,4,5};
  auto b = encode_envelope(e); b[10] ^= 0xFF;
  auto r = decode_envelope(b.data(), b.size());
  CHECK(!r.ok);
}

TEST(persistence_rejects_truncation) {
  PersistEnvelope e; e.blob = {1,2,3,4,5};
  auto b = encode_envelope(e); b.resize(b.size()-2);
  auto r = decode_envelope(b.data(), b.size());
  CHECK(!r.ok);
}

TEST(persistence_rejects_trailing_garbage) {
  PersistEnvelope e; e.blob = {1,2,3,4,5};
  auto b = encode_envelope(e); b.insert(b.end(), 0x00);
  auto r = decode_envelope(b.data(), b.size());
  CHECK(!r.ok);
}

TEST(persistence_rejects_schema_mismatch) {
  PersistEnvelope e; e.blob = {1,2,3,4,5};
  auto b = encode_envelope(e);
  b[4]=0xFF; b[5]=0xFF; b[6]=0xFF; b[7]=0xFF;
  auto r = decode_envelope(b.data(), b.size());
  CHECK(!r.ok);
}

TEST(persistence_absent_file_read_fails) {
  std::vector<std::uint8_t> data; std::string err;
  CHECK(!persist_read("no_such_file_xyz.bin", data, err));
}

TEST(bin_writer_reader_roundtrip) {
  BinWriter w;
  w.u32(0xDEADBEEF); w.u64(0x1234567890ABCDEFLL); w.i32(-42); w.bool_(true); w.str("hello");
  auto buf = w.take();
  BinReader r(buf);
  std::uint32_t a = 0; std::uint64_t b2 = 0; std::int32_t c = 0; bool d = false; std::string e;
  CHECK(r.u32(a) && r.u64(b2) && r.i32(c) && r.bool_(d) && r.str(e));
  CHECK(a == 0xDEADBEEF && b2 == 0x1234567890ABCDEFULL && c == -42 && d && e == "hello");
  CHECK(r.at_end());
}

TEST(bin_reader_rejects_truncated) {
  BinWriter w; w.u64(123); w.str("hello");
  auto buf = w.take();
  BinReader r(buf);
  std::uint64_t a; std::string e;
  CHECK(r.u64(a));
  // truncated string read should fail
  std::vector<std::uint8_t> trunc(buf.begin(), buf.begin()+8);
  BinReader r2(trunc);
  CHECK(!r2.str(e));
}

