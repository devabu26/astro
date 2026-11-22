use eyeball::SharedObservable;
use eyeball_im::VectorDiff;
use futures_util::{FutureExt, Stream, StreamExt};
use matrix_sdk::encryption::verification::{
    SasState, SasVerification, Verification, VerificationRequest, VerificationRequestState,
};
use matrix_sdk::ruma::api::client::device::delete_device;
use matrix_sdk::ruma::api::client::push::{PusherIds, PusherInit, PusherKind};
use matrix_sdk::ruma::api::client::receipt::create_receipt::v3::ReceiptType;
use matrix_sdk::ruma::api::client::uiaa;
use matrix_sdk::ruma::events::key::verification::VerificationMethod;
use matrix_sdk::ruma::events::room::message::{
    RoomMessageEventContent, RoomMessageEventContentWithoutRelation,
    TextMessageEventContent,
};
use matrix_sdk::room::edit::EditedContent;
use matrix_sdk::ruma::events::{
    key::verification::request::ToDeviceKeyVerificationRequestEvent,
    room::message::{MessageType, OriginalSyncRoomMessageEvent},
};
use matrix_sdk::ruma::push::HttpPusherData;
use matrix_sdk::{
    config::SyncSettings, ruma::room::RoomType, store::RoomLoadSettings, Client, LoopCtrl,
    Room, RoomMemberships, SqliteStoreConfig, TransmissionProgress,
};
use matrix_sdk_ui::spaces::SpaceService;
use matrix_sdk_ui::timeline::{
    MembershipChange, Timeline, TimelineBuilder, TimelineEventItemId,
    TimelineItem, TimelineItemContent,
};
use std::collections::HashMap;
use std::ffi::c_void;
use std::fs;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use tokio::runtime::{Builder, Runtime};

use crate::media::MediaManager;
use crate::notifications::{register_notification_handler, NotificationQueue};
use crate::session_storage::{delete_session, load_session, save_session};

static STORE_COUNTER: AtomicU64 = AtomicU64::new(0);

type TimelineUpdateStream = Pin<Box<dyn Stream<Item = Vec<VectorDiff<Arc<TimelineItem>>>>>>;

