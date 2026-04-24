import CryptoRefSupport
import Foundation

let keyBytes = Array("0123456789abcdef0123456789abcdef".utf8)
let nonceBytes = Array("0123456789ab".utf8)
let aadBytes = Array("nebula-aead".utf8)
let plaintextBytes = Array("nebula backend+crypto benchmark".utf8)

func workload() {
    var sealed = [UInt8](repeating: 0, count: 256)
    var sealedLen = 0
    let sealRc = keyBytes.withUnsafeBufferPointer { keyPtr in
        nonceBytes.withUnsafeBufferPointer { noncePtr in
            aadBytes.withUnsafeBufferPointer { aadPtr in
                plaintextBytes.withUnsafeBufferPointer { plainPtr in
                    backend_crypto_ref_chacha20_poly1305_seal(
                        &sealed,
                        sealed.count,
                        &sealedLen,
                        keyPtr.baseAddress,
                        noncePtr.baseAddress,
                        aadPtr.baseAddress,
                        aadBytes.count,
                        plainPtr.baseAddress,
                        plaintextBytes.count
                    )
                }
            }
        }
    }
    if sealRc != 0 {
        fatalError("seal failed")
    }

    var opened = [UInt8](repeating: 0, count: 256)
    var openedLen = 0
    let openRc = keyBytes.withUnsafeBufferPointer { keyPtr in
        nonceBytes.withUnsafeBufferPointer { noncePtr in
            aadBytes.withUnsafeBufferPointer { aadPtr in
                sealed.withUnsafeBufferPointer { sealedPtr in
                    backend_crypto_ref_chacha20_poly1305_open(
                        &opened,
                        opened.count,
                        &openedLen,
                        keyPtr.baseAddress,
                        noncePtr.baseAddress,
                        aadPtr.baseAddress,
                        aadBytes.count,
                        sealedPtr.baseAddress,
                        sealedLen
                    )
                }
            }
        }
    }
    if openRc != 0 || Array(opened.prefix(openedLen)) != plaintextBytes {
        fatalError("open failed")
    }

    var publicKey = [UInt8](repeating: 0, count: 1184)
    var secretKey = [UInt8](repeating: 0, count: 2400)
    var ciphertext = [UInt8](repeating: 0, count: 1088)
    var sharedSecret = [UInt8](repeating: 0, count: 32)
    var recoveredSharedSecret = [UInt8](repeating: 0, count: 32)
    if nebula_crypto_ml_kem_768_keypair(&publicKey, &secretKey) != 0 {
        fatalError("keypair failed")
    }
    if nebula_crypto_ml_kem_768_encapsulate(&ciphertext, &sharedSecret, publicKey) != 0 {
        fatalError("encapsulate failed")
    }
    if nebula_crypto_ml_kem_768_decapsulate(&recoveredSharedSecret, ciphertext, secretKey) != 0 {
        fatalError("decapsulate failed")
    }
    if sharedSecret != recoveredSharedSecret {
        fatalError("shared secret mismatch")
    }
}

let metrics = runBench(benchOps: 1, workload)
emitMetrics(metrics)
