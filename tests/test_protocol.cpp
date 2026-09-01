#include "test_framework.hpp"
#include "powergovernor/protocol.hpp"
#include <cstring>
using namespace pg;

TEST(protocol_roundtrip_register) {
  RegisterMsg m; m.worker = WorkerId::from_raw(7); m.boot = WorkerBootId::from_raw(9);
  m.node = NodeId::from_raw(11); m.name = "w0";
  Frame f{MsgType::REGISTER, encode_register(m)};
  auto r = decode_register(f);
  CHECK(r && r->worker.raw() == 7 && r->name == "w0");
}

TEST(protocol_roundtrip_power_observation) {
  PowerObsMsg m; m.worker = WorkerId::from_raw(1); m.boot = WorkerBootId::from_raw(2);
  m.epoch = CoordinatorEpoch::first(); m.accel = AcceleratorId::from_raw(3);
  m.device_gen = DeviceGeneration::from_raw(4); m.obs_gen = ObservationGeneration::from_raw(5);
  m.has_power = true; m.power_mw = 92000; m.has_temp = true; m.temp_centi = 3700;
  m.has_util = true; m.utilization_bp = 1300; m.provenance = 1; m.source = "fake"; m.timestamp_ns = 123;
  Frame f{MsgType::POWER_OBSERVATION, encode_power_obs(m)};
  auto r = decode_power_obs(f);
  CHECK(r && r->power_mw == 92000 && r->temp_centi == 3700 && r->utilization_bp == 1300 && r->epoch.raw() == 1);
}

TEST(protocol_rejects_invalid_thermal_enum) {
  ThermalObsMsg m; m.state = 9; m.worker = WorkerId::from_raw(1); m.boot = WorkerBootId::from_raw(2);
  m.epoch = CoordinatorEpoch::first(); m.accel = AcceleratorId::from_raw(3);
  m.therm_gen = ThermalGeneration::from_raw(4); m.obs_gen = ObservationGeneration::from_raw(5);
  Frame f{MsgType::THERMAL_OBSERVATION, encode_thermal_obs(m)};
  CHECK(!decode_thermal_obs(f));
}

TEST(protocol_rejects_bad_magic) {
  Frame f{MsgType::REGISTER, {1,2,3,4}};
  auto b = serialize_frame(f); b[0] = 0x00;
  auto fr = deserialize_frame(b.data(), b.size());
  CHECK(!fr.ok);
}

TEST(protocol_rejects_corrupt_crc) {
  Frame f{MsgType::REGISTER, {1,2,3,4}};
  auto b = serialize_frame(f); b[b.size()-1] ^= 0xFF;
  auto fr = deserialize_frame(b.data(), b.size());
  CHECK(!fr.ok);
}

TEST(protocol_rejects_unsupported_version) {
  Frame f{MsgType::REGISTER, {1,2,3,4}};
  auto b = serialize_frame(f); b[4] = 0x00; b[5] = 0x77;
  auto fr = deserialize_frame(b.data(), b.size());
  CHECK(!fr.ok);
}

TEST(protocol_rejects_oversized_frame) {
  // A 20 MiB frame exceeds MAX_FRAME and must be rejected.
  Frame f2{MsgType::REGISTER, std::vector<std::uint8_t>(20*1024*1024, 0)};
  CHECK_THROWS(serialize_frame(f2));
  // The deserializer also rejects an advertised length over the cap.
  std::vector<std::uint8_t> hdr(12, 0);
  hdr[0]=0x50; hdr[1]=0x47; hdr[2]=0x54; hdr[3]=0x43;  // magic
  hdr[6]=1;                                             // version
  hdr[8]=0xFF; hdr[9]=0xFF; hdr[10]=0xFF; hdr[11]=0x7F; // len = 0x7FFFFFFF
  auto fr = deserialize_frame(hdr.data(), hdr.size());
  CHECK(!fr.ok);
}

TEST(protocol_ack_roundtrip) {
  AckMsg m; m.message_id = 5; m.ok = true; m.message = "accepted";
  Frame f{MsgType::ACK, encode_ack(m)};
  auto r = decode_ack(f);
  CHECK(r && r->message_id == 5 && r->ok && r->message == "accepted");
}

TEST(protocol_trailing_garbage_parsed_first_frame) {
  Frame f{MsgType::REGISTER, {1,2,3,4}};
  auto b = serialize_frame(f); b.insert(b.end(), 0x00);
  auto fr = deserialize_frame(b.data(), b.size());
  CHECK(fr.ok && fr.consumed == b.size() - 1);
}

