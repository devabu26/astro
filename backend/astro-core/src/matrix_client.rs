use matrix_sdk::{
    config::SyncSettings,
    ruma::room::RoomType,
    store::RoomLoadSettings,
    Client,
    SqliteStoreConfig,
};
use matrix_sdk_ui::spaces::SpaceService;
use std::time::Duration;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::runtime::{Builder, Runtime};

use crate::media::MediaManager;
use crate::session_storage::{delete_session, load_session, save_session};

static STORE_COUNTER: AtomicU64 = AtomicU64::new(0);

pub struct MatrixClientManager {
    client: Option<Client>,
    runtime: Runtime,
    session_path: PathBuf,
}

fn create_new_store(database_path: &Path) -> Result<(String, PathBuf), String> {
    let stores_root = database_path.join("matrix-stores");

    fs::create_dir_all(&stores_root)
        .map_err(|error| format!("Could not create Matrix store directory: {error}"))?;

    for _ in 0..10 {
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|error| format!("System clock error: {error}"))?
            .as_nanos();

        let counter = STORE_COUNTER.fetch_add(1, Ordering::Relaxed);

        let store_id = format!("{:x}-{:x}-{:x}", timestamp, std::process::id(), counter);

        let store_path = stores_root.join(&store_id);

        match fs::create_dir(&store_path) {
            Ok(()) => return Ok((store_id, store_path)),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
                continue;
            }
            Err(error) => {
                return Err(format!("Could not create Matrix store: {error}"));
            }
        }
    }

    Err("Could not generate a unique Matrix store directory".into())
}

impl MatrixClientManager {
    fn create(
        database_path: &Path,
        passphrase: &str,
        homeserver: &str,
        session_path: PathBuf,
    ) -> Result<Self, String> {
        eprintln!("ASTRO DEBUG: Creating Tokio runtime");

        let runtime = Builder::new_multi_thread()
            .enable_all()
            .build()
            .map_err(|e| format!("Could not create Tokio runtime: {e}"))?;

        eprintln!("ASTRO DEBUG: Tokio runtime created");

        let config =
            SqliteStoreConfig::with_low_memory_config(database_path).passphrase(Some(passphrase));

        eprintln!("ASTRO DEBUG: Starting Matrix client build");

        let client = runtime.block_on(async {
            Client::builder()
                .homeserver_url(homeserver)
                .handle_refresh_tokens()
                .sqlite_store_with_config_and_cache_path(config, Some(database_path))
                .build()
                .await
                .map_err(|e| format!("Could not create Matrix client: {e}"))
        })?;

        eprintln!("ASTRO DEBUG: Matrix client build completed");

        Ok(Self {
            client: Some(client),
            runtime,
            session_path,
        })
    }

    pub fn login(
        database_path: &Path,
        passphrase: &str,
        homeserver: &str,
        username: &str,
        password: &str,
    ) -> Result<Self, String> {
        eprintln!("ASTRO DEBUG: Starting Matrix login");

        // Every new login gets a separate SDK store and device identity.
        let (store_id, store_path) = create_new_store(database_path)?;

        eprintln!("ASTRO DEBUG: Created a new Matrix store");

        let session_path = database_path.join("astro-session.bin");

        let manager = Self::create(&store_path, passphrase, homeserver, session_path)?;

        eprintln!("ASTRO DEBUG: Matrix client created for login");

        let client = manager.client.as_ref().unwrap();

        let login_result = manager.runtime.block_on(async {
            eprintln!("ASTRO DEBUG: Sending Matrix login request");

            client
                .matrix_auth()
                .login_username(username, password)
                .initial_device_display_name("Astro")
                .request_refresh_token()
                .send()
                .await
        });

        match login_result {
            Ok(response) => {
                eprintln!("ASTRO DEBUG: Matrix login request completed");

                // Do not log the response: it may contain session credentials.
                drop(response);
            }
            Err(error) => {
                eprintln!("ASTRO DEBUG: Matrix login request returned an error: {error}");
                return Err(format!("Matrix login failed: {error}"));
            }
        }

        let session = client
            .matrix_auth()
            .session()
            .ok_or_else(|| "Matrix login returned no session".to_string())?;

        eprintln!("ASTRO DEBUG: Matrix session obtained");

        manager.runtime.block_on(async {
            save_session(
                &manager.session_path,
                passphrase,
                homeserver,
                &store_id,
                &session,
            )
        })?;

        eprintln!("ASTRO DEBUG: Matrix session saved");
        eprintln!("ASTRO DEBUG: Matrix login completed successfully");

        Ok(manager)
    }

    pub fn restore(database_path: &Path, passphrase: &str) -> Result<Option<Self>, String> {
        eprintln!("ASTRO DEBUG: Checking for saved Matrix session");

        let session_path = database_path.join("astro-session.bin");

        let saved = load_session(&session_path, passphrase)?;

        let Some(saved) = saved else {
            eprintln!("ASTRO DEBUG: No saved Matrix session found");
            return Ok(None);
        };

        eprintln!("ASTRO DEBUG: Saved Matrix session found");

        // Version 2 sessions use the original database path.
        // Version 3 sessions use their own isolated store directory.
        let store_path = match saved.store_id.as_deref() {
            Some(store_id) => {
                let path = database_path.join("matrix-stores").join(store_id);

                if !path.is_dir() {
                    return Err("The Matrix store associated with this session is missing".into());
                }

                path
            }
            None => database_path.to_path_buf(),
        };

        let manager = Self::create(&store_path, passphrase, &saved.homeserver, session_path)?;

        eprintln!("ASTRO DEBUG: Restoring Matrix session");

        let client = manager.client.as_ref().unwrap();

        manager.runtime.block_on(async {
            client
                .matrix_auth()
                .restore_session(saved.session, RoomLoadSettings::default())
                .await
                .map_err(|e| format!("Could not restore Matrix session: {e}"))
        })?;

        eprintln!("ASTRO DEBUG: Matrix session restored");

        Ok(Some(manager))
    }

