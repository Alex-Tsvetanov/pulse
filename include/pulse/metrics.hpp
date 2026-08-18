// The measured values the server publishes, and the code that reads them from the
// operating system. Every field is a real reading or a counter the server increments,
// so nothing in the dashboard is synthetic.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pulse::metrics {

// Fixed bucket boundaries in microseconds. Fixed bounds keep the histogram addition to
// a compare and an increment, which matters because it runs on every served request.
class Histogram {
 public:
  static constexpr std::size_t kBucketCount = 9;
  static const std::array<double, kBucketCount - 1>& bounds();

  void add(double microseconds);
  // Linear interpolation inside the bucket that holds the requested rank. The last
  // bucket is open ended, so a quantile that falls in it reports the observed maximum.
  double quantile(double q) const;

  std::uint64_t count() const { return count_; }
  double mean() const { return count_ == 0 ? 0.0 : sum_ / static_cast<double>(count_); }
  double max() const { return max_; }
  const std::array<std::uint64_t, kBucketCount>& buckets() const { return buckets_; }

 private:
  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t count_ = 0;
  double sum_ = 0.0;
  double max_ = 0.0;
};

struct Snapshot {
  std::uint64_t sequence = 0;
  std::int64_t wall_microseconds = 0;  // Unix epoch, the client subtracts it for latency
  double uptime_seconds = 0.0;
  double cpu_percent = 0.0;      // of one core, over the interval since the last sample
  std::uint64_t rss_bytes = 0;   // resident set size of this process
  std::uint64_t requests_total = 0;
  double requests_per_second = 0.0;
  std::uint64_t bytes_sent = 0;
  std::uint32_t websocket_clients = 0;
  std::uint32_t sse_clients = 0;
  double latency_p50 = 0.0;  // microseconds
  double latency_p90 = 0.0;
  double latency_p99 = 0.0;
  Histogram histogram;
};

// Owned by the server loop and only ever touched from it, which is why it carries no
// lock. Adding one would be dead weight in a single threaded reactor.
class Collector {
 public:
  Collector();

  void record_request(double latency_microseconds);
  void record_bytes_sent(std::uint64_t bytes);
  void set_client_counts(std::uint32_t websocket_clients, std::uint32_t sse_clients);

  // Reads the operating system counters and returns the current picture. Calling it
  // advances the rate windows, so it is meant to run once per publish interval.
  Snapshot sample();

 private:
  std::uint64_t sequence_ = 0;
  std::uint64_t requests_total_ = 0;
  std::uint64_t bytes_sent_ = 0;
  std::uint32_t websocket_clients_ = 0;
  std::uint32_t sse_clients_ = 0;
  Histogram histogram_;

  std::int64_t start_wall_ = 0;
  std::int64_t last_wall_ = 0;
  double last_cpu_seconds_ = 0.0;
  std::uint64_t last_requests_total_ = 0;
};

// Unix epoch microseconds. Used for the timestamp that travels to the browser, because
// the browser has no access to a steady clock shared with the server.
std::int64_t wall_microseconds();

// Monotonic microseconds since an arbitrary origin. Used for every duration the server
// measures itself, because a wall clock can step backwards.
std::int64_t steady_microseconds();

// CPU time consumed by this process, user plus kernel, in seconds.
double process_cpu_seconds();

// Resident set size in bytes, or zero when the platform has no supported reading.
std::uint64_t resident_bytes();

}  // namespace pulse::metrics
