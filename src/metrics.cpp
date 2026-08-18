#include "pulse/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#  include <sys/time.h>
#  include <unistd.h>
#  include <cstdio>
#endif

namespace pulse::metrics {
namespace {

const std::array<double, Histogram::kBucketCount - 1> kBounds = {
    50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0};

}  // namespace

const std::array<double, Histogram::kBucketCount - 1>& Histogram::bounds() { return kBounds; }

void Histogram::add(double microseconds) {
  if (microseconds < 0.0) microseconds = 0.0;
  std::size_t index = kBucketCount - 1;
  for (std::size_t i = 0; i < kBounds.size(); ++i) {
    if (microseconds <= kBounds[i]) {
      index = i;
      break;
    }
  }
  ++buckets_[index];
  ++count_;
  sum_ += microseconds;
  max_ = std::max(max_, microseconds);
}

double Histogram::quantile(double q) const {
  if (count_ == 0) return 0.0;
  q = std::clamp(q, 0.0, 1.0);
  const double rank = q * static_cast<double>(count_);

  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    const std::uint64_t here = buckets_[i];
    if (here == 0) continue;
    if (static_cast<double>(seen + here) >= rank) {
      if (i == kBucketCount - 1) return max_;  // open ended bucket, report what was seen
      const double low = i == 0 ? 0.0 : kBounds[i - 1];
      const double high = kBounds[i];
      const double position = (rank - static_cast<double>(seen)) / static_cast<double>(here);
      return low + (high - low) * std::clamp(position, 0.0, 1.0);
    }
    seen += here;
  }
  return max_;
}

Collector::Collector() {
  start_wall_ = wall_microseconds();
  last_wall_ = start_wall_;
  last_cpu_seconds_ = process_cpu_seconds();
}

void Collector::record_request(double latency_microseconds) {
  ++requests_total_;
  histogram_.add(latency_microseconds);
}

void Collector::record_bytes_sent(std::uint64_t bytes) { bytes_sent_ += bytes; }

void Collector::set_client_counts(std::uint32_t websocket_clients, std::uint32_t sse_clients) {
  websocket_clients_ = websocket_clients;
  sse_clients_ = sse_clients;
}

Snapshot Collector::sample() {
  const std::int64_t now = wall_microseconds();
  const double elapsed = static_cast<double>(now - last_wall_) / 1e6;
  const double cpu_now = process_cpu_seconds();

  Snapshot snapshot;
  snapshot.sequence = ++sequence_;
  snapshot.wall_microseconds = now;
  snapshot.uptime_seconds = static_cast<double>(now - start_wall_) / 1e6;
  snapshot.cpu_percent =
      elapsed > 0.0 ? std::max(0.0, (cpu_now - last_cpu_seconds_) / elapsed * 100.0) : 0.0;
  snapshot.rss_bytes = resident_bytes();
  snapshot.requests_total = requests_total_;
  snapshot.requests_per_second =
      elapsed > 0.0 ? static_cast<double>(requests_total_ - last_requests_total_) / elapsed : 0.0;
  snapshot.bytes_sent = bytes_sent_;
  snapshot.websocket_clients = websocket_clients_;
  snapshot.sse_clients = sse_clients_;
  snapshot.latency_p50 = histogram_.quantile(0.50);
  snapshot.latency_p90 = histogram_.quantile(0.90);
  snapshot.latency_p99 = histogram_.quantile(0.99);
  snapshot.histogram = histogram_;

  last_wall_ = now;
  last_cpu_seconds_ = cpu_now;
  last_requests_total_ = requests_total_;
  return snapshot;
}

std::int64_t wall_microseconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

std::int64_t steady_microseconds() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

double process_cpu_seconds() {
#ifdef _WIN32
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0) return 0.0;
  const auto to_seconds = [](const FILETIME& value) {
    const std::uint64_t ticks = (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
                                value.dwLowDateTime;
    return static_cast<double>(ticks) / 1e7;  // the tick is 100 nanoseconds
  };
  return to_seconds(kernel) + to_seconds(user);
#else
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
  const auto to_seconds = [](const timeval& value) {
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1e6;
  };
  return to_seconds(usage.ru_utime) + to_seconds(usage.ru_stime);
#endif
}

std::uint64_t resident_bytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) == 0) return 0;
  return static_cast<std::uint64_t>(counters.WorkingSetSize);
#elif defined(__linux__)
  // statm reports the resident size in pages, field two.
  std::FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) return 0;
  long long total = 0;
  long long resident = 0;
  const int read = std::fscanf(file, "%lld %lld", &total, &resident);
  std::fclose(file);
  if (read != 2) return 0;
  return static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#else
  // No portable reading on this platform. Reporting zero is honest, a guess would not be.
  return 0;
#endif
}

}  // namespace pulse::metrics
