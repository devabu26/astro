//! MatrixRTC per-member media-key exchange (MSC4143 / MSC4195).
//!
//! This module distributes and validates media keys. It does not yet enable
//! LiveKit frame encryption; that remains a separate integration stage so the
//! existing audio/video publishing path is not changed here.

use std::collections::{BTreeMap, HashSet, VecDeque};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use futures_util::StreamExt;
use matrix_sdk::{
    ruma::{
        events::{AnyToDeviceEventContent, ToDeviceEventType},
        serde::Raw,
        to_device::DeviceIdOrAllDevices,
        OwnedDeviceId, OwnedUserId,
    },
    Client, Room,
};
use serde_json::Value;
use tokio::runtime::Runtime;
use tokio::task::AbortHandle;

const RTC_SLOT_ID: &str = "m.call#room";
const RTC_MEMBER_EVENT: &str = "m.rtc.member";
const RTC_KEY_EVENT: &str = "m.rtc.encryption_key";
const LEGACY_RTC_KEY_EVENT: &str = "io.element.call.encryption_keys";
const RTC_KEY_BYTES: usize = 32;
const RTC_KEY_ACTIVATION_DELAY: Duration = Duration::from_secs(5);
const RTC_PENDING_KEY_LIMIT: usize = 128;

#[derive(Clone)]
struct MediaKey {
    index: u8,
    bytes: Vec<u8>,
    activate_at: Instant,
}

#[derive(Clone)]
struct PendingMediaKey {
    room_id: String,
    member_id: String,
    sender: OwnedUserId,
    sender_device: OwnedDeviceId,
    index: u8,
    bytes: Vec<u8>,
}

#[derive(Clone)]
struct ReceivedMediaKey {
    livekit_identity: String,
    bytes: Vec<u8>,
    activate_at: Instant,
}

pub(crate) struct RtcE2eeSession {
    task: AbortHandle,
    local_key: Arc<Mutex<MediaKey>>,
    local_livekit_identity: String,
    received_keys: Arc<Mutex<BTreeMap<(String, u8), ReceivedMediaKey>>>,
}

impl RtcE2eeSession {
    pub(crate) fn start(
        runtime: &Runtime,
        client: Client,
        room: Room,
        local_member_id: String,
    ) -> Result<Self, String> {
        let mut initial_bytes = [0u8; RTC_KEY_BYTES];
        getrandom::getrandom(&mut initial_bytes)
            .map_err(|error| format!("Could not generate a MatrixRTC media key: {error}"))?;

        let local_key = Arc::new(Mutex::new(MediaKey {
            index: 0,
            bytes: initial_bytes.to_vec(),
            activate_at: Instant::now() + RTC_KEY_ACTIVATION_DELAY,
        }));
        let received_keys = Arc::new(Mutex::new(BTreeMap::new()));
        let local_livekit_identity = legacy_livekit_identity(&client)?;

        let task_local_key = Arc::clone(&local_key);
        let task_received_keys = Arc::clone(&received_keys);
        let task_client = client.clone();
        let task_room = room.clone();

        let handle = runtime.spawn(async move {
            let event_type = ToDeviceEventType::from(RTC_KEY_EVENT);
            let mut incoming = task_client
                .subscribe_to_custom_to_device_messages(vec![event_type]);
            let mut sticky_updates = task_room.sticky_events().subscribe();
            let mut rotation = tokio::time::interval(Duration::from_secs(15 * 60));
            // Skip interval's immediate first tick; the first key was generated above.
            rotation.tick().await;
            let mut initial_retry = tokio::time::interval(Duration::from_secs(5));
            initial_retry.tick().await;
            let mut pending = VecDeque::<PendingMediaKey>::new();

            let mut initial_key_distributed = publish_local_key(
                &task_client,
                &task_room,
                &local_member_id,
                &task_local_key,
            )
            .await;

            loop {
                tokio::select! {
                    message = incoming.next() => {
                        let Some(message) = message else { break };
                        if let Some(candidate) = parse_incoming_key(message) {
                            if try_store_received_key(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                continue;
                            }

                            if pending.len() == RTC_PENDING_KEY_LIMIT {
                                pending.pop_front();
                            }
                            pending.push_back(candidate);
                        }
                    }

                    _ = initial_retry.tick(), if !initial_key_distributed => {
                        initial_key_distributed = publish_local_key(
                            &task_client,
                            &task_room,
                            &local_member_id,
                            &task_local_key,
                        ).await;
                    }

                    _ = rotation.tick() => {
                        rotate_local_key(&task_local_key);
                        initial_key_distributed = publish_local_key(
                            &task_client,
                            &task_room,
                            &local_member_id,
                            &task_local_key,
                        ).await;
                    }

                    update = sticky_updates.recv() => {
                        let changed = match update {
                            Ok(update) => {
                                let added_or_updated = update.added.iter()
                                    .chain(update.updated.iter())
                                    .any(|event| {
                                        if event.key.event_type.to_string() != RTC_MEMBER_EVENT {
                                            return false;
                                        }
                                        serde_json::from_str::<Value>(event.raw().json().get())
                                            .ok()
                                            .and_then(|value| value.get("content").cloned())
                                            .and_then(|content| content.get("slot_id")
                                                .and_then(Value::as_str)
                                                .map(str::to_owned))
                                            .as_deref() == Some(RTC_SLOT_ID)
                                    });
                                let removed = update.removed.iter()
                                    .any(|(key, _)| key.event_type.to_string() == RTC_MEMBER_EVENT);
                                added_or_updated || removed
                            }
                            Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => true,
                            Err(tokio::sync::broadcast::error::RecvError::Closed) => break,
                        };

                        if changed {
                            rotate_local_key(&task_local_key);
                            initial_key_distributed = publish_local_key(
                                &task_client,
                                &task_room,
                                &local_member_id,
                                &task_local_key,
                            )
                            .await;
                        }

                        let mut still_pending = VecDeque::new();
                        while let Some(candidate) = pending.pop_front() {
                            if !try_store_received_key(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                still_pending.push_back(candidate);
                            }
                        }
                        pending = still_pending;
                    }
                }
            }
        });

        Ok(Self {
            task: handle.abort_handle(),
            local_key,
            local_livekit_identity,
            received_keys,
        })
    }


