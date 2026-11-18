use std::ffi::c_char;

pub mod ffi;
pub mod matrix_client;
pub mod media;
pub mod session_storage;
pub mod storage;

static STATUS: &[u8] = b"Astro Rust backend connected!\0";

#[no_mangle]
pub extern "C" fn astro_get_status() -> *const c_char {
    STATUS.as_ptr() as *const c_char
}
