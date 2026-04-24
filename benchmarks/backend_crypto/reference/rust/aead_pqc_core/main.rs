#[path = "../bench_runtime.rs"]
mod bench_runtime;
#[path = "../crypto_ref.rs"]
mod crypto_ref;

fn workload() {
    let key = *b"0123456789abcdef0123456789abcdef";
    let nonce = *b"0123456789ab";
    let aad = b"nebula-aead";
    let plaintext = b"nebula backend+crypto benchmark";

    let mut sealed = [0u8; 256];
    let mut sealed_len = 0usize;
    let seal_rc = unsafe {
        crypto_ref::backend_crypto_ref_chacha20_poly1305_seal(
            sealed.as_mut_ptr(),
            sealed.len(),
            &mut sealed_len,
            key.as_ptr(),
            nonce.as_ptr(),
            aad.as_ptr(),
            aad.len(),
            plaintext.as_ptr(),
            plaintext.len(),
        )
    };
    if seal_rc != 0 {
        panic!("seal failed");
    }

    let mut opened = [0u8; 256];
    let mut opened_len = 0usize;
    let open_rc = unsafe {
        crypto_ref::backend_crypto_ref_chacha20_poly1305_open(
            opened.as_mut_ptr(),
            opened.len(),
            &mut opened_len,
            key.as_ptr(),
            nonce.as_ptr(),
            aad.as_ptr(),
            aad.len(),
            sealed.as_ptr(),
            sealed_len,
        )
    };
    if open_rc != 0 || &opened[..opened_len] != plaintext {
        panic!("open failed");
    }

    let mut public_key = [0u8; crypto_ref::ML_KEM_768_PUBLIC_KEY_BYTES];
    let mut secret_key = [0u8; crypto_ref::ML_KEM_768_SECRET_KEY_BYTES];
    let mut ciphertext = [0u8; crypto_ref::ML_KEM_768_CIPHERTEXT_BYTES];
    let mut shared_secret = [0u8; crypto_ref::ML_KEM_768_SHARED_SECRET_BYTES];
    let mut recovered = [0u8; crypto_ref::ML_KEM_768_SHARED_SECRET_BYTES];
    unsafe {
        if crypto_ref::nebula_crypto_ml_kem_768_keypair(public_key.as_mut_ptr(), secret_key.as_mut_ptr()) != 0 {
            panic!("keypair failed");
        }
        if crypto_ref::nebula_crypto_ml_kem_768_encapsulate(
            ciphertext.as_mut_ptr(),
            shared_secret.as_mut_ptr(),
            public_key.as_ptr(),
        ) != 0
        {
            panic!("encapsulate failed");
        }
        if crypto_ref::nebula_crypto_ml_kem_768_decapsulate(
            recovered.as_mut_ptr(),
            ciphertext.as_ptr(),
            secret_key.as_ptr(),
        ) != 0
        {
            panic!("decapsulate failed");
        }
    }
    if shared_secret != recovered {
        panic!("shared secret mismatch");
    }
}

fn main() {
    let metrics = bench_runtime::run_bench(workload, 1);
    bench_runtime::emit_metrics_json(&metrics);
}