fn timeline_item_to_json(
    item: &TimelineItem,
    own_user_id: Option<&str>,
) -> Option<serde_json::Value> {
    let event = item.as_event()?;
    let content = event.content();
    let is_encrypted = content.is_unable_to_decrypt();
    let (sender_display_name, sender_avatar) = match event.sender_profile() {
        matrix_sdk_ui::timeline::TimelineDetails::Ready(profile) => (
            profile.display_name.clone(),
            profile.avatar_url.as_ref().map(ToString::to_string),
        ),
        _ => (None, None),
    };

    let sender_name = sender_display_name
        .as_deref()
        .filter(|name| !name.trim().is_empty())
        .map(str::to_owned)
        .unwrap_or_else(|| event.sender().to_string());

    // Convert selected non-message timeline items into readable room events.
    // These remain distinct from ordinary Matrix message events.
    let room_event_body = if is_encrypted {
        None
    } else {
        match content {
            TimelineItemContent::MembershipChange(change) => {
                let member_name = change
                    .display_name()
                    .unwrap_or_else(|| change.user_id().to_string());

                let action = match change.change() {
                    Some(MembershipChange::Joined) => Some("joined the room"),
                    Some(MembershipChange::Left) => Some("left the room"),
                    Some(MembershipChange::Invited) => Some("was invited"),
                    Some(MembershipChange::Kicked) => Some("was kicked"),
                    Some(MembershipChange::Banned) => Some("was banned"),
                    Some(MembershipChange::Unbanned) => Some("was unbanned"),
                    Some(MembershipChange::KickedAndBanned) => {
                        Some("was kicked and banned")
                    }
                    Some(MembershipChange::InvitationAccepted) => {
                        Some("accepted an invitation")
                    }
                    Some(MembershipChange::InvitationRejected) => {
                        Some("rejected an invitation")
                    }
                    Some(MembershipChange::InvitationRevoked) => {
                        Some("had their invitation revoked")
                    }
                    Some(MembershipChange::Knocked) => {
                        Some("requested to join the room")
                    }
                    Some(MembershipChange::KnockAccepted) => {
                        Some("had their join request accepted")
                    }
                    Some(MembershipChange::KnockRetracted) => {
                        Some("retracted their join request")
                    }
                    Some(MembershipChange::KnockDenied) => {
                        Some("had their join request denied")
                    }
                    _ => None,
                };

                action.map(|action| format!("{member_name} {action}"))
            }
            TimelineItemContent::CallInvite => {
                Some(format!("{sender_name} started a call"))
            }
            TimelineItemContent::RtcNotification { call_intent, .. } => {
                use matrix_sdk::ruma::events::rtc::notification::CallIntent;

                let description = match call_intent {
                    Some(CallIntent::Audio) => "started a voice call",
                    Some(CallIntent::Video) => "started a video call",
                    _ => "started a call",
                };
                Some(format!("{sender_name} {description}"))
            }
            _ => None,
        }
    };

    let message = if is_encrypted {
        None
    } else {
        content.as_message()
    };

    let sticker = if is_encrypted {
        None
    } else {
        content.as_sticker()
    };

    // Ignore unsupported non-message events instead of displaying them
    // as encrypted messages. Keep the placeholder for genuinely encrypted
    // events that could not be decrypted.
    if !is_encrypted
        && room_event_body.is_none()
        && message.is_none()
        && sticker.is_none()
    {
        return None;
    }

    let fallback_body = message
        .map(|message| message.body())
        .or_else(|| sticker.map(|sticker| sticker.content().body.as_str()))
        .unwrap_or("Encrypted message");

    let body = room_event_body.as_deref().unwrap_or(fallback_body);

    let mut msgtype = if room_event_body.is_some() {
        "m.room.event"
    } else {
        "m.text"
    };
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

                media_mimetype = image.info.as_ref().and_then(|info| info.mimetype.clone());
            }

            MessageType::Video(video) => {
                msgtype = "m.video";

                media_source = serde_json::to_value(&video.source).ok();

                media_mimetype = video.info.as_ref().and_then(|info| info.mimetype.clone());

                media_thumbnail_source = video
                    .info
                    .as_ref()
                    .and_then(|info| info.thumbnail_source.as_ref())
                    .and_then(|source| serde_json::to_value(source).ok());

                media_thumbnail_width = video
                    .info
                    .as_ref()
                    .and_then(|info| info.thumbnail_info.as_ref())
                    .and_then(|thumbnail| serde_json::to_value(&thumbnail.width).ok())
                    .and_then(|value| value.as_u64());

                media_thumbnail_height = video
                    .info
                    .as_ref()
                    .and_then(|info| info.thumbnail_info.as_ref())
                    .and_then(|thumbnail| serde_json::to_value(&thumbnail.height).ok())
                    .and_then(|value| value.as_u64());
            }

            MessageType::File(_) => {
                msgtype = "m.file";
            }

            _ => {}
        }
    }

    if let Some(sticker) = sticker {
        let sticker_content = sticker.content();

        msgtype = "m.sticker";
        media_source = serde_json::to_value(&sticker_content.source).ok();
        media_mimetype = sticker_content.info.mimetype.clone();
    }


    let reactions = content
        .reactions()
        .map(|by_key| {
            by_key
                .iter()
                .map(|(key, senders)| {
                    let sender_ids: Vec<String> = senders.keys().map(ToString::to_string).collect();

                    let is_own = own_user_id
                        .map(|user_id| senders.keys().any(|sender| sender.as_str() == user_id))
                        .unwrap_or(false);

                    serde_json::json!({
                        "key": key,
                        "count": senders.len(),
                        "senders": sender_ids,
                        "is_own": is_own
                    })
                })
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();

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
        "is_encrypted": is_encrypted,
        "reactions": reactions
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
    is_outgoing: bool,
    request: VerificationRequest,
}

pub struct MatrixClientManager {
    client: Option<Client>,
    runtime: std::sync::Arc<Runtime>,
    session_path: PathBuf,
    sync_task: Option<SyncTask>,
    active_timeline_room: Arc<Mutex<Option<String>>>,
    pending_timeline_recovery: Arc<Mutex<Vec<String>>>,
    timelines: HashMap<String, Timeline>,
    timeline_update_streams: HashMap<String, TimelineUpdateStream>,
    last_unread_counts: HashMap<String, (u64, u64)>,
    last_unread_check: Option<Instant>,
    verification_requests: Arc<Mutex<Vec<PendingVerificationRequest>>>,
    notification_queue: NotificationQueue,
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
        let verification_requests = Arc::new(Mutex::new(Vec::<PendingVerificationRequest>::new()));

        let to_device_queue = Arc::clone(&verification_requests);

        client.add_event_handler(
            move |event: ToDeviceKeyVerificationRequestEvent, client: Client| {
                let queue = Arc::clone(&to_device_queue);

                async move {
                    if let Some(request) = client
                        .encryption()
                        .get_verification_request(&event.sender, &event.content.transaction_id)
                        .await
                    {
                        let pending = PendingVerificationRequest {
                            user_id: event.sender.to_string(),
                            flow_id: event.content.transaction_id.to_string(),
                            room_id: None,
                            is_outgoing: false,
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
                    if !matches!(&event.content.msgtype, MessageType::VerificationRequest(_)) {
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
                            is_outgoing: false,
                            request,
                        };

                        if let Ok(mut queue) = queue.lock() {
                            queue.push(pending);
                        }
                    }
                }
            },
        );

        let notification_queue = NotificationQueue::default();
        register_notification_handler(&client, notification_queue.clone());

        Ok(Self {
            timelines: HashMap::new(),
            timeline_update_streams: HashMap::new(),
            last_unread_counts: HashMap::new(),
            last_unread_check: None,
            verification_requests,
            notification_queue,
            active_verification_requests: HashMap::new(),
            verification_sas_queue: Arc::new(Mutex::new(Vec::new())),
            active_verifications: HashMap::new(),
            last_verification_states: HashMap::new(),
            client: Some(client),
            runtime: std::sync::Arc::new(runtime),
            session_path,
            sync_task: None,
            active_timeline_room: Arc::new(Mutex::new(None)),
            pending_timeline_recovery: Arc::new(Mutex::new(Vec::new())),
        })
    }

    fn configure_session_callbacks(
        &self,
        passphrase: &str,
        homeserver: &str,
        store_id: Option<String>,
    ) -> Result<(), String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        let reload_path = self.session_path.clone();
        let reload_passphrase = passphrase.to_owned();

        let save_path = self.session_path.clone();
        let save_passphrase = passphrase.to_owned();
        let homeserver = homeserver.to_owned();

        client
            .set_session_callbacks(
                Box::new(move |_client: Client| {
                    let saved = load_session(&reload_path, &reload_passphrase)
                        .map_err(std::io::Error::other)?
                        .ok_or_else(|| {
                            std::io::Error::new(
                                std::io::ErrorKind::NotFound,
                                "No saved Matrix session is available",
                            )
                        })?;

                    Ok(saved.session.tokens)
                }),
                Box::new(move |client: Client| {
                    let session = client.matrix_auth().session().ok_or_else(|| {
                        std::io::Error::other("No active Matrix session to persist")
                    })?;

                    save_session(
                        &save_path,
                        &save_passphrase,
                        &homeserver,
                        store_id.as_deref(),
                        &session,
                    )
                    .map_err(std::io::Error::other)?;

                    eprintln!("ASTRO: Refreshed Matrix session saved");
                    Ok(())
                }),
            )
            .map_err(|error| format!("Could not configure Matrix session callbacks: {error}"))
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

        manager.configure_session_callbacks(passphrase, homeserver, Some(store_id.clone()))?;

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
                Some(&store_id),
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

        let saved_store_id = saved.store_id.clone();
        let saved_homeserver = saved.homeserver.clone();

        let manager = Self::create(&store_path, passphrase, &saved.homeserver, session_path)?;

        manager.configure_session_callbacks(
            passphrase,
            &saved_homeserver,
            saved_store_id.clone(),
        )?;

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

        if let Ok(mut active_room) = self.active_timeline_room.lock() {
            *active_room = None;
        }

        if let Ok(mut pending) = self.pending_timeline_recovery.lock() {
            pending.clear();
        }

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

    /// Retrieve all registered Matrix sessions and their encryption trust status.
    pub fn devices_json(&self) -> Result<String, String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        self.runtime.block_on(async {
            let user_id = client
                .user_id()
                .ok_or_else(|| "Matrix user ID is unavailable".to_string())?;

            let current_device_id = client.device_id().map(ToString::to_string);

            let response = client
                .devices()
                .await
                .map_err(|error| format!("Could not retrieve Matrix devices: {error}"))?;

            let mut devices = Vec::with_capacity(response.devices.len());

            for session in response.devices {
                let device_id = session.device_id.to_string();

                let is_verified = client
                    .encryption()
                    .get_device(user_id, &session.device_id)
                    .await
                    .map_err(|error| {
                        format!(
                            "Could not retrieve encryption status for device {device_id}: {error}"
                        )
                    })?
                    .map(|device| device.is_verified());

                devices.push(serde_json::json!({
                    "device_id": device_id,
                    "display_name": session.display_name,
                    "is_current": current_device_id.as_deref() == Some(device_id.as_str()),
                    "is_verified": is_verified,
                    "last_seen_ts": session.last_seen_ts
                }));
            }

            Ok(serde_json::json!({
                "devices": devices
            })
            .to_string())
        })
    }

    /// Start the continuous Matrix sync task if it is not already running.
    fn start_sync_task(&mut self) -> Result<(), String> {
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
            let active_timeline_room = Arc::clone(&self.active_timeline_room);
            let pending_timeline_recovery =
                Arc::clone(&self.pending_timeline_recovery);
            let (cancel_tx, mut cancel_rx) = tokio::sync::oneshot::channel::<()>();

            let thread = std::thread::Builder::new()
                .name("astro-matrix-sync".to_string())
                .spawn(move || {
                    eprintln!("ASTRO DEBUG: Matrix continuous sync thread started");

                    let local = tokio::task::LocalSet::new();

                    local.block_on(&runtime, async move {
                        let mut retry_delay_secs = 2u64;
                        let sync_was_disconnected = Arc::new(AtomicBool::new(false));

                        loop {
                            let callback_room = Arc::clone(&active_timeline_room);
                            let callback_recovery_queue =
                                Arc::clone(&pending_timeline_recovery);
                            let callback_disconnected = Arc::clone(&sync_was_disconnected);

                            let sync_result = tokio::select! {
                                _ = &mut cancel_rx => {
                                    eprintln!("ASTRO DEBUG: Matrix continuous sync cancelled");
                                    break;
                                }
                                result = client.sync_with_result_callback(
                                    SyncSettings::default(),
                                    move |result| {
                                        let active_room = Arc::clone(&callback_room);
                                        let recovery_queue =
                                            Arc::clone(&callback_recovery_queue);
                                        let disconnected = Arc::clone(&callback_disconnected);

                                        async move {
                                            match result {
                                                Err(error) => {
                                                    disconnected.store(true, Ordering::Release);
                                                    Err(error)
                                                }
                                                Ok(_) => {
                                                    if disconnected.swap(false, Ordering::AcqRel) {
                                                        let room_id = active_room
                                                            .lock()
                                                            .ok()
                                                            .and_then(|room| room.clone());

                                                        if let Some(room_id) = room_id {
                                                            match recovery_queue.lock() {
                                                                Ok(mut queue) => {
                                                                    if !queue.contains(&room_id) {
                                                                        queue.push(room_id.clone());
                                                                    }

                                                                    eprintln!(
                                                                        "ASTRO DEBUG: Matrix connection restored; queued timeline snapshot recovery: {room_id}"
                                                                    );
                                                                }
                                                                Err(_) => {
                                                                    eprintln!(
                                                                        "ASTRO DEBUG: Could not queue room timeline recovery"
                                                                    );
                                                                    disconnected.store(
                                                                        true,
                                                                        Ordering::Release
                                                                    );
                                                                }
                                                            }
                                                        }
                                                    }

                                                    Ok(LoopCtrl::Continue)
                                                }
                                            }
                                        }
                                    }
                                ) => result,
                            };

                            match sync_result {
                                Ok(()) => {
                                    eprintln!(
                                        "ASTRO DEBUG: Matrix continuous sync ended; restarting"
                                    );
                                    retry_delay_secs = 2;
                                }
                                Err(error) => {
                                    eprintln!(
                                        "ASTRO DEBUG: Matrix continuous sync failed; retrying in {retry_delay_secs}s: {error}"
                                    );
                                }
                            }

                            tokio::select! {
                                _ = &mut cancel_rx => {
                                    eprintln!("ASTRO DEBUG: Matrix continuous sync cancelled during retry wait");
                                    break;
                                }
                                _ = tokio::time::sleep(Duration::from_secs(retry_delay_secs)) => {}
                            }

                            retry_delay_secs = (retry_delay_secs * 2).min(30);
                        }
                    });
                })
                .map_err(|error| format!("Could not start Matrix sync thread: {error}"))?;

            self.sync_task = Some(SyncTask {
                cancel: cancel_tx,
                thread,
            });
        }

        Ok(())
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
                .event_cache()
                .subscribe()
                .map_err(|e| format!("Failed to subscribe Matrix event cache: {e}"))?;

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

        self.start_sync_task()?;

        Ok(rooms_json)
    }

    /// Resume Matrix syncing after the application returns to the foreground.
    ///
    /// Stop the previous sync task, perform a bounded catch-up sync, and
    /// restart continuous syncing even if the catch-up request fails.
    pub fn resume_sync(&mut self) -> Result<(), String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?
            .clone();

        if let Some(task) = self.sync_task.take() {
            task.stop();
        }

        let catch_up_result = self.runtime.block_on(async {
            client
                .sync_once(SyncSettings::default().timeout(Duration::from_secs(10)))
                .await
                .map_err(|error| format!("Matrix foreground catch-up sync failed: {error}"))
                .map(|_| ())
        });

        // Always try to restore continuous syncing after the catch-up attempt.
        let start_result = self.start_sync_task();

        match (catch_up_result, start_result) {
            (Ok(()), Ok(())) => {}
            (Err(catch_up_error), Ok(())) => return Err(catch_up_error),
            (Ok(()), Err(start_error)) => return Err(start_error),
            (Err(catch_up_error), Err(start_error)) => {
                return Err(format!(
                    "{catch_up_error}; additionally, {start_error}"
                ));
            }
        }

        // Rebuild the active room's timeline snapshot after successful catch-up.
        let active_room = self
            .active_timeline_room
            .lock()
            .map_err(|_| "Could not read active Matrix timeline room".to_string())?
            .clone();

        if let Some(room_id) = active_room {
            let mut queue = self
                .pending_timeline_recovery
                .lock()
                .map_err(|_| "Could not queue Matrix timeline recovery".to_string())?;

            if !queue.contains(&room_id) {
                queue.push(room_id);
            }
        }

        Ok(())
    }

    /// Build and store a room timeline, then return its initial message snapshot.
    pub fn room_timeline_json(&mut self, room_id: &str) -> Result<String, String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        let own_user_id = client.user_id().map(ToString::to_string);

        let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
            .map_err(|error| format!("Invalid Matrix room ID: {error}"))?;

        let room = client
            .get_room(&parsed_room_id)
            .ok_or_else(|| format!("Matrix room not found: {room_id}"))?;


        // Reuse an existing SDK Timeline when this room has already
        // been opened. Building another Timeline for the same room
        // can register duplicate room-cache state.
        if let Some(timeline) = self.timelines.get(room_id) {
            let runtime = std::sync::Arc::clone(&self.runtime);

            let (json, update_stream) = runtime.block_on(async {
                // Take a fresh snapshot and subscription together.
                let (items, updates) = timeline.subscribe().await;

                let messages: Vec<serde_json::Value> = items
                    .iter()
                    .filter_map(|item| {
                        timeline_item_to_json(
                            item.as_ref(),
                            own_user_id.as_deref(),
                        )
                    })
                    .collect();

                let json = serde_json::json!({
                    "room_id": room_id,
                    "messages": messages,
                    "reached_start": false
                })
                .to_string();

                Ok::<_, String>(
                    (json, Box::pin(updates) as TimelineUpdateStream)
                )
            })?;

            // Replace Astro's previous subscription with the fresh one.
            // Keep the original SDK Timeline alive.
            self.timeline_update_streams
                .insert(room_id.to_owned(), update_stream);

            if let Ok(mut active_room) = self.active_timeline_room.lock() {
                *active_room = Some(room_id.to_owned());
            } else {
                eprintln!(
                    "ASTRO DEBUG: Could not update active timeline room state"
                );
            }

            eprintln!(
                "ASTRO DEBUG: Reused existing room timeline: {room_id}"
            );

            return Ok(json);
        }


        let (timeline, json, update_stream) = self.runtime.block_on(async {
            let timeline = TimelineBuilder::new(&room)
                .build()
                .await
                .map_err(|error| format!("Could not build room timeline: {error}"))?;

            // Load the latest 20 historical events before subscribing.
            let reached_start = timeline
                .paginate_backwards(20)
                .await
                .map_err(|error| format!("Could not load room history: {error}"))?;

            // Retain this stream so ongoing Matrix sync changes can be observed.
            let (items, updates) = timeline.subscribe().await;

            let messages: Vec<serde_json::Value> = items
                .iter()
                .filter_map(|item| timeline_item_to_json(item.as_ref(), own_user_id.as_deref()))
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

        if let Ok(mut active_room) = self.active_timeline_room.lock() {
            *active_room = Some(room_id.to_owned());
        } else {
            eprintln!("ASTRO DEBUG: Could not update active timeline room state");
        }

        Ok(json)
    }

    /// Purge one room's SDK event cache and reload its existing timeline.
    ///
    /// Ordinary room navigation must use room_timeline_json() instead.
    pub fn refresh_room_timeline_json(&mut self, room_id: &str) -> Result<String, String> {
        eprintln!("ASTRO REFRESH TRACE: Starting refresh for {room_id}");

        let client = self
            .client
            .as_ref()
            .cloned()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
            .map_err(|error| format!("Invalid Matrix room ID: {error}"))?;

        let own_user_id = client.user_id().map(ToString::to_string);

        // Keep the existing SDK Timeline alive. It owns the room event-cache
        // handle and observer that must survive forget_room().
        let timeline = self
            .timelines
            .get(room_id)
            .ok_or_else(|| format!("No existing timeline to refresh for room: {room_id}"))?;

        // Temporarily detach this room from reconnect recovery.
        if let Ok(mut active_room) = self.active_timeline_room.lock() {
            if active_room.as_deref() == Some(room_id) {
                *active_room = None;
            }
        } else {
            eprintln!("ASTRO DEBUG: Could not detach active room before cache refresh");
        }

        if let Ok(mut pending) = self.pending_timeline_recovery.lock() {
            pending.retain(|pending_room| pending_room != room_id);
        } else {
            eprintln!("ASTRO DEBUG: Could not clear pending room recovery before cache refresh");
        }

        // Purge this room's SDK event cache while retaining the existing
        // Timeline and its observer. The SDK resets the existing room state
        // and notifies live observers; building another Timeline here would
        // attempt to register a duplicate room cache state.
        let (json, update_stream) = self.runtime.block_on(async {
            client
                .event_cache()
                .forget_room(&parsed_room_id)
                .await
                .map_err(|error| format!("Could not clear cached room events: {error}"))?;

            eprintln!("ASTRO DEBUG: Purged Matrix SDK event cache for room: {room_id}");

            // Reload history through the existing Timeline and cache handle.
            let reached_start = timeline
                .paginate_backwards(20)
                .await
                .map_err(|error| format!("Could not reload room history: {error}"))?;

            eprintln!(
                "ASTRO REFRESH TRACE: Pagination completed; reached_start={reached_start}"
            );

            // Take a fresh snapshot after pagination and establish a new
            // Astro-side subscription for subsequent timeline updates.
            let (items, updates) = timeline.subscribe().await;

            eprintln!(
                "ASTRO REFRESH TRACE: SDK timeline snapshot contains {} items",
                items.len()
            );

            let messages: Vec<serde_json::Value> = items
                .iter()
                .filter_map(|item| {
                    timeline_item_to_json(item.as_ref(), own_user_id.as_deref())
                })
                .collect();

            eprintln!("ASTRO REFRESH TRACE: Serialized {} messages", messages.len());

            let json = serde_json::json!({
                "room_id": room_id,
                "messages": messages,
                "reached_start": reached_start
            })
            .to_string();

            Ok::<_, String>((json, Box::pin(updates) as TimelineUpdateStream))
        })?;

        // Replace only Astro's subscription. The SDK Timeline itself remains
        // in self.timelines and continues owning the existing cache state.
        self.timeline_update_streams
            .insert(room_id.to_owned(), update_stream);

        if let Ok(mut active_room) = self.active_timeline_room.lock() {
            *active_room = Some(room_id.to_owned());
        } else {
            eprintln!("ASTRO DEBUG: Could not restore active room after cache refresh");
        }

        eprintln!("ASTRO DEBUG: Refreshed existing room timeline after cache purge: {room_id}");

        Ok(json)
    }

    /// Load another batch of older messages from an existing room timeline.
    pub fn paginate_room_timeline_json(&self, room_id: &str) -> Result<String, String> {
        let own_user_id = self
            .client
            .as_ref()
            .and_then(|client| client.user_id())
            .map(ToString::to_string);

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
                .filter_map(|item| timeline_item_to_json(item.as_ref(), own_user_id.as_deref()))
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
        let own_user_id = self
            .client
            .as_ref()
            .and_then(|client| client.user_id())
            .map(ToString::to_string);

        let runtime = std::sync::Arc::clone(&self.runtime);
        let mut updates = Vec::new();

        // Reconcile the active room after reconnecting. Take a fresh
        // subscription and snapshot from the existing timeline, rather
        // than building a temporary timeline or paginating backwards.
        let pending_recoveries = self
            .pending_timeline_recovery
            .lock()
            .map(|mut queue| std::mem::take(&mut *queue))
            .unwrap_or_default();

        for room_id in pending_recoveries {
            let Some(timeline) = self.timelines.get(&room_id) else {
                eprintln!(
                    "ASTRO DEBUG: Cannot recover room timeline; no active timeline exists: {room_id}"
                );
                continue;
            };

            let (items, update_stream) = runtime.block_on(timeline.subscribe());

            // Replace the old subscription with the fresh one. The
            // snapshot and stream now belong to the same subscription.
            self.timeline_update_streams
                .insert(room_id.clone(), Box::pin(update_stream) as TimelineUpdateStream);

            let mut recovered_count = 0usize;

            for item in items.iter() {
                if let Some(message) =
                    timeline_item_to_json(item.as_ref(), own_user_id.as_deref())
                {
                    updates.push(serde_json::json!({
                        "room_id": room_id,
                        "operation": "upsert",
                        "message": message
                    }));
                    recovered_count += 1;
                }
            }

            eprintln!(
                "ASTRO DEBUG: Room timeline snapshot recovery queued {recovered_count} messages: {room_id}"
            );
        }

        let _runtime_guard = runtime.enter();

        // Drain several ready batches per room so a temporary backlog
        // does not accumulate faster than the 100 ms polling interval.
        const MAX_BATCHES_PER_ROOM: usize = 32;

        for (room_id, stream) in self.timeline_update_streams.iter_mut() {
            for _ in 0..MAX_BATCHES_PER_ROOM {
                let batch = match stream.next().now_or_never() {
                    Some(Some(batch)) => batch,
                    // No batch is ready, or this stream has ended.
                    _ => break,
                };

                for diff in batch {
                    match diff {
                        VectorDiff::PushBack { value }
                        | VectorDiff::PushFront { value }
                        | VectorDiff::Insert { value, .. }
                        | VectorDiff::Set { value, .. } => {
                            if let Some(message) =
                                timeline_item_to_json(value.as_ref(), own_user_id.as_deref())
                            {
                                updates.push(serde_json::json!({
                                    "room_id": room_id,
                                    "operation": "upsert",
                                    "message": message
                                }));
                            }
                        }

                        VectorDiff::Append { values }
                        | VectorDiff::Reset { values } => {
                            // Reset contains the current vector snapshot.
                            // Upsert its events without deleting QML history:
                            // that model may also contain older paginated items.
                            for value in values.iter() {
                                if let Some(message) =
                                    timeline_item_to_json(value.as_ref(), own_user_id.as_deref())
                                {
                                    updates.push(serde_json::json!({
                                        "room_id": room_id,
                                        "operation": "upsert",
                                        "message": message
                                    }));
                                }
                            }
                        }

                        // Structural removals are deliberately not applied
                        // to QML, which may contain separately paginated history.
                        _ => {}
                    }
                }
            }
        }

        // Unread counts do not need to be scanned at the timeline's
        // 100 ms polling frequency. Check them once per second instead.
        let now = Instant::now();
        let should_check_unread = self.last_unread_check.map_or(true, |last| {
            now.duration_since(last) >= Duration::from_secs(1)
        });

        let mut unread_counts = Vec::new();
        let mut verification_events = Vec::new();
        let notification_events = self.notification_queue.drain_json();

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
                "is_outgoing": pending.is_outgoing,
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

            let (user_id, flow_id) = key.split_once('|').unwrap_or(("", ""));

            let event = serde_json::json!({
                "type": "verification_state",
                "user_id": user_id,
                "flow_id": flow_id,
                "state": state_name,
                "emojis": emojis
            });

            let signature = event.to_string();

            if self.last_verification_states.get(&key) != Some(&signature) {
                self.last_verification_states.insert(key.clone(), signature);

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

        if updates.is_empty()
            && unread_counts.is_empty()
            && verification_events.is_empty()
            && notification_events.is_empty()
        {
            return serde_json::json!({
                "updates": []
            })
            .to_string();
        }

        serde_json::json!({
            "updates": updates,
            "unread_counts": unread_counts,
            "verification_requests": verification_events,
            "notifications": notification_events
        })
        .to_string()
    }

    /// Change the display name of a registered Matrix device.
    pub fn rename_device(&self, device_id: &str, display_name: &str) -> Result<(), String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        if device_id.trim().is_empty() {
            return Err("Device ID is empty".to_string());
        }

        let display_name = display_name.trim().to_owned();

        if display_name.is_empty() {
            return Err("Device name cannot be empty".to_string());
        }

        let device_id: matrix_sdk::ruma::OwnedDeviceId = device_id.into();

        self.runtime.block_on(async move {
            client
                .rename_device(&device_id, &display_name)
                .await
                .map(|_| ())
                .map_err(|error| format!("Could not rename Matrix device: {error}"))
        })
    }


    /// Discover whether the homeserver advertises OAuth account management
    /// with support for deleting a specific device.
    ///
    /// Returns:
    /// - Ok(Some(url)): open this URL to manage the selected device.
    /// - Ok(None): use the legacy Matrix UIAA deletion flow.
    /// - Err(message): discovery failed; do not guess a fallback route.
    pub fn device_management_url(
        &self,
        device_id: &str,
    ) -> Result<Option<String>, String> {
        use matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::{
            AccountManagementAction,
            AccountManagementActionData,
            DeviceDeleteData,
            Request,
        };

        let client = self.client.as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        if device_id.trim().is_empty() {
            return Err("Device ID is empty".to_string());
        }

        if client.device_id().map(|id| id.as_str()) == Some(device_id) {
            return Err("Astro cannot sign out the current device".to_string());
        }

        let device_id: matrix_sdk::ruma::OwnedDeviceId = device_id.into();

        self.runtime.block_on(async move {
            let response = match client.send(Request::new()).await {
                Ok(response) => response,
                Err(error) => {
                    // An unsupported endpoint means this is a legacy server.
                    if error.as_client_api_error()
                        .is_some_and(|api_error| api_error.is_endpoint_not_implemented())
                    {
                        return Ok(None);
                    }

                    return Err(format!(
                        "Could not discover homeserver account-management support: {error}"
                    ));
                }
            };

            let metadata = response.metadata.deserialize()
                .map_err(|error| format!(
                    "Could not read homeserver authorization metadata: {error}"
                ))?;

            // Only use the web-management route when device deletion is
            // explicitly advertised (stable or its recognised unstable alias).
            if !metadata.is_account_management_action_supported(
                &AccountManagementAction::DeviceDelete
            ) {
                return Ok(None);
            }

            let action = AccountManagementActionData::DeviceDelete(
                DeviceDeleteData::new(&device_id)
            );

            let url = metadata.account_management_url_with_action(action)
                .ok_or_else(|| {
                    "Homeserver advertises device deletion but has no account-management URL."
                        .to_string()
                })?;

            Ok(Some(url.to_string()))
        })
    }


    /// Sign out another registered Matrix device using UIAA password authentication.
    pub fn revoke_device(&self, device_id: &str, password: &str) -> Result<(), String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        if device_id.trim().is_empty() {
            return Err("Device ID is empty".to_string());
        }

        if password.is_empty() {
            return Err("Account password is required".to_string());
        }

        if client.device_id().map(|id| id.as_str()) == Some(device_id) {
            return Err("Astro cannot sign out the current device".to_string());
        }

        let user_id = client
            .user_id()
            .ok_or_else(|| "Matrix user ID is unavailable".to_string())?
            .to_string();

        let device_id: matrix_sdk::ruma::OwnedDeviceId = device_id.into();
        let password = password.to_owned();

        self.runtime.block_on(async move {
            // Use the single-device DELETE endpoint.
            let request = delete_device::v3::Request::new(device_id.clone());

            // First request obtains the homeserver's UIAA challenge.
            match client.send(request).await {
                Ok(_) => Ok(()),
                Err(error) => {
                    let info = error
                        .as_uiaa_response()
                        .ok_or_else(|| format!("Could not start device sign-out: {error}"))?;

                    // Check whether a flow can be completed using the
                    // password stage and any stages already completed.
                    let password_flow_available = info.flows.iter().any(|flow| {
                        flow.stages
                            .iter()
                            .any(|stage| stage == &uiaa::AuthType::Password)
                            && flow.stages.iter().all(|stage| {
                                stage == &uiaa::AuthType::Password
                                    || info.completed.iter().any(|completed| completed == stage)
                            })
                    });

                    if !password_flow_available {
                        return Err("This homeserver requires an authentication method \
                             that Astro does not currently support for device sign-out."
                            .to_string());
                    }

                    let session = info.session.clone().ok_or_else(|| {
                        "The homeserver did not provide a UIAA session.".to_string()
                    })?;

                    let mut auth = uiaa::Password::new(
                        uiaa::UserIdentifier::Matrix(uiaa::MatrixUserIdentifier::new(user_id)),
                        password,
                    );

                    auth.session = Some(session);

                    let mut request = delete_device::v3::Request::new(device_id);

                    request.auth = Some(uiaa::AuthData::Password(auth));

                    client
                        .send(request)
                        .await
                        .map(|_| ())
                        .map_err(|error| format!("Could not sign out device: {error}"))
                }
            }
        })
    }

    /// Start an outgoing SAS-only verification with a registered device.
    pub fn request_device_verification(&self, device_id: &str) -> Result<(), String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        let user_id = client
            .user_id()
            .ok_or_else(|| "Matrix user ID is unavailable".to_string())?
            .to_owned();

        let device_id = device_id.to_owned();
        let request_queue = Arc::clone(&self.verification_requests);
        let sas_queue = Arc::clone(&self.verification_sas_queue);

        self.runtime.block_on(async move {
            let response = client
                .devices()
                .await
                .map_err(|error| format!("Could not retrieve Matrix devices: {error}"))?;

            let session = response
                .devices
                .into_iter()
                .find(|session| session.device_id.as_str() == device_id.as_str())
                .ok_or_else(|| "Registered Matrix device not found".to_string())?;

            let device = client
                .encryption()
                .get_device(&user_id, &session.device_id)
                .await
                .map_err(|error| format!("Could not retrieve device encryption data: {error}"))?
                .ok_or_else(|| {
                    "This device has no encryption identity and cannot be verified".to_string()
                })?;

            let request = device
                .request_verification_with_methods(vec![VerificationMethod::SasV1])
                .await
                .map_err(|error| format!("Could not start device verification: {error}"))?;

            let flow_id = request.flow_id().to_owned();
            let key = format!("{}|{}", user_id, flow_id);

            let pending = PendingVerificationRequest {
                user_id: user_id.to_string(),
                flow_id,
                room_id: None,
                is_outgoing: true,
                request: request.clone(),
            };

            request_queue
                .lock()
                .map_err(|_| "Verification request queue is unavailable".to_string())?
                .push(pending);

            // Follow the Matrix SDK verification sequence: both devices start
            // SAS after the request reaches Ready. The SDK resolves simultaneous
            // starts and determines which side must accept the resulting SAS flow.
            let mut changes = request.changes();
            tokio::spawn(async move {
                while let Some(state) = changes.next().await {
                    match state {
                        VerificationRequestState::Ready { .. } => {
                            if let Err(error) = request.start_sas().await {
                                eprintln!(
                                    "Astro: could not start outgoing SAS verification for {}: {}",
                                    key, error
                                );
                                break;
                            }
                        }
                        VerificationRequestState::Transitioned { verification } => {
                            if let Verification::SasV1(sas) = verification {
                                if let Err(error) = sas.accept().await {
                                    eprintln!(
                                        "Astro: could not accept outgoing SAS verification for {}: {}",
                                        key, error
                                    );
                                    break;
                                }

                                if let Ok(mut queue) = sas_queue.lock() {
                                    queue.push((key.clone(), sas));
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

    /// Accept an incoming verification request and negotiate SAS.
    pub fn accept_verification_request(&self, user_id: &str, flow_id: &str) -> Result<(), String> {
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

            // Wait for both devices to be ready before negotiating SAS.
            let mut changes = request.changes();

            tokio::spawn(async move {
                while let Some(state) = changes.next().await {
                    match state {
                        VerificationRequestState::Ready { .. } => {
                            if let Err(error) = request.start_sas().await {
                                eprintln!(
                                    "Astro: could not start incoming SAS verification for {}: {}",
                                    key, error
                                );
                                break;
                            }
                        }
                        VerificationRequestState::Transitioned { verification } => {
                            if let Verification::SasV1(sas) = verification {
                                if let Err(error) = sas.accept().await {
                                    eprintln!(
                                        "Astro: could not accept incoming SAS verification for {}: {}",
                                        key, error
                                    );
                                    break;
                                }

                                if let Ok(mut queue) = sas_queue.lock() {
                                    queue.push((key.clone(), sas));
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
    pub fn confirm_verification(&self, user_id: &str, flow_id: &str) -> Result<(), String> {
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
    pub fn mismatch_verification(&self, user_id: &str, flow_id: &str) -> Result<(), String> {
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


    /// Send a plain-text reply to an existing room timeline event.
    pub fn send_room_reply(
        &self,
        room_id: &str,
        event_id: &str,
        message: &str,
    ) -> Result<(), String> {
        if message.trim().is_empty() {
            return Err("Reply must not be empty".into());
        }

        let parsed_event_id = matrix_sdk::ruma::EventId::parse(event_id)
            .map_err(|error| format!("Invalid Matrix event ID: {error}"))?;

        let timeline = self
            .timelines
            .get(room_id)
            .ok_or_else(|| format!("No active timeline for room: {room_id}"))?;

        self.runtime.block_on(async {
            let content =
                RoomMessageEventContentWithoutRelation::text_plain(message.to_owned());

            timeline
                .send_reply(content, parsed_event_id)
                .await
                .map(|_| ())
                .map_err(|error| format!("Could not send Matrix reply: {error}"))
        })
    }

    /// Edit an existing plain-text room message.
    pub fn edit_room_message(
        &self,
        room_id: &str,
        event_id: &str,
        message: &str,
    ) -> Result<(), String> {
        if message.trim().is_empty() {
            return Err("Edited message must not be empty".into());
        }

        let parsed_event_id = matrix_sdk::ruma::EventId::parse(event_id)
            .map_err(|error| format!("Invalid Matrix event ID: {error}"))?;

        let timeline = self
            .timelines
            .get(room_id)
            .ok_or_else(|| format!("No active timeline for room: {room_id}"))?;

        let item_id = TimelineEventItemId::EventId(parsed_event_id);
        let content =
            RoomMessageEventContentWithoutRelation::text_plain(message.to_owned());

        self.runtime.block_on(async {
            timeline
                .edit(&item_id, EditedContent::RoomMessage(content))
                .await
                .map_err(|error| format!("Could not edit Matrix message: {error}"))
        })
    }

    /// Redact an existing room timeline event.
    pub fn redact_room_event(
        &self,
        room_id: &str,
        event_id: &str,
    ) -> Result<(), String> {
        let parsed_event_id = matrix_sdk::ruma::EventId::parse(event_id)
            .map_err(|error| format!("Invalid Matrix event ID: {error}"))?;

        let timeline = self
            .timelines
            .get(room_id)
            .ok_or_else(|| format!("No active timeline for room: {room_id}"))?;

        let item_id = TimelineEventItemId::EventId(parsed_event_id);

        self.runtime.block_on(async {
            timeline
                .redact(&item_id, None)
                .await
                .map_err(|error| format!("Could not redact Matrix event: {error}"))
        })
    }

    /// Upload and send an attachment to a Matrix room.
    ///
    /// The SDK selects the appropriate media event type and handles
    /// encrypted media automatically for encrypted rooms.
    pub fn send_room_attachment(
        &self,
        room_id: &str,
        file_path: &str,
        content_type: &str,
        caption: &str,
        progress_callback: Option<extern "C" fn(*mut c_void, usize, usize)>,
        progress_context: *mut c_void,
    ) -> Result<String, String> {
        const MAX_ATTACHMENT_SIZE: u64 = 256 * 1024 * 1024;

        if file_path.trim().is_empty() {
            return Err("Attachment file path must not be empty".into());
        }

        if content_type.trim().is_empty() {
            return Err("Attachment MIME type must not be empty".into());
        }

        let path = Path::new(file_path);

        let file_name = path
            .file_name()
            .and_then(|name| name.to_str())
            .filter(|name| !name.is_empty())
            .ok_or_else(|| "Could not determine attachment filename".to_string())?
            .to_owned();

        let metadata = fs::metadata(path)
            .map_err(|error| format!("Could not inspect attachment: {error}"))?;

        if !metadata.is_file() {
            return Err("Attachment path is not a regular file".into());
        }

        if metadata.len() == 0 {
            return Err("Attachment file is empty".into());
        }

        if metadata.len() > MAX_ATTACHMENT_SIZE {
            return Err("Attachment exceeds the 256 MiB upload limit".into());
        }

        let content_type: mime::Mime = content_type
            .parse()
            .map_err(|error| format!("Invalid attachment MIME type: {error}"))?;

        let data = fs::read(path)
            .map_err(|error| format!("Could not read attachment: {error}"))?;

        if data.is_empty() {
            return Err("Attachment file is empty".into());
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

        let caption = if caption.trim().is_empty() {
            None
        } else {
            Some(TextMessageEventContent::plain(caption.to_owned()))
        };

        let config = matrix_sdk::attachment::AttachmentConfig::new()
            .caption(caption);

        let progress = SharedObservable::new(
            TransmissionProgress::default()
        );

        let mut subscriber = progress.subscribe();
        let send_progress = progress.clone();

        let send_future = async move {
            room.send_attachment(
                file_name,
                &content_type,
                data,
                config,
            )
            .with_send_progress_observable(send_progress)
            .await
        };

        let result = self.runtime.block_on(async {
            tokio::pin!(send_future);

            loop {
                tokio::select! {
                    result = &mut send_future => {
                        break result;
                    }

                    update = subscriber.next() => {
                        if let Some(update) = update {
                            if let Some(callback) = progress_callback {
                                callback(
                                    progress_context,
                                    update.current,
                                    update.total,
                                );
                            }
                        }
                    }
                }
            }
        });

        result
            .map(|response| response.event_id.to_string())
            .map_err(|error| {
                format!("Could not upload and send attachment: {error}")
            })
    }

    /// Toggle a Matrix reaction on an existing room timeline event.
    ///
    /// Returns true when added and false when removed.
    pub fn toggle_room_reaction(
        &self,
        room_id: &str,
        event_id: &str,
        reaction_key: &str,
    ) -> Result<bool, String> {
        if reaction_key.is_empty() {
            return Err("Reaction key must not be empty".into());
        }

        let parsed_event_id = matrix_sdk::ruma::EventId::parse(event_id)
            .map_err(|error| format!("Invalid Matrix event ID: {error}"))?;

        let timeline = self
            .timelines
            .get(room_id)
            .ok_or_else(|| format!("No active timeline for room: {room_id}"))?;

        let item_id = TimelineEventItemId::EventId(parsed_event_id);

        self.runtime.block_on(async {
            timeline
                .toggle_reaction(&item_id, reaction_key)
                .await
                .map_err(|error| format!("Could not toggle Matrix reaction: {error}"))
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

    pub fn register_push_token(&self, push_token: &str, privacy_level: &str) -> Result<(), String> {
        if push_token.is_empty() {
            return Err("Ubuntu push token must not be empty".to_string());
        }

        let privacy_level = match privacy_level {
            "sender_room" | "full_message" | "private" => privacy_level,
            _ => "sender_room",
        };

        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        let pusher = PusherInit {
            ids: PusherIds::new(push_token.to_owned(), "astro.madabu".to_owned()),
            app_display_name: "Astro".to_owned(),
            kind: {
                let mut http_data = HttpPusherData::new(
                    "https://astro-push-gateway.devabu.workers.dev/_matrix/push/v1/notify".to_owned(),
                );

                http_data.data.insert(
                    "privacy_level".to_owned(),
                    serde_json::Value::String(privacy_level.to_owned()),
                );

                PusherKind::Http(http_data)
            },
            lang: "en".to_owned(),
            device_display_name: "Astro on Ubuntu Touch".to_owned(),
            profile_tag: None,
        };

        self.runtime.block_on(async {
            client
                .pusher()
                .set(pusher.into(), false)
                .await
                .map_err(|error| format!("Could not register Matrix push pusher: {error}"))
        })
    }

    pub fn unregister_push_token(&self, push_token: &str) -> Result<(), String> {
        if push_token.is_empty() {
            return Err("Ubuntu push token must not be empty".to_string());
        }

        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        let pusher_ids = PusherIds::new(
            push_token.to_owned(),
            "astro.madabu".to_owned(),
        );

        self.runtime.block_on(async {
            client
                .pusher()
                .delete(pusher_ids)
                .await
                .map_err(|error| format!("Could not unregister Matrix push pusher: {error}"))
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
