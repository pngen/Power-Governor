// Power Governor - framed TCP transport (Windows Winsock2).
#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <vector>

#ifdef _WIN32
#  define PG_WINSOCK 1
#  define WIN32_LEAN_AND_MEAN
#  define _WINSOCK_DEPRECATED_NO_WARNINGS
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

#include "protocol.hpp"

namespace pg {
#ifdef PG_WINSOCK

inline bool pg_net_init() {
  static bool done = false;
  if (done) return true;
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
  done = true;
  return true;
}

inline bool sock_send_all(SOCKET s, const std::uint8_t* p, std::size_t n) {
  std::size_t off = 0;
  while (off < n) {
    const int sent = ::send(s, reinterpret_cast<const char*>(p + off), static_cast<int>(n - off), 0);
    if (sent <= 0) return false;
    off += static_cast<std::size_t>(sent);
  }
  return true;
}

inline bool sock_recv_exact(SOCKET s, std::uint8_t* p, std::size_t n) {
  std::size_t off = 0;
  while (off < n) {
    const int got = ::recv(s, reinterpret_cast<char*>(p + off), static_cast<int>(n - off), 0);
    if (got <= 0) return false;
    off += static_cast<std::size_t>(got);
  }
  return true;
}

inline bool send_frame(SOCKET s, const Frame& f) {
  std::vector<std::uint8_t> bytes = serialize_frame(f);
  return sock_send_all(s, bytes.data(), bytes.size());
}

// Read exactly one frame. Blocks until a full frame arrives or the peer closes.
inline bool recv_frame(SOCKET s, Frame& out) {
  std::array<std::uint8_t, HEADER_SIZE> header{};
  if (!sock_recv_exact(s, header.data(), header.size())) return false;
  std::uint32_t magic = static_cast<std::uint32_t>(header[0]) |
      (static_cast<std::uint32_t>(header[1]) << 8) |
      (static_cast<std::uint32_t>(header[2]) << 16) |
      (static_cast<std::uint32_t>(header[3]) << 24);
  if (magic != FRAME_MAGIC) return false;
  const std::uint16_t major = static_cast<std::uint16_t>(header[4] | (header[5] << 8));
  (void)major;
  const std::uint16_t type = static_cast<std::uint16_t>(header[6] | (header[7] << 8));
  (void)type;
  const std::uint32_t len = static_cast<std::uint32_t>(header[8]) |
      (static_cast<std::uint32_t>(header[9]) << 8) |
      (static_cast<std::uint32_t>(header[10]) << 16) |
      (static_cast<std::uint32_t>(header[11]) << 24);
  if (len > MAX_FRAME) return false;
  std::vector<std::uint8_t> body(static_cast<std::size_t>(len) + TRAILER_SIZE);
  if (!sock_recv_exact(s, body.data(), body.size())) return false;
  std::vector<std::uint8_t> full(header.begin(), header.end());
  full.insert(full.end(), body.begin(), body.end());
  FrameResult fr = deserialize_frame(full.data(), full.size());
  if (!fr.ok) return false;
  out = fr.frame;
  return true;
}

struct TcpListener {
  SOCKET sock = INVALID_SOCKET;
  bool listen(const std::string& host, std::uint16_t port) {
    if (!pg_net_init()) return false;
    sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;
    int one = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) { ::closesocket(sock); sock = INVALID_SOCKET; return false; }
    if (::listen(sock, 16) == SOCKET_ERROR) { ::closesocket(sock); sock = INVALID_SOCKET; return false; }
    return true;
  }
  bool accept(SOCKET& peer) { peer = ::accept(sock, nullptr, nullptr); return peer != INVALID_SOCKET; }
  void close() { if (sock != INVALID_SOCKET) { ::closesocket(sock); sock = INVALID_SOCKET; } }
};

inline SOCKET connect_socket(const std::string& host, std::uint16_t port) {
  if (!pg_net_init()) return INVALID_SOCKET;
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return INVALID_SOCKET;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = inet_addr(host.c_str());
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) { ::closesocket(s); return INVALID_SOCKET; }
  return s;
}

inline void close_socket(SOCKET s) { if (s != INVALID_SOCKET) ::closesocket(s); }

#endif  // PG_WINSOCK
}  // namespace pg
