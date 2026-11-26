use crate::matrix_client::MatrixClientManager;
use crate::storage::StorageManager;
use livekit::webrtc::video_frame::I420Buffer;

use std::ffi::{c_char, c_void, CStr, CString};
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
    crate::init_tracing();

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
    crate::init_tracing();

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
pub unsafe extern "C" fn astro_matrix_register_push_token(
    handle: *mut MatrixClientManager,
    push_token: *const c_char,
    privacy_level: *const c_char,
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
        let push_token = unsafe { read_input(push_token, "Ubuntu push token")? };
        let privacy_level = unsafe {
            read_input(privacy_level, "notification privacy level")?
        };

        if push_token.is_empty() {
            return Err("Ubuntu push token must not be empty".to_string());
        }

        let manager = unsafe { &*handle };
        manager.register_push_token(push_token, privacy_level)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic during push registration");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_unregister_push_token(
    handle: *mut MatrixClientManager,
    push_token: *const c_char,
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
        let push_token = unsafe { read_input(push_token, "Ubuntu push token")? };

        if push_token.is_empty() {
            return Err("Ubuntu push token must not be empty".to_string());
        }

        let manager = unsafe { &*handle };
        manager.unregister_push_token(push_token)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic during push unregistration");
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
pub unsafe extern "C" fn astro_matrix_send_room_attachment(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    file_path: *const c_char,
    content_type: *const c_char,
    caption: *const c_char,
    progress_callback: Option<extern "C" fn(*mut c_void, usize, usize)>,
    progress_context: *mut c_void,
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
        let file_path = read_input(file_path, "Attachment file path")?;
        let content_type = read_input(content_type, "Attachment MIME type")?;
        let caption = read_input(caption, "Attachment caption")?;

        let manager = &*handle;

        manager.send_room_attachment(
            room_id,
            file_path,
            content_type,
            caption,
            progress_callback,
            progress_context,
        )
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
                "Unexpected Rust panic while sending Matrix attachment",
            );
            ptr::null_mut()
        }
    }
}


#[no_mangle]
pub unsafe extern "C" fn astro_matrix_join_rtc_room(
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).join_rtc_room(room_id)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while joining MatrixRTC presence");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_leave_rtc_room(
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).leave_rtc_room(room_id)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while leaving MatrixRTC presence");
            -1
        }
    }
}


#[no_mangle]
pub unsafe extern "C" fn astro_matrix_start_rtc_audio(
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).start_rtc_audio(room_id)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while starting LiveKit audio");
            -1
        }
    }
}

// ASTRO_RTC_VIDEO_STAGE1
#[no_mangle]
pub unsafe extern "C" fn astro_matrix_start_rtc_video(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    width: u32,
    height: u32,
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).start_rtc_video(room_id, width, height)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while starting LiveKit video");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_start_rtc_screenshare(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    width: u32,
    height: u32,
    include_device_audio: i32,
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).start_rtc_screenshare(room_id, width, height, include_device_audio != 0)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while starting LiveKit screen share");
            -1
        }
    }
}

// ASTRO_RTC_VIDEO_STAGE2
#[no_mangle]
pub unsafe extern "C" fn astro_matrix_push_rtc_video_frame(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    width: u32,
    height: u32,
    frame_data: *const u8,
    frame_data_len: u64,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }

    if frame_data.is_null() {
        set_error(error_out, "Video frame data must not be null");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let room_id = read_input(room_id, "Room ID")?;

        if width < 2 || height < 2
            || width > 1920 || height > 1080
            || width % 2 != 0 || height % 2 != 0
        {
            return Err("Invalid video frame dimensions".to_string());
        }

        let y_size = (width as usize)
            .checked_mul(height as usize)
            .ok_or_else(|| "Video frame dimensions overflow".to_string())?;
        let expected_size = y_size
            .checked_add(y_size / 2)
            .ok_or_else(|| "Video frame size overflow".to_string())?;

        let supplied_size = usize::try_from(frame_data_len)
            .map_err(|_| "Video frame is too large".to_string())?;

        if supplied_size != expected_size {
            return Err(format!(
                "Invalid I420 frame size: expected {expected_size} bytes, received {supplied_size}"
            ));
        }

        // The caller must keep frame_data valid for the duration of this call.
        // push_rtc_video_frame copies the data into a LiveKit-owned buffer.
        let bytes = std::slice::from_raw_parts(frame_data, supplied_size);

        (&mut *handle).push_rtc_video_frame(
            room_id,
            width,
            height,
            bytes,
        )
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while submitting LiveKit video frame");
            -1
        }
    }
}

