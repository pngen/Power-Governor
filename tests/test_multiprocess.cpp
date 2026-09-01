// Power Governor - multiprocess distributed proof over framed TCP.
#include "test_framework.hpp"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
std::string self_dir() {
  char buf[MAX_PATH]; GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string p(buf); auto c = p.find_last_of('\\');
  return c == std::string::npos ? "." : p.substr(0, c);
}
struct Proc {
  HANDLE h = INVALID_HANDLE_VALUE; HANDLE in = INVALID_HANDLE_VALUE; HANDLE out = INVALID_HANDLE_VALUE;
};
Proc spawn(const std::string& exe, const std::string& args) {
  Proc p;
  SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
  HANDLE cir, ciw, cor, cow;
  if (!CreatePipe(&cir, &ciw, &sa, 0)) return p;
  if (!CreatePipe(&cor, &cow, &sa, 0)) { CloseHandle(cir); CloseHandle(ciw); return p; }
  SetHandleInformation(ciw, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(cor, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = cir; si.hStdOutput = cow; si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  std::string cmdline = "\"" + exe + "\" " + args;
  std::vector<char> cmd(cmdline.begin(), cmdline.end()); cmd.push_back('\0');
  if (!CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(cir); CloseHandle(ciw); CloseHandle(cor); CloseHandle(cow); return p;
  }
  CloseHandle(cir); CloseHandle(cow);
  p.h = pi.hProcess; p.in = ciw; p.out = cor; CloseHandle(pi.hThread);
  return p;
}
void send_stdin(Proc& p, const std::string& s) { DWORD w; WriteFile(p.in, s.data(), (DWORD)s.size(), &w, nullptr); }
void close_stdin(Proc& p) { if (p.in != INVALID_HANDLE_VALUE) { CloseHandle(p.in); p.in = INVALID_HANDLE_VALUE; } }
void close_stdout(Proc& p) { if (p.out != INVALID_HANDLE_VALUE) { CloseHandle(p.out); p.out = INVALID_HANDLE_VALUE; } }
// Blocking single-line read (returns empty on EOF).
std::string read_line(Proc& p) {
  std::string line; char c; DWORD got = 0;
  while (ReadFile(p.out, &c, 1, &got, nullptr) && got > 0) {
    line.push_back(c);
    if (c == '\n') break;
  }
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  return line;
}
void kill(Proc& p) { if (p.h != INVALID_HANDLE_VALUE) { TerminateProcess(p.h, 1); CloseHandle(p.h); p.h = INVALID_HANDLE_VALUE; } }
}  // namespace

TEST(distributed_proof_stale_authority_rejected) {
  const std::string cli = self_dir() + "\\powergovernor.exe";
  const std::uint16_t port = 25117;

  Proc coord = spawn(cli, "run-coordinator --port " + std::to_string(port));
  CHECK(coord.h != INVALID_HANDLE_VALUE);
  CHECK(read_line(coord).find("READY") != std::string::npos);

  // 1. Start and register worker A and worker B.
  Proc a = spawn(cli, "run-worker --coordinator 127.0.0.1:" + std::to_string(port) + " --worker 1 --boot 1 --node 1");
  Proc b = spawn(cli, "run-worker --coordinator 127.0.0.1:" + std::to_string(port) + " --worker 2 --boot 2 --node 1");
  CHECK(a.h != INVALID_HANDLE_VALUE); CHECK(b.h != INVALID_HANDLE_VALUE);

    CHECK(read_line(a).find("REGISTER ok") != std::string::npos);
  CHECK(read_line(b).find("REGISTER ok") != std::string::npos);

  // 2. Publish observations (allows registration/observe handshake to settle).
  send_stdin(a, "observe 150000 4000\n");
  send_stdin(b, "observe 120000 3800\n");
  CHECK(read_line(a).find("observe -> ACCEPT") != std::string::npos);
  CHECK(read_line(b).find("observe -> ACCEPT") != std::string::npos);

  // 3. Install authoritative fleet/node/device budgets.
  send_stdin(coord, "install_budget 1 0 450 450\n");
  send_stdin(coord, "install_budget 2 1 402 402\n");
  send_stdin(coord, "set_policy 402\n");
  CHECK(read_line(coord).find("BUDGET") != std::string::npos);
  CHECK(read_line(coord).find("BUDGET") != std::string::npos);
  CHECK(read_line(coord).find("POLICY") != std::string::npos);

  // 4. Reserve power on both workers.
  send_stdin(a, "reserve 2 60000\n");
  send_stdin(b, "reserve 2 50000\n");
  CHECK(read_line(a).find("reserve -> ACCEPT") != std::string::npos);
  CHECK(read_line(b).find("reserve -> ACCEPT") != std::string::npos);
  send_stdin(b, "quit\n"); close_stdin(b);

  // 5. Kill worker A as a real OS process.
  kill(a);

  // 6. Roll coordinator epoch.
  send_stdin(coord, "roll_epoch\n");
  CHECK(read_line(coord).find("EPOCH") != std::string::npos);

  // 7. Restart worker A with a fresh boot; replay stale authority (old epoch=1, boot=1).
  Proc a2 = spawn(cli, "run-worker --coordinator 127.0.0.1:" + std::to_string(port) + " --worker 1 --boot 99 --node 1");
  CHECK(a2.h != INVALID_HANDLE_VALUE);
  CHECK(read_line(a2).find("REGISTER ok") != std::string::npos);
  send_stdin(a2, "stale_observe 1 1 90000\nstale_reserve 1 1 2 40000\nstale_release 1 1 1 2 60000\nquit\n");
  close_stdin(a2);
  CHECK(read_line(a2).find("STALE_EPOCH") != std::string::npos);
  CHECK(read_line(a2).find("STALE_EPOCH") != std::string::npos);
  CHECK(read_line(a2).find("STALE_EPOCH") != std::string::npos);

  // 8. Worker B remains valid: a fresh observation under the rolled epoch is accepted.
  Proc b2 = spawn(cli, "run-worker --coordinator 127.0.0.1:" + std::to_string(port) + " --worker 2 --boot 2 --node 1");
  CHECK(b2.h != INVALID_HANDLE_VALUE);
  CHECK(read_line(b2).find("REGISTER ok") != std::string::npos);
  send_stdin(b2, "observe 110000 3700\nquit\n");
  close_stdin(b2);
  CHECK(read_line(b2).find("observe -> ACCEPT") != std::string::npos);

  // 9. Persist and capture a stable state digest.
  send_stdin(coord, "snapshot\n");
  CHECK(read_line(coord).find("SNAPSHOT") != std::string::npos);
  send_stdin(coord, "save mp_state.bin\n");
  CHECK(read_line(coord).find("SAVED") != std::string::npos);
  send_stdin(coord, "quit\n");
  close_stdin(coord);
  kill(b2);
}
