#[path = "../bench_runtime.rs"]
mod bench_runtime;

struct Response {
    status: i32,
    content_type: &'static str,
    body: String,
}

fn workload() {
    let path = "/hello/nebula";
    let prefix = "/hello/";
    if !path.starts_with(prefix) {
        panic!("route prefix mismatch");
    }
    let name = path[prefix.len()..].to_string();
    let response = Response {
        status: 200,
        content_type: "text/plain; charset=utf-8",
        body: name,
    };
    if response.status != 200
        || response.content_type != "text/plain; charset=utf-8"
        || response.body != "nebula"
    {
        panic!("unexpected response");
    }
}

fn main() {
    let metrics = bench_runtime::run_bench(workload, 1);
    bench_runtime::emit_metrics_json(&metrics);
}
