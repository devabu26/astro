use std::ffi::c_char;

static STATUS: &[u8] = b"Astro Rust backend connected!\0";

#[no_mangle]
pub extern "C" fn astro_get_status() -> *const c_char {
    STATUS.as_ptr() as *const c_char
}
