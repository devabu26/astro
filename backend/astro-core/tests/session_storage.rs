use matrix_sdk::authentication::matrix::MatrixSession;
use matrix_sdk::authentication::SessionTokens;
use matrix_sdk::ruma::{device_id, user_id};
use matrix_sdk::SessionMeta;
use std::fs;
use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};

const TEST_HOMESERVER: &str = "https://matrix.example.org";
const TEST_STORE_ID: &str = "a57e-1234-abcd";

fn test_path() -> PathBuf {
    let id = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();

    std::env::temp_dir().join(format!("astro-session-test-{id}.bin"))
}

fn test_session() -> MatrixSession {
    MatrixSession {
        meta: SessionMeta {
            user_id: user_id!("@astro:test.example.org").to_owned(),
            device_id: device_id!("ASTRO_TEST_DEVICE").to_owned(),
        },
        tokens: SessionTokens {
            access_token: "dummy-access-token".to_string(),
            refresh_token: Some("dummy-refresh-token".to_string()),
        },
    }
}

#[test]
fn session_round_trip_and_wrong_passphrase() {
    let path = test_path();
    let original = test_session();

    astro_core::session_storage::save_session(
        &path,
        "correct-test-passphrase",
        TEST_HOMESERVER,
        TEST_STORE_ID,
        &original,
    )
    .unwrap();

    let restored = astro_core::session_storage::load_session(
        &path,
        "correct-test-passphrase",
    )
    .unwrap()
    .expect("session should be restored");

    assert_eq!(restored.homeserver, TEST_HOMESERVER);
    assert_eq!(restored.store_id.as_deref(), Some(TEST_STORE_ID));
    assert_eq!(restored.session, original);

    let wrong_passphrase =
        astro_core::session_storage::load_session(&path, "incorrect-test-passphrase");

    assert!(wrong_passphrase.is_err());

    astro_core::session_storage::delete_session(&path).unwrap();
}

#[test]
fn session_file_does_not_contain_plaintext_secrets() {
    let path = test_path();
    let session = test_session();

    astro_core::session_storage::save_session(
        &path,
        "correct-test-passphrase",
        TEST_HOMESERVER,
        TEST_STORE_ID,
        &session,
    )
    .unwrap();

    let contents = fs::read(&path).unwrap();

    for secret in [
        "dummy-access-token",
        "dummy-refresh-token",
        TEST_HOMESERVER,
        TEST_STORE_ID,
    ] {
        assert!(
            !contents
                .windows(secret.len())
                .any(|window| window == secret.as_bytes()),
            "Session file contains plaintext: {secret}"
        );
    }

    astro_core::session_storage::delete_session(&path).unwrap();
}

#[test]
fn session_file_delete_is_idempotent() {
    let path = test_path();

    astro_core::session_storage::delete_session(&path).unwrap();
    astro_core::session_storage::delete_session(&path).unwrap();

    assert!(!path.exists());
}

#[test]
fn missing_session_returns_none() {
    let path = test_path();

    let result =
        astro_core::session_storage::load_session(&path, "test-passphrase").unwrap();

    assert!(result.is_none());
}

#[test]
fn empty_homeserver_is_rejected() {
    let path = test_path();

    let result = astro_core::session_storage::save_session(
        &path,
        "test-passphrase",
        "",
        TEST_STORE_ID,
        &test_session(),
    );

    assert!(result.is_err());
    assert!(!path.exists());
}

#[test]
fn invalid_store_id_is_rejected() {
    let path = test_path();

    let result = astro_core::session_storage::save_session(
        &path,
        "test-passphrase",
        TEST_HOMESERVER,
        "invalid-store-id",
        &test_session(),
    );

    assert!(result.is_err());
    assert!(!path.exists());
}