    /// Start the legacy MSC3401 room-state key exchange used when MSC4354
    /// sticky memberships are unavailable. Legacy member IDs are the
    /// Element Call convention: `sender_user_id:device_id`, unless the
    /// membership explicitly supplies a custom `membershipID`.
    pub(crate) fn start_legacy(
        runtime: &Runtime,
        client: Client,
        room: Room,
        local_member_id: String,
    ) -> Result<Self, String> {
        let mut initial_bytes = [0u8; RTC_KEY_BYTES];
        getrandom::getrandom(&mut initial_bytes)
            .map_err(|error| format!("Could not generate a MatrixRTC media key: {error}"))?;

        let local_key = Arc::new(Mutex::new(MediaKey {
            index: 0,
            bytes: initial_bytes.to_vec(),
            activate_at: Instant::now() + RTC_KEY_ACTIVATION_DELAY,
        }));
        let received_keys = Arc::new(Mutex::new(BTreeMap::new()));

        let task_local_key = Arc::clone(&local_key);
        let task_received_keys = Arc::clone(&received_keys);
        let task_client = client.clone();
        let task_room = room.clone();

        let handle = runtime.spawn(async move {
            let event_types = vec![ToDeviceEventType::from(LEGACY_RTC_KEY_EVENT)];
            let mut incoming = task_client
                .subscribe_to_custom_to_device_messages(event_types);
            let mut poll = tokio::time::interval(Duration::from_secs(5));
            let mut rotation = tokio::time::interval(Duration::from_secs(15 * 60));
            poll.tick().await;
            rotation.tick().await;

            let mut pending = VecDeque::<PendingMediaKey>::new();
            let mut previous_members = HashSet::<String>::new();
            let mut initial_key_distributed = false;

            // Poll immediately after subscription so the local legacy state
            // membership and other participants are visible before key send.
            if let Some(members) = legacy_memberships(&task_room).await {
                previous_members = members.iter()
                    .map(|member| member.member_id.clone())
                    .collect();
                initial_key_distributed = publish_local_key_legacy(
                    &task_client,
                    &task_room,
                    &local_member_id,
                    &task_local_key,
                    &members,
                ).await;
            }

            loop {
                tokio::select! {
                    message = incoming.next() => {
                        let Some(message) = message else { break };
                        if let Some(candidate) = parse_incoming_key(message) {
                            if try_store_received_key_legacy(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                continue;
                            }
                            if pending.len() == RTC_PENDING_KEY_LIMIT {
                                pending.pop_front();
                            }
                            pending.push_back(candidate);
                        }
                    }

                    _ = poll.tick() => {
                        let Some(members) = legacy_memberships(&task_room).await else {
                            continue;
                        };
                        let current_members = members.iter()
                            .map(|member| member.member_id.clone())
                            .collect::<HashSet<_>>();
                        if !initial_key_distributed || current_members != previous_members {
                            if initial_key_distributed {
                                rotate_local_key(&task_local_key);
                            }
                            initial_key_distributed = publish_local_key_legacy(
                                &task_client,
                                &task_room,
                                &local_member_id,
                                &task_local_key,
                                &members,
                            ).await;
                            previous_members = current_members;
                        }

                        let mut still_pending = VecDeque::new();
                        while let Some(candidate) = pending.pop_front() {
                            if !try_store_received_key_legacy(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                still_pending.push_back(candidate);
                            }
                        }
                        pending = still_pending;
                    }

                    _ = rotation.tick() => {
                        rotate_local_key(&task_local_key);
                        let members = legacy_memberships(&task_room).await.unwrap_or_default();
                        initial_key_distributed = publish_local_key_legacy(
                            &task_client,
                            &task_room,
                            &local_member_id,
                            &task_local_key,
                            &members,
                        ).await;
                    }
                }
            }
        });

        Ok(Self {
            task: handle.abort_handle(),
            local_key,
            received_keys,
        })
    }

