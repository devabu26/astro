use matrix_sdk::{
    config::SyncSettings,
    ruma::room::RoomType,
    store::RoomLoadSettings,
    Client,
    RoomMemberships,
    SqliteStoreConfig,
};
use matrix_sdk_ui::spaces::SpaceService;
use matrix_sdk_ui::timeline::{Timeline, TimelineBuilder};
use std::time::Duration;
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use tokio::runtime::{Builder, Runtime};

use crate::media::MediaManager;
use crate::session_storage::{delete_session, load_session, save_session};

static STORE_COUNTER: AtomicU64 = AtomicU64::new(0);

struct SyncTask {
    cancel: tokio::sync::oneshot::Sender<()>,
    thread: std::thread::JoinHandle<()>,
}

impl SyncTask {
    fn stop(self) {
        let _ = self.cancel.send(());

        if self.thread.join().is_err() {
            eprintln!("ASTRO DEBUG: Matrix sync thread panicked");
        }
    }
}

pub struct MatrixClientManager {
    client: Option<Client>,
    runtime: std::sync::Arc<Runtime>,
    session_path: PathBuf,
    sync_task: Option<SyncTask>,
    timelines: HashMap<String, Timeline>,
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
            timelines: HashMap::new(),
            client: Some(client),
            runtime: std::sync::Arc::new(runtime),
            session_path,
            sync_task: None,
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

        // Stop continuous syncing before logging out.
        if let Some(task) = self.sync_task.take() {
            task.stop();
        }

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

        self.timelines.clear();

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

                let (name, avatar_url) = if is_dm {
                    let members = room
                        .members_no_sync(RoomMemberships::JOIN)
                        .await
                        .unwrap_or_default();

                    let other_member = members
                        .iter()
                        .find(|member| Some(member.user_id()) != client.user_id());

                    let name = other_member
                        .and_then(|member| member.display_name().map(str::to_owned))
                        .or_else(|| info.name().map(str::to_owned))
                        .unwrap_or_else(|| room_id.clone());

                    let avatar_url = other_member
                        .and_then(|member| member.avatar_url().map(ToString::to_string))
                        .or_else(|| info.avatar_url().map(ToString::to_string));

                    (name, avatar_url)
                } else {
                    let name = info
                        .name()
                        .map(str::to_owned)
                        .unwrap_or_else(|| room_id.clone());

                    let avatar_url = info.avatar_url().map(ToString::to_string);

                    (name, avatar_url)
                };

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
                    "avatar_url": avatar_url,
                    "member_count": info.joined_members_count(),
                    "parent_space_ids": parents
                }));
            }

            Ok(serde_json::json!({
                "rooms": room_entries
            }).to_string())
        })
    }

    /// Perform an initial sync, return joined rooms and Space relationships,
    /// and start continuous syncing if it is not already running.
    pub fn rooms_json(&mut self) -> Result<String, String> {
            let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

            let rooms_json = self.runtime.block_on(async {
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

                let (name, avatar_url) = if is_dm {
                    let members = room
                        .members_no_sync(RoomMemberships::JOIN)
                        .await
                        .unwrap_or_default();

                    let other_member = members
                        .iter()
                        .find(|member| Some(member.user_id()) != client.user_id());

                    let name = other_member
                        .and_then(|member| member.display_name().map(str::to_owned))
                        .or_else(|| info.name().map(str::to_owned))
                        .unwrap_or_else(|| room_id.clone());

                    let avatar_url = other_member
                        .and_then(|member| member.avatar_url().map(ToString::to_string))
                        .or_else(|| info.avatar_url().map(ToString::to_string));

                    (name, avatar_url)
                } else {
                    let name = info
                        .name()
                        .map(str::to_owned)
                        .unwrap_or_else(|| room_id.clone());

                    let avatar_url = info.avatar_url().map(ToString::to_string);

                    (name, avatar_url)
                };

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
                    "avatar_url": avatar_url,
                    "member_count": info.joined_members_count(),
                    "parent_space_ids": parents
                }));
            }

            Ok::<String, String>(serde_json::json!({
                "rooms": room_entries
            }).to_string())
                })?;

        // Start continuous syncing once the initial room sync has succeeded.
        if self
            .sync_task
            .as_ref()
            .map_or(true, |task| task.thread.is_finished())
        {
            // Reap a previous sync thread if it has finished.
            if let Some(task) = self.sync_task.take() {
                task.stop();
            }

            let client = self
                .client
                .as_ref()
                .ok_or_else(|| "No active Matrix session".to_string())?
                .clone();

            let runtime = std::sync::Arc::clone(&self.runtime);
            let (cancel_tx, cancel_rx) = tokio::sync::oneshot::channel::<()>();

            let thread = std::thread::Builder::new()
                .name("astro-matrix-sync".to_string())
                .spawn(move || {
                    eprintln!("ASTRO DEBUG: Matrix continuous sync thread started");

                    let local = tokio::task::LocalSet::new();

                    local.block_on(&runtime, async move {
                        tokio::select! {
                            _ = cancel_rx => {
                                eprintln!("ASTRO DEBUG: Matrix continuous sync cancelled");
                            }
                            result = client.sync(SyncSettings::default()) => {
                                if let Err(error) = result {
                                    eprintln!(
                                        "ASTRO DEBUG: Matrix continuous sync stopped: {error}"
                                    );
                                }
                            }
                        }
                    });
                })
                .map_err(|error| format!("Could not start Matrix sync thread: {error}"))?;

            self.sync_task = Some(SyncTask {
                cancel: cancel_tx,
                thread,
            });
        }

        Ok(rooms_json)
    }

    /// Build a room timeline and return its initial message snapshot as JSON.
    /// Build and store a room timeline, then return its initial message snapshot.
