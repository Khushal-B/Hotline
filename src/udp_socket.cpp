#include "hotline/udp_socket.hpp"

#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace hotline {

#ifdef _WIN32
namespace {
struct WsaInit {
  WsaInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
  ~WsaInit() { WSACleanup(); }
};
const WsaInit g_wsa;
}  // namespace
#endif

UdpSocket::UdpSocket() : fd_(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {}

UdpSocket::~UdpSocket() {
#ifdef _WIN32
  if (fd_ != INVALID_SOCKET) ::closesocket(fd_);
#else
  if (fd_ >= 0) ::close(fd_);
#endif
}

bool UdpSocket::bind(std::uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(port);
  return ::bind(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
}

bool UdpSocket::set_recv_timeout_us(int us) {
#ifdef _WIN32
  DWORD ms = static_cast<DWORD>(us / 1000);
  if (ms == 0) ms = 1;
  return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms) == 0;
#else
  timeval tv{};
  tv.tv_sec = us / 1000000;
  tv.tv_usec = us % 1000000;
  return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0;
#endif
}

int UdpSocket::send_to(const void* data, std::size_t len, const sockaddr_in& to) noexcept {
  return static_cast<int>(::sendto(fd_, static_cast<const char*>(data), static_cast<int>(len), 0,
                                   reinterpret_cast<const sockaddr*>(&to), sizeof to));
}

int UdpSocket::recv_from(void* buf, std::size_t cap, sockaddr_in* from) noexcept {
  sockaddr_in tmp{};
#ifdef _WIN32
  int fl = sizeof tmp;
#else
  socklen_t fl = sizeof tmp;
#endif
  const int n = static_cast<int>(::recvfrom(fd_, static_cast<char*>(buf), static_cast<int>(cap), 0,
                                            reinterpret_cast<sockaddr*>(&tmp), &fl));
  if (n >= 0) {
    if (from) *from = tmp;
    return n;
  }
#ifdef _WIN32
  const int e = WSAGetLastError();
  if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK || e == WSAECONNRESET) return 0;
#else
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
#endif
  return -1;
}

bool UdpSocket::resolve(const std::string& host, std::uint16_t port, sockaddr_in& out) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
  std::memcpy(&out, res->ai_addr, sizeof out);
  out.sin_port = htons(port);
  freeaddrinfo(res);
  return true;
}

}  // namespace hotline