    /// The active sender key, available only after the MSC4143 delivery grace.
    #[allow(dead_code)]
    pub(crate) fn current_local_key(&self) -> Option<(u8, Vec<u8>)> {
        let key = self.local_key.lock().ok()?;
        (Instant::now() >= key.activate_at)
            .then(|| (key.index, key.bytes.clone()))
    }

    /// Look up a received sender key by MatrixRTC member ID and key index.
    #[allow(dead_code)]
    pub(crate) fn received_key(&self, member_id: &str, index: u8) -> Option<Vec<u8>> {
        let key = self.received_keys.lock().ok()?
            .get(&(member_id.to_owned(), index)).cloned()?;
        (Instant::now() >= key.activate_at).then_some(key.bytes)
    }

    pub(crate) fn local_livekit_identity(&self) -> &str {
        &self.local_livekit_identity
    }

    /// Return all keys that have passed the MatrixRTC five-second activation
    /// grace, mapped to the legacy /sfu/get LiveKit participant identity.
    pub(crate) fn livekit_key_snapshot(&self) -> Vec<(String, u8, Vec<u8>)> {
        let mut snapshot = Vec::new();
        if let Some((index, key)) = self.current_local_key() {
            snapshot.push((self.local_livekit_identity.clone(), index, key));
        }

        let now = Instant::now();
        if let Ok(keys) = self.received_keys.lock() {
            for ((_, index), key) in keys.iter() {
                if now >= key.activate_at {
                    snapshot.push((key.livekit_identity.clone(), *index, key.bytes.clone()));
                }
            }
        }
        snapshot
    }
}

impl Drop for RtcE2eeSession {
    fn drop(&mut self) {
        self.task.abort();
    }
}

fn legacy_livekit_identity(client: &Client) -> Result<String, String> {
    let user_id = client.user_id()
        .ok_or_else(|| "Matrix user ID is unavailable for LiveKit E2EE identity".to_string())?;
    let device_id = client.device_id()
        .ok_or_else(|| "Matrix device ID is unavailable for LiveKit E2EE identity".to_string())?;
    // Astro currently obtains credentials through the legacy /sfu/get flow,
    // whose LiveKit identity is the legacy user-id:device-id form.
    Ok(format!("{user_id}:{device_id}"))
}

