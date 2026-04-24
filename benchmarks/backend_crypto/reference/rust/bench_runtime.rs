use std::time::Instant;

pub struct BenchMetrics {
    pub warmup_iterations: usize,
    pub measure_iterations: usize,
    pub samples: usize,
    pub p50_ms: f64,
    pub p90_ms: f64,
    pub p99_ms: f64,
    pub mean_ms: f64,
    pub stddev_ms: f64,
    pub throughput_ops_s: f64,
    pub clock: &'static str,
    pub platform: &'static str,
    pub perf_capability: &'static str,
    pub perf_counters: &'static str,
    pub perf_reason: &'static str,
}

fn host_platform() -> &'static str {
    if cfg!(target_os = "macos") && cfg!(target_arch = "aarch64") {
        "macos-arm64"
    } else if cfg!(target_os = "macos") && cfg!(target_arch = "x86_64") {
        "macos-x64"
    } else if cfg!(target_os = "linux") && cfg!(target_arch = "aarch64") {
        "linux-arm64"
    } else if cfg!(target_os = "linux") && cfg!(target_arch = "x86_64") {
        "linux-x64"
    } else if cfg!(target_os = "windows") && cfg!(target_arch = "x86_64") {
        "windows-x64"
    } else {
        "unknown-unknown"
    }
}

fn percentile(samples_ms: &[f64], p: f64) -> f64 {
    if samples_ms.is_empty() {
        return 0.0;
    }
    let idx = ((p / 100.0) * ((samples_ms.len() - 1) as f64)) as usize;
    samples_ms[idx]
}

pub fn run_bench<F>(mut f: F, bench_ops: usize) -> BenchMetrics
where
    F: FnMut(),
{
    let warmup = 50usize;
    let iters = 1000usize;
    let mut samples_ms = Vec::with_capacity(iters);

    for _ in 0..warmup {
        f();
    }
    for _ in 0..iters {
        let started = Instant::now();
        f();
        let elapsed_ms = started.elapsed().as_secs_f64() * 1000.0;
        samples_ms.push(elapsed_ms);
    }
    samples_ms.sort_by(|lhs, rhs| lhs.partial_cmp(rhs).unwrap());

    let sum_ms: f64 = samples_ms.iter().sum();
    let mean_ms = if samples_ms.is_empty() {
        0.0
    } else {
        sum_ms / (samples_ms.len() as f64)
    };
    let variance = if samples_ms.len() <= 1 {
        0.0
    } else {
        samples_ms
            .iter()
            .map(|sample| {
                let delta = *sample - mean_ms;
                delta * delta
            })
            .sum::<f64>()
            / ((samples_ms.len() - 1) as f64)
    };
    let total_s = sum_ms / 1000.0;
    let throughput = if total_s > 0.0 {
        ((iters * bench_ops) as f64) / total_s
    } else {
        0.0
    };

    BenchMetrics {
        warmup_iterations: warmup,
        measure_iterations: iters,
        samples: samples_ms.len(),
        p50_ms: percentile(&samples_ms, 50.0),
        p90_ms: percentile(&samples_ms, 90.0),
        p99_ms: percentile(&samples_ms, 99.0),
        mean_ms,
        stddev_ms: variance.sqrt(),
        throughput_ops_s: throughput,
        clock: "steady_clock",
        platform: host_platform(),
        perf_capability: "unsupported",
        perf_counters: "unsupported",
        perf_reason: "not_implemented",
    }
}

pub fn emit_metrics_json(metrics: &BenchMetrics) {
    println!(
        "{{\"warmup_iterations\":{},\"measure_iterations\":{},\"samples\":{},\"p50_ms\":{},\"p90_ms\":{},\"p99_ms\":{},\"mean_ms\":{},\"stddev_ms\":{},\"throughput_ops_s\":{},\"clock\":\"{}\",\"platform\":\"{}\",\"perf_capability\":\"{}\",\"perf_counters\":\"{}\",\"perf_reason\":\"{}\"}}",
        metrics.warmup_iterations,
        metrics.measure_iterations,
        metrics.samples,
        metrics.p50_ms,
        metrics.p90_ms,
        metrics.p99_ms,
        metrics.mean_ms,
        metrics.stddev_ms,
        metrics.throughput_ops_s,
        metrics.clock,
        metrics.platform,
        metrics.perf_capability,
        metrics.perf_counters,
        metrics.perf_reason
    );
}
