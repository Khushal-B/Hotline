#pragma once
// Thin IPv4 UDP socket wrapper (POSIX + Winsock).
#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

namespace hotline {

class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  bool bind(std::uint16_t port);
  bool set_recv_timeout_us(int us);
  int send_to(const void* data, std::size_t len, const sockaddr_in& to) noexcept;
  // Returns bytes received, 0 on timeout, -1 on error.
  int recv_from(void* buf, std::size_t cap, sockaddr_in* from) noexcept;
  static bool resolve(const std::string& host, std::uint16_t port, sockaddr_in& out);

 private:
#ifdef _WIN32
  SOCKET fd_;
#else
  int fd_;
#endif
};

}  // namespace hotline
