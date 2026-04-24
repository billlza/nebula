pub const BLAKE3_BYTES: usize = 32;
pub const SHA3_256_BYTES: usize = 32;
pub const CHACHA20_POLY1305_KEY_BYTES: usize = 32;
pub const CHACHA20_POLY1305_NONCE_BYTES: usize = 12;
pub const CHACHA20_POLY1305_TAG_BYTES: usize = 16;
pub const ML_KEM_768_PUBLIC_KEY_BYTES: usize = 1184;
pub const ML_KEM_768_SECRET_KEY_BYTES: usize = 2400;
pub const ML_KEM_768_CIPHERTEXT_BYTES: usize = 1088;
pub const ML_KEM_768_SHARED_SECRET_BYTES: usize = 32;

#[link(name = "backend_crypto_refsupport", kind = "static")]
unsafe extern "C" {
    pub fn nebula_crypto_blake3_digest(input: *const u8, input_len: usize, out: *mut u8);
    pub fn nebula_crypto_sha3_256_digest(input: *const u8, input_len: usize, out: *mut u8);
    pub fn nebula_crypto_ml_kem_768_keypair(public_key: *mut u8, secret_key: *mut u8) -> i32;
    pub fn nebula_crypto_ml_kem_768_encapsulate(
        ciphertext: *mut u8,
        shared_secret: *mut u8,
        public_key: *const u8,
    ) -> i32;
    pub fn nebula_crypto_ml_kem_768_decapsulate(
        shared_secret: *mut u8,
        ciphertext: *const u8,
        secret_key: *const u8,
    ) -> i32;
    pub fn backend_crypto_ref_chacha20_poly1305_seal(
        out: *mut u8,
        out_capacity: usize,
        out_len: *mut usize,
        key: *const u8,
        nonce: *const u8,
        aad: *const u8,
        aad_len: usize,
        plaintext: *const u8,
        plaintext_len: usize,
    ) -> i32;
    pub fn backend_crypto_ref_chacha20_poly1305_open(
        out: *mut u8,
        out_capacity: usize,
        out_len: *mut usize,
        key: *const u8,
        nonce: *const u8,
        aad: *const u8,
        aad_len: usize,
        ciphertext: *const u8,
        ciphertext_len: usize,
    ) -> i32;
}
