// The server: one listening socket, one event loop, three transports.
//
// The loop is single threaded on purpose. Every piece of shared state (the connection
// table, the collector, the last snapshot) is touched from exactly one thread, so the
// project carries no locks and no lock ordering to reason about. The cost is that a
// slow handler would stall every client, which is acceptable because no handler here
// does anything but format a string that is already in memory.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pulse/metrics.hpp"

namespace pulse {

struct Options {
  std::uint16_t port = 8080;
  std::string web_root = "web";
  int interval_ms = 250;  // how often a snapshot is taken and pushed
  bool quiet = false;
};

class Server {
 public:
  explicit Server(Options options);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds and listens. Returns false and fills error when the port is unavailable.
  bool start(std::string& error);

  // Runs until stop() is called. Safe to call from a worker thread.
  void run();

  // Runs one pass of the loop with the given timeout. Exposed so a test can drive the
  // server without a second thread.
  void poll(int timeout_ms);

  void stop() { stopping_.store(true); }

  // The port actually bound, which differs from the requested one when port 0 was asked
  // for and the operating system chose a free one.
  std::uint16_t port() const;

  const metrics::Snapshot& last_snapshot() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
  std::atomic<bool> stopping_{false};
};

}  // namespace pulse
