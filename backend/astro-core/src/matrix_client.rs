use matrix_sdk::ruma::api::client::receipt::create_receipt::v3::ReceiptType;
use matrix_sdk::encryption::verification::{
    SasState, SasVerification, Verification, VerificationRequest,
    VerificationRequestState,
};
use matrix_sdk::ruma::events::key::verification::VerificationMethod;
use matrix_sdk::ruma::events::{
    key::verification::request::ToDeviceKeyVerificationRequestEvent,
    room::message::{MessageType, OriginalSyncRoomMessageEvent},
};
use matrix_sdk::ruma::events::room::message::RoomMessageEventContent;
use matrix_sdk::{
    config::SyncSettings, ruma::room::RoomType, store::RoomLoadSettings, Client, Room, RoomMemberships,
    SqliteStoreConfig,
};
use matrix_sdk_ui::spaces::SpaceService;
use matrix_sdk_ui::timeline::{Timeline, TimelineBuilder, TimelineItem};
use eyeball_im::VectorDiff;
use futures_util::{FutureExt, Stream, StreamExt};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use tokio::runtime::{Builder, Runtime};

use crate::media::MediaManager;
use crate::session_storage::{delete_session, load_session, save_session};

static STORE_COUNTER: AtomicU64 = AtomicU64::new(0);

type TimelineUpdateStream =
    Pin<Box<dyn Stream<Item = Vec<VectorDiff<Arc<TimelineItem>>>>>>;


fn timeline_item_to_json(item: &TimelineItem) -> Option<serde_json::Value> {
    let event = item.as_event()?;
    let content = event.content();
    let is_encrypted = content.is_unable_to_decrypt();

    let message = if is_encrypted {
        None
    } else {
        content.as_message()
    };

    let body = message
        .map(|message| message.body())
        .unwrap_or("Encrypted message");

    let mut msgtype = "m.text";
    let mut media_source = None;
    let mut media_mimetype = None;
    let mut media_thumbnail_source = None;
    let mut media_thumbnail_width = None;
    let mut media_thumbnail_height = None;

    if let Some(message) = message {
        match message.msgtype() {
            MessageType::Image(image) => {
                msgtype = "m.image";

                media_source = serde_json::to_value(&image.source).ok();

                media_mimetype = image
                    .info
                    .as_ref()
                    .and_then(|info| info.mimetype.clone());
            }

            MessageType::Video(video) => {
                msgtype = "m.video";

                media_source = serde_json::to_value(&video.source).ok();

                media_mimetype = video
                    .info
                    .as_ref()
                    .and_then(|info| info.mimetype.clone());

                media_thumbnail_source = video
                    .info
                    .as_ref()
                    .and_then(|info| info.thumbnail_source.as_ref())
                    .and_then(|source| serde_json::to_value(source).ok());

                media_thumbnail_width = video
                    .info
                    .as_ref()
                    .and_then(|info| info.thumbnail_info.as_ref())
                    .and_then(|thumbnail| {
                        serde_json::to_value(&thumbnail.width).ok()
                    })
                    .and_then(|value| value.as_u64());

                media_thumbnail_height = video
                    .info
                    .as_ref()
                    .and_then(|info| info.thumbnail_info.as_ref())
                    .and_then(|thumbnail| {
                        serde_json::to_value(&thumbnail.height).ok()
                    })
                    .and_then(|value| value.as_u64());
            }

            MessageType::File(_) => {
                msgtype = "m.file";
            }

            _ => {}
        }
    }

    let (sender_display_name, sender_avatar) =
        match event.sender_profile() {
            matrix_sdk_ui::timeline::TimelineDetails::Ready(profile) => (
                profile.display_name.clone(),
                profile.avatar_url.as_ref().map(ToString::to_string),
            ),
            _ => (None, None),
        };

    Some(serde_json::json!({
        "event_id": event.event_id().map(ToString::to_string),
        "sender": event.sender().to_string(),
        "sender_display_name": sender_display_name,
        "sender_avatar": sender_avatar,
        "body": body,
        "msgtype": msgtype,
        "media_source": media_source,
        "media_mimetype": media_mimetype,
        "media_thumbnail_source": media_thumbnail_source,
        "media_thumbnail_width": media_thumbnail_width,
        "media_thumbnail_height": media_thumbnail_height,
        "timestamp": u64::from(event.timestamp().get()),
        "is_own": event.is_own(),
        "is_encrypted": is_encrypted
    }))
}

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

