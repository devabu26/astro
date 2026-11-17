use astro_core::ffi::{astro_storage_close, astro_storage_open, astro_string_free};
use std::ffi::{CStr, CString};
use std::fs;
use std::ptr;

#[test]
fn ffi_storage_open_close_and_error_handling() {
    // Null inputs must fail safely and return an error.
    let mut error = ptr::null_mut();

    let handle = unsafe { astro_storage_open(ptr::null(), ptr::null(), &mut error) };

    assert!(handle.is_null());
    assert!(!error.is_null());

    let message = unsafe { CStr::from_ptr(error).to_string_lossy().into_owned() };

    assert!(
        message.contains("Storage path must not be null"),
        "Unexpected error: {message}"
    );

    unsafe {
        astro_string_free(error);
    }

    // Empty passphrases must be rejected.
    let path = CString::new("/tmp/astro-ffi-empty-passphrase").unwrap();
    let empty_passphrase = CString::new("").unwrap();

    error = ptr::null_mut();

    let handle =
        unsafe { astro_storage_open(path.as_ptr(), empty_passphrase.as_ptr(), &mut error) };

    assert!(handle.is_null());
    assert!(!error.is_null());

    unsafe {
        astro_string_free(error);
    }

    // A valid passphrase should open and close storage.
    let directory =
        std::env::temp_dir().join(format!("astro-ffi-storage-test-{}", std::process::id()));

    if directory.exists() {
        fs::remove_dir_all(&directory).unwrap();
    }

    let path = CString::new(directory.to_str().unwrap()).unwrap();
    let passphrase = CString::new("Astro-ffi-test-passphrase-2026").unwrap();

    error = ptr::null_mut();

    let handle = unsafe { astro_storage_open(path.as_ptr(), passphrase.as_ptr(), &mut error) };

    assert!(
        !handle.is_null(),
        "Storage open failed: {}",
        if error.is_null() {
            "no error returned".to_string()
        } else {
            unsafe { CStr::from_ptr(error).to_string_lossy().into_owned() }
        }
    );

    assert!(error.is_null());

    unsafe {
        astro_storage_close(handle);
    }

    fs::remove_dir_all(&directory).unwrap();
}
