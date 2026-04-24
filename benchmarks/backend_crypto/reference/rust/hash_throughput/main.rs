#[path = "../bench_runtime.rs"]
mod bench_runtime;
#[path = "../crypto_ref.rs"]
mod crypto_ref;

fn payload() -> &'static [u8] {
    b"nebula-backend-crypto-benchmark-0123456789abcdef0123456789abcdef"
}

fn workload() {
    let mut blake = [0u8; crypto_ref::BLAKE3_BYTES];
    let mut sha = [0u8; crypto_ref::SHA3_256_BYTES];
    unsafe {
        crypto_ref::nebula_crypto_blake3_digest(payload().as_ptr(), payload().len(), blake.as_mut_ptr());
        crypto_ref::nebula_crypto_sha3_256_digest(payload().as_ptr(), payload().len(), sha.as_mut_ptr());
    }
    if blake[0] == 0 && sha[0] == 0 {
        panic!("unexpected digests");
    }
}

fn main() {
    let metrics = bench_runtime::run_bench(workload, 1);
    bench_runtime::emit_metrics_json(&metrics);
}
