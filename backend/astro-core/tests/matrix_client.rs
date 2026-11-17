use astro_core::matrix_client::MatrixClientManager;
use std::fs;
use std::path::PathBuf;

const TEST_PASSPHRASE: &str = "Astro-matrix-client-test-2026";

fn test_store_path() -> PathBuf {
    std::env::temp_dir().join(format!("astro-matrix-client-test-{}", std::process::id()))
}

#[test]
fn restore_without_saved_session_returns_none() {
    let path = test_store_path();

    if path.exists() {
        fs::remove_dir_all(&path).unwrap();
    }

    let result = MatrixClientManager::restore(&path, TEST_PASSPHRASE)
        .expect("Checking for a saved session should succeed");

    assert!(
        result.is_none(),
        "A fresh installation should have no Matrix session"
    );

    assert!(
        !path.exists(),
        "Checking for a session should not create database files"
    );
}

use astro_core::ffi::{astro_matrix_restore, astro_string_free};
use std::ffi::{CStr, CString};
use std::ptr;

#[test]
fn ffi_restore_reports_missing_session() {
    let path = test_store_path();

    if path.exists() {
        fs::remove_dir_all(&path).unwrap();
    }

    let path_c = CString::new(path.to_str().unwrap()).unwrap();
    let passphrase_c = CString::new(TEST_PASSPHRASE).unwrap();

    let mut client = ptr::null_mut();
    let mut error = ptr::null_mut();

    let result = unsafe {
        astro_matrix_restore(
            path_c.as_ptr(),
            passphrase_c.as_ptr(),
            &mut client,
            &mut error,
        )
    };

    assert_eq!(result, 0);
    assert!(client.is_null());
    assert!(error.is_null());

    let result = unsafe {
        astro_matrix_restore(
            path_c.as_ptr(),
            passphrase_c.as_ptr(),
            ptr::null_mut(),
            &mut error,
        )
    };

    assert_eq!(result, -1);
    assert!(!error.is_null());

    unsafe {
        let message = CStr::from_ptr(error).to_string_lossy();
        assert!(message.contains("output pointer"));
        astro_string_free(error);
    }
}