struct PendingVerificationRequest {
    user_id: String,
    flow_id: String,
    room_id: Option<String>,
    request: VerificationRequest,
}

pub struct MatrixClientManager {
    client: Option<Client>,
    runtime: std::sync::Arc<Runtime>,
    session_path: PathBuf,
    sync_task: Option<SyncTask>,
    timelines: HashMap<String, Timeline>,
    timeline_update_streams: HashMap<String, TimelineUpdateStream>,
    last_unread_counts: HashMap<String, (u64, u64)>,
    last_unread_check: Option<Instant>,
    verification_requests: Arc<Mutex<Vec<PendingVerificationRequest>>>,
    active_verification_requests: HashMap<String, PendingVerificationRequest>,
    verification_sas_queue: Arc<Mutex<Vec<(String, SasVerification)>>>,
    active_verifications: HashMap<String, SasVerification>,
    last_verification_states: HashMap<String, String>,
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

        // Incoming verification requests are collected here and delivered
        // to the UI through Astro's existing polling path.
        let verification_requests =
            Arc::new(Mutex::new(Vec::<PendingVerificationRequest>::new()));

        let to_device_queue = Arc::clone(&verification_requests);

        client.add_event_handler(
            move |event: ToDeviceKeyVerificationRequestEvent, client: Client| {
                let queue = Arc::clone(&to_device_queue);

                async move {
                    if let Some(request) = client
                        .encryption()
                        .get_verification_request(
                            &event.sender,
                            &event.content.transaction_id,
                        )
                        .await
                    {
                        let pending = PendingVerificationRequest {
                            user_id: event.sender.to_string(),
                            flow_id: event.content.transaction_id.to_string(),
                            room_id: None,
                            request,
                        };

                        if let Ok(mut queue) = queue.lock() {
                            queue.push(pending);
                        }
                    }
                }
            },
        );

        let room_queue = Arc::clone(&verification_requests);

        client.add_event_handler(
            move |event: OriginalSyncRoomMessageEvent, room: Room, client: Client| {
                let queue = Arc::clone(&room_queue);

                async move {
                    if !matches!(
                        &event.content.msgtype,
                        MessageType::VerificationRequest(_)
                    ) {
                        return;
                    }

                    if let Some(request) = client
                        .encryption()
                        .get_verification_request(&event.sender, &event.event_id)
                        .await
                    {
                        let pending = PendingVerificationRequest {
                            user_id: event.sender.to_string(),
                            flow_id: event.event_id.to_string(),
                            room_id: Some(room.room_id().to_string()),
                            request,
                        };

                        if let Ok(mut queue) = queue.lock() {
                            queue.push(pending);
                        }
                    }
                }
            },
        );

        Ok(Self {
            timelines: HashMap::new(),
            timeline_update_streams: HashMap::new(),
            last_unread_counts: HashMap::new(),
            last_unread_check: None,
            verification_requests,
            active_verification_requests: HashMap::new(),
            verification_sas_queue: Arc::new(Mutex::new(Vec::new())),
            active_verifications: HashMap::new(),
            last_verification_states: HashMap::new(),
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

        self.timeline_update_streams.clear();
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

                let parents: Vec<String> = parent_ids.iter().map(ToString::to_string).collect();

                room_entries.push(serde_json::json!({
                    "room_id": room_id,
                    "kind": kind,
                    "name": name,
                    "topic": info.topic(),
                    "avatar_url": avatar_url,
                    "member_count": info.joined_members_count(),
                    "unread_count": room.unread_notification_counts().notification_count,
                    "mention_count": room.unread_notification_counts().highlight_count,
                    "parent_space_ids": parents
                }));
            }

            Ok(serde_json::json!({
                "rooms": room_entries
            })
            .to_string())
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

                let parents: Vec<String> = parent_ids.iter().map(ToString::to_string).collect();

                room_entries.push(serde_json::json!({
                    "room_id": room_id,
                    "kind": kind,
                    "name": name,
                    "topic": info.topic(),
                    "avatar_url": avatar_url,
                    "member_count": info.joined_members_count(),
                    "unread_count": room.unread_notification_counts().notification_count,
                    "mention_count": room.unread_notification_counts().highlight_count,
                    "parent_space_ids": parents
                }));
            }

            Ok::<String, String>(
                serde_json::json!({
                    "rooms": room_entries
                })
                .to_string(),
            )
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

