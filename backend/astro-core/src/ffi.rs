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
pub unsafe extern "C" fn astro_matrix_send_room_message(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    message: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut c_char {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let room_id = read_input(room_id, "Room ID")?;
        let message = read_input(message, "Message")?;

        let manager = &*handle;
        manager.send_room_message(room_id, message)
    }));

    match result {
        Ok(Ok(event_id)) => match CString::new(event_id) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(error_out, "Invalid event ID returned by Matrix");
                ptr::null_mut()
            }
        },
        Ok(Err(message)) => {
            set_error(error_out, &message);
            ptr::null_mut()
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while sending Matrix message",
            );
            ptr::null_mut()
        }
    }
}


#[no_mangle]
pub unsafe extern "C" fn astro_matrix_accept_verification_request(
    handle: *mut MatrixClientManager,
    user_id: *const c_char,
    flow_id: *const c_char,
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
        let user_id = read_input(user_id, "User ID")?;
        let flow_id = read_input(flow_id, "Flow ID")?;
        let manager = &*handle;
        manager.accept_verification_request(user_id, flow_id)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while accepting verification");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_cancel_verification_request(
    handle: *mut MatrixClientManager,
    user_id: *const c_char,
    flow_id: *const c_char,
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
        let user_id = read_input(user_id, "User ID")?;
        let flow_id = read_input(flow_id, "Flow ID")?;
        let manager = &mut *handle;
        manager.cancel_verification_request(user_id, flow_id)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while cancelling verification");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_confirm_verification(
    handle: *mut MatrixClientManager,
    user_id: *const c_char,
    flow_id: *const c_char,
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
        let user_id = read_input(user_id, "User ID")?;
        let flow_id = read_input(flow_id, "Flow ID")?;
        let manager = &*handle;
        manager.confirm_verification(user_id, flow_id)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while confirming verification");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_mismatch_verification(
    handle: *mut MatrixClientManager,
    user_id: *const c_char,
    flow_id: *const c_char,
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
        let user_id = read_input(user_id, "User ID")?;
        let flow_id = read_input(flow_id, "Flow ID")?;
        let manager = &*handle;
        manager.mismatch_verification(user_id, flow_id)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while rejecting verification");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_get_cached_rooms_json(
    handle: *mut MatrixClientManager,
    error_out: *mut *mut c_char,
) -> *mut c_char {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = unsafe { &mut *handle };
        manager.cached_rooms_json()
    }));

    match result {
        Ok(Ok(json)) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(error_out, "Room data contained an invalid null character");
                ptr::null_mut()
            }
        },
        Ok(Err(message)) => {
            set_error(error_out, &message);
            ptr::null_mut()
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while retrieving cached Matrix rooms",
            );
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_get_rooms_json(
    handle: *mut MatrixClientManager,
    error_out: *mut *mut c_char,
) -> *mut c_char {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = unsafe { &mut *handle };
        manager.rooms_json()
    }));

    match result {
        Ok(Ok(json)) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(error_out, "Room data contained an invalid null character");
                ptr::null_mut()
            }
        },
        Ok(Err(message)) => {
            set_error(error_out, &message);
            ptr::null_mut()
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while retrieving Matrix rooms",
            );
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_get_room_timeline_json(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut c_char {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let room_id = unsafe { read_input(room_id, "Room ID")? };
        let manager = unsafe { &mut *handle };
        manager.room_timeline_json(room_id)
    }));

    match result {
        Ok(Ok(json)) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(
                    error_out,
                    "Timeline data contained an invalid null character",
                );
                ptr::null_mut()
            }
        },
        Ok(Err(message)) => {
            set_error(error_out, &message);
            ptr::null_mut()
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while retrieving Matrix timeline",
            );
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_mark_room_as_read(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
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
        let room_id = unsafe { read_input(room_id, "Room ID")? };
        let manager = unsafe { &*handle };
        manager.mark_room_as_read(room_id)
    }));

    match result {
        Ok(Ok(true)) => 1,
        Ok(Ok(false)) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while sending Matrix read receipt",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_paginate_room_timeline_json(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut c_char {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let room_id = unsafe { read_input(room_id, "Room ID")? };
        let manager = unsafe { &mut *handle };
        manager.paginate_room_timeline_json(room_id)
    }));

    match result {
        Ok(Ok(json)) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(
                    error_out,
                    "Timeline data contained an invalid null character",
                );
                ptr::null_mut()
            }
        },
        Ok(Err(message)) => {
            set_error(error_out, &message);
            ptr::null_mut()
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while paginating room timeline",
            );
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_poll_room_timeline_updates_json(
    handle: *mut MatrixClientManager,
    error_out: *mut *mut c_char,
) -> *mut c_char {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = unsafe { &mut *handle };
        manager.poll_room_timeline_updates_json()
    }));

    match result {
        Ok(json) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(
                    error_out,
                    "Timeline update data contained an invalid null character",
                );
                ptr::null_mut()
            }
        },
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while polling Matrix timeline updates",
            );
            ptr::null_mut()
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
pub unsafe extern "C" fn astro_matrix_thumbnail(
    handle: *mut MatrixClientManager,
    mxc_uri: *const c_char,
    width: u32,
    height: u32,
    data_out: *mut *mut u8,
    len_out: *mut usize,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if !data_out.is_null() {
        *data_out = ptr::null_mut();
    }

    if !len_out.is_null() {
        *len_out = 0;
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }

    if mxc_uri.is_null() {
        set_error(error_out, "MXC URI must not be null");
        return -1;
    }

    if data_out.is_null() || len_out.is_null() {
        set_error(error_out, "Media output pointers must not be null");
        return -1;
    }

    if width == 0 || height == 0 {
        set_error(error_out, "Thumbnail dimensions must be greater than zero");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = &*handle;
        let uri = CStr::from_ptr(mxc_uri)
            .to_str()
            .map_err(|_| "MXC URI is not valid UTF-8".to_string())?;

        let owned_uri = matrix_sdk::ruma::OwnedMxcUri::from(uri);

        if !owned_uri.is_valid() {
            return Err(format!("Invalid MXC URI: {uri}"));
        }

        let source = matrix_sdk::ruma::events::room::MediaSource::Plain(owned_uri);

        manager.thumbnail(source, width, height)
    }));

    match result {
        Ok(Ok(mut data)) => {
            let len = data.len();

            if len == 0 {
                return 0;
            }

            let ptr = data.as_mut_ptr();
            std::mem::forget(data);

            *data_out = ptr;
            *len_out = len;

            1
        }

        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }

        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while downloading thumbnail",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_media_thumbnail(
    handle: *mut MatrixClientManager,
    media_source_json: *const c_char,
    width: u32,
    height: u32,
    data_out: *mut *mut u8,
    len_out: *mut usize,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if !data_out.is_null() {
        *data_out = ptr::null_mut();
    }

    if !len_out.is_null() {
        *len_out = 0;
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }

    if media_source_json.is_null() {
        set_error(error_out, "Media source JSON must not be null");
        return -1;
    }

    if data_out.is_null() || len_out.is_null() {
        set_error(error_out, "Media output pointers must not be null");
        return -1;
    }

    if width == 0 || height == 0 {
        set_error(error_out, "Thumbnail dimensions must be greater than zero");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = &*handle;

        let source_json = CStr::from_ptr(media_source_json)
            .to_str()
            .map_err(|_| "Media source JSON is not valid UTF-8".to_string())?;

        let source: matrix_sdk::ruma::events::room::MediaSource =
            serde_json::from_str(source_json)
                .map_err(|e| format!("Invalid Matrix media source: {e}"))?;

        manager.thumbnail(source, width, height)
    }));

    match result {
        Ok(Ok(mut data)) => {
            let len = data.len();

            if len == 0 {
                return 0;
            }

            let data_ptr = data.as_mut_ptr();
            std::mem::forget(data);

            *data_out = data_ptr;
            *len_out = len;

            1
        }

        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }

        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while downloading media thumbnail",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_image(
    handle: *mut MatrixClientManager,
    media_source_json: *const c_char,
    data_out: *mut *mut u8,
    len_out: *mut usize,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if !data_out.is_null() {
        *data_out = ptr::null_mut();
    }

    if !len_out.is_null() {
        *len_out = 0;
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }

    if media_source_json.is_null() {
        set_error(error_out, "Media source JSON must not be null");
        return -1;
    }

    if data_out.is_null() || len_out.is_null() {
        set_error(error_out, "Media output pointers must not be null");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = &*handle;

        let source_json = CStr::from_ptr(media_source_json)
            .to_str()
            .map_err(|_| "Media source JSON is not valid UTF-8".to_string())?;

        let source: matrix_sdk::ruma::events::room::MediaSource =
            serde_json::from_str(source_json)
                .map_err(|e| format!("Invalid Matrix media source: {e}"))?;

        manager.image(source)
    }));

    match result {
        Ok(Ok(mut data)) => {
            let len = data.len();

            if len == 0 {
                return 0;
            }

            let data_ptr = data.as_mut_ptr();
            std::mem::forget(data);

            *data_out = data_ptr;
            *len_out = len;

            1
        }

        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }

        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while downloading image",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_video(
    handle: *mut MatrixClientManager,
    media_source_json: *const c_char,
    data_out: *mut *mut u8,
    len_out: *mut usize,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if !data_out.is_null() {
        *data_out = ptr::null_mut();
    }

    if !len_out.is_null() {
        *len_out = 0;
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }

    if media_source_json.is_null() {
        set_error(error_out, "Media source JSON must not be null");
        return -1;
    }

    if data_out.is_null() || len_out.is_null() {
        set_error(error_out, "Media output pointers must not be null");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = &*handle;

        let source_json = CStr::from_ptr(media_source_json)
            .to_str()
            .map_err(|_| "Media source JSON is not valid UTF-8".to_string())?;

        let source: matrix_sdk::ruma::events::room::MediaSource =
            serde_json::from_str(source_json)
                .map_err(|e| format!("Invalid Matrix media source: {e}"))?;

        manager.video(source)
    }));

    match result {
        Ok(Ok(mut data)) => {
            let len = data.len();

            if len == 0 {
                return 0;
            }

            let data_ptr = data.as_mut_ptr();
            std::mem::forget(data);

            *data_out = data_ptr;
            *len_out = len;

            1
        }

        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }

        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while downloading video",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_media_bytes_free(data: *mut u8, len: usize) {
    if !data.is_null() {
        drop(Vec::from_raw_parts(data, len, len));
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