#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace backend_crypto_bench {

struct BenchMetrics {
  int warmup_iterations;
  int measure_iterations;
  int samples;
  double p50_ms;
  double p90_ms;
  double p99_ms;
  double mean_ms;
  double stddev_ms;
  double throughput_ops_s;
  std::string clock;
  std::string platform;
  std::string perf_capability;
  std::string perf_counters;
  std::string perf_reason;
};

inline std::string host_platform() {
#if defined(__APPLE__)
  const char* os = "macos";
#elif defined(__linux__)
  const char* os = "linux";
#elif defined(_WIN32)
  const char* os = "windows";
#else
  const char* os = "unknown";
#endif

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  const char* arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  const char* arch = "x64";
#elif defined(__i386__) || defined(_M_IX86)
  const char* arch = "x86";
#else
  const char* arch = "unknown";
#endif

  return std::string(os) + "-" + arch;
}

inline double percentile(const std::vector<double>& samples_ms, double p) {
  if (samples_ms.empty()) {
    return 0.0;
  }
  const std::size_t idx =
      static_cast<std::size_t>((p / 100.0) * static_cast<double>(samples_ms.size() - 1));
  return samples_ms[idx];
}

template <typename Fn>
BenchMetrics run_bench(Fn&& fn, int bench_ops = 1) {
  using Clock = std::chrono::steady_clock;
  constexpr int warmup = 50;
  constexpr int iters = 1000;
  std::vector<double> samples_ms;
  samples_ms.reserve(iters);

  for (int i = 0; i < warmup; ++i) {
    fn();
  }
  for (int i = 0; i < iters; ++i) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    samples_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  std::sort(samples_ms.begin(), samples_ms.end());
  double sum_ms = 0.0;
  for (double x : samples_ms) {
    sum_ms += x;
  }
  const double mean_ms = samples_ms.empty() ? 0.0 : (sum_ms / static_cast<double>(samples_ms.size()));
  double variance = 0.0;
  for (double x : samples_ms) {
    const double delta = x - mean_ms;
    variance += delta * delta;
  }
  const double stddev_ms = samples_ms.size() <= 1
                               ? 0.0
                               : std::sqrt(variance / static_cast<double>(samples_ms.size() - 1));
  const double total_s = sum_ms / 1000.0;
  const double throughput = total_s > 0.0 ? (static_cast<double>(iters * bench_ops) / total_s) : 0.0;

  return BenchMetrics{
      warmup,
      iters,
      static_cast<int>(samples_ms.size()),
      percentile(samples_ms, 50.0),
      percentile(samples_ms, 90.0),
      percentile(samples_ms, 99.0),
      mean_ms,
      stddev_ms,
      throughput,
      "steady_clock",
      host_platform(),
      "unsupported",
      "unsupported",
      "not_implemented",
  };
}

inline void emit_metrics_json(const BenchMetrics& metrics) {
  std::cout << "{\"warmup_iterations\":" << metrics.warmup_iterations
            << ",\"measure_iterations\":" << metrics.measure_iterations
            << ",\"samples\":" << metrics.samples
            << ",\"p50_ms\":" << metrics.p50_ms
            << ",\"p90_ms\":" << metrics.p90_ms
            << ",\"p99_ms\":" << metrics.p99_ms
            << ",\"mean_ms\":" << metrics.mean_ms
            << ",\"stddev_ms\":" << metrics.stddev_ms
            << ",\"throughput_ops_s\":" << metrics.throughput_ops_s
            << ",\"clock\":\"" << metrics.clock
            << "\",\"platform\":\"" << metrics.platform
            << "\",\"perf_capability\":\"" << metrics.perf_capability
            << "\",\"perf_counters\":\"" << metrics.perf_counters
            << "\",\"perf_reason\":\"" << metrics.perf_reason << "\"}\n";
}

}  // namespace backend_crypto_bench
