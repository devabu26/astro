use astro_core::storage::StorageManager;
use std::fs;
use std::path::PathBuf;

const TEST_PASSPHRASE: &str = "Astro-storage-manager-test-2026";

fn test_store_path() -> PathBuf {
    std::env::temp_dir().join(format!("astro-storage-manager-test-{}", std::process::id()))
}

#[test]
fn storage_manager_opens_reopens_and_rejects_wrong_passphrase() {
    let path = test_store_path();

    if path.exists() {
        fs::remove_dir_all(&path).unwrap();
    }

    // First opening.
    let manager =
        StorageManager::open(&path, TEST_PASSPHRASE).expect("Storage manager should open");

    // Close all stores and the runtime.
    drop(manager);

    // Reopening with the correct passphrase.
    let manager =
        StorageManager::open(&path, TEST_PASSPHRASE).expect("Storage manager should reopen");

    drop(manager);

    // A wrong passphrase must be rejected.
    let result = StorageManager::open(&path, "definitely-the-wrong-passphrase");

    assert!(
        result.is_err(),
        "Storage manager must reject a wrong passphrase"
    );

    fs::remove_dir_all(&path).unwrap();
}
