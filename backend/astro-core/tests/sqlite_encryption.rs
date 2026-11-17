use matrix_sdk::{
    SqliteCryptoStore, SqliteEventCacheStore, SqliteMediaStore, SqliteStateStore, SqliteStoreConfig,
};
use std::fs;
use std::path::PathBuf;

const TEST_PASSPHRASE: &str = "Astro-test-only-passphrase-2026";

fn test_store_path() -> PathBuf {
    std::env::temp_dir().join(format!(
        "astro-sqlite-encryption-test-{}",
        std::process::id()
    ))
}

#[tokio::test]
async fn all_sqlite_stores_reject_wrong_passphrase() {
    let path = test_store_path();

    if path.exists() {
        fs::remove_dir_all(&path).unwrap();
    }

    fs::create_dir_all(&path).unwrap();

    // Open all four stores with the same passphrase.
    let config = SqliteStoreConfig::with_low_memory_config(&path).passphrase(Some(TEST_PASSPHRASE));

    let state = SqliteStateStore::open_with_config(&config)
        .await
        .expect("State store should open");

    let events = SqliteEventCacheStore::open_with_config(&config)
        .await
        .expect("Event cache store should open");

    let media = SqliteMediaStore::open_with_config(&config)
        .await
        .expect("Media store should open");

    let crypto = SqliteCryptoStore::open_with_config(&config)
        .await
        .expect("Crypto store should open");

    drop((state, events, media, crypto));

    // Reopen all four stores with the correct passphrase.
    let config = SqliteStoreConfig::with_low_memory_config(&path).passphrase(Some(TEST_PASSPHRASE));

    let state = SqliteStateStore::open_with_config(&config)
        .await
        .expect("State store should reopen");

    let events = SqliteEventCacheStore::open_with_config(&config)
        .await
        .expect("Event cache store should reopen");

    let media = SqliteMediaStore::open_with_config(&config)
        .await
        .expect("Media store should reopen");

    let crypto = SqliteCryptoStore::open_with_config(&config)
        .await
        .expect("Crypto store should reopen");

    drop((state, events, media, crypto));

    // A wrong passphrase must be rejected.
    let config = SqliteStoreConfig::with_low_memory_config(&path)
        .passphrase(Some("definitely-the-wrong-passphrase"));

    assert!(
        SqliteStateStore::open_with_config(&config).await.is_err(),
        "State store must reject a wrong passphrase"
    );

    assert!(
        SqliteEventCacheStore::open_with_config(&config)
            .await
            .is_err(),
        "Event cache store must reject a wrong passphrase"
    );

    assert!(
        SqliteMediaStore::open_with_config(&config).await.is_err(),
        "Media store must reject a wrong passphrase"
    );

    assert!(
        SqliteCryptoStore::open_with_config(&config).await.is_err(),
        "Crypto store must reject a wrong passphrase"
    );

    fs::remove_dir_all(&path).unwrap();
}
