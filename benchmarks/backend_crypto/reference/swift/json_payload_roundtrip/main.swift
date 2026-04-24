import Foundation

func payloadJson() -> String {
    return "{\"user\":\"nebula\",\"count\":7,\"ok\":true,\"zone\":\"prod-cn\",\"kind\":\"api\"}"
}

func extractString(_ text: String, _ key: String) -> String {
    let needle = "\"\(key)\":\""
    guard let start = text.range(of: needle) else {
        fatalError("missing string key")
    }
    let valueStart = start.upperBound
    guard let end = text[valueStart...].firstIndex(of: "\"") else {
        fatalError("unterminated string")
    }
    return String(text[valueStart..<end])
}

func extractInt(_ text: String, _ key: String) -> Int {
    let needle = "\"\(key)\":"
    guard let start = text.range(of: needle) else {
        fatalError("missing int key")
    }
    let valueStart = start.upperBound
    let suffix = text[valueStart...]
    guard let end = suffix.firstIndex(where: { $0 == "," || $0 == "}" }) else {
        fatalError("unterminated int")
    }
    return Int(text[valueStart..<end])!
}

func extractBool(_ text: String, _ key: String) -> Bool {
    let needle = "\"\(key)\":"
    guard let start = text.range(of: needle) else {
        fatalError("missing bool key")
    }
    let valueStart = start.upperBound
    let suffix = text[valueStart...]
    if suffix.hasPrefix("true") {
        return true
    }
    if suffix.hasPrefix("false") {
        return false
    }
    fatalError("bad bool")
}

func workload() {
    let text = payloadJson()
    if extractString(text, "user") != "nebula" || extractInt(text, "count") != 7 || !extractBool(text, "ok") {
        fatalError("json mismatch")
    }
}

let metrics = runBench(benchOps: 1, workload)
emitMetrics(metrics)
