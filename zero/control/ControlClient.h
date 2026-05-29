#pragma once

#include <functional>
#include <string>

namespace zero {

// Minimal non-blocking line client for the ClashRig regression orchestrator. Connects out to a
// localhost TCP control server, sends newline-terminated lines, and invokes on_line for each
// complete inbound line. Poll() is called once per bot tick from RegressionZoneController, so
// on_line runs on the mainloop thread (no cross-thread concerns). Tab-delimited text protocol
// (see ClashRig's ControlProtocol) so no JSON dependency is needed.
struct ControlClient {
  bool connected = false;
  std::function<void(const std::string&)> on_line;

  // Blocking connect to host:port, then switches the socket to non-blocking. Returns success.
  bool Connect(const char* host, int port);

  // Queues a line (a '\n' is appended) to be flushed on the next Poll().
  void SendLine(const std::string& line);

  // Flushes queued output and drains available input, dispatching complete lines to on_line.
  void Poll();

  void Close();
  ~ControlClient();

 private:
  long long sock_ = -1;  // native socket handle (-1 = invalid on both platforms)
  std::string inbuf_;
  std::string outbuf_;
};

}  // namespace zero
