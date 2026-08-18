// Thin portable wrapper over Winsock and BSD sockets.
//
// The rest of the project never sees a platform socket call. Everything that differs
// between Windows and the Unix family (the handle type, the invalid value, the error
// query, the close call, the one time library start-up) is resolved here once.
#pragma once

#ifdef _WIN32
// Must precede <winsock2.h>: the default of 64 is too small for the connection scaling
// experiment, and fd_set is sized by this macro at compile time.
#  ifndef FD_SETSIZE
#    define FD_SETSIZE 1024
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::net {

#ifdef _WIN32
using Handle = SOCKET;
inline const Handle kInvalid = INVALID_SOCKET;
#else
using Handle = int;
inline constexpr Handle kInvalid = -1;
#endif

// Winsock needs WSAStartup before any socket call and WSACleanup after the last one.
// On the Unix family both are no-ops. Construct one of these in main.
class Library {
 public:
  Library();
  ~Library();
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
};

// Owns a socket handle and closes it exactly once.
class Socket {
 public:
  Socket() = default;
  explicit Socket(Handle h) : h_(h) {}
  ~Socket() { close(); }

  Socket(Socket&& other) noexcept : h_(other.h_) { other.h_ = kInvalid; }
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      close();
      h_ = other.h_;
      other.h_ = kInvalid;
    }
    return *this;
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  Handle get() const { return h_; }
  bool valid() const { return h_ != kInvalid; }
  Handle release() {
    Handle h = h_;
    h_ = kInvalid;
    return h;
  }
  void close();

 private:
  Handle h_ = kInvalid;
};

// Binds to the loopback-reachable wildcard address and starts listening.
// Port 0 asks the operating system for a free port; read it back with local_port.
Socket listen_on(std::uint16_t port, std::string& error);
Socket connect_to(const std::string& host, std::uint16_t port, std::string& error);
Socket accept_on(Handle listener);

std::uint16_t local_port(Handle sock);
bool set_nonblocking(Handle sock, bool on);
bool set_nodelay(Handle sock);

// Return values follow recv/send: negative on error, zero on orderly shutdown for recv.
long recv_some(Handle sock, char* buffer, std::size_t size);
long send_some(Handle sock, const char* buffer, std::size_t size);

// Blocking convenience used by the measurement client, not by the server loop.
bool send_all(Handle sock, std::string_view data);

// True when the last error was "no data yet" rather than a real failure.
bool last_error_was_would_block();
int last_error();

}  // namespace pulse::net