pub fn room_timeline_json(&mut self, room_id: &str) -> Result<String, String> {
    let client = self
        .client
        .as_ref()
        .ok_or_else(|| "No active Matrix session".to_string())?;

    let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
        .map_err(|error| format!("Invalid Matrix room ID: {error}"))?;

    let room = client
        .get_room(&parsed_room_id)
        .ok_or_else(|| format!("Matrix room not found: {room_id}"))?;

    let (timeline, json) = self.runtime.block_on(async {
        let timeline = TimelineBuilder::new(&room)
            .build()
            .await
            .map_err(|error| format!("Could not build room timeline: {error}"))?;

        // Load the latest 50 historical events.
        let reached_start = timeline
            .paginate_backwards(50)
            .await
            .map_err(|error| format!("Could not load room history: {error}"))?;

        let (items, _updates) = timeline.subscribe().await;

        let messages: Vec<serde_json::Value> = items
            .iter()
            .filter_map(|item| {
                let event = item.as_event()?;
                let content = event.content();

                let is_encrypted = content.is_unable_to_decrypt();

                let body = if is_encrypted {
                    "Encrypted message"
                } else {
                    content.as_message()?.body()
                };

                Some(serde_json::json!({
                    "event_id": event.event_id().map(ToString::to_string),
                    "sender": event.sender().to_string(),
                    "body": body,
                    "timestamp": u64::from(event.timestamp().get()),
                    "is_own": event.is_own(),
                    "is_encrypted": is_encrypted
                }))
            })
            .collect();

        let json = serde_json::json!({
            "room_id": room_id,
            "messages": messages,
            "reached_start": reached_start
        })
        .to_string();

        Ok::<_, String>((timeline, json))
    })?;

    // Retain this timeline so subsequent requests paginate the same instance.
    self.timelines.insert(room_id.to_owned(), timeline);

    Ok(json)
}

/// Load another batch of older messages from an existing room timeline.
pub fn paginate_room_timeline_json(
    &self,
    room_id: &str,
) -> Result<String, String> {
    let timeline = self
        .timelines
        .get(room_id)
        .ok_or_else(|| format!("No active timeline for room: {room_id}"))?;

    self.runtime.block_on(async {
        let reached_start = timeline
            .paginate_backwards(50)
            .await
            .map_err(|error| format!("Could not load older room history: {error}"))?;

        let (items, _updates) = timeline.subscribe().await;

        let messages: Vec<serde_json::Value> = items
            .iter()
            .filter_map(|item| {
                let event = item.as_event()?;
                let content = event.content();

                let is_encrypted = content.is_unable_to_decrypt();

                let body = if is_encrypted {
                    "Encrypted message"
                } else {
                    content.as_message()?.body()
                };

                Some(serde_json::json!({
                    "event_id": event.event_id().map(ToString::to_string),
                    "sender": event.sender().to_string(),
                    "body": body,
                    "timestamp": u64::from(event.timestamp().get()),
                    "is_own": event.is_own(),
                    "is_encrypted": is_encrypted
                }))
            })
            .collect();

        Ok(serde_json::json!({
            "room_id": room_id,
            "messages": messages,
            "reached_start": reached_start
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
        // Stop continuous syncing before destroying the Matrix client.
        if let Some(task) = self.sync_task.take() {
            task.stop();
        }

        // Make this runtime the current Tokio context while
        // Matrix SDK and SQLite resources are being destroyed.
        let _runtime_guard = self.runtime.enter();
        self.timelines.clear();

        eprintln!("ASTRO DEBUG: Dropping Matrix client manager");

        drop(self.client.take());

        eprintln!("ASTRO DEBUG: Matrix client manager cleanup completed");
    }
}
