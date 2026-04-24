import Foundation

struct Response {
    let status: Int
    let contentType: String
    let body: String
}

func workload() {
    let path = "/hello/nebula"
    let prefix = "/hello/"
    guard path.hasPrefix(prefix) else {
        fatalError("route prefix mismatch")
    }
    let name = String(path.dropFirst(prefix.count))
    let response = Response(status: 200, contentType: "text/plain; charset=utf-8", body: name)
    if response.status != 200 || response.contentType != "text/plain; charset=utf-8" || response.body != "nebula" {
        fatalError("unexpected response")
    }
}

let metrics = runBench(benchOps: 1, workload)
emitMetrics(metrics)
