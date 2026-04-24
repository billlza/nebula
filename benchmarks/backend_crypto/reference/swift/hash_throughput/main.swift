import CryptoRefSupport
import Foundation

func payload() -> [UInt8] {
    return Array("nebula-backend-crypto-benchmark-0123456789abcdef0123456789abcdef".utf8)
}

func workload() {
    let bytes = payload()
    var blake = [UInt8](repeating: 0, count: 32)
    var sha = [UInt8](repeating: 0, count: 32)
    bytes.withUnsafeBufferPointer { input in
        nebula_crypto_blake3_digest(input.baseAddress, bytes.count, &blake)
        nebula_crypto_sha3_256_digest(input.baseAddress, bytes.count, &sha)
    }
    if blake[0] == 0 && sha[0] == 0 {
        fatalError("unexpected digests")
    }
}

let metrics = runBench(benchOps: 1, workload)
emitMetrics(metrics)
