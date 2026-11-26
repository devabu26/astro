#![recursion_limit = "256"]
use std::ffi::c_char;
use std::sync::OnceLock;

pub mod ffi;
pub mod matrix_client;
pub mod media;
pub mod notifications;
pub mod rtc_e2ee;
pub mod session_storage;
pub mod storage;

/// Initialise Rust diagnostic logging once for the application.
pub fn init_tracing() {
    static INIT: OnceLock<()> = OnceLock::new();

    INIT.get_or_init(|| {
        // Matrix SDK and LiveKit enable different Rustls crypto providers.
        // Select AWS-LC explicitly before either client creates a TLS connection.
        let _ = rustls::crypto::aws_lc_rs::default_provider().install_default();

        let _ = tracing_subscriber::fmt()
            .with_max_level(tracing::Level::DEBUG)
            .with_target(true)
            .try_init();
    });
}

static STATUS: &[u8] = b"Astro Rust backend connected!\0";

#[no_mangle]
pub extern "C" fn astro_get_status() -> *const c_char {
    STATUS.as_ptr() as *const c_char
}