fn rotate_local_key(local_key: &Arc<Mutex<MediaKey>>) {
    let mut bytes = [0u8; RTC_KEY_BYTES];
    if let Err(error) = getrandom::getrandom(&mut bytes) {
        eprintln!("ASTRO RTC E2EE: media-key rotation failed: {error}");
        return;
    }

    if let Ok(mut key) = local_key.lock() {
        key.index = key.index.wrapping_add(1);
        key.bytes = bytes.to_vec();
        key.activate_at = Instant::now() + RTC_KEY_ACTIVATION_DELAY;
    }
}

fn is_verified(info: &matrix_sdk::deserialized_responses::EncryptionInfo) -> bool {
    matches!(
        info.verification_state,
        matrix_sdk::deserialized_responses::VerificationState::Verified
    )
}

async fn device_is_cross_signed(
    client: &Client,
    user_id: &OwnedUserId,
    device_id: &OwnedDeviceId,
) -> bool {
    match client.encryption().get_device(user_id, device_id).await {
        Ok(Some(device)) => device.is_cross_signed_by_owner(),
        Ok(None) | Err(_) => false,
    }
}

async fn publish_local_key(
    client: &Client,
    room: &Room,
    member_id: &str,
    local_key: &Arc<Mutex<MediaKey>>,
) -> bool {
    let key = match local_key.lock() {
        Ok(key) => key.clone(),
        Err(_) => return false,
    };

    let own_user = client.user_id().map(ToString::to_string);
    let own_device = client.device_id().map(ToString::to_string);
    let mut recipients: BTreeMap<OwnedUserId, Vec<DeviceIdOrAllDevices>> = BTreeMap::new();
    let mut seen = HashSet::<(String, String)>::new();

    for event in room.sticky_events().live() {
        if event.key.event_type.to_string() != RTC_MEMBER_EVENT {
            continue;
        }

        let value: Value = match serde_json::from_str(event.raw().json().get()) {
            Ok(value) => value,
            Err(_) => continue,
        };
        let Some(content) = value.get("content") else { continue };
        let Some(member) = content.get("member") else { continue };
        let Some(event_member_id) = member.get("id").and_then(Value::as_str) else { continue };

        if content.get("slot_id").and_then(Value::as_str) != Some(RTC_SLOT_ID)
            || member.get("membership").and_then(Value::as_str) != Some("join")
            || content.get("sticky_key").and_then(Value::as_str) != Some(event_member_id)
        {
            continue;
        }

        let Some(info) = event.encryption_info() else {
            // Encrypted rooms require authenticated member events.
            continue;
        };
        if !is_verified(info)
            || value.get("sender").and_then(Value::as_str) != Some(info.sender.as_str())
        {
            continue;
        }

        let Some(device_id) = info.sender_device.as_ref() else { continue };
        if own_user.as_deref() == Some(info.sender.as_str())
            && own_device.as_deref() == Some(device_id.as_str())
        {
            continue;
        }

        let pair = (info.sender.to_string(), device_id.to_string());
        if !seen.insert(pair) {
            continue;
        }

        if !device_is_cross_signed(client, &info.sender, device_id).await {
            continue;
        }

        let Ok(device) = DeviceIdOrAllDevices::try_from(device_id.as_str()) else {
            continue;
        };
        recipients.entry(info.sender.clone()).or_default().push(device);
    }

    if recipients.is_empty() {
        return false;
    }

    let content = serde_json::json!({
        "room_id": room.room_id().as_str(),
        "member_id": member_id,
        "media_key": {
            "index": key.index,
            "key": encode_base64_no_pad(&key.bytes)
        }
    });

    let raw = match Raw::<AnyToDeviceEventContent>::from_json_string(content.to_string()) {
        Ok(raw) => raw,
        Err(error) => {
            eprintln!("ASTRO RTC E2EE: could not encode media-key message: {error}");
            return false;
        }
    };

    match client
        .send_encrypted_to_device(
            &ToDeviceEventType::from(RTC_KEY_EVENT),
            recipients,
            raw,
        )
        .await
    {
        Ok(failures) if failures.is_empty() => {
            eprintln!("ASTRO RTC E2EE: distributed media key index {} to active verified participants", key.index);
            true
        }
        Ok(failures) => {
            eprintln!("ASTRO RTC E2EE: media-key delivery had {} recipient failure group(s)", failures.len());
            false
        }
        Err(error) => {
            eprintln!("ASTRO RTC E2EE: encrypted media-key delivery failed: {error}");
            false
        }
    }
}

