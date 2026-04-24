import Foundation

func payload() -> String {
    return "0123456789abcdef0123456789abcdef"
}

func workload() {
    let frame = "hdr:" + payload() + "|" + payload()
    if frame.isEmpty || frame.count != 69 {
        fatalError("unexpected frame")
    }
}

let metrics = runBench(benchOps: 1, workload)
emitMetrics(metrics)