        let (timeline, json, update_stream) = self.runtime.block_on(async {
            let timeline = TimelineBuilder::new(&room)
                .build()
                .await
                .map_err(|error| format!("Could not build room timeline: {error}"))?;

            // Load the latest 50 historical events before subscribing.
            let reached_start = timeline
                .paginate_backwards(5)
                .await
                .map_err(|error| format!("Could not load room history: {error}"))?;

            // Retain this stream so ongoing Matrix sync changes can be observed.
            let (items, updates) = timeline.subscribe().await;

            let messages: Vec<serde_json::Value> = items
                .iter()
                .filter_map(|item| timeline_item_to_json(item.as_ref()))
                .collect();

            let json = serde_json::json!({
                "room_id": room_id,
                "messages": messages,
                "reached_start": reached_start
            })
            .to_string();

            Ok::<_, String>((timeline, json, Box::pin(updates) as TimelineUpdateStream))
        })?;

        // Keep the timeline for pagination and its stream for live updates.
        self.timelines.insert(room_id.to_owned(), timeline);
        self.timeline_update_streams
            .insert(room_id.to_owned(), update_stream);

        Ok(json)
    }

    /// Load another batch of older messages from an existing room timeline.
    pub fn paginate_room_timeline_json(&self, room_id: &str) -> Result<String, String> {
        let timeline = self
            .timelines
            .get(room_id)
            .ok_or_else(|| format!("No active timeline for room: {room_id}"))?;

        self.runtime.block_on(async {
            let reached_start = timeline
                .paginate_backwards(20)
                .await
                .map_err(|error| format!("Could not load older room history: {error}"))?;

            let (items, _updates) = timeline.subscribe().await;

            let messages: Vec<serde_json::Value> = items
                .iter()
                .filter_map(|item| timeline_item_to_json(item.as_ref()))
                .collect();

            Ok(serde_json::json!({
                "room_id": room_id,
                "messages": messages,
                "reached_start": reached_start
            })
            .to_string())
        })
    }

    /// Send a read receipt for the latest event in a room timeline.
    pub fn mark_room_as_read(&self, room_id: &str) -> Result<bool, String> {
        let timeline = self
            .timelines
            .get(room_id)
            .ok_or_else(|| format!("No active timeline for room: {room_id}"))?;

        self.runtime.block_on(async {
            timeline
                .mark_as_read(ReceiptType::Read)
                .await
                .map_err(|error| format!("Could not send read receipt: {error}"))
        })
    }

    /// Drain timeline changes and report changed unread counts.
    pub fn poll_room_timeline_updates_json(&mut self) -> String {
        let runtime = std::sync::Arc::clone(&self.runtime);
        let mut updates = Vec::new();

        let _runtime_guard = runtime.enter();

        for (room_id, stream) in self.timeline_update_streams.iter_mut() {
            let next_batch = stream.next().now_or_never();

            let Some(Some(batch)) = next_batch else {
                continue;
            };

            for diff in batch {
                match diff {
                    VectorDiff::PushBack { value }
                    | VectorDiff::Set { value, .. } => {
                        if let Some(message) = timeline_item_to_json(value.as_ref()) {
                            updates.push(serde_json::json!({
                                "room_id": room_id,
                                "operation": "upsert",
                                "message": message
                            }));
                        }
                    }

                    VectorDiff::Append { values } => {
                        for value in values.iter() {
                            if let Some(message) = timeline_item_to_json(value.as_ref()) {
                                updates.push(serde_json::json!({
                                    "room_id": room_id,
                                    "operation": "upsert",
                                    "message": message
                                }));
                            }
                        }
                    }

                    // Backward pagination uses its separate snapshot path.
                    _ => {}
                }
            }
        }

        // Unread counts do not need to be scanned at the timeline's
        // 100 ms polling frequency. Check them once per second instead.
        let now = Instant::now();
        let should_check_unread = self.last_unread_check.map_or(
            true,
            |last| now.duration_since(last) >= Duration::from_secs(1),
        );

        let mut unread_counts = Vec::new();
        let mut verification_events = Vec::new();

        // Move newly received requests into the manager so their SDK
        // handles remain available for later accept/cancel operations.
        let queued_requests = self
            .verification_requests
            .lock()
            .map(|mut queue| std::mem::take(&mut *queue))
            .unwrap_or_default();

        for pending in queued_requests {
            let key = format!("{}|{}", pending.user_id, pending.flow_id);

            if self.active_verification_requests.contains_key(&key) {
                continue;
            }

            verification_events.push(serde_json::json!({
                "user_id": pending.user_id,
                "flow_id": pending.flow_id,
                "room_id": pending.room_id,
                "is_self_verification": pending.request.is_self_verification()
            }));

            self.active_verification_requests.insert(key, pending);
        }

        // Collect SAS handles created by the asynchronous request watcher.
        let queued_sas = self
            .verification_sas_queue
            .lock()
            .map(|mut queue| std::mem::take(&mut *queue))
            .unwrap_or_default();

        for (key, sas) in queued_sas {
            self.active_verifications.insert(key, sas);
        }

        // Report only changed SAS states, avoiding repeated UI updates
        // at the 100 ms polling frequency.
        let active_sas: Vec<(String, SasVerification)> = self
            .active_verifications
            .iter()
            .map(|(key, sas)| (key.clone(), sas.clone()))
            .collect();

        let mut finished_verifications = Vec::new();

        for (key, sas) in active_sas {
            let state = sas.state();

            let (state_name, is_finished) = match state {
                SasState::Created { .. } => ("created", false),
                SasState::Started { .. } => ("started", false),
                SasState::Accepted { .. } => ("accepted", false),
                SasState::KeysExchanged { .. } => ("keys_exchanged", false),
                SasState::Confirmed => ("confirmed", false),
                SasState::Done { .. } => ("done", true),
                SasState::Cancelled(_) => ("cancelled", true),
            };

            let emojis = sas.emoji().map(|items| {
                items
                    .iter()
                    .map(|emoji| {
                        serde_json::json!({
                            "symbol": emoji.symbol,
                            "description": emoji.description
                        })
                    })
                    .collect::<Vec<_>>()
            });

            let (user_id, flow_id) = key
                .split_once('|')
                .unwrap_or(("", ""));

            let event = serde_json::json!({
                "type": "verification_state",
                "user_id": user_id,
                "flow_id": flow_id,
                "state": state_name,
                "emojis": emojis
            });

            let signature = event.to_string();

            if self.last_verification_states.get(&key) != Some(&signature) {
                self.last_verification_states
                    .insert(key.clone(), signature);

                verification_events.push(event);
            }

            if is_finished {
                finished_verifications.push(key);
            }
        }

        for key in finished_verifications {
            self.active_verifications.remove(&key);
            self.active_verification_requests.remove(&key);
            self.last_verification_states.remove(&key);
        }

        if should_check_unread {
            self.last_unread_check = Some(now);

            if let Some(client) = self.client.as_ref() {
                for room in client.joined_rooms() {
                    let room_id = room.room_id().to_string();
                    let server_counts = room.unread_notification_counts();
                    let unread = server_counts.notification_count;
                    let mentions = server_counts.highlight_count;
                    let counts = (unread, mentions);
                    if self.last_unread_counts.get(&room_id) != Some(&counts) {
                        self.last_unread_counts.insert(room_id.clone(), counts);

                        unread_counts.push(serde_json::json!({
                            "room_id": room_id,
                            "unread_count": unread,
                            "mention_count": mentions
                        }));
                    }
                }
            }
        }

        if updates.is_empty() && unread_counts.is_empty() && verification_events.is_empty() {
            return serde_json::json!({
                "updates": []
            })
            .to_string();
        }

        serde_json::json!({
            "updates": updates,
            "unread_counts": unread_counts,
            "verification_requests": verification_events
        })
        .to_string()
    }

    /// Accept an incoming verification request, advertising SAS only.
    pub fn accept_verification_request(
        &self,
        user_id: &str,
        flow_id: &str,
    ) -> Result<(), String> {
        let key = format!("{user_id}|{flow_id}");

        let request = self
            .active_verification_requests
            .get(&key)
            .map(|pending| pending.request.clone())
            .ok_or_else(|| "Verification request not found".to_string())?;

        let sas_queue = Arc::clone(&self.verification_sas_queue);

        self.runtime.block_on(async move {
            request
                .accept_with_methods(vec![VerificationMethod::SasV1])
                .await
                .map_err(|error| format!("Could not accept verification request: {error}"))?;

            // Wait for the other participant to start SAS. The SDK emits
            // Transitioned when the request becomes a concrete SAS flow.
            let mut changes = request.changes();

            tokio::spawn(async move {
                while let Some(state) = changes.next().await {
                    match state {
                        VerificationRequestState::Transitioned { verification } => {
                            if let Verification::SasV1(sas) = verification {
                                match sas.accept().await {
                                    Ok(()) => {
                                        if let Ok(mut queue) = sas_queue.lock() {
                                            queue.push((key.clone(), sas));
                                        }
                                    }
                                    Err(error) => {
                                        eprintln!(
                                            "Astro: could not accept SAS verification for {}: {}",
                                            key,
                                            error
                                        );
                                    }
                                }
                            }
                            break;
                        }
                        VerificationRequestState::Done
                        | VerificationRequestState::Cancelled(_) => break,
                        _ => {}
                    }
                }
            });

            Ok(())
        })
    }

    /// Cancel an incoming verification request or active SAS flow.
    pub fn cancel_verification_request(
        &mut self,
        user_id: &str,
        flow_id: &str,
    ) -> Result<(), String> {
        let key = format!("{user_id}|{flow_id}");

        if let Some(sas) = self.active_verifications.get(&key).cloned() {
            self.runtime.block_on(async {
                sas.cancel()
                    .await
                    .map_err(|error| format!("Could not cancel verification: {error}"))
            })?;
        } else if let Some(pending) = self.active_verification_requests.get(&key) {
            let request = pending.request.clone();

            self.runtime.block_on(async {
                request
                    .cancel()
                    .await
                    .map_err(|error| format!("Could not cancel verification request: {error}"))
            })?;
        } else {
            return Err("Verification request not found".into());
        }

        self.active_verifications.remove(&key);
        self.active_verification_requests.remove(&key);
        self.last_verification_states.remove(&key);

        Ok(())
    }

    /// Confirm that the displayed SAS emoji match the other device.
    pub fn confirm_verification(
        &self,
        user_id: &str,
        flow_id: &str,
    ) -> Result<(), String> {
        let key = format!("{user_id}|{flow_id}");

        let sas = self
            .active_verifications
            .get(&key)
            .cloned()
            .ok_or_else(|| "Active SAS verification not found".to_string())?;

        self.runtime.block_on(async {
            sas.confirm()
                .await
                .map_err(|error| format!("Could not confirm verification: {error}"))
        })
    }

    /// Reject the SAS exchange because the emoji do not match.
    pub fn mismatch_verification(
        &self,
        user_id: &str,
        flow_id: &str,
    ) -> Result<(), String> {
        let key = format!("{user_id}|{flow_id}");

        let sas = self
            .active_verifications
            .get(&key)
            .cloned()
            .ok_or_else(|| "Active SAS verification not found".to_string())?;

        self.runtime.block_on(async {
            sas.mismatch()
                .await
                .map_err(|error| format!("Could not reject verification: {error}"))
        })
    }

    /// Send a plain-text message to a Matrix room.
    ///
    /// The Matrix SDK handles encryption automatically for encrypted rooms.
    pub fn send_room_message(&self, room_id: &str, message: &str) -> Result<String, String> {
        if message.trim().is_empty() {
            return Err("Message must not be empty".into());
        }

        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
            .map_err(|error| format!("Invalid Matrix room ID: {error}"))?;

        let room = client
            .get_room(&parsed_room_id)
            .ok_or_else(|| format!("Matrix room not found: {room_id}"))?;

        self.runtime.block_on(async {
            let content = RoomMessageEventContent::text_plain(message.to_owned());

            let response = room
                .send(content)
                .await
                .map_err(|error| format!("Could not send Matrix message: {error}"))?;

            Ok(response.response.event_id.to_string())
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

            media.thumbnail(source, width, height).await
        })
    }

    pub fn image(
        &self,
        source: matrix_sdk::ruma::events::room::MediaSource,
    ) -> Result<Vec<u8>, String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        self.runtime.block_on(async {
            let media = MediaManager::new(client);
            media.image(source).await
        })
    }

    pub fn video(
        &self,
        source: matrix_sdk::ruma::events::room::MediaSource,
    ) -> Result<Vec<u8>, String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        self.runtime.block_on(async {
            let media = MediaManager::new(client);
            media.video(source).await
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

        self.timeline_update_streams.clear();
        self.timelines.clear();

        eprintln!("ASTRO DEBUG: Dropping Matrix client manager");

        drop(self.client.take());

        eprintln!("ASTRO DEBUG: Matrix client manager cleanup completed");
    }
}