fn parse_incoming_key(message: matrix_sdk::ToDeviceMessage) -> Option<PendingMediaKey> {
    let info = message.encryption_info?;
    if !is_verified(&info) {
        return None;
    }

    let value: Value = serde_json::from_str(message.raw.json().get()).ok()?;
    let event_type = value.get("type").and_then(Value::as_str)?;
    if event_type != RTC_KEY_EVENT && event_type != LEGACY_RTC_KEY_EVENT {
        return None;
    }
    if value.get("sender").and_then(Value::as_str) != Some(info.sender.as_str()) {
        return None;
    }

    let sender_device = info.sender_device.clone()?;
    let content = value.get("content")?;
    let room_id = content.get("room_id")?.as_str()?.to_owned();
    if room_id.is_empty() { return None; }

    let (member_id, index, encoded_key) = if event_type == RTC_KEY_EVENT {
        let member_id = content.get("member_id")?.as_str()?.to_owned();
        let media_key = content.get("media_key")?;
        (
            member_id,
            u8::try_from(media_key.get("index")?.as_u64()?).ok()?,
            media_key.get("key")?.as_str()?,
        )
    } else {
        let session = content.get("session")?;
        if session.get("application").and_then(Value::as_str) != Some("m.call")
            || session.get("call_id").and_then(Value::as_str) != Some("")
            || session.get("scope").and_then(Value::as_str) != Some("m.room")
        {
            return None;
        }
        let member = content.get("member")?;
        let claimed_device_id = member.get("claimed_device_id")?.as_str()?;
        // Claimed device metadata is only a consistency check; Olm metadata
        // and the legacy room-state membership establish the trusted identity.
        if claimed_device_id != sender_device.as_str() { return None; }
        let member_id = member.get("id").and_then(Value::as_str)
            .map(str::to_owned)
            .unwrap_or_else(|| format!("{}:{claimed_device_id}", info.sender));
        let keys = content.get("keys")?;
        (
            member_id,
            u8::try_from(keys.get("index")?.as_u64()?).ok()?,
            keys.get("key")?.as_str()?,
        )
    };

    if member_id.is_empty() { return None; }
    let bytes = decode_base64_no_pad(encoded_key)?;
    if bytes.len() != RTC_KEY_BYTES { return None; }

    Some(PendingMediaKey {
        room_id,
        member_id,
        sender: info.sender.clone(),
        sender_device,
        index,
        bytes,
    })
}
async fn try_store_received_key(
    client: &Client,
    room: &Room,
    received_keys: &Arc<Mutex<BTreeMap<(String, u8), Vec<u8>>>>,
    candidate: &PendingMediaKey,
) -> bool {
    if candidate.room_id != room.room_id().as_str() {
        return true;
    }

    let live = room.sticky_events().live();

    for event in &live {
        if event.key.event_type.to_string() != RTC_MEMBER_EVENT {
            continue;
        }

        let value: Value = match serde_json::from_str(event.raw().json().get()) {
            Ok(value) => value,
            Err(_) => continue,
        };
        let Some(content) = value.get("content") else { continue };
        let Some(member) = content.get("member") else { continue };
        if content.get("slot_id").and_then(Value::as_str) != Some(RTC_SLOT_ID)
            || content.get("sticky_key").and_then(Value::as_str) != Some(candidate.member_id.as_str())
            || member.get("id").and_then(Value::as_str) != Some(candidate.member_id.as_str())
            || member.get("membership").and_then(Value::as_str) != Some("join")
        {
            continue;
        }

        let Some(info) = event.encryption_info() else { continue };
        if !is_verified(info)
            || info.sender != candidate.sender
            || info.sender_device.as_ref() != Some(&candidate.sender_device)
            || value.get("sender").and_then(Value::as_str) != Some(candidate.sender.as_str())
        {
            continue;
        }

        if !device_is_cross_signed(client, &candidate.sender, &candidate.sender_device).await {
            return true;
        }

        if let Ok(mut keys) = received_keys.lock() {
            keys.insert(
    (candidate.member_id.clone(), candidate.index),
    ReceivedMediaKey {
        livekit_identity: format!("{}:{}", candidate.sender, candidate.sender_device),
        bytes: candidate.bytes.clone(),
        activate_at: Instant::now() + RTC_KEY_ACTIVATION_DELAY,
    },
);
        }
        return true;
    }

    false
}