    pub fn logout(&mut self) -> Result<(), String> {
        eprintln!("ASTRO DEBUG: Starting Matrix logout");

        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        self.runtime.block_on(async {
            client
                .logout()
                .await
                .map_err(|e| format!("Matrix logout failed: {e}"))
        })?;

        eprintln!("ASTRO DEBUG: Server logout completed");

        // Remove the saved login, but retain the encrypted SDK store.
        delete_session(&self.session_path)?;

        eprintln!("ASTRO DEBUG: Saved Matrix session deleted");

        // Destroy the client while its Tokio runtime is active.
        let _runtime_guard = self.runtime.enter();
        drop(self.client.take());

        eprintln!("ASTRO DEBUG: Matrix client destroyed");

        Ok(())
    }

    /// Retrieved Cached Rooms as a one time occurance
    ///
    /// This does not start a continuous sync loop.
    pub fn cached_rooms_json(&self) -> Result<String, String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        self.runtime.block_on(async {
            // Build the Matrix Space graph from the synchronised room state.
            let space_service = SpaceService::new(client.clone()).await;
            let _top_level_spaces = space_service.top_level_joined_spaces().await;

            let joined_rooms = client.joined_rooms();
            let mut room_entries = Vec::with_capacity(joined_rooms.len());

            for room in joined_rooms {
                let info = room.clone_info();
                let room_id = room.room_id().to_string();

                let is_space = info.room_type() == Some(&RoomType::Space);
                let is_dm = !is_space && room.is_dm();

                let kind = if is_space {
                    "space"
                } else if is_dm {
                    "dm"
                } else {
                    "room"
                };

                let name = info
                    .name()
                    .map(str::to_owned)
                    .unwrap_or_else(|| room_id.clone());

                let parent_ids = space_service
                    .joined_parent_ids_of_child(room.room_id())
                    .await;

                let parents: Vec<String> =
                    parent_ids.iter().map(ToString::to_string).collect();

                room_entries.push(serde_json::json!({
                    "room_id": room_id,
                    "kind": kind,
                    "name": name,
                    "topic": info.topic(),
                    "avatar_url": info.avatar_url().map(ToString::to_string),
                    "member_count": info.joined_members_count(),
                    "parent_space_ids": parents
                }));
            }

            Ok(serde_json::json!({
                "rooms": room_entries
            }).to_string())
        })
    }

    /// Perform a one-off sync and return joined rooms and Space relationships.
    ///
    /// This does not start a continuous sync loop.
    pub fn rooms_json(&self) -> Result<String, String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        self.runtime.block_on(async {
            client
                .sync_once(SyncSettings::default().timeout(Duration::from_secs(10)))
                .await
                .map_err(|e| format!("Matrix room sync failed: {e}"))?;

            // Build the Matrix Space graph from the synchronised room state.
            let space_service = SpaceService::new(client.clone()).await;
            let _top_level_spaces = space_service.top_level_joined_spaces().await;

            let joined_rooms = client.joined_rooms();
            let mut room_entries = Vec::with_capacity(joined_rooms.len());

            for room in joined_rooms {
                let info = room.clone_info();
                let room_id = room.room_id().to_string();

                let is_space = info.room_type() == Some(&RoomType::Space);
                let is_dm = !is_space && room.is_dm();

                let kind = if is_space {
                    "space"
                } else if is_dm {
                    "dm"
                } else {
                    "room"
                };

                let name = info
                    .name()
                    .map(str::to_owned)
                    .unwrap_or_else(|| room_id.clone());

                let parent_ids = space_service
                    .joined_parent_ids_of_child(room.room_id())
                    .await;

                let parents: Vec<String> =
                    parent_ids.iter().map(ToString::to_string).collect();

                room_entries.push(serde_json::json!({
                    "room_id": room_id,
                    "kind": kind,
                    "name": name,
                    "topic": info.topic(),
                    "avatar_url": info.avatar_url().map(ToString::to_string),
                    "member_count": info.joined_members_count(),
                    "parent_space_ids": parents
                }));
            }

            Ok(serde_json::json!({
                "rooms": room_entries
            }).to_string())
        })
    }

    pub fn thumbnail(
        &self,
        source: matrix_sdk::ruma::events::room::MediaSource,
        width: u32,
        height: u32,
    ) -> Result<Vec<u8>, String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        self.runtime.block_on(async {
            let media = MediaManager::new(client);

            media
                .thumbnail(source, width, height)
                .await
        })
    }

    pub fn client(&self) -> &Client {
        self.client.as_ref().unwrap()
    }
}

impl Drop for MatrixClientManager {
    fn drop(&mut self) {
        // Make this runtime the current Tokio context while
        // Matrix SDK and SQLite resources are being destroyed.
        let _runtime_guard = self.runtime.enter();

        eprintln!("ASTRO DEBUG: Dropping Matrix client manager");

        drop(self.client.take());

        eprintln!("ASTRO DEBUG: Matrix client manager cleanup completed");
    }
}
