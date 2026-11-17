use matrix_sdk::{
    SqliteCryptoStore, SqliteEventCacheStore, SqliteMediaStore, SqliteStateStore, SqliteStoreConfig,
};
use std::path::Path;
use tokio::runtime::{Builder, Runtime};

struct SqliteStores {
    _state: SqliteStateStore,
    _events: SqliteEventCacheStore,
    _media: SqliteMediaStore,
    _crypto: SqliteCryptoStore,
}

pub struct StorageManager {
    stores: Option<SqliteStores>,
    runtime: Runtime,
}

impl StorageManager {
    pub fn open(path: &Path, passphrase: &str) -> Result<Self, String> {
        let runtime = Builder::new_multi_thread()
            .enable_all()
            .build()
            .map_err(|error| format!("Could not create Tokio runtime: {error}"))?;

        let config = SqliteStoreConfig::with_low_memory_config(path).passphrase(Some(passphrase));

        let stores = runtime.block_on(async {
            let state = SqliteStateStore::open_with_config(&config)
                .await
                .map_err(|error| format!("Could not open state store: {error}"))?;

            let events = SqliteEventCacheStore::open_with_config(&config)
                .await
                .map_err(|error| format!("Could not open event cache: {error}"))?;

            let media = SqliteMediaStore::open_with_config(&config)
                .await
                .map_err(|error| format!("Could not open media store: {error}"))?;

            let crypto = SqliteCryptoStore::open_with_config(&config)
                .await
                .map_err(|error| format!("Could not open crypto store: {error}"))?;

            Ok::<_, String>(SqliteStores {
                _state: state,
                _events: events,
                _media: media,
                _crypto: crypto,
            })
        })?;

        Ok(Self {
            stores: Some(stores),
            runtime,
        })
    }
}

impl Drop for StorageManager {
    fn drop(&mut self) {
        // SQLite pools must be dropped while their Tokio runtime is active.
        let stores = self.stores.take();

        self.runtime.block_on(async move {
            drop(stores);
        });
    }
}