#[derive(Clone)]
struct LegacyRtcMember {
    member_id: String,
    user_id: OwnedUserId,
    device_id: OwnedDeviceId,
}

async fn legacy_memberships(room: &Room) -> Option<Vec<LegacyRtcMember>> {
    let events = room.get_state_events(
        matrix_sdk::ruma::events::StateEventType::from(
            "org.matrix.msc3401.call.member"
        )
    ).await.ok()?;

    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64;
    let mut members = Vec::new();
    let mut seen = HashSet::<(String, String)>::new();

    for event in events {
        let raw = match event {
            matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Sync(raw) => raw,
            matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Stripped(_) => continue,
        };
        let value: Value = match serde_json::from_str(raw.json().get()) {
            Ok(value) => value,
            Err(_) => continue,
        };
        let Some(sender) = value.get("sender").and_then(Value::as_str) else { continue };
        let Ok(user_id) = matrix_sdk::ruma::UserId::parse(sender) else { continue };
        let content = match value.get("content") {
            Some(content) if content.is_object() => content,
            _ => continue,
        };
        let entries: Vec<&Value> = content.get("memberships")
            .and_then(Value::as_array)
            .map(|items| items.iter().collect())
            .unwrap_or_else(|| vec![content]);

        for entry in entries {
            let application = entry.get("application").and_then(Value::as_str)
                .or_else(|| content.get("application").and_then(Value::as_str))
                .unwrap_or("");
            let call_id = entry.get("call_id").and_then(Value::as_str).unwrap_or("");
            let scope = entry.get("scope").and_then(Value::as_str).unwrap_or("");
            if application != "m.call" || !call_id.is_empty()
                || (!scope.is_empty() && scope != "m.room") {
                continue;
            }

            let Some(device_id_str) = entry.get("device_id").and_then(Value::as_str)
                .filter(|value| !value.is_empty()) else { continue };
            let device_id: OwnedDeviceId = device_id_str.into();

            let expiry = entry.get("expires_ts").and_then(Value::as_u64).or_else(|| {
                let duration = entry.get("expires").and_then(Value::as_u64)?;
                let base = entry.get("created_ts").and_then(Value::as_u64)
                    .or_else(|| value.get("origin_server_ts").and_then(Value::as_u64))?;
                base.checked_add(duration)
            });
            if expiry.is_some_and(|expiry| now_ms > expiry) {
                continue;
            }

            let member_id = entry.get("membershipID").and_then(Value::as_str)
                .filter(|value| !value.is_empty())
                .map(str::to_owned)
                .unwrap_or_else(|| format!("{sender}:{device_id_str}"));
            if !seen.insert((sender.to_owned(), device_id_str.to_owned())) {
                continue;
            }

            members.push(LegacyRtcMember {
                member_id,
                user_id: user_id.to_owned(),
                device_id: device_id.to_owned(),
            });
        }
    }

    Some(members)
}

