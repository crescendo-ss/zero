#include <zero/control/ControlClient.h>

#include <zero/game/Logger.h>

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using socket_t = int;
static const socket_t kInvalidSocket = -1;
#define closesocket ::close
#endif

namespace zero {

static bool WouldBlock() {
#ifdef _WIN32
  return WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

#ifdef _WIN32
static bool EnsureWinsock() {
  static bool initialized = false;
  if (!initialized) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    initialized = true;
  }
  return true;
}
#endif

bool ControlClient::Connect(const char* host, int port) {
#ifdef _WIN32
  if (!EnsureWinsock()) return false;
#endif

  socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocket) return false;

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    closesocket(s);
    return false;
  }

  if (::connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
    Log(LogLevel::Warning, "ControlClient: connect to %s:%d failed.", host, port);
    closesocket(s);
    return false;
  }

  // Switch to non-blocking so Poll() never stalls the bot tick.
#ifdef _WIN32
  u_long nonblocking = 1;
  ioctlsocket(s, FIONBIO, &nonblocking);
#else
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

  sock_ = (long long)s;
  connected = true;
  return true;
}

void ControlClient::SendLine(const std::string& line) {
  if (!connected) return;
  outbuf_ += line;
  outbuf_ += '\n';
}

void ControlClient::Poll() {
  if (!connected) return;
  socket_t s = (socket_t)sock_;

  // Flush queued output.
  if (!outbuf_.empty()) {
    int sent = (int)::send(s, outbuf_.data(), (int)outbuf_.size(), 0);
    if (sent > 0) {
      outbuf_.erase(0, sent);
    } else if (sent < 0 && !WouldBlock()) {
      Close();
      return;
    }
  }

  // Drain available input.
  char buffer[1024];
  for (;;) {
    int received = (int)::recv(s, buffer, sizeof(buffer), 0);
    if (received > 0) {
      inbuf_.append(buffer, received);
      continue;
    }
    if (received == 0) {  // peer closed
      Close();
      return;
    }
    if (WouldBlock()) break;  // no more data right now
    Close();
    return;
  }

  // Dispatch complete lines.
  size_t newline;
  while ((newline = inbuf_.find('\n')) != std::string::npos) {
    std::string line = inbuf_.substr(0, newline);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    inbuf_.erase(0, newline + 1);
    if (on_line) on_line(line);
  }
}

void ControlClient::Close() {
  if (sock_ != -1) {
    closesocket((socket_t)sock_);
    sock_ = -1;
  }
  connected = false;
}

ControlClient::~ControlClient() {
  Close();
}

}  // namespace zero
