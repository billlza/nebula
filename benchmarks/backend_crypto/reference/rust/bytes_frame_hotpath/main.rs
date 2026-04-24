#[path = "../bench_runtime.rs"]
mod bench_runtime;

fn payload() -> &'static str {
    "0123456789abcdef0123456789abcdef"
}

fn workload() {
    let frame = format!("hdr:{}|{}", payload(), payload());
    if frame.is_empty() || frame.len() != 69 {
        panic!("unexpected frame");
    }
}

fn main() {
    let metrics = bench_runtime::run_bench(workload, 1);
    bench_runtime::emit_metrics_json(&metrics);
}
