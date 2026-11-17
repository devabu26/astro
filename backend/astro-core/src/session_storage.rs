use matrix_sdk::authentication::matrix::MatrixSession;
use matrix_sdk_store_encryption::StoreCipher;
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

const MAGIC: &[u8; 8] = b"ASTROSES";
const VERSION: u32 = 3;
const LEGACY_VERSION: u32 = 2;
const HEADER_SIZE: usize = 20;
const MAX_FILE_SIZE: usize = 16 * 1024 * 1024;

pub struct SavedMatrixSession {
    pub homeserver: String,
    pub store_id: Option<String>,
    pub session: MatrixSession,
}

fn validate_store_id(id: &str) -> Result<(), String> {
    if id.is_empty() || id.len() > 80 || !id.bytes().all(|b| b.is_ascii_hexdigit() || b == b'-') {
        return Err("Invalid Matrix store identifier".into());
    }

    Ok(())
}

fn decode_file(data: &[u8]) -> Result<(u32, &[u8], &[u8]), String> {
    if data.len() < HEADER_SIZE || &data[..8] != MAGIC {
        return Err("Invalid Astro session file".into());
    }

    let version = u32::from_le_bytes(data[8..12].try_into().unwrap());

    if version != VERSION && version != LEGACY_VERSION {
        return Err(format!("Unsupported session file version: {version}"));
    }

    let export_len = u32::from_le_bytes(data[12..16].try_into().unwrap()) as usize;

    let session_len = u32::from_le_bytes(data[16..20].try_into().unwrap()) as usize;

    let expected_len = HEADER_SIZE
        .checked_add(export_len)
        .and_then(|n| n.checked_add(session_len))
        .ok_or("Invalid session file lengths")?;

    if expected_len != data.len() || expected_len > MAX_FILE_SIZE {
        return Err("Invalid session file lengths".into());
    }

    let export_end = HEADER_SIZE + export_len;

    Ok((version, &data[HEADER_SIZE..export_end], &data[export_end..]))
}

fn encode_file(export: &[u8], encrypted_session: &[u8]) -> Result<Vec<u8>, String> {
    let export_len = u32::try_from(export.len()).map_err(|_| "Cipher export is too large")?;

    let session_len =
        u32::try_from(encrypted_session.len()).map_err(|_| "Encrypted session is too large")?;

    let total = HEADER_SIZE
        .checked_add(export.len())
        .and_then(|n| n.checked_add(encrypted_session.len()))
        .ok_or("Session file is too large")?;

    if total > MAX_FILE_SIZE {
        return Err("Session file is too large".into());
    }

    let mut data = Vec::with_capacity(total);

    data.extend_from_slice(MAGIC);
    data.extend_from_slice(&VERSION.to_le_bytes());
    data.extend_from_slice(&export_len.to_le_bytes());
    data.extend_from_slice(&session_len.to_le_bytes());
    data.extend_from_slice(export);
    data.extend_from_slice(encrypted_session);

    Ok(data)
}

fn read_file(path: &Path) -> Result<Vec<u8>, String> {
    let mut file =
        fs::File::open(path).map_err(|error| format!("Could not read session file: {error}"))?;

    let size = file
        .metadata()
        .map_err(|error| format!("Could not inspect session file: {error}"))?
        .len();

    if size > MAX_FILE_SIZE as u64 {
        return Err("Session file is too large".into());
    }

    let mut data = Vec::with_capacity(size as usize);

    file.read_to_end(&mut data)
        .map_err(|error| format!("Could not read session file: {error}"))?;

    Ok(data)
}

