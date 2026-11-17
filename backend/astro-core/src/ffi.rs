use crate::matrix_client::MatrixClientManager;
use crate::storage::StorageManager;

use std::ffi::{c_char, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::ptr;

unsafe fn read_input<'a>(value: *const c_char, name: &str) -> Result<&'a str, String> {
    if value.is_null() {
        return Err(format!("{name} must not be null"));
    }

    CStr::from_ptr(value)
        .to_str()
        .map_err(|_| format!("{name} is not valid UTF-8"))
}

unsafe fn set_error(error_out: *mut *mut c_char, message: &str) {
    if error_out.is_null() {
        return;
    }

    let safe_message = message.replace('\0', "�");
    let error = CString::new(safe_message).expect("NUL characters were replaced");

    *error_out = error.into_raw();
}

#[no_mangle]
pub unsafe extern "C" fn astro_storage_open(
    path: *const c_char,
    passphrase: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut StorageManager {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let path = read_input(path, "Storage path")?;
        let passphrase = read_input(passphrase, "Passphrase")?;

        if path.is_empty() {
            return Err("Storage path must not be empty".to_string());
        }

        if passphrase.is_empty() {
            return Err("Passphrase must not be empty".to_string());
        }

        StorageManager::open(Path::new(path), passphrase)
    }));

    match result {
        Ok(Ok(manager)) => Box::into_raw(Box::new(manager)),
        Ok(Err(message)) => {
            set_error(error_out, &message);
            ptr::null_mut()
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while opening storage");
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_storage_close(handle: *mut StorageManager) {
    if !handle.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(Box::from_raw(handle));
        }));
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_login(
    path: *const c_char,
    passphrase: *const c_char,
    homeserver: *const c_char,
    username: *const c_char,
    password: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut MatrixClientManager {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let path = read_input(path, "Storage path")?;
        let passphrase = read_input(passphrase, "Passphrase")?;
        let homeserver = read_input(homeserver, "Homeserver")?;
        let username = read_input(username, "Username")?;
        let password = read_input(password, "Password")?;

        if path.is_empty()
            || passphrase.is_empty()
            || homeserver.is_empty()
            || username.is_empty()
            || password.is_empty()
        {
            return Err("Login fields must not be empty".to_string());
        }

        MatrixClientManager::login(Path::new(path), passphrase, homeserver, username, password)
    }));

    match result {
        Ok(Ok(manager)) => Box::into_raw(Box::new(manager)),
        Ok(Err(message)) => {
            set_error(error_out, &message);
            ptr::null_mut()
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic during Matrix login");
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_restore(
    path: *const c_char,
    passphrase: *const c_char,
    client_out: *mut *mut MatrixClientManager,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if client_out.is_null() {
        set_error(error_out, "Client output pointer must not be null");
        return -1;
    }

    *client_out = ptr::null_mut();

    let result = catch_unwind(AssertUnwindSafe(|| {
        let path = read_input(path, "Storage path")?;
        let passphrase = read_input(passphrase, "Passphrase")?;

        if path.is_empty() || passphrase.is_empty() {
            return Err("Storage path and passphrase must not be empty".to_string());
        }

        MatrixClientManager::restore(Path::new(path), passphrase)
    }));

    match result {
        Ok(Ok(Some(manager))) => {
            *client_out = Box::into_raw(Box::new(manager));
            1
        }
        Ok(Ok(None)) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while restoring Matrix session",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_logout(
    handle: *mut MatrixClientManager,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = unsafe { &mut *handle };
        manager.logout()
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic during Matrix logout");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_close(handle: *mut MatrixClientManager) {
    if !handle.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(Box::from_raw(handle));
        }));
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_string_free(value: *mut c_char) {
    if !value.is_null() {
        drop(CString::from_raw(value));
    }
}