// Opaque Rust-owned I420 allocation used by the Mir screen capture path.
// C++ writes into the returned planes, then transfers this object to submit.
// submit consumes it on every path because LiveKit may retain the frame.
pub struct AstroRtcI420Frame {
    buffer: Option<I420Buffer>,
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_create_rtc_screenshare_frame(
    width: u32,
    height: u32,
    frame_out: *mut *mut AstroRtcI420Frame,
    plane_y_out: *mut *mut u8,
    stride_y_out: *mut u32,
    plane_u_out: *mut *mut u8,
    stride_u_out: *mut u32,
    plane_v_out: *mut *mut u8,
    stride_v_out: *mut u32,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() { *error_out = ptr::null_mut(); }
    if !frame_out.is_null() { *frame_out = ptr::null_mut(); }
    if !plane_y_out.is_null() { *plane_y_out = ptr::null_mut(); }
    if !plane_u_out.is_null() { *plane_u_out = ptr::null_mut(); }
    if !plane_v_out.is_null() { *plane_v_out = ptr::null_mut(); }
    if !stride_y_out.is_null() { *stride_y_out = 0; }
    if !stride_u_out.is_null() { *stride_u_out = 0; }
    if !stride_v_out.is_null() { *stride_v_out = 0; }

    if frame_out.is_null() || plane_y_out.is_null() || stride_y_out.is_null()
        || plane_u_out.is_null() || stride_u_out.is_null()
        || plane_v_out.is_null() || stride_v_out.is_null()
    {
        set_error(error_out, "Screen-share frame output pointers must not be null");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| -> Result<*mut AstroRtcI420Frame, String> {
        let pixels = (width as u64).checked_mul(height as u64)
            .ok_or_else(|| "Screen-share frame dimensions overflow".to_string())?;
        if width < 2 || height < 2 || width > 1920 || height > 1920
            || width % 2 != 0 || height % 2 != 0 || pixels > 2_073_600
        {
            return Err("Screen-share frame dimensions must be even, no larger than 1920 pixels per side, and within a 1080p pixel budget".into());
        }

        let mut buffer = I420Buffer::new(width, height);
        let (stride_y, stride_u, stride_v) = buffer.strides();
        let (plane_y, plane_u, plane_v) = buffer.data_mut();
        let min_y = (stride_y as usize).checked_mul(height as usize)
            .ok_or_else(|| "I420 luma plane size overflow".to_string())?;
        let chroma_height = (height / 2) as usize;
        let min_u = (stride_u as usize).checked_mul(chroma_height)
            .ok_or_else(|| "I420 U plane size overflow".to_string())?;
        let min_v = (stride_v as usize).checked_mul(chroma_height)
            .ok_or_else(|| "I420 V plane size overflow".to_string())?;
        if plane_y.len() < min_y || plane_u.len() < min_u || plane_v.len() < min_v {
            return Err("LiveKit allocated an unexpectedly small I420 frame".into());
        }

        *plane_y_out = plane_y.as_mut_ptr();
        *stride_y_out = stride_y;
        *plane_u_out = plane_u.as_mut_ptr();
        *stride_u_out = stride_u;
        *plane_v_out = plane_v.as_mut_ptr();
        *stride_v_out = stride_v;

        Ok(Box::into_raw(Box::new(AstroRtcI420Frame { buffer: Some(buffer) })))
    }));