async fn publish_local_key_legacy(
    client: &Client,
    room: &Room,
    member_id: &str,
    local_key: &Arc<Mutex<MediaKey>>,
    members: &[LegacyRtcMember],
) -> bool {
    let key = match local_key.lock() {
        Ok(key) => key.clone(),
        Err(_) => return false,
    };

    let own_user = client.user_id().map(ToString::to_string);
    let own_device = client.device_id().map(ToString::to_string);
    let mut recipients: BTreeMap<OwnedUserId, Vec<DeviceIdOrAllDevices>> = BTreeMap::new();
    let mut seen = HashSet::<(String, String)>::new();

    for member in members {
        if own_user.as_deref() == Some(member.user_id.as_str())
            && own_device.as_deref() == Some(member.device_id.as_str()) {
            continue;
        }
        let pair = (member.user_id.to_string(), member.device_id.to_string());
        if !seen.insert(pair) {
            continue;
        }
        if !device_is_cross_signed(client, &member.user_id, &member.device_id).await {
            continue;
        }
        let Ok(device) = DeviceIdOrAllDevices::try_from(member.device_id.as_str()) else {
            continue;
        };
        recipients.entry(member.user_id.clone()).or_default().push(device);
    }

    if recipients.is_empty() {
        return false;
    }

    let own_device = match client.device_id() {
        Some(device_id) => device_id.to_string(),
        None => return false,
    };
    let sent_ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64;
    // Pre-sticky Element Call clients use the legacy event name and envelope.
    let content = serde_json::json!({
        "room_id": room.room_id().as_str(),
        "keys": { "index": key.index, "key": encode_base64_no_pad(&key.bytes) },
        "member": { "id": member_id, "claimed_device_id": own_device },
        "session": { "application": "m.call", "call_id": "", "scope": "m.room" },
        "sent_ts": sent_ts
    });
    let raw = match Raw::<AnyToDeviceEventContent>::from_json_string(content.to_string()) {
        Ok(raw) => raw,
        Err(error) => {
            eprintln!("ASTRO RTC E2EE legacy: could not encode media-key message: {error}");
            return false;
        }
    };
    match client.send_encrypted_to_device(
        &ToDeviceEventType::from(LEGACY_RTC_KEY_EVENT),
        recipients,
        raw,
    ).await {
        Ok(failures) if failures.is_empty() => {
            eprintln!("ASTRO RTC E2EE legacy: distributed key index {} to verified legacy members", key.index);
            true
        }
        Ok(failures) => {
            eprintln!("ASTRO RTC E2EE legacy: key delivery had {} recipient failure group(s)", failures.len());
            false
        }
        Err(error) => {
            eprintln!("ASTRO RTC E2EE legacy: encrypted key delivery failed: {error}");
            false
        }
    }
}

async fn try_store_received_key_legacy(
    client: &Client,
    room: &Room,
    received_keys: &Arc<Mutex<BTreeMap<(String, u8), Vec<u8>>>>,
    candidate: &PendingMediaKey,
) -> bool {
    if candidate.room_id != room.room_id().as_str() {
        return true;
    }

    let Some(members) = legacy_memberships(room).await else { return false };
    let matching = members.iter().any(|member| {
        member.member_id == candidate.member_id
            && member.user_id == candidate.sender
            && member.device_id == candidate.sender_device
    });
    if !matching {
        return false;
    }

    if !device_is_cross_signed(client, &candidate.sender, &candidate.sender_device).await {
        return true;
    }

    if let Ok(mut keys) = received_keys.lock() {
        keys.insert((candidate.member_id.clone(), candidate.index), candidate.bytes.clone());
    }
    true
}

fn encode_base64_no_pad(bytes: &[u8]) -> String {
    const TABLE: &[u8; 64] =
        b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut output = String::with_capacity((bytes.len() * 4 + 2) / 3);

    for chunk in bytes.chunks(3) {
        let a = chunk[0] as u32;
        let b = chunk.get(1).copied().unwrap_or(0) as u32;
        let c = chunk.get(2).copied().unwrap_or(0) as u32;
        let value = (a << 16) | (b << 8) | c;

        output.push(TABLE[((value >> 18) & 0x3f) as usize] as char);
        output.push(TABLE[((value >> 12) & 0x3f) as usize] as char);
        if chunk.len() > 1 {
            output.push(TABLE[((value >> 6) & 0x3f) as usize] as char);
        }
        if chunk.len() > 2 {
            output.push(TABLE[(value & 0x3f) as usize] as char);
        }
    }

    output
}

fn decode_base64_no_pad(input: &str) -> Option<Vec<u8>> {
    fn value(byte: u8) -> Option<u8> {
        match byte {
            b'A'..=b'Z' => Some(byte - b'A'),
            b'a'..=b'z' => Some(byte - b'a' + 26),
            b'0'..=b'9' => Some(byte - b'0' + 52),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    }

    if input.is_empty() || input.contains('=') || input.len() % 4 == 1 {
        return None;
    }

    let mut output = Vec::with_capacity(input.len() * 3 / 4);
    let mut accumulator = 0u32;
    let mut bits = 0u8;

    for byte in input.bytes() {
        accumulator = (accumulator << 6) | value(byte)? as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            output.push(((accumulator >> bits) & 0xff) as u8);
        }
    }

    Some(output)
}
