import Foundation

struct BenchMetrics {
    let warmupIterations: Int
    let measureIterations: Int
    let samples: Int
    let p50Ms: Double
    let p90Ms: Double
    let p99Ms: Double
    let meanMs: Double
    let stddevMs: Double
    let throughputOpsS: Double
    let clock: String
    let platform: String
    let perfCapability: String
    let perfCounters: String
    let perfReason: String
}

private func hostPlatform() -> String {
    #if os(macOS)
    #if arch(arm64)
    return "macos-arm64"
    #elseif arch(x86_64)
    return "macos-x64"
    #else
    return "macos-unknown"
    #endif
    #elseif os(Linux)
    #if arch(arm64)
    return "linux-arm64"
    #elseif arch(x86_64)
    return "linux-x64"
    #else
    return "linux-unknown"
    #endif
    #elseif os(Windows)
    return "windows-x64"
    #else
    return "unknown-unknown"
    #endif
}

private func percentile(_ samplesMs: [Double], _ p: Double) -> Double {
    if samplesMs.isEmpty {
        return 0.0
    }
    let idx = Int((p / 100.0) * Double(samplesMs.count - 1))
    return samplesMs[idx]
}

func runBench(benchOps: Int = 1, _ body: () -> Void) -> BenchMetrics {
    let warmup = 50
    let iters = 1000
    var samplesMs: [Double] = []
    samplesMs.reserveCapacity(iters)

    for _ in 0..<warmup {
        body()
    }
    for _ in 0..<iters {
        let t0 = DispatchTime.now().uptimeNanoseconds
        body()
        let t1 = DispatchTime.now().uptimeNanoseconds
        samplesMs.append(Double(t1 - t0) / 1_000_000.0)
    }

    samplesMs.sort()
    let sumMs = samplesMs.reduce(0.0, +)
    let meanMs = samplesMs.isEmpty ? 0.0 : (sumMs / Double(samplesMs.count))
    let variance: Double
    if samplesMs.count <= 1 {
        variance = 0.0
    } else {
        variance = samplesMs.reduce(0.0) { partial, sample in
            let delta = sample - meanMs
            return partial + delta * delta
        } / Double(samplesMs.count - 1)
    }
    let totalS = sumMs / 1000.0
    let throughput = totalS > 0.0 ? (Double(iters * benchOps) / totalS) : 0.0

    return BenchMetrics(
        warmupIterations: warmup,
        measureIterations: iters,
        samples: samplesMs.count,
        p50Ms: percentile(samplesMs, 50.0),
        p90Ms: percentile(samplesMs, 90.0),
        p99Ms: percentile(samplesMs, 99.0),
        meanMs: meanMs,
        stddevMs: variance.squareRoot(),
        throughputOpsS: throughput,
        clock: "steady_clock",
        platform: hostPlatform(),
        perfCapability: "unsupported",
        perfCounters: "unsupported",
        perfReason: "not_implemented"
    )
}

func emitMetrics(_ metrics: BenchMetrics) {
    print("{\"warmup_iterations\":\(metrics.warmupIterations),\"measure_iterations\":\(metrics.measureIterations),\"samples\":\(metrics.samples),\"p50_ms\":\(metrics.p50Ms),\"p90_ms\":\(metrics.p90Ms),\"p99_ms\":\(metrics.p99Ms),\"mean_ms\":\(metrics.meanMs),\"stddev_ms\":\(metrics.stddevMs),\"throughput_ops_s\":\(metrics.throughputOpsS),\"clock\":\"\(metrics.clock)\",\"platform\":\"\(metrics.platform)\",\"perf_capability\":\"\(metrics.perfCapability)\",\"perf_counters\":\"\(metrics.perfCounters)\",\"perf_reason\":\"\(metrics.perfReason)\"}")
}
