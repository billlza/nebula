#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace app_platform_cpp {

inline constexpr int kWarmupIterations = 50;
inline constexpr int kMeasureIterations = 1000;
inline volatile std::uint64_t g_benchmark_sink = 0;

[[noreturn]] inline void fail(const std::string& message) {
  std::cerr << message << "\n";
  std::exit(2);
}

inline void expect_eq(const std::string& actual, const std::string& expected, const std::string& label) {
  if (actual != expected) {
    fail(label + " mismatch");
  }
}

inline void expect_eq(int actual, int expected, const std::string& label) {
  if (actual != expected) {
    fail(label + " mismatch");
  }
}

inline std::uint64_t checksum_text(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline std::string host_platform_tag() {
#if defined(__APPLE__) && defined(__aarch64__)
  return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
  return "macos-x64";
#elif defined(__linux__) && defined(__aarch64__)
  return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
  return "linux-x64";
#elif defined(_WIN32) && defined(_M_X64)
  return "windows-x64";
#else
  return "unknown";
#endif
}

inline double percentile(const std::vector<double>& sorted_samples, double fraction) {
  if (sorted_samples.empty()) {
    fail("cannot compute percentile for empty sample set");
  }
  if (sorted_samples.size() == 1) {
    return sorted_samples[0];
  }
  const auto index = static_cast<std::size_t>(
      std::llround(static_cast<double>(sorted_samples.size() - 1U) * fraction));
  return sorted_samples[std::min(index, sorted_samples.size() - 1U)];
}

template <typename Fn>
int run_benchmark(Fn fn) {
  std::uint64_t checksum = 0;
  for (int i = 0; i < kWarmupIterations; ++i) {
    checksum ^= fn();
  }

  std::vector<double> samples;
  samples.reserve(kMeasureIterations);
  using clock = std::chrono::steady_clock;
  for (int i = 0; i < kMeasureIterations; ++i) {
    const auto start = clock::now();
    checksum ^= fn();
    const auto end = clock::now();
    const std::chrono::duration<double, std::milli> elapsed = end - start;
    samples.push_back(elapsed.count());
  }
  g_benchmark_sink = checksum;

  std::vector<double> ordered = samples;
  std::sort(ordered.begin(), ordered.end());
  const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
  const double mean = sum / static_cast<double>(samples.size());
  double variance = 0.0;
  for (double sample : samples) {
    const double delta = sample - mean;
    variance += delta * delta;
  }
  variance /= static_cast<double>(samples.size());
  const double throughput = mean > 0.0 ? 1000.0 / mean : 0.0;

  std::cout << std::setprecision(10)
            << "NEBULA_BENCH"
            << " warmup_iterations=" << kWarmupIterations
            << " measure_iterations=" << kMeasureIterations
            << " samples=" << samples.size()
            << " p50_ms=" << percentile(ordered, 0.50)
            << " p90_ms=" << percentile(ordered, 0.90)
            << " p99_ms=" << percentile(ordered, 0.99)
            << " mean_ms=" << mean
            << " stddev_ms=" << std::sqrt(variance)
            << " throughput_ops_s=" << throughput
            << " clock=steady_clock"
            << " platform=" << host_platform_tag()
            << " perf_capability=unsupported"
            << " perf_counters=unsupported"
            << " perf_reason=not_implemented"
            << "\n";
  return g_benchmark_sink == UINT64_MAX ? 1 : 0;
}

}  // namespace app_platform_cpp