    match result {
        Ok(Ok(frame)) => {
            *frame_out = frame;
            0
        }
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while allocating LiveKit screen-share frame");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_free_rtc_screenshare_frame(
    frame: *mut AstroRtcI420Frame,
) {
    if !frame.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(frame))));
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_submit_rtc_screenshare_frame(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    frame: *mut AstroRtcI420Frame,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() { *error_out = ptr::null_mut(); }
    if frame.is_null() {
        set_error(error_out, "Screen-share I420 frame must not be null");
        return -1;
    }

    // Consume immediately, including error paths.
    let frame = Box::from_raw(frame);
    if handle.is_null() {
        drop(frame);
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let room_id = read_input(room_id, "Room ID")?;
        let buffer = frame.buffer
            .ok_or_else(|| "Screen-share I420 frame was already consumed".to_string())?;
        (&mut *handle).push_rtc_screenshare_i420_buffer(room_id, buffer)
    }));

    // On success the buffer has moved into VideoFrame. On failure it is
    // released when the owned wrapper is dropped here.
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while submitting LiveKit screen-share frame");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_stop_rtc_video(
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).stop_rtc_video(room_id)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while stopping LiveKit video");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_push_rtc_screenshare_audio_frame(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    pcm_data: *const i16,
    pcm_samples: u64,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }
    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }
    if pcm_data.is_null() {
        set_error(error_out, "Screen-share audio data must not be null");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let room_id = read_input(room_id, "Room ID")?;
        let sample_count = usize::try_from(pcm_samples)
            .map_err(|_| "Screen-share audio frame is too large".to_string())?;
        if sample_count != 960 {
            return Err(format!(
                "Invalid screen-share audio frame: expected 960 samples, received {sample_count}"
            ));
        }
        let samples = std::slice::from_raw_parts(pcm_data, sample_count);
        (&mut *handle).push_rtc_screenshare_audio_frame(room_id, samples)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while submitting screen-share audio");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_stop_rtc_screenshare(
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).stop_rtc_screenshare(room_id)
    }));

    match result {
        Ok(Ok(true)) => 1,
        Ok(Ok(false)) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while starting asynchronous screen-share teardown");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_poll_rtc_screenshare_stop(
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).poll_rtc_screenshare_stop(room_id)
    }));

    match result {
        Ok(Ok(None)) => 1,
        Ok(Ok(Some(Ok(())))) => 0,
        Ok(Ok(Some(Err(message)))) | Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while polling screen-share teardown");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_stop_rtc_audio(
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).stop_rtc_audio(room_id)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while stopping LiveKit audio");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_set_rtc_audio_muted(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    muted: i32,
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
        let room_id = read_input(room_id, "Room ID")?;
        (&mut *handle).set_rtc_audio_muted(room_id, muted != 0)
    }));

    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while changing LiveKit microphone mute state");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_toggle_room_reaction(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    event_id: *const c_char,
    reaction_key: *const c_char,
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
        let room_id = read_input(room_id, "Room ID")?;
        let event_id = read_input(event_id, "Event ID")?;
        let reaction_key = read_input(reaction_key, "Reaction key")?;

        let manager = &*handle;
        manager.toggle_room_reaction(room_id, event_id, reaction_key)
    }));

    match result {
        Ok(Ok(added)) => {
            if added {
                1
            } else {
                0
            }
        }
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while toggling Matrix reaction",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_send_room_reply(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    event_id: *const c_char,
    message: *const c_char,
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
        let room_id = read_input(room_id, "Room ID")?;
        let event_id = read_input(event_id, "Event ID")?;
        let message = read_input(message, "Message")?;
        (&*handle).send_room_reply(room_id, event_id, message)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while sending Matrix reply");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_edit_room_message(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    event_id: *const c_char,
    message: *const c_char,
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
        let room_id = read_input(room_id, "Room ID")?;
        let event_id = read_input(event_id, "Event ID")?;
        let message = read_input(message, "Message")?;
        (&*handle).edit_room_message(room_id, event_id, message)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while editing Matrix message");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_redact_room_event(
    handle: *mut MatrixClientManager,
    room_id: *const c_char,
    event_id: *const c_char,
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
        let room_id = read_input(room_id, "Room ID")?;
        let event_id = read_input(event_id, "Event ID")?;
        (&*handle).redact_room_event(room_id, event_id)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while redacting Matrix event");
            -1
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
            set_error(
                error_out,
                "Unexpected Rust panic while accepting verification",
            );
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
            set_error(
                error_out,
                "Unexpected Rust panic while cancelling verification",
            );
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
            set_error(
                error_out,
                "Unexpected Rust panic while confirming verification",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_rename_device(
    handle: *mut MatrixClientManager,
    device_id: *const c_char,
    display_name: *const c_char,
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
        let device_id = read_input(device_id, "Device ID")?;
        let display_name = read_input(display_name, "Device display name")?;
        let manager = unsafe { &*handle };
        manager.rename_device(device_id, display_name)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while renaming Matrix device",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_revoke_device(
    handle: *mut MatrixClientManager,
    device_id: *const c_char,
    password: *const c_char,
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
        let device_id = read_input(device_id, "Device ID")?;
        let password = read_input(password, "Account password")?;
        let manager = unsafe { &*handle };
        manager.revoke_device(device_id, password)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while signing out Matrix device",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_request_device_verification(
    handle: *mut MatrixClientManager,
    device_id: *const c_char,
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
        let device_id = read_input(device_id, "Device ID")?;
        let manager = unsafe { &*handle };
        manager.request_device_verification(device_id)
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while starting device verification",
            );
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
            set_error(
                error_out,
                "Unexpected Rust panic while rejecting verification",
            );
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_get_account_profile_json(
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
        let manager = unsafe { &*handle };
        manager.account_profile_json()
    }));

    match result {
        Ok(Ok(json)) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(error_out, "Account profile contained an invalid null character");
                ptr::null_mut()
            }
        },
        Ok(Err(message)) => {
            set_error(error_out, &message);
            ptr::null_mut()
        }
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while retrieving Matrix account profile");
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_get_devices_json(
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
        let manager = unsafe { &*handle };
        manager.devices_json()
    }));

    match result {
        Ok(Ok(json)) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(error_out, "Device data contained an invalid null character");
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
                "Unexpected Rust panic while retrieving Matrix devices",
            );
            ptr::null_mut()
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
pub unsafe extern "C" fn astro_matrix_refresh_room_timeline_json(
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
        manager.refresh_room_timeline_json(room_id)
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
                "Unexpected Rust panic while refreshing Matrix timeline",
            );
            ptr::null_mut()
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
pub unsafe extern "C" fn astro_matrix_poll_rtc_memberships_json(
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
        manager.poll_rtc_memberships_json()
    }));

    match result {
        Ok(json) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(
                    error_out,
                    "MatrixRTC membership data contained an invalid null character",
                );
                ptr::null_mut()
            }
        },
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while polling MatrixRTC memberships",
            );
            ptr::null_mut()
        }
    }
}


