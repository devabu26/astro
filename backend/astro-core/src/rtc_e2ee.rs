//! MatrixRTC per-member media-key exchange (MSC4143 / MSC4195).
//!
//! This module distributes MatrixRTC media keys and exposes them to LiveKit's
//! per-participant frame cryptor key provider.

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
use sha2::Digest;
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
        let own_user_id = client.user_id()
            .ok_or_else(|| "Matrix user ID is unavailable for MSC4195 LiveKit identity".to_string())?;
        let local_livekit_identity =
            modern_livekit_identity(own_user_id.as_str(), &local_member_id)?;
        // The compatibility state membership and /sfu/get token still use
        // the legacy user_id:device_id identity.
        let task_legacy_member_id = legacy_livekit_identity(&client)?;

        let task_local_key = Arc::clone(&local_key);
        let task_received_keys = Arc::clone(&received_keys);
        let task_client = client.clone();
        let task_room = room.clone();

        let handle = runtime.spawn(async move {
            // Listen to both the current MatrixRTC key event and the legacy
            // Element Call event. Astro publishes sticky membership for the
            // modern transport and legacy state membership for call
            // compatibility, so distribute keys in both event formats.
            let event_types = vec![
                ToDeviceEventType::from(RTC_KEY_EVENT),
                ToDeviceEventType::from(LEGACY_RTC_KEY_EVENT),
            ];
            let mut incoming = Box::pin(
                task_client.subscribe_to_custom_to_device_messages(event_types),
            );
            let mut sticky_updates = task_room.sticky_events().subscribe();
            let mut rotation = tokio::time::interval(Duration::from_secs(15 * 60));
            rotation.tick().await;
            let mut initial_retry = tokio::time::interval(Duration::from_secs(5));
            initial_retry.tick().await;
            let mut legacy_poll = tokio::time::interval(Duration::from_secs(5));
            legacy_poll.tick().await;
            let mut pending = VecDeque::<PendingMediaKey>::new();

            let mut initial_key_distributed = publish_local_key(
                &task_client,
                &task_room,
                &local_member_id,
                &task_local_key,
            ).await;

            // Compatibility distribution for clients which still advertise
            // membership through org.matrix.msc3401.call.member. Keep the
            // same sender key and index for both membership generations.
            let mut previous_legacy_members = HashSet::<String>::new();
            let mut legacy_key_distributed = false;
            if let Some(members) = legacy_memberships(&task_room).await {
                previous_legacy_members = members.iter()
                    .map(|member| member.member_id.clone())
                    .collect();
                legacy_key_distributed = publish_local_key_legacy(
                    &task_client,
                    &task_room,
                    &task_legacy_member_id,
                    &task_local_key,
                    &members,
                ).await;
            }

            loop {
                tokio::select! {
                    message = incoming.next() => {
                        let Some(message) = message else { break };
                        if let Some(candidate) = parse_incoming_key(message) {
                            let stored = if try_store_received_key(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                true
                            } else {
                                try_store_received_key_legacy(
                                    &task_client,
                                    &task_room,
                                    &task_received_keys,
                                    &candidate,
                                ).await
                            };

                            if !stored {
                                if pending.len() == RTC_PENDING_KEY_LIMIT {
                                    pending.pop_front();
                                }
                                pending.push_back(candidate);
                            }
                        }
                    }

                    _ = initial_retry.tick(), if !initial_key_distributed || !legacy_key_distributed => {
                        if !initial_key_distributed {
                            initial_key_distributed = publish_local_key(
                                &task_client,
                                &task_room,
                                &local_member_id,
                                &task_local_key,
                            ).await;
                        }
                        if !legacy_key_distributed {
                            let members = legacy_memberships(&task_room).await.unwrap_or_default();
                            legacy_key_distributed = publish_local_key_legacy(
                                &task_client,
                                &task_room,
                                &task_legacy_member_id,
                                &task_local_key,
                                &members,
                            ).await;
                        }
                    }

                    _ = legacy_poll.tick() => {
                        let Some(members) = legacy_memberships(&task_room).await else {
                            continue;
                        };
                        let current_members = members.iter()
                            .map(|member| member.member_id.clone())
                            .collect::<HashSet<_>>();
                        if !legacy_key_distributed || current_members != previous_legacy_members {
                            if legacy_key_distributed || current_members != previous_legacy_members {
                                rotate_local_key(&task_local_key);
                            }
                            // Rotate once for the shared membership set and
                            // distribute the same key to both protocol paths.
                            initial_key_distributed = publish_local_key(
                                &task_client,
                                &task_room,
                                &local_member_id,
                                &task_local_key,
                            ).await;
                            legacy_key_distributed = publish_local_key_legacy(
                                &task_client,
                                &task_room,
                                &task_legacy_member_id,
                                &task_local_key,
                                &members,
                            ).await;
                            previous_legacy_members = current_members;
                        }

                        let mut still_pending = VecDeque::new();
                        while let Some(candidate) = pending.pop_front() {
                            let stored = if try_store_received_key(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                true
                            } else {
                                try_store_received_key_legacy(
                                    &task_client,
                                    &task_room,
                                    &task_received_keys,
                                    &candidate,
                                ).await
                            };
                            if !stored {
                                still_pending.push_back(candidate);
                            }
                        }
                        pending = still_pending;
                    }

                    _ = rotation.tick() => {
                        rotate_local_key(&task_local_key);
                        initial_key_distributed = publish_local_key(
                            &task_client,
                            &task_room,
                            &local_member_id,
                            &task_local_key,
                        ).await;
                        let members = legacy_memberships(&task_room).await.unwrap_or_default();
                        legacy_key_distributed = publish_local_key_legacy(
                            &task_client,
                            &task_room,
                            &task_legacy_member_id,
                            &task_local_key,
                            &members,
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
                            ).await;
                            let members = legacy_memberships(&task_room).await.unwrap_or_default();
                            legacy_key_distributed = publish_local_key_legacy(
                                &task_client,
                                &task_room,
                                &task_legacy_member_id,
                                &task_local_key,
                                &members,
                            ).await;
                        }

                        let mut still_pending = VecDeque::new();
                        while let Some(candidate) = pending.pop_front() {
                            let stored = if try_store_received_key(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                true
                            } else {
                                try_store_received_key_legacy(
                                    &task_client,
                                    &task_room,
                                    &task_received_keys,
                                    &candidate,
                                ).await
                            };
                            if !stored {
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
        let local_livekit_identity = legacy_livekit_identity(&client)?;

        let task_local_key = Arc::clone(&local_key);
        let task_received_keys = Arc::clone(&received_keys);
        let task_client = client.clone();
        let task_room = room.clone();

        let handle = runtime.spawn(async move {
            // Listen to both protocol generations even when Astro must
            // publish legacy membership. A peer may still have a visible
            // modern sticky membership, which is independently validated
            // before accepting its modern key.
            let event_types = vec![
                ToDeviceEventType::from(RTC_KEY_EVENT),
                ToDeviceEventType::from(LEGACY_RTC_KEY_EVENT),
            ];
            // Matrix SDK returns a stream that is not Unpin; pin it so
            // StreamExt::next can be used inside tokio::select!.
            let mut incoming = Box::pin(
                task_client.subscribe_to_custom_to_device_messages(event_types),
            );
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
                            let stored = if try_store_received_key(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                true
                            } else {
                                try_store_received_key_legacy(
                                    &task_client,
                                    &task_room,
                                    &task_received_keys,
                                    &candidate,
                                ).await
                            };
                            if stored {
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
                            let stored = if try_store_received_key(
                                &task_client,
                                &task_room,
                                &task_received_keys,
                                &candidate,
                            ).await {
                                true
                            } else {
                                try_store_received_key_legacy(
                                    &task_client,
                                    &task_room,
                                    &task_received_keys,
                                    &candidate,
                                ).await
                            };
                            if !stored {
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
            local_livekit_identity,
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

/// Derive the pseudonymous LiveKit participant identity from the Matrix
/// sender and the exact member.id published in m.rtc.member (MSC4195).
///
/// Both values are strings, so serde_json's compact serialization is also
/// the canonical JSON representation required for this array.
fn modern_livekit_identity(sender: &str, member_id: &str) -> Result<String, String> {
    let canonical_json = serde_json::to_vec(&[sender, member_id])
        .map_err(|error| format!("Could not serialize MSC4195 participant identity: {error}"))?;
    let digest = sha2::Sha256::digest(&canonical_json);
    Ok(encode_base64_no_pad(&digest))
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

fn restart_local_key_activation_delay(local_key: &Arc<Mutex<MediaKey>>) {
    if let Ok(mut key) = local_key.lock() {
        // MSC4143's grace period starts after key delivery, not key creation.
        key.activate_at = Instant::now() + RTC_KEY_ACTIVATION_DELAY;
    }
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

// MSC4143 / MSC4153 require the sending device to be cross-signed by
// its owner. Do not additionally require it to be verified by the local user:
// that is a stronger trust relationship and would prevent ordinary room
// participants from exchanging media keys.
async fn device_is_cross_signed(
    client: &Client,
    user_id: &OwnedUserId,
    device_id: &OwnedDeviceId,
) -> bool {
    match client.encryption().get_device(user_id, device_id).await {
        Ok(Some(device)) => {
            let owner_cross_signed = device.is_cross_signed_by_owner();
            eprintln!(
                "ASTRO RTC E2EE trust diagnostic: user={}, device={}, owner_cross_signed={}",
                user_id,
                device_id,
                owner_cross_signed
            );
            owner_cross_signed
        }
        Ok(None) => {
            eprintln!(
                "ASTRO RTC E2EE trust diagnostic: device absent from local crypto store; user={}, device={}",
                user_id, device_id
            );
            false
        }
        Err(error) => {
            eprintln!(
                "ASTRO RTC E2EE trust diagnostic: local device lookup failed; user={}, device={}, error={}",
                user_id, device_id, error
            );
            false
        }
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
    let mut sticky_events_seen = 0usize;
    let mut active_members_seen = 0usize;
    let mut missing_encryption_metadata = 0usize;
    let mut missing_sender_device = 0usize;
    let mut owner_cross_signing_rejected = 0usize;

    for event in room.sticky_events().live() {
        if event.key.event_type.to_string() != RTC_MEMBER_EVENT {
            continue;
        }
        sticky_events_seen += 1;

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
        active_members_seen += 1;

        let Some(info) = event.encryption_info() else {
            // Encrypted rooms require authenticated member events.
            missing_encryption_metadata += 1;
            continue;
        };
        if value.get("sender").and_then(Value::as_str) != Some(info.sender.as_str()) {
            continue;
        }

        let Some(device_id) = info.sender_device.as_ref() else {
            missing_sender_device += 1;
            continue;
        };
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
            owner_cross_signing_rejected += 1;
            continue;
        }

        let Ok(device) = DeviceIdOrAllDevices::try_from(device_id.as_str()) else {
            continue;
        };
        recipients.entry(info.sender.clone()).or_default().push(device);
    }

    if recipients.is_empty() {
        eprintln!(
            "ASTRO RTC E2EE: no eligible sticky recipients; sticky_member_events={sticky_events_seen}, active_room_members={active_members_seen}, missing_authenticated_event_metadata={missing_encryption_metadata}, missing_sender_device={missing_sender_device}, rejected_not_owner_cross_signed={owner_cross_signing_rejected}"
        );
        return false;
    }
    let recipient_count: usize = recipients.values().map(Vec::len).sum();
    eprintln!(
        "ASTRO RTC E2EE: sending stable media key to {recipient_count} sticky member device(s); sticky_member_events={sticky_events_seen}, active_room_members={active_members_seen}, rejected_not_owner_cross_signed={owner_cross_signing_rejected}"
    );

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

    let delivery = client
        .send_encrypted_to_device(
            &ToDeviceEventType::from(RTC_KEY_EVENT),
            recipients,
            raw,
        )
        .await;
    if delivery.is_ok() {
        restart_local_key_activation_delay(local_key);
    }

    match delivery {
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
    // Keep diagnostics privacy-safe: report field presence and rejection
    // reasons, never the media-key material or the full event content.
    let value: Value = match serde_json::from_str(message.raw.json().get()) {
        Ok(value) => value,
        Err(error) => {
            eprintln!("ASTRO RTC E2EE: rejected incoming to-device event; reason=invalid_json, error={error}");
            return None;
        }
    };
    let event_type = value.get("type").and_then(Value::as_str).unwrap_or("<missing>");
    let info = message.encryption_info.as_ref();
    eprintln!(
        "ASTRO RTC E2EE: observed subscribed to-device message; event_type={}, sender={}, device={}, encrypted={}",
        event_type,
        info.map(|info| info.sender.as_str()).unwrap_or("<unknown>"),
        info.and_then(|info| info.sender_device.as_ref())
            .map(|device| device.as_str()).unwrap_or("<unknown>"),
        info.is_some()
    );

    let Some(info) = info else {
        eprintln!("ASTRO RTC E2EE: rejected incoming key event; reason=missing_authenticated_olm_metadata");
        return None;
    };
    if event_type != RTC_KEY_EVENT && event_type != LEGACY_RTC_KEY_EVENT {
        eprintln!("ASTRO RTC E2EE: ignored subscribed to-device event; reason=unexpected_event_type, event_type={event_type}");
        return None;
    }
    if value.get("sender").and_then(Value::as_str) != Some(info.sender.as_str()) {
        eprintln!("ASTRO RTC E2EE: rejected incoming key event; reason=event_sender_does_not_match_authenticated_sender");
        return None;
    }
    let Some(sender_device) = info.sender_device.clone() else {
        eprintln!("ASTRO RTC E2EE: rejected incoming key event; reason=missing_authenticated_sender_device");
        return None;
    };
    let Some(content) = value.get("content") else {
        eprintln!("ASTRO RTC E2EE: rejected incoming key event; reason=missing_content");
        return None;
    };
    let Some(room_id) = content.get("room_id").and_then(Value::as_str) else {
        eprintln!(
            "ASTRO RTC E2EE: rejected incoming key event; reason=missing_or_invalid_room_id, content_fields={}",
            content.as_object().map(|o| o.keys().cloned().collect::<Vec<_>>().join(",")).unwrap_or_else(|| "<non-object>".to_owned())
        );
        return None;
    };
    if room_id.is_empty() {
        eprintln!("ASTRO RTC E2EE: rejected incoming key event; reason=empty_room_id");
        return None;
    }

    let (member_id, index, encoded_key) = if event_type == RTC_KEY_EVENT {
        let Some(member_id) = content.get("member_id").and_then(Value::as_str) else {
            eprintln!("ASTRO RTC E2EE: rejected modern key event; reason=missing_member_id");
            return None;
        };
        let Some(media_key) = content.get("media_key") else {
            eprintln!("ASTRO RTC E2EE: rejected modern key event; reason=missing_media_key");
            return None;
        };
        let Some(index) = media_key.get("index").and_then(Value::as_u64).and_then(|v| u8::try_from(v).ok()) else {
            eprintln!("ASTRO RTC E2EE: rejected modern key event; reason=missing_or_invalid_key_index");
            return None;
        };
        let Some(encoded_key) = media_key.get("key").and_then(Value::as_str) else {
            eprintln!("ASTRO RTC E2EE: rejected modern key event; reason=missing_or_invalid_key_string");
            return None;
        };
        (member_id.to_owned(), index, encoded_key)
    } else {
        let Some(member) = content.get("member") else {
            eprintln!("ASTRO RTC E2EE: rejected legacy key event; reason=missing_member_object");
            return None;
        };
        let Some(claimed_device_id) = member.get("claimed_device_id").and_then(Value::as_str) else {
            eprintln!("ASTRO RTC E2EE: rejected legacy key event; reason=missing_claimed_device_id");
            return None;
        };
        if claimed_device_id != sender_device.as_str() {
            eprintln!(
                "ASTRO RTC E2EE: rejected legacy key event; reason=claimed_device_mismatch, authenticated_device={}, claimed_device={}",
                sender_device, claimed_device_id
            );
            return None;
        }
        let member_id = content.get("member_id").and_then(Value::as_str)
            .or_else(|| member.get("id").and_then(Value::as_str))
            .map(str::to_owned)
            .unwrap_or_else(|| format!("{}:{claimed_device_id}", info.sender));
        let Some(media_key) = content.get("media_key").or_else(|| content.get("keys")) else {
            eprintln!(
                "ASTRO RTC E2EE: rejected legacy key event; reason=missing_media_key_and_keys, content_fields={}",
                content.as_object().map(|o| o.keys().cloned().collect::<Vec<_>>().join(",")).unwrap_or_else(|| "<non-object>".to_owned())
            );
            return None;
        };
        let Some(index) = media_key.get("index").and_then(Value::as_u64).and_then(|v| u8::try_from(v).ok()) else {
            eprintln!("ASTRO RTC E2EE: rejected legacy key event; reason=missing_or_invalid_key_index");
            return None;
        };
        let Some(encoded_key) = media_key.get("key").and_then(Value::as_str) else {
            eprintln!("ASTRO RTC E2EE: rejected legacy key event; reason=missing_or_invalid_key_string");
            return None;
        };
        (member_id, index, encoded_key)
    };

    if member_id.is_empty() {
        eprintln!("ASTRO RTC E2EE: rejected incoming key event; reason=empty_member_id");
        return None;
    }
    let Some(bytes) = decode_base64_no_pad(encoded_key) else {
        eprintln!(
            "ASTRO RTC E2EE: rejected incoming key event; reason=invalid_unpadded_base64, encoded_key_length={}",
            encoded_key.len()
        );
        return None;
    };
    if bytes.len() != RTC_KEY_BYTES {
        eprintln!(
            "ASTRO RTC E2EE: rejected incoming key event; reason=invalid_key_length, decoded_bytes={}",
            bytes.len()
        );
        return None;
    }

    eprintln!(
        "ASTRO RTC E2EE: parsed encrypted media-key event; event_type={}, sender={}, device={}, member_id={}, index={}, key_bytes={}",
        event_type, info.sender, sender_device, member_id, index, bytes.len()
    );

    Some(PendingMediaKey {
        room_id: room_id.to_owned(),
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
    received_keys: &Arc<Mutex<BTreeMap<(String, u8), ReceivedMediaKey>>>,
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
        if info.sender != candidate.sender
            || info.sender_device.as_ref() != Some(&candidate.sender_device)
            || value.get("sender").and_then(Value::as_str) != Some(candidate.sender.as_str())
        {
            continue;
        }

        if !device_is_cross_signed(client, &candidate.sender, &candidate.sender_device).await {
            eprintln!(
                "ASTRO RTC E2EE: rejected incoming MSC4195 key; sender device is not owner-cross-signed; sender={}, device={}, member_id={}",
                candidate.sender, candidate.sender_device, candidate.member_id
            );
            return true;
        }

        let livekit_identity = match modern_livekit_identity(
            candidate.sender.as_str(), &candidate.member_id
        ) {
            Ok(identity) => identity,
            Err(error) => {
                eprintln!("ASTRO RTC E2EE: could not derive MSC4195 participant identity: {error}");
                return true;
            }
        };
        if let Ok(mut keys) = received_keys.lock() {
            keys.insert(
                (candidate.member_id.clone(), candidate.index),
                ReceivedMediaKey {
                    livekit_identity,
                    bytes: candidate.bytes.clone(),
                    activate_at: Instant::now() + RTC_KEY_ACTIVATION_DELAY,
                },
            );
        }
        eprintln!(
            "ASTRO RTC E2EE: accepted MSC4195 key index {}; sender={}, device={}, member_id={}",
            candidate.index, candidate.sender, candidate.sender_device, candidate.member_id
        );
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

/// Whether the room currently contains an active legacy MSC3401 call member.
pub(crate) async fn has_active_legacy_membership(room: &Room) -> bool {
    legacy_memberships(room).await.is_some_and(|members| !members.is_empty())
}

/// Whether the room currently contains an active MSC4354 sticky MatrixRTC member.
pub(crate) fn has_active_modern_membership(room: &Room) -> bool {
    room.sticky_events().live().iter().any(|event| {
        if event.key.event_type.to_string() != RTC_MEMBER_EVENT {
            return false;
        }
        let Ok(value) = serde_json::from_str::<Value>(event.raw().json().get()) else {
            return false;
        };
        let Some(content) = value.get("content") else { return false };
        let Some(member) = content.get("member") else { return false };
        let Some(member_id) = member.get("id").and_then(Value::as_str) else {
            return false;
        };
        content.get("slot_id").and_then(Value::as_str) == Some(RTC_SLOT_ID)
            && content.get("sticky_key").and_then(Value::as_str) == Some(member_id)
            && member.get("membership").and_then(Value::as_str) == Some("join")
    })
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
    let mut rejected_not_owner_cross_signed = 0usize;

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
            rejected_not_owner_cross_signed += 1;
            eprintln!(
                "ASTRO RTC E2EE legacy: excluded member device by owner-cross-signing policy; user={}, device={}, member_id={}, reason=not_owner_cross_signed",
                member.user_id, member.device_id, member.member_id
            );
            continue;
        }
        eprintln!(
            "ASTRO RTC E2EE legacy: eligible key recipient; user={}, device={}, member_id={}",
            member.user_id, member.device_id, member.member_id
        );
        let Ok(device) = DeviceIdOrAllDevices::try_from(member.device_id.as_str()) else {
            continue;
        };
        recipients.entry(member.user_id.clone()).or_default().push(device);
    }

    if recipients.is_empty() {
        eprintln!(
            "ASTRO RTC E2EE legacy: no eligible recipients; parsed_legacy_members={}, rejected_not_owner_cross_signed={rejected_not_owner_cross_signed}",
            members.len()
        );
        return false;
    }
    let recipient_count: usize = recipients.values().map(Vec::len).sum();
    eprintln!("ASTRO RTC E2EE legacy: sending compatibility media key to {recipient_count} legacy member device(s)");

    let own_device = match client.device_id() {
        Some(device_id) => device_id.to_string(),
        None => return false,
    };
    // Match matrix-js-sdk's legacy to-device payload exactly. The deprecated
    // keys/member fields are retained alongside member_id/media_key so older
    // and newer MatrixRTC clients can consume the same encrypted event.
    let media_key = serde_json::json!({
        "index": key.index,
        "key": encode_base64_no_pad(&key.bytes)
    });
    let content = serde_json::json!({
        "room_id": room.room_id().as_str(),
        "member_id": member_id,
        "media_key": media_key.clone(),
        "keys": media_key,
        "member": {
            "id": member_id,
            "claimed_device_id": own_device
        }
    });
    let raw = match Raw::<AnyToDeviceEventContent>::from_json_string(content.to_string()) {
        Ok(raw) => raw,
        Err(error) => {
            eprintln!("ASTRO RTC E2EE legacy: could not encode media-key message: {error}");
            return false;
        }
    };
    let delivery = client.send_encrypted_to_device(
        &ToDeviceEventType::from(LEGACY_RTC_KEY_EVENT),
        recipients,
        raw,
    ).await;
    if delivery.is_ok() {
        restart_local_key_activation_delay(local_key);
    }

    match delivery {
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
    received_keys: &Arc<Mutex<BTreeMap<(String, u8), ReceivedMediaKey>>>,
    candidate: &PendingMediaKey,
) -> bool {
    if candidate.room_id != room.room_id().as_str() {
        return true;
    }

    let Some(members) = legacy_memberships(room).await else {
        eprintln!(
            "ASTRO RTC E2EE legacy: incoming key is waiting for room-state membership; sender={}, device={}, member_id={}, index={}",
            candidate.sender, candidate.sender_device, candidate.member_id, candidate.index
        );
        return false;
    };
    let matching = members.iter().any(|member| {
        member.member_id == candidate.member_id
            && member.user_id == candidate.sender
            && member.device_id == candidate.sender_device
    });
    if !matching {
        let observed_members = members.iter()
            .map(|member| format!("{}|{}|{}", member.user_id, member.device_id, member.member_id))
            .collect::<Vec<_>>()
            .join(",");
        eprintln!(
            "ASTRO RTC E2EE legacy: incoming key does not match an active legacy member; sender={}, device={}, member_id={}, index={}, active_members=[{}]",
            candidate.sender, candidate.sender_device, candidate.member_id, candidate.index, observed_members
        );
        return false;
    }

    if !device_is_cross_signed(client, &candidate.sender, &candidate.sender_device).await {
        eprintln!(
            "ASTRO RTC E2EE legacy: rejected incoming key; sender device is not owner-cross-signed; sender={}, device={}, member_id={}",
            candidate.sender, candidate.sender_device, candidate.member_id
        );
        return true;
    }

    if let Ok(mut keys) = received_keys.lock() {
        keys.insert(
            (candidate.member_id.clone(), candidate.index),
            ReceivedMediaKey {
                livekit_identity: candidate.member_id.clone(),
                bytes: candidate.bytes.clone(),
                activate_at: Instant::now() + RTC_KEY_ACTIVATION_DELAY,
            },
        );
    }
    eprintln!(
        "ASTRO RTC E2EE legacy: accepted key index {}; sender={}, device={}, member_id={}",
        candidate.index, candidate.sender, candidate.sender_device, candidate.member_id
    );
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


#[cfg(test)]
mod tests {
    use super::modern_livekit_identity;

    #[test]
    fn msc4195_participant_identity_matches_spec_test_vector() {
        assert_eq!(
            modern_livekit_identity("@alice:example.com", "memberABC").unwrap(),
            "M3VnsLXrkbxIDINXO64u8PZzFyD9ZYFiQULR2dshWYs"
        );
    }
}
