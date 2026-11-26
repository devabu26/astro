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

pub(crate) struct RtcE2eeSession {
    task: AbortHandle,
    local_key: Arc<Mutex<MediaKey>>,
    received_keys: Arc<Mutex<BTreeMap<(String, u8), Vec<u8>>>>,
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

        let task_local_key = Arc::clone(&local_key);
        let task_received_keys = Arc::clone(&received_keys);
        let task_client = client.clone();
        let task_room = room.clone();

        let handle = runtime.spawn(async move {
            let event_type = ToDeviceEventType::from(RTC_KEY_EVENT);
            let mut incoming = task_client
                .subscribe_to_custom_to_device_messages(vec![event_type]);
            let mut sticky_updates = task_room.sticky_events().subscribe();
            let mut pending = VecDeque::<PendingMediaKey>::new();

            publish_local_key(
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
                            publish_local_key(
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
        self.received_keys
            .lock()
            .ok()?
            .get(&(member_id.to_owned(), index))
            .cloned()
    }
}

impl Drop for RtcE2eeSession {
    fn drop(&mut self) {
        self.task.abort();
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
) {
    let key = match local_key.lock() {
        Ok(key) => key.clone(),
        Err(_) => return,
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
        return;
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
            return;
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
        }
        Ok(failures) => {
            eprintln!("ASTRO RTC E2EE: media-key delivery had {} recipient failure group(s)", failures.len());
        }
        Err(error) => {
            eprintln!("ASTRO RTC E2EE: encrypted media-key delivery failed: {error}");
        }
    }
}

fn parse_incoming_key(message: matrix_sdk::ToDeviceMessage) -> Option<PendingMediaKey> {
    let info = message.encryption_info?;
    if !is_verified(&info) {
        return None;
    }

    let value: Value = serde_json::from_str(message.raw.json().get()).ok()?;
    if value.get("type").and_then(Value::as_str) != Some(RTC_KEY_EVENT)
        || value.get("sender").and_then(Value::as_str) != Some(info.sender.as_str())
    {
        return None;
    }

    let content = value.get("content")?;
    let room_id = content.get("room_id")?.as_str()?.to_owned();
    let member_id = content.get("member_id")?.as_str()?.to_owned();
    if room_id.is_empty() || member_id.is_empty() {
        return None;
    }

    let media_key = content.get("media_key")?;
    let index = u8::try_from(media_key.get("index")?.as_u64()?).ok()?;
    let encoded_key = media_key.get("key")?.as_str()?;
    let bytes = decode_base64_no_pad(encoded_key)?;
    if bytes.len() != RTC_KEY_BYTES {
        return None;
    }

    Some(PendingMediaKey {
        room_id,
        member_id,
        sender: info.sender.clone(),
        sender_device: info.sender_device.clone()?,
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
            keys.insert((candidate.member_id.clone(), candidate.index), candidate.bytes.clone());
        }
        return true;
    }

    false
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