#[no_mangle]
pub unsafe extern "C" fn astro_matrix_poll_rtc_audio_events_json(
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
        manager.poll_rtc_audio_events_json()
    }));

    match result {
        Ok(json) => match CString::new(json) {
            Ok(value) => value.into_raw(),
            Err(_) => {
                set_error(
                    error_out,
                    "LiveKit audio event data contained an invalid null character",
                );
                ptr::null_mut()
            }
        },
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while polling LiveKit audio events",
            );
            ptr::null_mut()
        }
    }
}

#[repr(C)]
pub struct AstroRtcVideoFrame {
    pub room_id: *mut c_char,
    pub identity: *mut c_char,
    pub source: *mut c_char,
    pub width: u32,
    pub height: u32,
    pub data: *mut u8,
    pub len: usize,
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_take_rtc_video_frame(
    handle: *mut MatrixClientManager,
    error_out: *mut *mut c_char,
) -> *mut AstroRtcVideoFrame {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }
    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return ptr::null_mut();
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = unsafe { &mut *handle };
        manager.take_rtc_video_frame()
    }));

    match result {
        Ok(Some((room_id, identity, source, width, height, data))) => {
            let room_id = match CString::new(room_id) {
                Ok(value) => value.into_raw(),
                Err(_) => {
                    set_error(error_out, "Video frame room ID contained a null byte");
                    return ptr::null_mut();
                }
            };
            let identity = match CString::new(identity) {
                Ok(value) => value.into_raw(),
                Err(_) => {
                    unsafe { drop(CString::from_raw(room_id)); }
                    set_error(error_out, "Video frame identity contained a null byte");
                    return ptr::null_mut();
                }
            };
            let source = match CString::new(source) {
                Ok(value) => value.into_raw(),
                Err(_) => {
                    unsafe {
                        drop(CString::from_raw(room_id));
                        drop(CString::from_raw(identity));
                    }
                    set_error(error_out, "Video frame source contained a null byte");
                    return ptr::null_mut();
                }
            };
            let mut data = data.into_boxed_slice();
            let len = data.len();
            let data_ptr = Box::into_raw(data) as *mut u8;
            Box::into_raw(Box::new(AstroRtcVideoFrame {
                room_id,
                identity,
                source,
                width,
                height,
                data: data_ptr,
                len,
            }))
        }
        Ok(None) => ptr::null_mut(),
        Err(_) => {
            set_error(error_out, "Unexpected Rust panic while retrieving a LiveKit video frame");
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_rtc_video_frame_free(
    frame: *mut AstroRtcVideoFrame,
) {
    if frame.is_null() {
        return;
    }
    let frame = unsafe { Box::from_raw(frame) };
    if !frame.room_id.is_null() {
        unsafe { drop(CString::from_raw(frame.room_id)); }
    }
    if !frame.identity.is_null() {
        unsafe { drop(CString::from_raw(frame.identity)); }
    }
    if !frame.source.is_null() {
        unsafe { drop(CString::from_raw(frame.source)); }
    }
    if !frame.data.is_null() {
        let slice = ptr::slice_from_raw_parts_mut(frame.data, frame.len);
        unsafe { drop(Box::from_raw(slice)); }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_resume_sync(
    handle: *mut MatrixClientManager,
    error_out: *mut *mut c_char,
) -> i32 {
    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return 0;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let manager = unsafe { &mut *handle };
        manager.resume_sync()
    }));

    match result {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            0
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while resuming Matrix sync",
            );
            0
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

        let source: matrix_sdk::ruma::events::room::MediaSource = serde_json::from_str(source_json)
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

        let source: matrix_sdk::ruma::events::room::MediaSource = serde_json::from_str(source_json)
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
            set_error(error_out, "Unexpected Rust panic while downloading image");
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn astro_matrix_device_management_url(
    handle: *mut MatrixClientManager,
    device_id: *const c_char,
    url_out: *mut *mut c_char,
    error_out: *mut *mut c_char,
) -> i32 {
    if !url_out.is_null() {
        *url_out = ptr::null_mut();
    }

    if !error_out.is_null() {
        *error_out = ptr::null_mut();
    }

    if handle.is_null() {
        set_error(error_out, "Matrix client handle must not be null");
        return -1;
    }

    if url_out.is_null() {
        set_error(error_out, "URL output pointer must not be null");
        return -1;
    }

    let result = catch_unwind(AssertUnwindSafe(|| {
        let device_id = read_input(device_id, "Device ID")?;
        let manager = unsafe { &*handle };
        manager.device_management_url(device_id)
    }));

    match result {
        Ok(Ok(Some(url))) => {
            match std::ffi::CString::new(url) {
                Ok(url) => {
                    unsafe {
                        *url_out = url.into_raw();
                    }
                    1
                }
                Err(_) => {
                    set_error(error_out, "Account-management URL contains invalid data");
                    -1
                }
            }
        }
        Ok(Ok(None)) => 0,
        Ok(Err(message)) => {
            set_error(error_out, &message);
            -1
        }
        Err(_) => {
            set_error(
                error_out,
                "Unexpected Rust panic while discovering device management support",
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

        let source: matrix_sdk::ruma::events::room::MediaSource = serde_json::from_str(source_json)
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
            set_error(error_out, "Unexpected Rust panic while downloading video");
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