fn write_atomic(path: &Path, data: &[u8]) -> Result<(), String> {
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));

    fs::create_dir_all(parent)
        .map_err(|error| format!("Could not create session directory: {error}"))?;

    let file_name = path
        .file_name()
        .ok_or("Invalid session file path")?
        .to_string_lossy();

    let temporary_path: PathBuf = parent.join(format!(".{file_name}.{}.tmp", std::process::id()));

    let result = (|| {
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);

        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }

        let mut file = options
            .open(&temporary_path)
            .map_err(|error| format!("Could not create temporary session file: {error}"))?;

        file.write_all(data)
            .map_err(|error| format!("Could not write session file: {error}"))?;

        file.sync_all()
            .map_err(|error| format!("Could not sync session file: {error}"))?;

        fs::rename(&temporary_path, path)
            .map_err(|error| format!("Could not replace session file: {error}"))?;

        if let Ok(directory) = fs::File::open(parent) {
            let _ = directory.sync_all();
        }

        Ok(())
    })();

    if result.is_err() {
        let _ = fs::remove_file(&temporary_path);
    }

    result
}

pub fn save_session(
    path: &Path,
    passphrase: &str,
    homeserver: &str,
    store_id: &str,
    session: &MatrixSession,
) -> Result<(), String> {
    if homeserver.trim().is_empty() {
        return Err("Homeserver URL must not be empty".into());
    }

    validate_store_id(store_id)?;

    let cipher = match fs::metadata(path) {
        Ok(_) => {
            let existing = read_file(path)?;
            let (_, export, _) = decode_file(&existing)?;

            StoreCipher::import(passphrase, export)
                .map_err(|error| format!("Could not unlock session encryption: {error}"))?
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => StoreCipher::new()
            .map_err(|error| format!("Could not create session encryption: {error}"))?,
        Err(error) => {
            return Err(format!("Could not inspect session file: {error}"));
        }
    };

    let export = cipher
        .export(passphrase)
        .map_err(|error| format!("Could not protect session encryption key: {error}"))?;

    // The homeserver, store identifier and Matrix session are encrypted together.
    let payload = (homeserver.to_owned(), store_id.to_owned(), session.clone());

    let encrypted_session = cipher
        .encrypt_value(&payload)
        .map_err(|error| format!("Could not encrypt Matrix session: {error}"))?;

    let data = encode_file(&export, &encrypted_session)?;

    write_atomic(path, &data)
}

pub fn load_session(path: &Path, passphrase: &str) -> Result<Option<SavedMatrixSession>, String> {
    let mut file = match fs::File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Ok(None);
        }
        Err(error) => {
            return Err(format!("Could not read session file: {error}"));
        }
    };

    let size = file
        .metadata()
        .map_err(|error| format!("Could not inspect session file: {error}"))?
        .len();

    if size > MAX_FILE_SIZE as u64 {
        return Err("Session file is too large".into());
    }

    let mut data = Vec::with_capacity(size as usize);

    file.read_to_end(&mut data)
        .map_err(|error| format!("Could not read session file: {error}"))?;

    let (version, export, encrypted_session) = decode_file(&data)?;

    let cipher = StoreCipher::import(passphrase, export)
        .map_err(|_| "Incorrect passphrase or damaged session file".to_string())?;

    let (homeserver, store_id, session) = match version {
        LEGACY_VERSION => {
            // Version 2 sessions used the original application data directory.
            let payload: (String, MatrixSession) = cipher
                .decrypt_value(encrypted_session)
                .map_err(|_| "Could not decrypt Matrix session".to_string())?;

            (payload.0, None, payload.1)
        }
        VERSION => {
            let payload: (String, String, MatrixSession) = cipher
                .decrypt_value(encrypted_session)
                .map_err(|_| "Could not decrypt Matrix session".to_string())?;

            validate_store_id(&payload.1)?;

            (payload.0, Some(payload.1), payload.2)
        }
        _ => {
            return Err("Unsupported session file version".into());
        }
    };

    Ok(Some(SavedMatrixSession {
        homeserver,
        store_id,
        session,
    }))
}

pub fn delete_session(path: &Path) -> Result<(), String> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(format!("Could not delete Matrix session: {error}")),
    }
}
