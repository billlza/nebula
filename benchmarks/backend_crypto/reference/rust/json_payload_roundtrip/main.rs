#[path = "../bench_runtime.rs"]
mod bench_runtime;

fn payload_json() -> &'static str {
    "{\"user\":\"nebula\",\"count\":7,\"ok\":true,\"zone\":\"prod-cn\",\"kind\":\"api\"}"
}

fn extract_string(text: &str, key: &str) -> String {
    let needle = format!("\"{}\":\"", key);
    let start = text.find(&needle).expect("missing string key") + needle.len();
    let end = text[start..].find('"').expect("unterminated string") + start;
    text[start..end].to_string()
}

fn extract_int(text: &str, key: &str) -> i32 {
    let needle = format!("\"{}\":", key);
    let start = text.find(&needle).expect("missing int key") + needle.len();
    let end = text[start..]
        .find(|ch: char| ch == ',' || ch == '}')
        .expect("unterminated int")
        + start;
    text[start..end].parse::<i32>().expect("bad int")
}

fn extract_bool(text: &str, key: &str) -> bool {
    let needle = format!("\"{}\":", key);
    let start = text.find(&needle).expect("missing bool key") + needle.len();
    if text[start..].starts_with("true") {
        true
    } else if text[start..].starts_with("false") {
        false
    } else {
        panic!("bad bool")
    }
}

fn workload() {
    let text = payload_json();
    if extract_string(text, "user") != "nebula"
        || extract_int(text, "count") != 7
        || !extract_bool(text, "ok")
    {
        panic!("json mismatch");
    }
}

fn main() {
    let metrics = bench_runtime::run_bench(workload, 1);
    bench_runtime::emit_metrics_json(&metrics);
}
