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
use matrix_sdk::sliding_sync::Version;
use matrix_sdk::ruma::events::{
    key::verification::request::ToDeviceKeyVerificationRequestEvent,
    room::message::{MessageType, OriginalSyncRoomMessageEvent},
};
use matrix_sdk::ruma::push::{
    Action as PushAction, EventMatchConditionData, EventPropertyContainsConditionData,
    EventPropertyIsConditionData, HttpPusherData, PushCondition, RuleKind, Tweak,
};
use matrix_sdk::{
    config::SyncSettings,
    encryption::{BackupDownloadStrategy, EncryptionSettings},
    ruma::room::RoomType,
    store::RoomLoadSettings,
    Client, LoopCtrl, Room, RoomMemberships, SqliteStoreConfig, TransmissionProgress,
};
use matrix_sdk_base::crypto::{CollectStrategy, DecryptionSettings, TrustRequirement};
use livekit::prelude::{LocalAudioTrack, LocalVideoTrack, LocalTrack, PlatformAudio, RemoteTrack, Room as LiveKitRoom, RoomEvent, RoomOptions, TrackSource};
use livekit::options::TrackPublishOptions;
use livekit::e2ee::{
    key_provider::{KeyDerivationAlgorithm, KeyProvider, KeyProviderOptions},
    E2eeOptions, EncryptionType,
};
use livekit::prelude::ParticipantIdentity;
use livekit::webrtc::audio_frame::AudioFrame;
use livekit::webrtc::audio_stream::native::NativeAudioStream;
use livekit::webrtc::audio_source::native::NativeAudioSource;
use livekit::webrtc::audio_source::{AudioSourceOptions, RtcAudioSource};
use livekit::webrtc::video_stream::native::NativeVideoStream;
use livekit::webrtc::video_frame::native::VideoFrameBufferExt;
use livekit::webrtc::video_frame::{VideoBuffer, VideoBufferType};
use livekit::webrtc::video_source::native::NativeVideoSource;
use livekit::webrtc::video_source::{RtcVideoSource, VideoResolution};
use livekit::webrtc::video_frame::{
    I420Buffer, VideoFrame, VideoRotation,
};


use matrix_sdk_ui::spaces::SpaceService;
use matrix_sdk_ui::timeline::{
    MembershipChange, Timeline, TimelineBuilder, TimelineEventItemId,
    TimelineItem, TimelineItemContent,
};
use matrix_sdk_ui::unable_to_decrypt_hook::{
    UnableToDecryptHook, UnableToDecryptInfo, UtdHookManager,
};
use std::collections::{HashMap, HashSet, VecDeque};
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
use crate::rtc_e2ee::{has_active_legacy_membership, has_active_modern_membership, RtcE2eeSession};
use crate::notifications::{register_incoming_call_handler, register_notification_handler, NotificationQueue};
use crate::session_storage::{delete_session, load_session, save_session};

static STORE_COUNTER: AtomicU64 = AtomicU64::new(0);

const RTC_VIDEO_FRAME_QUEUE_LIMIT: usize = 12;
const RTC_AUDIO_FRAME_QUEUE_LIMIT: usize = 200;
const RTC_VIDEO_MAX_WIDTH: u32 = 1920;
const RTC_VIDEO_MAX_HEIGHT: u32 = 1080;

struct RtcRemoteAudioFrame {
    key: String,
    room_id: String,
    identity: String,
    sample_rate: u32,
    channels: u32,
    data: Vec<u8>,
}

struct RtcRemoteVideoFrame {
    key: String,
    room_id: String,
    identity: String,
    source: String,
    width: u32,
    height: u32,
    data: Vec<u8>,
}

fn pack_remote_i420_frame(buffer: &I420Buffer) -> Option<Vec<u8>> {
    let width = buffer.width() as usize;
    let height = buffer.height() as usize;

    if width < 2 || height < 2 || width % 2 != 0 || height % 2 != 0
        || width > RTC_VIDEO_MAX_WIDTH as usize
        || height > RTC_VIDEO_MAX_HEIGHT as usize
    {
        return None;
    }

    let chroma_width = (width + 1) / 2;
    let chroma_height = (height + 1) / 2;
    let (stride_y, stride_u, stride_v) = buffer.strides();
    let (plane_y, plane_u, plane_v) = buffer.data();

    let capacity = width.checked_mul(height)?
        .checked_add(chroma_width.checked_mul(chroma_height)?.checked_mul(2)?)?;
    let mut packed = Vec::with_capacity(capacity);

    fn append_plane(
        output: &mut Vec<u8>,
        source: &[u8],
        stride: usize,
        row_width: usize,
        rows: usize,
    ) -> Option<()> {
        if stride < row_width {
            return None;
        }
        for row in 0..rows {
            let start = row.checked_mul(stride)?;
            let end = start.checked_add(row_width)?;
            output.extend_from_slice(source.get(start..end)?);
        }
        Some(())
    }

    append_plane(&mut packed, plane_y, stride_y as usize, width, height)?;
    append_plane(&mut packed, plane_u, stride_u as usize, chroma_width, chroma_height)?;
    append_plane(&mut packed, plane_v, stride_v as usize, chroma_width, chroma_height)?;

    (packed.len() == capacity).then_some(packed)
}

fn pack_remote_video_frame(buffer: &dyn VideoBuffer) -> Option<Vec<u8>> {
    match buffer.buffer_type() {
        VideoBufferType::I420 => pack_remote_i420_frame(buffer.as_i420()?),
        VideoBufferType::Native => {
            let i420 = buffer.as_native()?.to_i420();
            pack_remote_i420_frame(&i420)
        }
        VideoBufferType::I420A => {
            let i420 = buffer.as_i420a()?.to_i420();
            pack_remote_i420_frame(&i420)
        }
        VideoBufferType::I422 => {
            let i420 = buffer.as_i422()?.to_i420();
            pack_remote_i420_frame(&i420)
        }
        VideoBufferType::I444 => {
            let i420 = buffer.as_i444()?.to_i420();
            pack_remote_i420_frame(&i420)
        }
        VideoBufferType::I010 => {
            let i420 = buffer.as_i010()?.to_i420();
            pack_remote_i420_frame(&i420)
        }
        VideoBufferType::NV12 => {
            let i420 = buffer.as_nv12()?.to_i420();
            pack_remote_i420_frame(&i420)
        }
        _ => None,
    }
}

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

    // RTC notification events are signalling, not chat messages. They are
    // handled by the global incoming-call notification queue; exposing them
    // as m.room.event makes the QML message renderer show "unsupported message".
    if matches!(content, TimelineItemContent::RtcNotification { .. }) {
        return None;
    }

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


    let reactions = event
        .reactions()
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
        .collect::<Vec<_>>();

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

/// Reports Matrix SDK undecryptable-event causes without logging message content.
#[derive(Debug)]
struct AstroUnableToDecryptLogger;

impl UnableToDecryptHook for AstroUnableToDecryptLogger {
    fn on_utd(&self, info: UnableToDecryptInfo) {
        let late_decryption_ms = info.time_to_decrypt.map(|duration| duration.as_millis());
        eprintln!(
            "ASTRO E2EE UTD: event_id={}, cause={:?}, late_decryption_ms={:?}, event_local_age_ms={}, own_identity_trusted={}, sender_homeserver={}, own_homeserver={:?}",
            info.event_id,
            info.cause,
            late_decryption_ms,
            info.event_local_age_millis,
            info.user_trusts_own_identity,
            info.sender_homeserver,
            info.own_homeserver
        );
    }
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

struct RtcStickyTask {
    cancel: tokio::sync::oneshot::Sender<()>,
    thread: std::thread::JoinHandle<()>,
}

impl RtcStickyTask {
    fn stop(self) {
        let _ = self.cancel.send(());

        if self.thread.join().is_err() {
            eprintln!("ASTRO DEBUG: Matrix RTC sticky-events thread panicked");
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

#[derive(Clone)]
struct RtcParticipantProfile {
    display_name: Option<String>,
    avatar_url: Option<String>,
}

fn rtc_membership_state_key(room: &Room, user_id: &str, device_id: &str) -> String {
    let room_version = room.clone_info()
        .room_version()
        .map(ToString::to_string)
        .unwrap_or_default();

    let prefix = if matches!(
        room_version.as_str(),
        "org.matrix.msc3757" | "org.matrix.msc3779"
    ) {
        ""
    } else {
        "_"
    };

    format!("{prefix}{user_id}_{device_id}_m.call")
}


#[derive(Clone, Debug)]
struct RtcLiveKitTransport {
    // MSC4195 m.livekit transport: WebSocket URL; the homeserver issues the JWT.
    sfu_url: Option<String>,
    // Compatibility transport: standalone JWT service URL used with /sfu/get.
    legacy_service_url: Option<String>,
}

/// Discover both current MSC4195 and legacy LiveKit transport shapes.
///
/// The pinned Matrix SDK predates the m.livekit transport variant. Ruma
/// therefore deserializes it as a custom transport, whose data map preserves
/// the url field. Do not use only the typed legacy LiveKit variant here:
/// that would silently discard the modern SFU URL.
async fn discover_livekit_transport(client: &Client) -> Result<RtcLiveKitTransport, String> {
    let access_token = client.access_token()
        .ok_or_else(|| "Matrix access token is unavailable for RTC transport discovery".to_string())?;
    let homeserver = client.homeserver();

    // The pinned Matrix SDK's typed discovery predates the stable response
    // field name and m.livekit transport. Fetch the raw response first so the
    // modern SFU URL cannot be dropped by an older Ruma deserializer.
    for path in [
        "_matrix/client/v1/rtc/transports",
        "_matrix/client/unstable/org.matrix.msc4143/rtc/transports",
    ] {
        let endpoint = homeserver.join(path)
            .map_err(|error| format!("Could not construct MatrixRTC transport endpoint: {error}"))?;
        let response = client.http_client()
            .get(endpoint)
            .bearer_auth(&access_token)
            .send().await
            .map_err(|error| format!("MatrixRTC transport discovery failed: {error}"))?;
        let status = response.status();
        let bytes = response.bytes().await
            .map_err(|error| format!("Could not read MatrixRTC transport response: {error}"))?;

        if status.as_u16() == 404 {
            continue;
        }
        if !status.is_success() {
            return Err(format!("MatrixRTC transport discovery returned HTTP {status}"));
        }

        let value: serde_json::Value = serde_json::from_slice(&bytes)
            .map_err(|error| format!("Invalid MatrixRTC transport response: {error}"))?;
        let entries = value.get("transports")
            .or_else(|| value.get("rtc_transports"))
            .and_then(serde_json::Value::as_array)
            .ok_or_else(|| "MatrixRTC transport response has no transports array".to_string())?;

        // Preserve both generations when the homeserver advertises them.
        // Membership format is selected from the active call, not inferred
        // from whichever transport entry happens to appear first.
        let mut modern_sfu_url = None;
        let mut legacy_service_url = None;
        for entry in entries {
            let transport_type = entry.get("type").and_then(serde_json::Value::as_str).unwrap_or("");
            let sfu_url = entry.get("url")
                .and_then(serde_json::Value::as_str)
                .filter(|url| !url.is_empty())
                .map(str::to_owned);
            let service_url = entry.get("livekit_service_url")
                .and_then(serde_json::Value::as_str)
                .filter(|url| !url.is_empty())
                .map(str::to_owned);

            if transport_type == "m.livekit" {
                modern_sfu_url = sfu_url.or(modern_sfu_url);
            } else if transport_type == "livekit" {
                if legacy_service_url.is_none() {
                    legacy_service_url = service_url;
                }
                if modern_sfu_url.is_none() {
                    modern_sfu_url = sfu_url;
                }
            }
        }

        if modern_sfu_url.is_some() || legacy_service_url.is_some() {
            return Ok(RtcLiveKitTransport {
                sfu_url: modern_sfu_url,
                legacy_service_url,
            });
        }
        return Err("The homeserver did not advertise a supported LiveKit transport".into());
    }

    // Older homeservers may expose transports only through .well-known. Let
    // the SDK retain that compatibility path after both Client-Server routes
    // proved unavailable.
    let transports = client
        .discover_rtc_transports()
        .await
        .map_err(|error| format!("Could not discover fallback MatrixRTC transports: {error}"))?;
    let Some(transports) = transports else {
        return Err("The homeserver did not advertise any MatrixRTC transports".into());
    };

    let mut modern_sfu_url = None;
    let mut legacy_service_url = None;
    for transport in transports {
        let transport_type = transport.transport_type();
        let data = transport.data();
        let url = data.get("url")
            .and_then(serde_json::Value::as_str)
            .filter(|url| !url.is_empty())
            .map(str::to_owned);
        if transport_type == "m.livekit" {
            modern_sfu_url = url.or(modern_sfu_url);
        } else if transport_type == "livekit" {
            if legacy_service_url.is_none() {
                legacy_service_url = data.get("livekit_service_url")
                    .and_then(serde_json::Value::as_str)
                    .filter(|url| !url.is_empty())
                    .map(str::to_owned);
            }
            if modern_sfu_url.is_none() {
                modern_sfu_url = url;
            }
        }
    }

    if modern_sfu_url.is_some() || legacy_service_url.is_some() {
        Ok(RtcLiveKitTransport {
            sfu_url: modern_sfu_url,
            legacy_service_url,
        })
    } else {
        Err("The homeserver did not advertise a supported LiveKit transport".into())
    }
}

async fn fetch_modern_rtc_livekit_token(
    client: &Client,
    room: &Room,
    sfu_url: &str,
    member_id: &str,
) -> Result<String, String> {
    let access_token = client.access_token()
        .ok_or_else(|| "Matrix access token is unavailable for MSC4195 LiveKit authorization".to_string())?;
    let body = serde_json::json!({
        "url": sfu_url,
        "room_id": room.room_id().as_str(),
        "slot_id": "m.call#room",
        "member_id": member_id
    });
    let homeserver = client.homeserver();
    // MSC4195 has both its stable and unstable client-server paths. Try the
    // stable route first, then fall back only when that route is absent.
    let endpoint_paths = [
        "_matrix/client/v1/rtc/livekit/get_token",
        "_matrix/client/unstable/io.element.msc4195/rtc/livekit/get_token",
    ];
    let mut last_status = None;
    for path in endpoint_paths {
        let endpoint = homeserver.join(path)
            .map_err(|error| format!("Could not construct MSC4195 token endpoint: {error}"))?;
        let request_body = serde_json::to_vec(&body)
            .map_err(|error| format!("Could not encode MSC4195 LiveKit token request: {error}"))?;
        let response = client.http_client()
            .post(endpoint)
            .bearer_auth(&access_token)
            .header("content-type", "application/json")
            .body(request_body)
            .send().await
            .map_err(|error| format!("MSC4195 LiveKit token request failed: {error}"))?;
        let status = response.status();
        let bytes = response.bytes().await
            .map_err(|error| format!("Could not read MSC4195 LiveKit token response: {error}"))?;
        if status.is_success() {
            let value: serde_json::Value = serde_json::from_slice(&bytes)
                .map_err(|error| format!("Invalid MSC4195 LiveKit token response: {error}"))?;
            return value.get("jwt").and_then(serde_json::Value::as_str)
                .filter(|jwt| !jwt.is_empty())
                .map(str::to_owned)
                .ok_or_else(|| "MSC4195 LiveKit token response has no JWT".to_string());
        }

        if status.as_u16() == 404 {
            last_status = Some(status);
            continue;
        }

        let detail = serde_json::from_slice::<serde_json::Value>(&bytes).ok()
            .and_then(|value| value.get("error").and_then(serde_json::Value::as_str).map(str::to_owned));
        return Err(match detail {
            Some(detail) => format!("Homeserver MSC4195 token endpoint returned HTTP {status}: {detail}"),
            None => format!("Homeserver MSC4195 token endpoint returned HTTP {status}"),
        });
    }

    Err(format!(
        "Homeserver does not expose either MSC4195 token endpoint (last response: HTTP {})",
        last_status.map(|status| status.as_u16()).unwrap_or(404)
    ))
}

async fn fetch_rtc_livekit_credentials(
    client: &Client,
    room: &Room,
    transport: &RtcLiveKitTransport,
    member_id: Option<&str>,
) -> Result<(String, String), String> {
    if let (Some(sfu_url), Some(member_id)) = (transport.sfu_url.as_deref(), member_id) {
        let jwt = fetch_modern_rtc_livekit_token(client, room, sfu_url, member_id).await?;
        return Ok((sfu_url.to_owned(), jwt));
    }

    let service_url = transport.legacy_service_url.as_deref()
        .ok_or_else(|| "The selected LiveKit transport requires an MSC4195 member ID and SFU URL".to_string())?;
    let device_id = client.device_id().ok_or_else(|| "Matrix device ID is unavailable".to_string())?.to_string();
    let openid_token = client.account().request_openid_token().await
        .map_err(|error| format!("Could not obtain Matrix OpenID token: {error}"))?;
    let token_type = serde_json::to_value(&openid_token.token_type)
        .map_err(|error| format!("Could not encode OpenID token type: {error}"))?;
    let token_type = token_type.as_str().ok_or_else(|| "Matrix returned an invalid OpenID token type".to_string())?;
    let request_body = serde_json::json!({
        "room": room.room_id().as_str(), "device_id": device_id,
        "openid_token": {
            "access_token": openid_token.access_token, "token_type": token_type,
            "matrix_server_name": openid_token.matrix_server_name.to_string(),
            "expires_in": openid_token.expires_in.as_secs()
        }
    });
    let request_bytes = serde_json::to_vec(&request_body).map_err(|error| format!("Could not encode LiveKit token request: {error}"))?;
    let endpoint = format!("{}/sfu/get", service_url.trim_end_matches('/'));
    let response = client.http_client().post(&endpoint).header("content-type", "application/json")
        .body(request_bytes).send().await
        .map_err(|error| format!("LiveKit authorization request failed: {error}"))?;
    let status = response.status();
    let response_bytes = response.bytes().await.map_err(|error| format!("Could not read LiveKit authorization response: {error}"))?;
    if !status.is_success() { return Err(format!("LiveKit authorization service returned HTTP {status}")); }
    let value: serde_json::Value = serde_json::from_slice(&response_bytes)
        .map_err(|error| format!("Invalid LiveKit authorization response: {error}"))?;
    let url = value.get("url").and_then(serde_json::Value::as_str).filter(|v| !v.is_empty())
        .ok_or_else(|| "LiveKit authorization response has no URL".to_string())?.to_owned();
    let jwt = value.get("jwt").and_then(serde_json::Value::as_str).filter(|v| !v.is_empty())
        .ok_or_else(|| "LiveKit authorization response has no JWT".to_string())?.to_owned();
    Ok((url, jwt))
}

fn rtc_random_member_id() -> Result<String, String> {
    let mut bytes = [0u8; 16];
    getrandom::getrandom(&mut bytes).map_err(|error| format!("Could not generate a secure MatrixRTC member ID: {error}"))?;
    Ok(bytes.iter().map(|byte| format!("{byte:02x}")).collect())
}

pub struct MatrixClientManager {
    client: Option<Client>,
    utd_hook: Arc<UtdHookManager>,
    rtc_livekit_audio_sessions: HashMap<String, LiveKitAudioSession>,
    rtc_pending_screenshare_stops: HashMap<String, Arc<Mutex<Option<Result<(), String>>>>>,
    rtc_remote_video_frames: Arc<Mutex<VecDeque<RtcRemoteVideoFrame>>>,
    rtc_remote_video_tasks: HashMap<String, tokio::task::AbortHandle>,
    rtc_remote_audio_frames: Arc<Mutex<VecDeque<RtcRemoteAudioFrame>>>,
    rtc_remote_audio_tasks: HashMap<String, tokio::task::AbortHandle>,
    rtc_participant_profiles: HashMap<(String, String), RtcParticipantProfile>,
    rtc_last_snapshots: HashMap<String, serde_json::Value>,
    rtc_local_memberships: HashMap<String, String>,
    rtc_outgoing_call_notifications: HashMap<String, (String, Vec<String>)>,
    rtc_modern_memberships: HashMap<String, (String, serde_json::Value)>,
    rtc_e2ee_sessions: HashMap<String, RtcE2eeSession>,
    rtc_livekit_services: HashMap<String, RtcLiveKitTransport>,
    rtc_modern_renewals: Arc<Mutex<HashMap<String, serde_json::Value>>>,
    rtc_renewals: Arc<Mutex<HashMap<String, String>>>,
    runtime: std::sync::Arc<Runtime>,
    session_path: PathBuf,
    sync_task: Option<SyncTask>,
    rtc_sticky_task: Option<RtcStickyTask>,
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

struct LiveKitAudioSession {
    room: LiveKitRoom,
    events: tokio::sync::mpsc::UnboundedReceiver<RoomEvent>,
    _platform_audio: PlatformAudio,
    microphone: LocalAudioTrack,
    muted: bool,
    video_source: Option<NativeVideoSource>,
    video_track: Option<LocalVideoTrack>,
    screen_share_source: Option<NativeVideoSource>,
    screen_share_track: Option<LocalVideoTrack>,
    screen_share_audio_source: Option<NativeAudioSource>,
    screen_share_audio_track: Option<LocalAudioTrack>,
    screen_share_audio_sender: Option<tokio::sync::mpsc::Sender<Vec<i16>>>,
    screen_share_frames_submitted: u64,
    screen_share_frames_accepted: u64,
    screen_share_frames_rejected: u64,
    screen_share_stats_last_report: Instant,
    e2ee_key_provider: Option<KeyProvider>,
    e2ee_installed_keys: HashMap<(String, u8), Vec<u8>>,
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
                // Chat E2EE trust policy: only share room keys with devices
                // signed by their owner's cross-signing identity, and only
                // decrypt messages from cross-signed devices.
                .with_room_key_recipient_strategy(CollectStrategy::IdentityBasedStrategy)
                .with_decryption_settings(DecryptionSettings {
                    sender_device_trust_requirement: TrustRequirement::CrossSigned,
                })
                // If an existing server-side backup key is available, retry
                // a failed Megolm decryption against that backup. Do not
                // automatically create backups or bootstrap cross-signing.
                .with_encryption_settings(EncryptionSettings {
                    auto_enable_cross_signing: false,
                    backup_download_strategy: BackupDownloadStrategy::AfterDecryptionFailure,
                    auto_enable_backups: false,
                })
                .sqlite_store_with_config_and_cache_path(config, Some(database_path))
                .build()
                .await
                .map_err(|e| format!("Could not create Matrix client: {e}"))
        })?;

        eprintln!("ASTRO DEBUG: Matrix client build completed");

        let utd_hook = runtime.block_on(async {
            let mut hook = UtdHookManager::new(
                Arc::new(AstroUnableToDecryptLogger),
                client.clone(),
            )
            .with_max_delay(Duration::from_secs(15));

            if let Err(error) = hook.reload_from_store().await {
                eprintln!("ASTRO E2EE UTD: could not restore diagnostic history: {error}");
            }

            Arc::new(hook)
        });

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
        register_incoming_call_handler(&client, notification_queue.clone());

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
            utd_hook,
            rtc_livekit_audio_sessions: HashMap::new(),
            rtc_pending_screenshare_stops: HashMap::new(),
            rtc_remote_video_frames: Arc::new(Mutex::new(VecDeque::new())),
            rtc_remote_video_tasks: HashMap::new(),
            rtc_remote_audio_frames: Arc::new(Mutex::new(VecDeque::new())),
            rtc_remote_audio_tasks: HashMap::new(),
            rtc_participant_profiles: HashMap::new(),
            rtc_last_snapshots: HashMap::new(),
            rtc_local_memberships: HashMap::new(),
            rtc_outgoing_call_notifications: HashMap::new(),
            rtc_modern_memberships: HashMap::new(),
            rtc_e2ee_sessions: HashMap::new(),
            rtc_livekit_services: HashMap::new(),
            rtc_modern_renewals: Arc::new(Mutex::new(HashMap::new())),
            rtc_renewals: Arc::new(Mutex::new(HashMap::new())),
            runtime: std::sync::Arc::new(runtime),
            session_path,
            sync_task: None,
            rtc_sticky_task: None,
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

        // Stop MatrixRTC sticky-event syncing before logging out.
        if let Some(task) = self.rtc_sticky_task.take() {
            task.stop();
        }

        // Stop continuous syncing before logging out.

        if let Some(task) = self.sync_task.take() {
            task.stop();
        }

        let active_rtc_rooms: Vec<String> =
            self.rtc_livekit_audio_sessions.keys().cloned().collect();
        for room_id in active_rtc_rooms {
            if let Err(error) = self.stop_rtc_audio(&room_id) {
                eprintln!("ASTRO DEBUG: Could not close LiveKit room {room_id} during logout: {error}");
            }
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
        self.rtc_participant_profiles.clear();
        self.rtc_last_snapshots.clear();
        self.rtc_local_memberships.clear();
        self.rtc_modern_memberships.clear();
        self.rtc_e2ee_sessions.clear();
        self.rtc_livekit_services.clear();
        if let Ok(mut renewals) = self.rtc_renewals.lock() {
            renewals.clear();
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

            // Compute authoritative top-level Space activity from every
            // joined descendant room. A Space room itself often has no
            // message timestamp, so sorting its own latest-event value is
            // insufficient.
            let mut top_level_space_activity: HashMap<String, u64> = HashMap::new();
            for joined_room in &joined_rooms {
                let timestamp: u64 = joined_room
                    .latest_event_timestamp()
                    .map(|value| value.get().into())
                    .unwrap_or(0);

                if timestamp == 0 {
                    continue;
                }

                let ancestors = space_service
                    .top_level_ancestors_of(joined_room.room_id())
                    .await;

                for space_id in ancestors {
                    let entry = top_level_space_activity
                        .entry(space_id.to_string())
                        .or_insert(0);
                    *entry = (*entry).max(timestamp);
                }
            }

            for room in joined_rooms {
                let info = room.clone_info();
                let room_id = room.room_id().to_string();

                let is_space = info.room_type() == Some(&RoomType::Space);
                let is_dm = !is_space && room.is_dm();
                // Use the room's current Matrix encryption state for the chat
                // header indicator. If state cannot be read, fail closed.
                let is_encrypted = room.latest_encryption_state().await
                    .map(|state| state.is_encrypted())
                    .unwrap_or(false);

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
                    "is_encrypted": is_encrypted,
                    "name": name,
                    "topic": info.topic(),
                    "avatar_url": avatar_url,
                    "member_count": info.joined_members_count(),
                    "unread_count": room.unread_notification_counts().notification_count,
                    "mention_count": room.unread_notification_counts().highlight_count,
                    "last_activity": room.latest_event_timestamp().map(|timestamp| timestamp.get()),
                    "space_last_activity": top_level_space_activity
                        .get(&room_id)
                        .copied()
                        .unwrap_or(0),
                    "parent_space_ids": parents
                }));
            }

            Ok(serde_json::json!({
                "rooms": room_entries
            })
            .to_string())
        })
    }

    /// Retrieve the current Matrix account's user ID, display name and avatar URI.
    pub fn account_profile_json(&self) -> Result<String, String> {
        let client = self
            .client
            .as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?;

        let user_id = client
            .user_id()
            .ok_or_else(|| "Matrix user ID is unavailable".to_string())?
            .to_string();

        self.runtime.block_on(async {
            let account = client.account();
            let display_name = account
                .get_display_name()
                .await
                .map_err(|error| format!("Could not retrieve Matrix display name: {error}"))?;
            let avatar_url = account
                .get_avatar_url()
                .await
                .map_err(|error| format!("Could not retrieve Matrix avatar: {error}"))?;

            Ok(serde_json::json!({
                "user_id": user_id,
                "display_name": display_name,
                "avatar_url": avatar_url.map(|url| url.to_string())
            }).to_string())
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

    /// Start the MatrixRTC compatibility state-event listener and renewal task.
    fn start_rtc_sticky_task(&mut self) -> Result<(), String> {
        if let Some(task) = self.rtc_sticky_task.as_ref() {
            if !task.thread.is_finished() {
                return Ok(());
            }
        }

        if let Some(task) = self.rtc_sticky_task.take() {
            task.stop();
        }

        let client = self.client.as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?
            .clone();

        let runtime = std::sync::Arc::clone(&self.runtime);
        let (cancel_tx, mut cancel_rx) = tokio::sync::oneshot::channel::<()>();
        let renewals = Arc::clone(&self.rtc_renewals);
        let modern_renewals = Arc::clone(&self.rtc_modern_renewals);

        let thread = std::thread::Builder::new()
            .name("astro-matrix-rtc-compat".to_string())
            .spawn(move || {
                eprintln!("ASTRO DEBUG: MatrixRTC compatibility thread started");

                let local = tokio::task::LocalSet::new();

                local.block_on(&runtime, async move {
                    let mut retry_delay_secs = 2u64;
                    let mut room_info_updates = client.room_info_notable_update_receiver();

                    loop {
                        let available_versions =
                            client.available_sliding_sync_versions().await;

                        let native_available = available_versions
                            .iter()
                            .any(|version| matches!(version, Version::Native));

                        if !native_available {
                            eprintln!(
                                "ASTRO DEBUG: Native Sliding Sync unavailable; compatibility memberships will use regular Matrix sync and renewal"
                            );

                            let active = renewals.lock()
                                .map(|items| items.clone())
                                .unwrap_or_default();

                            let joined_rooms = client.joined_rooms();

                            for (room_id, state_key) in active {
                                let Some(room) = joined_rooms.iter().find(|room| {
                                    room.room_id().as_str() == room_id
                                }) else {
                                    if let Ok(mut items) = renewals.lock() {
                                        items.remove(&room_id);
                                    }
                                    continue;
                                };

                                let event = room.get_state_event(
                                    matrix_sdk::ruma::events::StateEventType::from(
                                        "org.matrix.msc3401.call.member"
                                    ),
                                    &state_key,
                                ).await;

                                let content = match event {
                                    Ok(Some(matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Sync(raw))) => {
                                        serde_json::from_str::<serde_json::Value>(
                                            raw.json().get()
                                        ).ok().and_then(|value| value.get("content").cloned())
                                    }
                                    Ok(Some(matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Stripped(raw))) => {
                                        serde_json::from_str::<serde_json::Value>(
                                            raw.json().get()
                                        ).ok().and_then(|value| value.get("content").cloned())
                                    }
                                    _ => None,
                                };

                                let Some(content) = content.filter(|value| {
                                    value.is_object() && !value.as_object().unwrap().is_empty()
                                }) else {
                                    continue;
                                };

                                if let Err(error) = room.send_state_event_raw(
                                    "org.matrix.msc3401.call.member",
                                    &state_key,
                                    content,
                                ).await {
                                    eprintln!(
                                        "ASTRO DEBUG: MatrixRTC compatibility renewal failed in {room_id}: {error}"
                                    );
                                } else {
                                    eprintln!(
                                        "ASTRO DEBUG: Renewed MatrixRTC compatibility membership in {room_id}"
                                    );
                                }
                            }

                            let modern_active = modern_renewals.lock().map(|items| items.clone()).unwrap_or_default();
                            for (room_id, content) in modern_active {
                                let Some(room) = joined_rooms.iter().find(|room| room.room_id().as_str() == room_id) else {
                                    if let Ok(mut items) = modern_renewals.lock() { items.remove(&room_id); }
                                    continue;
                                };
                                if let Err(error) = room.send_raw("m.rtc.member", content)
                                    .with_sticky_duration(Duration::from_secs(60 * 60)).await {
                                    eprintln!("ASTRO RTC: modern membership renewal failed in {room_id}: {error}");
                                }
                            }
                            tokio::select! {
                                _ = &mut cancel_rx => return,
                                _ = tokio::time::sleep(Duration::from_secs(30 * 60)) => {}
                            }
                            continue;
                        }

                        let sticky_enabled = client.supports_sticky_events().await.unwrap_or(false);
                        if !sticky_enabled {
                            eprintln!("ASTRO RTC: MSC4354 unavailable; continuing native Sliding Sync for legacy call membership");
                        }

                        let builder = match client.sliding_sync("astro-rtc-compat") {
                            Ok(builder) => builder,
                            Err(error) => {
                                eprintln!(
                                    "ASTRO DEBUG: Could not create MatrixRTC compatibility Sliding Sync: {error}"
                                );
                                tokio::select! {
                                    _ = &mut cancel_rx => return,
                                    _ = tokio::time::sleep(Duration::from_secs(retry_delay_secs)) => {}
                                }
                                retry_delay_secs = (retry_delay_secs * 2).min(30);
                                continue;
                            }
                        };

                        let builder = if sticky_enabled {
                            builder.with_sticky_events_extension({
                                let mut sticky_events =
                                    matrix_sdk::ruma::api::client::sync::sync_events::v5::request::StickyEvents::default();
                                sticky_events.enabled = Some(true);
                                sticky_events
                            })
                        } else {
                            builder
                        };

                        let sliding_sync = match builder
                            .version(Version::Native)
                            .build()
                            .await
                        {
                            Ok(sync) => sync,
                            Err(error) => {
                                eprintln!(
                                    "ASTRO DEBUG: Could not build MatrixRTC compatibility Sliding Sync: {error}"
                                );
                                tokio::select! {
                                    _ = &mut cancel_rx => return,
                                    _ = tokio::time::sleep(Duration::from_secs(retry_delay_secs)) => {}
                                }
                                retry_delay_secs = (retry_delay_secs * 2).min(30);
                                continue;
                            }
                        };

                        let joined_rooms = client.joined_rooms();
                        let joined_room_ids: Vec<_> = joined_rooms
                            .iter()
                            .map(|room| room.room_id())
                            .collect();

                        sliding_sync.set_room_subscriptions(
                            &joined_room_ids,
                            Some({
                                let mut settings =
                                    matrix_sdk::ruma::api::client::sync::sync_events::v5::request::RoomSubscription::default();
                                settings.required_state = vec![
                                    (matrix_sdk::ruma::events::StateEventType::from("org.matrix.msc3401.call.member"), "*".to_owned()),
                                    (matrix_sdk::ruma::events::StateEventType::from("m.rtc.slot"), "m.call#room".to_owned()),
                                ];
                                settings
                            }),
                            false,
                        );

                        eprintln!(
                            "ASTRO DEBUG: MatrixRTC compatibility Sliding Sync subscribed to {} joined rooms",
                            joined_room_ids.len()
                        );

                        let mut stream = Box::pin(sliding_sync.sync());
                        let mut renewal_interval =
                            tokio::time::interval(Duration::from_secs(30 * 60));
                        renewal_interval.tick().await;

                        let mut restart = false;

                        while !restart {
                            tokio::select! {
                                _ = &mut cancel_rx => {
                                    eprintln!("ASTRO DEBUG: MatrixRTC compatibility task cancelled");
                                    return;
                                }

                                _ = renewal_interval.tick() => {
                                    let active = renewals.lock()
                                        .map(|items| items.clone())
                                        .unwrap_or_default();

                                    let joined_rooms = client.joined_rooms();

                                    for (room_id, state_key) in active {
                                        let Some(room) = joined_rooms.iter().find(|room| {
                                            room.room_id().as_str() == room_id
                                        }) else {
                                            if let Ok(mut items) = renewals.lock() {
                                                items.remove(&room_id);
                                            }
                                            continue;
                                        };

                                        let event = room.get_state_event(
                                            matrix_sdk::ruma::events::StateEventType::from(
                                                "org.matrix.msc3401.call.member"
                                            ),
                                            &state_key,
                                        ).await;

                                        let content = match event {
                                            Ok(Some(matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Sync(raw))) => {
                                                serde_json::from_str::<serde_json::Value>(
                                                    raw.json().get()
                                                ).ok().and_then(|value| value.get("content").cloned())
                                            }
                                            Ok(Some(matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Stripped(raw))) => {
                                                serde_json::from_str::<serde_json::Value>(
                                                    raw.json().get()
                                                ).ok().and_then(|value| value.get("content").cloned())
                                            }
                                            _ => None,
                                        };

                                        let Some(content) = content.filter(|value| {
                                            value.is_object() && !value.as_object().unwrap().is_empty()
                                        }) else {
                                            continue;
                                        };

                                        match room.send_state_event_raw(
                                            "org.matrix.msc3401.call.member",
                                            &state_key,
                                            content,
                                        ).await {
                                            Ok(_) => eprintln!(
                                                "ASTRO DEBUG: Renewed MatrixRTC compatibility membership in {room_id}"
                                            ),
                                            Err(error) => eprintln!(
                                                "ASTRO DEBUG: MatrixRTC compatibility renewal failed in {room_id}: {error}"
                                            ),
                                        }
                                    }

                                    let modern_active = modern_renewals.lock()
                                        .map(|items| items.clone())
                                        .unwrap_or_default();
                                    for (room_id, content) in modern_active {
                                        let Some(room) = joined_rooms.iter().find(|room| {
                                            room.room_id().as_str() == room_id
                                        }) else {
                                            if let Ok(mut items) = modern_renewals.lock() {
                                                items.remove(&room_id);
                                            }
                                            continue;
                                        };

                                        if let Err(error) = room.send_raw("m.rtc.member", content)
                                            .with_sticky_duration(Duration::from_secs(60 * 60))
                                            .await {
                                            eprintln!(
                                                "ASTRO RTC: modern membership renewal failed in {room_id}: {error}"
                                            );
                                        } else {
                                            eprintln!(
                                                "ASTRO RTC: renewed modern MatrixRTC membership in {room_id}"
                                            );
                                        }
                                    }
                                }

                                room_update = room_info_updates.recv() => {
                                    match room_update {
                                        Ok(update) => {
                                            let is_joined = client.joined_rooms()
                                                .iter()
                                                .any(|room| room.room_id().as_str()
                                                    == update.room_id.as_str());

                                            if is_joined {
                                                sliding_sync.add_room_subscriptions(
                                                    &[&update.room_id],
                                                    Some({
                                                        let mut settings =
                                                            matrix_sdk::ruma::api::client::sync::sync_events::v5::request::RoomSubscription::default();
                                                        settings.required_state = vec![
                                                            (matrix_sdk::ruma::events::StateEventType::from("org.matrix.msc3401.call.member"), "*".to_owned()),
                                                            (matrix_sdk::ruma::events::StateEventType::from("m.rtc.slot"), "m.call#room".to_owned()),
                                                        ];
                                                        settings
                                                    }),
                                                    true,
                                                );
                                            } else {
                                                sliding_sync.remove_room_subscriptions(
                                                    &[&update.room_id],
                                                    true,
                                                );
                                            }
                                        }
                                        Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {
                                            let joined_rooms = client.joined_rooms();
                                            let joined_room_ids: Vec<_> = joined_rooms
                                                .iter()
                                                .map(|room| room.room_id())
                                                .collect();

                                            sliding_sync.set_room_subscriptions(
                                                &joined_room_ids,
                                                Some({
                                                    let mut settings =
                                                        matrix_sdk::ruma::api::client::sync::sync_events::v5::request::RoomSubscription::default();
                                                    settings.required_state = vec![
                                                        (matrix_sdk::ruma::events::StateEventType::from("org.matrix.msc3401.call.member"), "*".to_owned()),
                                                        (matrix_sdk::ruma::events::StateEventType::from("m.rtc.slot"), "m.call#room".to_owned()),
                                                    ];
                                                    settings
                                                }),
                                                true,
                                            );
                                        }
                                        Err(tokio::sync::broadcast::error::RecvError::Closed) => {
                                            eprintln!(
                                                "ASTRO DEBUG: MatrixRTC room-update listener closed"
                                            );
                                            return;
                                        }
                                    }
                                }

                                update = stream.next() => {
                                    match update {
                                        Some(Ok(_)) => {
                                            retry_delay_secs = 2;
                                        }
                                        Some(Err(error)) => {
                                            eprintln!(
                                                "ASTRO DEBUG: MatrixRTC compatibility Sliding Sync failed: {error}"
                                            );
                                            restart = true;
                                        }
                                        None => {
                                            eprintln!(
                                                "ASTRO DEBUG: MatrixRTC compatibility Sliding Sync ended"
                                            );
                                            restart = true;
                                        }
                                    }
                                }
                            }
                        }

                        tokio::select! {
                            _ = &mut cancel_rx => return,
                            _ = tokio::time::sleep(Duration::from_secs(retry_delay_secs)) => {}
                        }

                        retry_delay_secs = (retry_delay_secs * 2).min(30);
                    }
                });
            })
            .map_err(|error| {
                format!("Could not start MatrixRTC compatibility thread: {error}")
            })?;

        self.rtc_sticky_task = Some(RtcStickyTask {
            cancel: cancel_tx,
            thread,
        });

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
                .sync_once(SyncSettings::default().timeout(Duration::from_secs(10)))
                .await
                .map_err(|e| format!("Matrix room sync failed: {e}"))?;

            // Build the Matrix Space graph from the synchronised room state.
            let space_service = SpaceService::new(client.clone()).await;
            let _top_level_spaces = space_service.top_level_joined_spaces().await;

            let joined_rooms = client.joined_rooms();
            let mut room_entries = Vec::with_capacity(joined_rooms.len());

            // Aggregate each top-level Space's activity from every joined
            // descendant room, including rooms nested under child Spaces.
            let mut top_level_space_activity: HashMap<String, u64> = HashMap::new();
            for joined_room in &joined_rooms {
                let timestamp: u64 = joined_room
                    .latest_event_timestamp()
                    .map(|value| value.get().into())
                    .unwrap_or(0);

                if timestamp == 0 {
                    continue;
                }

                let ancestors = space_service
                    .top_level_ancestors_of(joined_room.room_id())
                    .await;

                for space_id in ancestors {
                    let entry = top_level_space_activity
                        .entry(space_id.to_string())
                        .or_insert(0);
                    *entry = (*entry).max(timestamp);
                }
            }

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
                    "last_activity": room.latest_event_timestamp().map(|timestamp| timestamp.get()),
                    "space_last_activity": top_level_space_activity
                        .get(&room_id)
                        .copied()
                        .unwrap_or(0),
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

        // MatrixRTC sticky events are optional; never block normal chat syncing.
        if let Err(error) = self.start_rtc_sticky_task() {
            eprintln!("ASTRO DEBUG: Could not start MatrixRTC sticky-event syncing: {error}");
        }

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


        if let Some(task) = self.rtc_sticky_task.take() {
            task.stop();
        }

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

        // Restart optional MatrixRTC sticky-event syncing independently.
        if let Err(error) = self.start_rtc_sticky_task() {
            eprintln!("ASTRO DEBUG: Could not restart MatrixRTC sticky-event syncing: {error}");
        }

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
                // A room can be reopened after a previous member/profile fetch
                // failed or while membership state was still lazy-loaded.
                // Refresh member state before serialising this snapshot so
                // sender_profile() can provide avatar MXC URIs.
                timeline.fetch_members().await;

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
                .with_unable_to_decrypt_hook(Arc::clone(&self.utd_hook))
                .build()
                .await
                .map_err(|error| format!("Could not build room timeline: {error}"))?;

            // Load the latest 20 historical events before subscribing.
            let reached_start = timeline
                .paginate_backwards(20)
                .await
                .map_err(|error| format!("Could not load room history: {error}"))?;

            // Timeline sender profiles are not guaranteed to be available
            // until the room member list has been synchronised. Without this,
            // sender_profile() commonly remains Unavailable and Astro serialises
            // messages without sender_avatar, so QML has no MXC URI to fetch.
            // Fetch members after pagination so the SDK can also update the
            // profiles of the events already loaded into this timeline.
            timeline.fetch_members().await;

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
                .ok_or_else(|| "Matrix event cache is unavailable".to_string())?
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

            // Cache refresh can leave member/profile details unavailable too.
            // Re-sync room members before serialising the refreshed snapshot.
            timeline.fetch_members().await;

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
        // A decline ends an outgoing one-to-one call for the caller. In a
        // group room it only rejects that invite; other participants remain.
        let declines: Vec<(String, String, String)> =
            self.notification_queue.drain_incoming_call_declines();
        if !declines.is_empty() {
            let client = self.client.as_ref().cloned();
            for (room_id, notification_event_id, sender) in declines {
                let Some((expected_event_id, recipients)) =
                    self.rtc_outgoing_call_notifications.get(&room_id).cloned()
                else {
                    continue;
                };
                if expected_event_id != notification_event_id
                    || !recipients.iter().any(|recipient| recipient == &sender)
                {
                    continue;
                }

                let Some(client) = client.as_ref() else { continue; };
                let parsed_room_id = match matrix_sdk::ruma::RoomId::parse(&room_id) {
                    Ok(id) => id,
                    Err(_) => continue,
                };
                let is_one_to_one = self.runtime.block_on(async {
                    let Some(room) = client.get_room(&parsed_room_id) else {
                        return false;
                    };
                    let is_direct = room.is_direct().await.unwrap_or(false);
                    let has_two_members = room.human_member_ids(RoomMemberships::JOIN).await
                        .map(|members| members.len() == 2)
                        .unwrap_or(false);
                    is_direct && has_two_members
                });

                if !is_one_to_one {
                    eprintln!(
                        "ASTRO RTC CALL: decline from {sender} in {room_id}; leaving group call active"
                    );
                    continue;
                }

                eprintln!(
                    "ASTRO RTC CALL: one-to-one call declined by {sender}; ending local call in {room_id}"
                );
                if let Err(error) = self.stop_rtc_audio(&room_id) {
                    eprintln!("ASTRO RTC CALL: could not close LiveKit after decline: {error}");
                }
                if let Err(error) = self.leave_rtc_room(&room_id) {
                    eprintln!("ASTRO RTC CALL: could not withdraw membership after decline: {error}");
                }
                self.rtc_outgoing_call_notifications.remove(&room_id);
            }
        }

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
        let incoming_call_events = self.notification_queue.drain_incoming_calls_json();

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
            && incoming_call_events.is_empty()
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
            "notifications": notification_events,
            "incoming_calls": incoming_call_events
        })
        .to_string()
    }


    /// Decline a specific incoming MatrixRTC ring using MSC4310.
    pub fn decline_rtc_call(&self, room_id: &str, notification_event_id: &str) -> Result<(), String> {
        let room_id = room_id.trim();
        let notification_event_id = notification_event_id.trim();
        if room_id.is_empty() || notification_event_id.is_empty() {
            return Err("Room ID and call notification event ID are required".into());
        }
        let client = self.client.as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?
            .clone();
        let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
            .map_err(|error| format!("Invalid room ID: {error}"))?;
        let parsed_event_id = matrix_sdk::ruma::EventId::parse(notification_event_id)
            .map_err(|error| format!("Invalid call notification event ID: {error}"))?;
        self.runtime.block_on(async move {
            let room = client.get_room(&parsed_room_id)
                .ok_or_else(|| "The call's Matrix room is no longer available".to_string())?;
            let content = serde_json::json!({
                "m.relates_to": {
                    "rel_type": "m.reference",
                    "event_id": parsed_event_id.as_str()
                }
            });
            room.send_raw("org.matrix.msc4310.rtc.decline", content).await
                .map_err(|error| format!("Could not decline the MatrixRTC call: {error}"))?;
            Ok(())
        })
    }

    /// Publish Astro's membership-only presence using Element Call compatibility state.
    pub fn join_rtc_room(&mut self, room_id: &str) -> Result<(), String> {
        self.join_rtc_room_internal(room_id, true)
    }

    /// Accept an incoming ring without sending another ring notification.
    pub fn accept_rtc_call(&mut self, room_id: &str) -> Result<(), String> {
        self.join_rtc_room_internal(room_id, false)
    }

    fn join_rtc_room_internal(
        &mut self,
        room_id: &str,
        notify_recipients: bool,
    ) -> Result<(), String> {
        let room_id = room_id.trim();
        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }
        // Reuse an existing membership, but repair any missing transport or
        // E2EE session before reporting a successful join. A previous attempt
        // can publish membership successfully and then fail while creating the
        // media-key exchange; returning here would leave encrypted calls
        // permanently unable to start.
        if self.rtc_local_memberships.contains_key(room_id) {
            let has_e2ee_session = self.rtc_e2ee_sessions.contains_key(room_id);
            let has_transport = self.rtc_livekit_services.contains_key(room_id);
            if has_e2ee_session && has_transport {
                eprintln!(
                    "ASTRO RTC NEGOTIATION: reusing complete membership; room={}, notify_recipients={}",
                    room_id, notify_recipients
                );
                return Ok(());
            }

            let client = self.client.as_ref()
                .ok_or_else(|| "No active Matrix session".to_string())?
                .clone();
            let lookup_client = client.clone();
            let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
                .map_err(|error| format!("Invalid room ID: {error}"))?;
            let existing_transport = self.rtc_livekit_services.get(room_id).cloned();
            let (room, encrypted, transport) = self.runtime.block_on(async move {
                let room = lookup_client.get_room(&parsed_room_id)
                    .ok_or_else(|| "The existing MatrixRTC membership refers to an unavailable room".to_string())?;
                let encrypted = room.latest_encryption_state().await
                    .map_err(|error| format!("Could not determine room encryption state: {error}"))?
                    .is_encrypted();
                let transport = match existing_transport {
                    Some(transport) => transport,
                    None => discover_livekit_transport(&lookup_client).await?,
                };
                Ok::<_, String>((room, encrypted, transport))
            })?;
            self.rtc_livekit_services.insert(room_id.to_owned(), transport);

            if encrypted && !self.rtc_e2ee_sessions.contains_key(room_id) {
                let session = if let Some((member_id, _)) = self.rtc_modern_memberships.get(room_id) {
                    RtcE2eeSession::start(
                        self.runtime.as_ref(), client.clone(), room, member_id.clone()
                    )?
                } else {
                    let user_id = client.user_id()
                        .ok_or_else(|| "Matrix user ID is unavailable for legacy RTC E2EE".to_string())?;
                    let device_id = client.device_id()
                        .ok_or_else(|| "Matrix device ID is unavailable for legacy RTC E2EE".to_string())?;
                    RtcE2eeSession::start_legacy(
                        self.runtime.as_ref(),
                        client.clone(),
                        room,
                        format!("{user_id}:{device_id}"),
                    )?
                };
                eprintln!(
                    "ASTRO RTC NEGOTIATION: repaired missing E2EE session for existing membership; room={}, membership_format={}",
                    room_id,
                    if self.rtc_modern_memberships.contains_key(room_id) { "msc4354_sticky" } else { "legacy_msc3401" }
                );
                self.rtc_e2ee_sessions.insert(room_id.to_owned(), session);
            }

            eprintln!(
                "ASTRO RTC NEGOTIATION: existing membership repaired; room={}, encrypted={}, had_e2ee_session={}, had_transport={}, has_e2ee_session={}, has_transport={}",
                room_id, encrypted, has_e2ee_session, has_transport,
                self.rtc_e2ee_sessions.contains_key(room_id),
                self.rtc_livekit_services.contains_key(room_id)
            );
            return Ok(());
        }

        let client = self.client.as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?
            .clone();

        let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
            .map_err(|error| format!("Invalid room ID: {error}"))?;

        let result = self.runtime.block_on(async move {
            let room = client.joined_rooms().into_iter()
                .find(|room| room.room_id() == &parsed_room_id)
                .ok_or_else(|| "You must be joined to this room before joining its call presence".to_string())?;

            let user_id = client.user_id()
                .ok_or_else(|| "Matrix user ID is unavailable".to_string())?
                .to_string();
            let device_id = client.device_id()
                .ok_or_else(|| "Matrix device ID is unavailable".to_string())?
                .to_string();

            let encrypted = room.latest_encryption_state().await
                .map_err(|error| format!("Could not determine room encryption state: {error}"))?.is_encrypted();

            let livekit_transport = discover_livekit_transport(&client).await?;
            eprintln!(
                "ASTRO RTC NEGOTIATION: discovered transport; modern_sfu_url_present={}, legacy_authorization_url_present={}",
                livekit_transport.sfu_url.is_some(),
                livekit_transport.legacy_service_url.is_some()
            );

            // The active call's membership generation takes precedence over
            // local capability and transport discovery. In particular, older
            // Element Call/Cinny sessions publish MSC3401 room-state members
            // even when the homeserver also advertises the newer m.livekit
            // transport. Selecting MSC4354 solely from local capabilities can
            // connect to the SFU with an incompatible member/key identity.
            let sticky_events_supported = client.supports_sticky_events().await.unwrap_or(false);
            let native_sliding_sync_available = client.available_sliding_sync_versions().await.iter()
                .any(|version| matches!(version, Version::Native));
            let sticky_supported = sticky_events_supported && native_sliding_sync_available;
            let active_legacy_membership = has_active_legacy_membership(&room).await;
            let active_modern_membership = has_active_modern_membership(&room);
            let modern_transport_available = livekit_transport.sfu_url.is_some();
            let legacy_transport_available = livekit_transport.legacy_service_url.is_some();

            if active_modern_membership && (!sticky_supported || !modern_transport_available) {
                return Err("This call already has MSC4354 sticky members, but Astro cannot use the matching modern MatrixRTC transport".into());
            }
            if active_legacy_membership && !active_modern_membership && !legacy_transport_available {
                return Err("This call uses legacy MSC3401 membership, but the homeserver did not advertise the legacy LiveKit authorization service required to join it".into());
            }

            let use_modern_membership = if active_modern_membership {
                true
            } else if active_legacy_membership {
                false
            } else {
                sticky_supported && modern_transport_available
            };
            eprintln!(
                "ASTRO RTC NEGOTIATION: membership decision; active_legacy_membership={}, active_modern_membership={}, sticky_events_supported={}, native_sliding_sync_available={}, modern_sfu_url_present={}, legacy_authorization_url_present={}, selected_membership_format={}, selection_reason={}",
                active_legacy_membership,
                active_modern_membership,
                sticky_events_supported,
                native_sliding_sync_available,
                modern_transport_available,
                legacy_transport_available,
                if use_modern_membership { "msc4354_sticky_m_rtc_member" } else { "legacy_msc3401_room_state" },
                if active_modern_membership { "existing_modern_call_member" }
                    else if active_legacy_membership { "existing_legacy_call_member" }
                    else if use_modern_membership { "new_call_modern_capability" }
                    else { "new_call_legacy_fallback" }
            );

            if use_modern_membership && !sticky_supported {
                return Err("The homeserver advertises the MSC4195 LiveKit transport, but Astro cannot use it without MatrixRTC sticky-event support".into());
            }
            if !use_modern_membership && !legacy_transport_available {
                return Err("The homeserver did not advertise the legacy LiveKit authorization service required for MSC3401 membership".into());
            }

            let mut modern_membership: Option<serde_json::Value> = None;
            let mut member_id: Option<String> = None;

            if use_modern_membership {
                let slot = room.get_state_event(
                    matrix_sdk::ruma::events::StateEventType::from("m.rtc.slot"), "m.call#room"
                ).await.map_err(|error| format!("Could not check the MatrixRTC room-call slot: {error}"))?;
                let slot_content = match slot {
                    Some(matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Sync(raw)) =>
                        serde_json::from_str::<serde_json::Value>(raw.json().get()).ok().and_then(|v| v.get("content").cloned()),
                    Some(matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Stripped(raw)) =>
                        serde_json::from_str::<serde_json::Value>(raw.json().get()).ok().and_then(|v| v.get("content").cloned()),
                    None => None,
                };
                if let Some(content) = slot_content {
                    let status = content["status"].as_str().unwrap_or("");
                    let application = content["application"]["type"].as_str().unwrap_or("");
                    let encryption_type = content["encryption"]["type"].as_str().unwrap_or("");
                    eprintln!(
                        "ASTRO RTC NEGOTIATION: existing room-call slot; status={}, application={}, encryption_type_present={}, encryption_type_m_per_member={}",
                        status,
                        application,
                        !encryption_type.is_empty(),
                        encryption_type == "m.per_member"
                    );
                    if status != "open" { return Err("The MatrixRTC room-call slot exists but is not open; ask an administrator to open it".into()); }
                    if application != "m.call" { return Err("The MatrixRTC room-call slot has an incompatible application type".into()); }
                    if encrypted && encryption_type != "m.per_member" { return Err("The encrypted room-call slot does not enable m.per_member encryption".into()); }
                    if !encrypted && encryption_type == "m.per_member" { return Err("The room-call slot requires per-member encryption, but this room is not encrypted".into()); }
                } else {
                    let mut content = serde_json::json!({
                        "status": "open", "application": { "type": "m.call" }
                    });
                    if encrypted { content["encryption"] = serde_json::json!({ "type": "m.per_member" }); }
                    room.send_state_event_raw("m.rtc.slot", "m.call#room", content)
                        .await.map_err(|error| format!("Could not create the MatrixRTC room-call slot; Astro may lack permission to send m.rtc.slot state events: {error}"))?;
                    eprintln!("ASTRO RTC: created m.call#room slot");
                }

                let id = rtc_random_member_id()?;
                let (livekit_url, _jwt) = fetch_rtc_livekit_credentials(
                    &client, &room, &livekit_transport, Some(&id)
                ).await?;
                let content = serde_json::json!({
                    "slot_id": "m.call#room",
                    "member": { "id": id, "membership": "join" },
                    "application": { "type": "m.call" },
                    "transports": {
                        "published": [{ "type": "m.livekit", "url": livekit_url }],
                        "can_subscribe": ["m.livekit"]
                    },
                    "sticky_key": id
                });
                room.send_raw("m.rtc.member", content.clone())
                    .with_sticky_duration(Duration::from_secs(60 * 60)).await
                    .map_err(|error| format!("Could not publish modern MatrixRTC membership: {error}"))?;
                member_id = Some(id);
                modern_membership = Some(content);
            } else {
                eprintln!(
                    "ASTRO RTC NEGOTIATION: using legacy membership; reason_sticky_or_native_sync_unavailable={}, reason_no_modern_sfu_url={}, reason_legacy_authorization_also_advertised={}",
                    !sticky_supported,
                    livekit_transport.sfu_url.is_none(),
                    livekit_transport.legacy_service_url.is_some()
                );
            }

            let state_key = rtc_membership_state_key(&room, &user_id, &device_id);

            let mut content = serde_json::json!({
                "application": "m.call",
                "call_id": "",
                "scope": "m.room",
                "device_id": device_id.as_str(),
                "expires": 14_400_000,
                "focus_active": {
                    "type": "livekit",
                    "focus_selection": "oldest_membership"
                }
            });
            // Only advertise the standalone JWT service in legacy state
            // membership. An MSC4195 SFU URL is not a /sfu/get endpoint.
            if let Some(service_url) = livekit_transport.legacy_service_url.as_deref() {
                content["foci_preferred"] = serde_json::json!([{
                    "type": "livekit",
                    "livekit_service_url": service_url
                }]);
            }

            let membership_response = room.send_state_event_raw(
                "org.matrix.msc3401.call.member",
                &state_key,
                content
            ).await
                .map_err(|error| format!("Could not publish MatrixRTC call presence: {error}"))?;

            // MatrixRTC membership only advertises that this device has joined.
            // Match the event formats used by Element Call / matrix-js-sdk 39:
            // MSC4075's then-current event name plus the legacy m.call.notify
            // event for clients that still consume the compatibility format.
            let notification_event_id = membership_response.event_id;
            let recipients = room.human_member_ids(RoomMemberships::JOIN).await
                .map_err(|error| format!("Could not load joined room members for call notification: {error}"))?
                .into_iter()
                .filter(|user_id| user_id != &client.user_id().expect("authenticated Matrix client"))
                .map(|user_id| user_id.to_string())
                .collect::<Vec<_>>();

            let mut outgoing_notification: Option<(String, Vec<String>)> = None;
            if notify_recipients && !recipients.is_empty() {
                let sender_ts = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_millis() as u64;

                // Cinny's incoming-call listener currently recognises
                // org.matrix.msc4075.rtc.notification (not the later stable
                // m.rtc.notification event name).
                let notification_content = serde_json::json!({
                    "sender_ts": sender_ts,
                    "lifetime": 30_000,
                    "notification_type": "ring",
                    "m.call.intent": "audio",
                    "m.mentions": {
                        "user_ids": recipients
                    },
                    "m.relates_to": {
                        "rel_type": "m.reference",
                        "event_id": notification_event_id
                    }
                });

                match room.send_raw(
                    "org.matrix.msc4075.rtc.notification",
                    notification_content
                ).await {
                    Ok(response) => {
                        outgoing_notification = Some((
                            response.response.event_id.to_string(),
                            recipients.iter().map(ToString::to_string).collect()
                        ));
                        eprintln!(
                            "ASTRO RTC CALL: sent MSC4075 incoming-call notification {} to {} other room member(s)",
                            response.response.event_id,
                            recipients.len()
                        );
                    },
                    Err(error) => eprintln!(
                        "ASTRO RTC CALL: failed to send MSC4075 incoming-call notification: {error}"
                    ),
                }

                // Keep the legacy event in sync with matrix-js-sdk's
                // compatibility sender. Older clients expect notify_type
                // ("ring" / "notify") and call_id rather than the MSC4075
                // notification_type and membership relation fields.
                let legacy_content = serde_json::json!({
                    "application": "m.call",
                    "m.mentions": {
                        "user_ids": recipients
                    },
                    "notify_type": "ring",
                    "call_id": ""
                });

                match room.send_raw("m.call.notify", legacy_content).await {
                    Ok(response) => eprintln!(
                        "ASTRO RTC CALL: sent legacy call notification {} to {} other room member(s)",
                        response.response.event_id,
                        recipients.len()
                    ),
                    Err(error) => eprintln!(
                        "ASTRO RTC CALL: failed to send legacy call notification: {error}"
                    ),
                }
            } else {
                eprintln!("ASTRO RTC CALL: no other joined room members to notify");
            }

            Ok::<(String, Option<String>, Option<serde_json::Value>, bool, Option<(String, Vec<String>)>, RtcLiveKitTransport), String>((state_key, member_id, modern_membership, encrypted, outgoing_notification, livekit_transport))
        });

        let (state_key, member_id, modern_membership, encrypted, outgoing_notification, livekit_transport) = result?;
        self.rtc_local_memberships.insert(room_id.to_owned(), state_key.clone());
        if let Some(notification) = outgoing_notification {
            self.rtc_outgoing_call_notifications.insert(room_id.to_owned(), notification);
        }
        if let (Some(member_id), Some(modern_membership)) = (member_id, modern_membership) {
            self.rtc_modern_memberships.insert(room_id.to_owned(), (member_id.clone(), modern_membership.clone()));
            if let Ok(mut renewals) = self.rtc_modern_renewals.lock() {
                renewals.insert(room_id.to_owned(), modern_membership);
            }
        }
        self.rtc_livekit_services.insert(
            room_id.to_owned(),
            livekit_transport,
        );

        if encrypted {
            let client = self.client.as_ref()
                .ok_or_else(|| "No active Matrix session".to_string())?
                .clone();
            let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
                .map_err(|error| format!("Invalid room ID for MatrixRTC E2EE: {error}"))?;
            let room = self.runtime.block_on(async {
                client.get_room(&parsed_room_id)
                    .ok_or_else(|| "Matrix room disappeared before E2EE key exchange could start".to_string())
            })?;

            let session = if let Some((member_id, _)) = self.rtc_modern_memberships.get(room_id) {
                RtcE2eeSession::start(
                    self.runtime.as_ref(),
                    client,
                    room,
                    member_id.clone(),
                )?
            } else {
                // Legacy MatrixRTC identifies the LiveKit participant as
                // `$user_id:$device_id` unless a custom membershipID is set.
                // Astro does not set a custom membershipID in its state event.
                let user_id = client.user_id()
                    .ok_or_else(|| "Matrix user ID is unavailable for legacy RTC E2EE".to_string())?;
                let device_id = client.device_id()
                    .ok_or_else(|| "Matrix device ID is unavailable for legacy RTC E2EE".to_string())?;
                let legacy_member_id = format!("{user_id}:{device_id}");
                RtcE2eeSession::start_legacy(
                    self.runtime.as_ref(),
                    client,
                    room,
                    legacy_member_id,
                )?
            };
            let key_identity = session.local_livekit_identity().to_owned();
            self.rtc_e2ee_sessions.insert(room_id.to_owned(), session);
            eprintln!(
                "ASTRO RTC E2EE NEGOTIATION: key exchange started; room={}, membership_format={}, local_livekit_identity={}",
                room_id,
                if self.rtc_modern_memberships.contains_key(room_id) { "msc4354_sticky" } else { "legacy_msc3401" },
                key_identity
            );
        }

        if let Ok(mut renewals) = self.rtc_renewals.lock() {
            renewals.insert(room_id.to_owned(), state_key);
        }

        Ok(())
    }

    /// Obtain LiveKit credentials for an active MatrixRTC membership.
    ///
    /// Uses the MSC4195 homeserver endpoint for modern transports and the
    /// standalone /sfu/get service for legacy transports.
    fn rtc_livekit_credentials_json(
        &mut self,
        room_id: &str,
    ) -> Result<String, String> {
        let room_id = room_id.trim();
        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        if !self.rtc_local_memberships.contains_key(room_id) {
            return Err("Astro has no active MatrixRTC membership in this room".into());
        }

        let transport = self.rtc_livekit_services
            .get(room_id)
            .cloned()
            .ok_or_else(|| "No LiveKit transport was selected for this room".to_string())?;
        let member_id = self.rtc_modern_memberships
            .get(room_id)
            .map(|(member_id, _)| member_id.clone());

        let client = self.client.as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?
            .clone();

        let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
            .map_err(|error| format!("Invalid room ID: {error}"))?;

        self.runtime.block_on(async move {
            let room = client.joined_rooms().into_iter()
                .find(|room| room.room_id() == &parsed_room_id)
                .ok_or_else(|| "You are no longer joined to this room".to_string())?;
            let (livekit_url, jwt) = fetch_rtc_livekit_credentials(
                &client, &room, &transport, member_id.as_deref()
            ).await?;
            Ok(serde_json::json!({ "url": livekit_url, "jwt": jwt }).to_string())
        })
    }

    /// Connect to LiveKit and publish Astro's microphone.
    ///
    /// The session is owned by MatrixClientManager, not by a QML page.
    pub fn start_rtc_audio(&mut self, room_id: &str) -> Result<(), String> {
        let room_id = room_id.trim().to_owned();

        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        if self.rtc_livekit_audio_sessions.contains_key(&room_id) {
            return Err("An audio call is already active in this room".into());
        }

        eprintln!(
            "ASTRO RTC NEGOTIATION: starting LiveKit; room={}, has_membership={}, has_modern_membership={}, has_transport={}, has_e2ee_session={}",
            room_id,
            self.rtc_local_memberships.contains_key(&room_id),
            self.rtc_modern_memberships.contains_key(&room_id),
            self.rtc_livekit_services.contains_key(&room_id),
            self.rtc_e2ee_sessions.contains_key(&room_id)
        );
        if !self.rtc_e2ee_sessions.contains_key(&room_id) {
            let client = self.client.as_ref()
                .ok_or_else(|| "No active Matrix session".to_string())?
                .clone();
            let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id.as_str())
                .map_err(|error| format!("Invalid room ID: {error}"))?;
            let encrypted = self.runtime.block_on(async {
                let room = client.get_room(&parsed_room_id)
                    .ok_or_else(|| "Matrix room is unavailable while checking call encryption".to_string())?;
                room.latest_encryption_state().await
                    .map(|state| state.is_encrypted())
                    .map_err(|error| format!("Could not determine room encryption state: {error}"))
            })?;
            if encrypted {
                return Err("This Matrix room is encrypted, but Astro has no active MatrixRTC key exchange; refusing to connect without LiveKit E2EE".into());
            }
        }

        // If this Matrix room is encrypted, wait for the MatrixRTC key grace
        // period and refuse to connect without a usable local key. This prevents
        // accidentally publishing media into an encrypted call without E2EE.
        let e2ee_material = if let Some(e2ee_session) = self.rtc_e2ee_sessions.get(&room_id) {
            let local_identity = e2ee_session.local_livekit_identity().to_owned();
            let runtime = Arc::clone(&self.runtime);
            let keys = runtime.block_on(async {
                // Membership changes can rotate the local MatrixRTC key while
                // an incoming call is being accepted. Each successful key
                // distribution restarts the protocol's five-second activation
                // grace, so an eight-second connection deadline is too short
                // when the first membership snapshot is still settling.
                let activation_wait_started = std::time::Instant::now();
                for _ in 0..200 {
                    let snapshot = e2ee_session.livekit_key_snapshot();
                    if let Some((_, index, _)) = snapshot.iter()
                        .find(|(identity, _, _)| identity == &local_identity)
                    {
                        eprintln!(
                            "ASTRO RTC E2EE: local media key index {} became active after {} ms",
                            index,
                            activation_wait_started.elapsed().as_millis()
                        );
                        return Ok(snapshot);
                    }
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
                let snapshot = e2ee_session.livekit_key_snapshot();
                eprintln!(
                    "ASTRO RTC E2EE: local media key activation timed out after {} ms; active_key_count={}, expected_local_identity_present={}",
                    activation_wait_started.elapsed().as_millis(),
                    snapshot.len(),
                    snapshot.iter().any(|(identity, _, _)| identity == &local_identity)
                );
                Err("MatrixRTC local media key did not become active within 20 seconds; refusing an unencrypted LiveKit connection".to_string())
            })?;
            eprintln!(
                "ASTRO RTC E2EE NEGOTIATION: active key snapshot; local_identity={}, keys={}",
                local_identity,
                keys.iter().map(|(identity, index, _)| format!("{identity}#{index}")).collect::<Vec<_>>().join(",")
            );
            Some((local_identity, keys))
        } else {
            eprintln!("ASTRO RTC E2EE NEGOTIATION: no media E2EE session; room encryption check determines whether connection is refused");
            None
        };

        let mut e2ee_key_provider = None;
        let mut e2ee_installed_keys = HashMap::new();
        let mut expected_local_e2ee_identity: Option<String> = None;
        if let Some((local_identity, keys)) = e2ee_material {
            expected_local_e2ee_identity = Some(local_identity.clone());
            let mut key_options = KeyProviderOptions::default();
            key_options.key_ring_size = 256;
            key_options.key_derivation_algorithm = KeyDerivationAlgorithm::HKDF;
            // MSC4195 requires a 128-bit derived AES key. LiveKit's AES-GCM
            // frame cryptor uses 128-bit keys with this HKDF provider.
            let provider = KeyProvider::new(key_options);
            let mut keys = keys;
            // KeyProvider::set_key updates its default send index. Install
            // remote keys first and the local sender key last.
            keys.sort_by_key(|(identity, _, _)| identity == &local_identity);

            for (identity, index, key) in keys {
                eprintln!(
                    "ASTRO RTC E2EE: installing key for LiveKit participant {}; index={}",
                    identity, index
                );
                let participant_identity = ParticipantIdentity::from(identity.clone());
                if !provider.set_key(&participant_identity, i32::from(index), key.clone()) {
                    return Err(format!(
                        "LiveKit rejected MatrixRTC key index {index} for participant {identity}"
                    ));
                }
                e2ee_installed_keys.insert((identity, index), key);
            }

            if !e2ee_installed_keys.keys().any(|(identity, _)| identity == &local_identity) {
                return Err("No active local MatrixRTC key is installed in LiveKit".into());
            }
            eprintln!(
                "ASTRO RTC E2EE: configured LiveKit HKDF key provider with {} participant key(s)",
                e2ee_installed_keys.len()
            );
            e2ee_key_provider = Some(provider);
        }

        // Retrieve credentials internally. The JWT never reaches QML.
        let credentials_json = self.rtc_livekit_credentials_json(&room_id)?;

        let credentials: serde_json::Value =
            serde_json::from_str(&credentials_json)
                .map_err(|error| format!("Invalid LiveKit credentials: {error}"))?;

        let livekit_url = credentials
            .get("url")
            .and_then(serde_json::Value::as_str)
            .filter(|value| !value.is_empty())
            .ok_or_else(|| "LiveKit credentials contain no URL".to_string())?
            .to_owned();

        let jwt = credentials
            .get("jwt")
            .and_then(serde_json::Value::as_str)
            .filter(|value| !value.is_empty())
            .ok_or_else(|| "LiveKit credentials contain no JWT".to_string())?
            .to_owned();

        let platform_audio = PlatformAudio::new()
            .map_err(|error| format!("Could not initialise platform audio: {error}"))?;

        let microphone = LocalAudioTrack::create_audio_track(
            "microphone",
            platform_audio.rtc_source(),
        );

        let runtime = Arc::clone(&self.runtime);

        let audio_session = runtime.block_on(async move {
            let mut room_options = RoomOptions::default();
            if let Some(provider) = e2ee_key_provider.clone() {
                room_options.encryption = Some(E2eeOptions {
                    encryption_type: EncryptionType::Gcm,
                    key_provider: provider,
                });
            }

            let (room, events) = LiveKitRoom::connect(
                &livekit_url,
                &jwt,
                room_options,
            )
            .await
            .map_err(|error| format!("Could not connect to LiveKit: {error}"))?;

            if let Some(expected_identity) = expected_local_e2ee_identity.as_deref() {
                let actual_identity = room.local_participant().identity().to_string();
                if actual_identity != expected_identity {
                    let _ = room.close().await;
                    return Err(format!(
                        "LiveKit assigned participant identity '{actual_identity}', but MatrixRTC requires '{expected_identity}' for E2EE"
                    ));
                }
            }

            let publish_options = TrackPublishOptions {
                source: TrackSource::Microphone,
                ..Default::default()
            };

            if let Err(error) = room
                .local_participant()
                .publish_track(
                    LocalTrack::Audio(microphone.clone()),
                    publish_options,
                )
                .await
            {
                let _ = room.close().await;
                return Err(format!("Could not publish microphone: {error}"));
            }

            Ok::<LiveKitAudioSession, String>(LiveKitAudioSession {
                room,
                events,
                _platform_audio: platform_audio,
                microphone,
                muted: false,
                video_source: None,
                video_track: None,
                screen_share_source: None,
                screen_share_track: None,
                screen_share_audio_source: None,
                screen_share_audio_track: None,
                screen_share_audio_sender: None,
                screen_share_frames_submitted: 0,
                screen_share_frames_accepted: 0,
                screen_share_frames_rejected: 0,
                screen_share_stats_last_report: Instant::now(),
                e2ee_key_provider,
                e2ee_installed_keys,
            })
        })?;

        self.rtc_livekit_audio_sessions.insert(room_id, audio_session);

        Ok(())
    }

    // ASTRO_RTC_VIDEO_STAGE1
    /// Publish a camera video track into an existing LiveKit audio session.
    /// Camera frames are supplied separately by the native video capture path.
    pub fn start_rtc_video(
        &mut self,
        room_id: &str,
        width: u32,
        height: u32,
    ) -> Result<(), String> {
        let room_id = room_id.trim();

        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        if width < 2 || height < 2 || width % 2 != 0 || height % 2 != 0 {
            return Err("Camera dimensions must be positive even numbers".into());
        }

        let runtime = Arc::clone(&self.runtime);
        let session = self
            .rtc_livekit_audio_sessions
            .get_mut(room_id)
            .ok_or_else(|| "No active LiveKit call in this room".to_string())?;

        if session.video_track.is_some() {
            return Err("Camera video is already active in this room".into());
        }

        let (source, track) = {
            let _runtime_guard = runtime.enter();

            let source = NativeVideoSource::new(
                VideoResolution { width, height },
                false,
            );

            let track = LocalVideoTrack::create_video_track(
                "camera",
                RtcVideoSource::Native(source.clone()),
            );

            (source, track)
        };

        let options = TrackPublishOptions {
            source: TrackSource::Camera,
            ..Default::default()
        };

        runtime.block_on(async {
            session
                .room
                .local_participant()
                .publish_track(LocalTrack::Video(track.clone()), options)
                .await
                .map_err(|error| {
                    format!("Could not publish LiveKit camera track: {error}")
                })
        })?;

        session.video_source = Some(source);
        session.video_track = Some(track);

        Ok(())
    }

    pub fn start_rtc_screenshare(
        &mut self,
        room_id: &str,
        width: u32,
        height: u32,
        include_device_audio: bool,
    ) -> Result<(), String> {
        let room_id = room_id.trim();

        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        if width < 2 || height < 2 || width % 2 != 0 || height % 2 != 0 {
            return Err("Screen-share dimensions must be positive even numbers".into());
        }

        let runtime = Arc::clone(&self.runtime);
        let session = self
            .rtc_livekit_audio_sessions
            .get_mut(room_id)
            .ok_or_else(|| "No active LiveKit call in this room".to_string())?;

        if session.screen_share_track.is_some() {
            return Err("Screen sharing is already active in this room".into());
        }

        let (source, track) = {
            let _runtime_guard = runtime.enter();

            let source = NativeVideoSource::new(
                VideoResolution { width, height },
                true,
            );

            let track = LocalVideoTrack::create_video_track(
                "screen_share",
                RtcVideoSource::Native(source.clone()),
            );

            (source, track)
        };

        let options = TrackPublishOptions {
            source: TrackSource::Screenshare,
            ..Default::default()
        };

        runtime.block_on(async {
            session
                .room
                .local_participant()
                .publish_track(LocalTrack::Video(track.clone()), options)
                .await
                .map_err(|error| {
                    format!("Could not publish LiveKit screen-share track: {error}")
                })
        })?;

        session.screen_share_source = Some(source);
        session.screen_share_track = Some(track.clone());

        if include_device_audio {
            let (audio_source, audio_track) = {
                let _runtime_guard = runtime.enter();
                let source = NativeAudioSource::new(
                    AudioSourceOptions::default(),
                    48_000,
                    2,
                    100,
                );
                let track = LocalAudioTrack::create_audio_track(
                    "screen_share_audio",
                    RtcAudioSource::Native(source.clone()),
                );
                (source, track)
            };

            let audio_options = TrackPublishOptions {
                source: TrackSource::ScreenshareAudio,
                ..Default::default()
            };

            if let Err(error) = runtime.block_on(async {
                session
                    .room
                    .local_participant()
                    .publish_track(LocalTrack::Audio(audio_track.clone()), audio_options)
                    .await
                    .map_err(|error| {
                        format!("Could not publish LiveKit screen-share audio track: {error}")
                    })
            }) {
                let video_sid = LocalTrack::Video(track.clone()).sid();
                let _ = runtime.block_on(async {
                    session.room.local_participant().unpublish_track(&video_sid).await
                });
                session.screen_share_source = None;
                session.screen_share_track = None;
                return Err(error);
            }

            // Keep LiveKit's async audio submission off the Qt capture thread.
            // A small bounded queue limits latency and drops stale samples rather
            // than allowing audio backpressure to stall screen capture.
            let (audio_sender, mut audio_receiver) =
                tokio::sync::mpsc::channel::<Vec<i16>>(5);
            let audio_source_for_task = audio_source.clone();
            runtime.spawn(async move {
                while let Some(samples) = audio_receiver.recv().await {
                    let frame = AudioFrame {
                        data: std::borrow::Cow::Owned(samples),
                        sample_rate: 48_000,
                        num_channels: 2,
                        samples_per_channel: 480,
                    };
                    if let Err(error) = audio_source_for_task.capture_frame(&frame).await {
                        eprintln!("ASTRO_SCREEN_SHARE_AUDIO: LiveKit submission failed: {error}");
                    }
                }
            });

            session.screen_share_audio_source = Some(audio_source);
            session.screen_share_audio_track = Some(audio_track);
            session.screen_share_audio_sender = Some(audio_sender);
        }

        Ok(())
    }


    // ASTRO_RTC_VIDEO_STAGE2
    /// Submit one tightly packed I420 frame to the active camera source.
    ///
    /// Layout: Y plane, followed by U plane, followed by V plane.
    /// Width and height must be even.
    pub fn push_rtc_video_frame(
        &mut self,
        room_id: &str,
        width: u32,
        height: u32,
        bytes: &[u8],
    ) -> Result<(), String> {
        let room_id = room_id.trim();

        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        let runtime = Arc::clone(&self.runtime);

        let frame_pixel_count = u64::from(width) * u64::from(height);
        if width < 2 || height < 2
            || width > 1920 || height > 1920
            || frame_pixel_count > 1920_u64 * 1080_u64
            || width % 2 != 0 || height % 2 != 0
        {
            return Err("Video frame dimensions must be even and no larger than 1920x1080 pixels".into());
        }

        let y_size = (width as usize)
            .checked_mul(height as usize)
            .ok_or_else(|| "Video frame dimensions overflow".to_string())?;
        let chroma_size = y_size / 4;
        let expected_size = y_size
            .checked_add(chroma_size.checked_mul(2)
                .ok_or_else(|| "Video frame size overflow".to_string())?)
            .ok_or_else(|| "Video frame size overflow".to_string())?;

        if bytes.len() != expected_size {
            return Err(format!(
                "Invalid I420 frame size: expected {expected_size} bytes, received {}",
                bytes.len()
            ));
        }

        let session = self
            .rtc_livekit_audio_sessions
            .get(room_id)
            .ok_or_else(|| "No active LiveKit call in this room".to_string())?;

        let source = session
            .video_source
            .as_ref()
            .ok_or_else(|| "Camera video is not active in this room".to_string())?;

        let mut buffer = I420Buffer::new(width, height);
        let (plane_y, plane_u, plane_v) = buffer.data_mut();

        if plane_y.len() < y_size
            || plane_u.len() < chroma_size
            || plane_v.len() < chroma_size
        {
            return Err("LiveKit allocated an unexpectedly small I420 buffer".into());
        }

        plane_y[..y_size].copy_from_slice(&bytes[..y_size]);
        plane_u[..chroma_size].copy_from_slice(
            &bytes[y_size..y_size + chroma_size]
        );
        plane_v[..chroma_size].copy_from_slice(
            &bytes[y_size + chroma_size..expected_size]
        );

        let frame = VideoFrame::new(VideoRotation::VideoRotation0, buffer);

        let source_resolution = source.video_resolution();
        if source_resolution.width != width || source_resolution.height != height {
            return Err(format!(
                "Camera frame size {}x{} does not match LiveKit source size {}x{}",
                width, height, source_resolution.width, source_resolution.height
            ));
        }

        let captured = {
            let _runtime_guard = runtime.enter();
            source.capture_frame(&frame)
        };

        // Like the screen-share source below, LiveKit may return false
        // when its video adapter drops a frame during frame-rate or
        // resolution adaptation. This is not a malformed I420 buffer:
        // dimensions, packed plane sizes and source resolution were
        // validated above. A dropped frame must not be surfaced as a
        // camera-capture failure.
        let _frame_forwarded = captured;

        Ok(())
    }

    pub fn push_rtc_screenshare_i420_buffer(
        &mut self,
        room_id: &str,
        buffer: I420Buffer,
    ) -> Result<(), String> {
        let room_id = room_id.trim();
        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        let runtime = Arc::clone(&self.runtime);
        let width = buffer.width();
        let height = buffer.height();
        if width < 2 || height < 2
            || width > 1920 || height > 1920
            || width % 2 != 0 || height % 2 != 0
            || (width as u64).checked_mul(height as u64)
                .is_none_or(|pixels| pixels > 2_073_600)
        {
            return Err("Screen-share frame dimensions must be even, no larger than 1920 pixels per side, and within a 1080p pixel budget".into());
        }

        let session = self
            .rtc_livekit_audio_sessions
            .get_mut(room_id)
            .ok_or_else(|| "No active LiveKit call in this room".to_string())?;
        let source = session
            .screen_share_source
            .as_ref()
            .ok_or_else(|| "Screen-share video is not active in this room".to_string())?;

        let source_resolution = source.video_resolution();
        if source_resolution.width != width || source_resolution.height != height {
            return Err(format!(
                "Screen-share frame size {}x{} does not match LiveKit source size {}x{}",
                width, height, source_resolution.width, source_resolution.height
            ));
        }

        // The I420 allocation is transferred from the C++ capture stage.
        // capture_frame may retain it asynchronously, so never recycle or
        // mutate this buffer after handing it to LiveKit.
        let frame = VideoFrame::new(VideoRotation::VideoRotation0, buffer);
        let captured = {
            let _runtime_guard = runtime.enter();
            source.capture_frame(&frame)
        };

        session.screen_share_frames_submitted =
            session.screen_share_frames_submitted.saturating_add(1);
        if captured {
            session.screen_share_frames_accepted =
                session.screen_share_frames_accepted.saturating_add(1);
        } else {
            session.screen_share_frames_rejected =
                session.screen_share_frames_rejected.saturating_add(1);
        }

        if session.screen_share_stats_last_report.elapsed() >= Duration::from_secs(5) {
            session.screen_share_stats_last_report = Instant::now();
            eprintln!(
                "ASTRO_SCREEN_SHARE_RTC: submitted={} accepted={} rejected={} stats=disabled",
                session.screen_share_frames_submitted,
                session.screen_share_frames_accepted,
                session.screen_share_frames_rejected
            );
        }

        Ok(())
    }


    /// Unpublish the camera track without stopping the microphone.
    pub fn stop_rtc_video(&mut self, room_id: &str) -> Result<(), String> {
        let room_id = room_id.trim();

        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        let runtime = Arc::clone(&self.runtime);
        let session = self
            .rtc_livekit_audio_sessions
            .get_mut(room_id)
            .ok_or_else(|| "No active LiveKit call in this room".to_string())?;

        let Some(track) = session.video_track.as_ref() else {
            return Ok(());
        };

        let track_sid = LocalTrack::Video(track.clone()).sid();

        runtime.block_on(async {
            session
                .room
                .local_participant()
                .unpublish_track(&track_sid)
                .await
                .map_err(|error| {
                    format!("Could not unpublish LiveKit camera track: {error}")
                })
        })?;

        session.video_track = None;
        session.video_source = None;

        Ok(())
    }

    pub fn push_rtc_screenshare_audio_frame(
        &mut self,
        room_id: &str,
        pcm: &[i16],
    ) -> Result<(), String> {
        let room_id = room_id.trim();
        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }
        if pcm.len() != 960 {
            return Err(format!(
                "Invalid screen-share audio frame: expected 960 samples, received {}",
                pcm.len()
            ));
        }

        let session = self
            .rtc_livekit_audio_sessions
            .get(room_id)
            .ok_or_else(|| "No active LiveKit call in this room".to_string())?;
        let sender = session
            .screen_share_audio_sender
            .as_ref()
            .ok_or_else(|| "Screen-share audio is not active in this room".to_string())?;

        // Never wait for LiveKit on the Qt audio callback. If the bounded
        // queue is full, discard this 10 ms frame to keep capture responsive.
        match sender.try_send(pcm.to_vec()) {
            Ok(()) => Ok(()),
            Err(tokio::sync::mpsc::error::TrySendError::Full(_)) => Ok(()),
            Err(tokio::sync::mpsc::error::TrySendError::Closed(_)) => {
                Err("Screen-share audio sender has stopped".into())
            }
        }
    }


    /// Begin unpublishing screen-share tracks without blocking the Qt worker.
    /// Returns true while LiveKit teardown is running on the existing Tokio runtime.
    pub fn stop_rtc_screenshare(&mut self, room_id: &str) -> Result<bool, String> {
        let room_id = room_id.trim();

        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        if self.rtc_pending_screenshare_stops.contains_key(room_id) {
            return Ok(true);
        }

        let session = self
            .rtc_livekit_audio_sessions
            .get_mut(room_id)
            .ok_or_else(|| "No active LiveKit call in this room".to_string())?;

        let video_track = session.screen_share_track.clone();
        let audio_track = session.screen_share_audio_track.clone();
        if video_track.is_none() && audio_track.is_none() {
            session.screen_share_audio_sender = None;
            session.screen_share_source = None;
            session.screen_share_track = None;
            session.screen_share_audio_source = None;
            session.screen_share_audio_track = None;
            return Ok(false);
        }

        let participant = session.room.local_participant();
        session.screen_share_audio_sender = None;
        session.screen_share_track = None;
        session.screen_share_source = None;
        session.screen_share_audio_track = None;
        session.screen_share_audio_source = None;

        let completion = Arc::new(Mutex::new(None));
        let task_completion = Arc::clone(&completion);
        self.runtime.spawn(async move {
            // LiveKit signalling can stall during shutdown on a weak or
            // disconnected mobile network. Bound the whole unpublish phase so
            // the Qt Stop action always receives a completion instead of
            // remaining pending forever.
            let unpublish = async {
                let mut failures = Vec::new();

                if let Some(track) = video_track {
                    let track_sid = LocalTrack::Video(track).sid();
                    if let Err(error) = participant.unpublish_track(&track_sid).await {
                        failures.push(format!(
                            "Could not unpublish LiveKit screen share track: {error}"
                        ));
                    }
                }

                if let Some(track) = audio_track {
                    let track_sid = LocalTrack::Audio(track).sid();
                    if let Err(error) = participant.unpublish_track(&track_sid).await {
                        failures.push(format!(
                            "Could not unpublish LiveKit screen-share audio track: {error}"
                        ));
                    }
                }

                if failures.is_empty() {
                    Ok(())
                } else {
                    Err(failures.join("; "))
                }
            };

            let result = match tokio::time::timeout(
                Duration::from_secs(12),
                unpublish,
            ).await {
                Ok(result) => result,
                Err(_) => Err(
                    "Timed out after 12 seconds while unpublishing the LiveKit screen share".into()
                ),
            };

            *task_completion.lock().unwrap() = Some(result);
        });

        self.rtc_pending_screenshare_stops
            .insert(room_id.to_owned(), completion);

        Ok(true)
    }

    /// Poll an asynchronous screen-share teardown. None means it is still running.
    pub fn poll_rtc_screenshare_stop(
        &mut self,
        room_id: &str,
    ) -> Result<Option<Result<(), String>>, String> {
        let completion = self
            .rtc_pending_screenshare_stops
            .get(room_id)
            .cloned()
            .ok_or_else(|| "No pending screen-share teardown for this room".to_string())?;

        let result = completion.lock().unwrap().take();
        if result.is_some() {
            self.rtc_pending_screenshare_stops.remove(room_id);
        }

        Ok(result)
    }


    /// Disconnect the LiveKit audio session for a room.
    pub fn stop_rtc_audio(&mut self, room_id: &str) -> Result<(), String> {
        let room_id = room_id.trim();

        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        let runtime = Arc::clone(&self.runtime);

        {
            let Some(session) = self.rtc_livekit_audio_sessions.get(room_id) else {
                return Ok(());
            };

            runtime.block_on(async {
                session.room.close()
                    .await
                    .map_err(|error| format!("Could not close LiveKit room: {error}"))
            })?;
        }

        self.rtc_livekit_audio_sessions.remove(room_id);

        let room_prefix = format!("{room_id}\u{1f}");
        let task_keys: Vec<String> = self.rtc_remote_video_tasks
            .keys()
            .chain(self.rtc_remote_audio_tasks.keys())
            .filter(|key| key.starts_with(&room_prefix))
            .cloned()
            .collect::<HashSet<_>>()
            .into_iter()
            .collect();
        for key in task_keys {
            if let Some(task) = self.rtc_remote_video_tasks.remove(&key) {
                task.abort();
            }
            if let Some(task) = self.rtc_remote_audio_tasks.remove(&key) {
                task.abort();
            }
        }
        if let Ok(mut frames) = self.rtc_remote_video_frames.lock() {
            frames.retain(|frame| !frame.key.starts_with(&room_prefix));
        }
        if let Ok(mut frames) = self.rtc_remote_audio_frames.lock() {
            frames.retain(|frame| !frame.key.starts_with(&room_prefix));
        }

        Ok(())
    }

    /// Set the microphone mute state for a room's LiveKit session.
    pub fn set_rtc_audio_muted(
        &mut self,
        room_id: &str,
        muted: bool,
    ) -> Result<(), String> {
        let room_id = room_id.trim();

        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        let runtime = Arc::clone(&self.runtime);

        let session = self
            .rtc_livekit_audio_sessions
            .get_mut(room_id)
            .ok_or_else(|| "No active audio call in this room".to_string())?;

        if session.muted == muted {
            return Ok(());
        }

        {
            let _runtime_guard = runtime.enter();

            if muted {
                session.microphone.mute();
            } else {
                session.microphone.unmute();
            }
        }

        session.muted = muted;
        Ok(())
    }

    /// Drain queued LiveKit events for the active audio sessions.
    ///
    /// This is intended to be called by StorageWorker's existing polling timer.
    pub fn poll_rtc_audio_events_json(&mut self) -> String {
        let mut updates = Vec::new();
        let runtime = Arc::clone(&self.runtime);
        let frame_queue = Arc::clone(&self.rtc_remote_video_frames);
        let audio_frame_queue = Arc::clone(&self.rtc_remote_audio_frames);
        let mut tasks_to_add = Vec::new();
        let mut audio_tasks_to_add = Vec::new();
        let mut cancelled_keys = HashSet::new();
        let mut cancelled_prefixes = HashSet::new();

        // Feed MatrixRTC rotations and newly received participant keys into
        // the already-connected LiveKit frame-cryptor key providers.
        let e2ee_snapshots: HashMap<String, Vec<(String, u8, Vec<u8>)>> =
            self.rtc_e2ee_sessions.iter()
                .map(|(room_id, e2ee)| (room_id.clone(), e2ee.livekit_key_snapshot()))
                .collect();

        for (room_id, session) in self.rtc_livekit_audio_sessions.iter_mut() {
            if let (Some(provider), Some(keys)) =
                (session.e2ee_key_provider.as_ref(), e2ee_snapshots.get(room_id))
            {
                let local_identity = session.room.local_participant().identity().to_string();
                let mut local_key = None;
                let mut remote_key_attempted = false;

                // Remote keys may be installed at any time, but always restore
                // the local key index last because LiveKit tracks one latest
                // sending index on the KeyProvider.
                for (identity, index, key) in keys {
                    if identity == &local_identity {
                        local_key = Some((*index, key.clone()));
                        continue;
                    }

                    let cache_key = (identity.clone(), *index);
                    if session.e2ee_installed_keys.get(&cache_key) == Some(key) {
                        continue;
                    }

                    let participant_identity = ParticipantIdentity::from(identity.clone());
                    remote_key_attempted = true;
                    if provider.set_key(&participant_identity, i32::from(*index), key.clone()) {
                        session.e2ee_installed_keys.insert(cache_key, key.clone());
                        eprintln!(
                            "ASTRO RTC E2EE NEGOTIATION: provider accepted remote key; room={}, identity={}, index={}",
                            room_id, identity, index
                        );
                    } else {
                        eprintln!(
                            "ASTRO RTC E2EE NEGOTIATION: provider rejected remote key; room={}, identity={}, index={}",
                            room_id, identity, index
                        );
                    }
                }

                let mut active_local_key_index = None;
                if let Some((index, key)) = local_key {
                    let cache_key = (local_identity.clone(), index);
                    let local_key_changed = session.e2ee_installed_keys.get(&cache_key) != Some(&key);
                    if remote_key_attempted || local_key_changed {
                        let participant_identity = ParticipantIdentity::from(local_identity.clone());
                        if provider.set_key(&participant_identity, i32::from(index), key.clone()) {
                            session.e2ee_installed_keys.insert(cache_key.clone(), key.clone());
                        } else {
                            eprintln!(
                                "ASTRO RTC E2EE: LiveKit rejected local sender key index {}",
                                index
                            );
                        }
                    }

                    // livekit-rust-sdk 0.9.7 stores a newly supplied local key
                    // in the provider, but does not advance existing sender
                    // FrameCryptors to that key index. Element Call's JS SDK
                    // does advance its sender cryptors when a key is set.
                    // Keep every local audio/video sender on the same index
                    // that MatrixRTC has distributed, including tracks
                    // published after a rotation.
                    if session.e2ee_installed_keys.get(&cache_key) == Some(&key) {
                        active_local_key_index = Some(index);
                    }
                }

                if let Some(index) = active_local_key_index {
                    let expected_index = i32::from(index);
                    for ((identity, track_sid), cryptor) in
                        session.room.e2ee_manager().frame_cryptors()
                    {
                        if identity.to_string() != local_identity {
                            continue;
                        }

                        let current_index = cryptor.key_index();
                        if current_index != expected_index {
                            cryptor.set_key_index(expected_index);
                            eprintln!(
                                "ASTRO RTC E2EE: synchronized local LiveKit frame cryptor index; room={}, track={}, previous_index={}, key_index={}",
                                room_id, track_sid, current_index, expected_index
                            );
                        }
                    }
                }
            }
            // Bound work per room so a large event burst cannot monopolise the worker.
            for _ in 0..256 {
                let event = match session.events.try_recv() {
                    Ok(event) => event,
                    Err(_) => break,
                };

                let update = match event {
                    RoomEvent::ParticipantConnected(participant) => {
                        let participant_identity = participant.identity().to_string();
                        let matching_installed_keys = session.e2ee_installed_keys.keys()
                            .filter(|(identity, _)| identity == &participant_identity)
                            .map(|(_, index)| index.to_string())
                            .collect::<Vec<_>>();
                        eprintln!(
                            "ASTRO RTC E2EE NEGOTIATION: remote participant connected; room={}, identity={}, installed_key_indexes={}",
                            room_id,
                            participant_identity,
                            if matching_installed_keys.is_empty() { "<none>".to_owned() } else { matching_installed_keys.join(",") }
                        );
                        tracing::info!(
                            room_id = %room_id,
                            participant = %participant.identity(),
                            participant_sid = %participant.sid(),
                            name = %participant.name(),
                            "LiveKit remote participant connected"
                        );
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "participant_connected",
                            "identity": participant.identity().to_string(),
                            "name": participant.name(),
                            "metadata": participant.metadata(),
                            "speaking": participant.is_speaking(),
                            "audio_level": participant.audio_level()
                        }))
                    }
                    RoomEvent::ParticipantActive(participant) => {
                        tracing::info!(
                            room_id = %room_id,
                            participant = %participant.identity(),
                            participant_sid = %participant.sid(),
                            "LiveKit remote participant became active"
                        );
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "participant_active",
                            "identity": participant.identity().to_string(),
                            "name": participant.name(),
                            "metadata": participant.metadata(),
                            "speaking": participant.is_speaking(),
                            "audio_level": participant.audio_level()
                        }))
                    }
                    RoomEvent::ParticipantDisconnected(participant) => {
                        let prefix = format!("{room_id}\u{1f}{}\u{1f}", participant.identity());
                        cancelled_prefixes.insert(prefix);
                        tracing::info!(
                            room_id = %room_id,
                            participant = %participant.identity(),
                            participant_sid = %participant.sid(),
                            "LiveKit participant disconnected"
                        );
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "participant_disconnected",
                            "identity": participant.identity().to_string(),
                            "name": participant.name()
                        }))
                    }
                    RoomEvent::Connected { participants_with_tracks } => {
                        let participant_count = participants_with_tracks.len();
                        let track_count: usize = participants_with_tracks
                            .iter()
                            .map(|(_, publications)| publications.len())
                            .sum();
                        tracing::info!(
                            room_id = %room_id,
                            participants = participant_count,
                            published_tracks = track_count,
                            "LiveKit room connected with existing remote publications"
                        );
                        for (participant, publications) in &participants_with_tracks {
                            tracing::info!(
                                room_id = %room_id,
                                participant = %participant.identity(),
                                participant_sid = %participant.sid(),
                                "LiveKit remote participant was already present when Astro connected"
                            );
                            for publication in publications {
                                tracing::info!(
                                    room_id = %room_id,
                                    participant = %participant.identity(),
                                    participant_sid = %participant.sid(),
                                    track_sid = %publication.sid(),
                                    track_kind = ?publication.kind(),
                                    track_source = ?publication.source(),
                                    muted = publication.is_muted(),
                                    subscribed = publication.is_subscribed(),
                                    "LiveKit existing remote track publication"
                                );
                            }
                        }
                        // ParticipantConnected is not guaranteed to be delivered
                        // for peers that were already in the room when this client
                        // finished connecting. Surface the existing participant to
                        // QML as an answer event as well, otherwise outgoing
                        // ringback can continue until the no-answer timer fires.
                        participants_with_tracks.first().map(|(participant, _)| {
                            serde_json::json!({
                                "room_id": room_id,
                                "type": "participant_connected",
                                "identity": participant.identity().to_string(),
                                "name": participant.name(),
                                "metadata": participant.metadata(),
                                "speaking": participant.is_speaking(),
                                "audio_level": participant.audio_level()
                            })
                        })
                    }
                    RoomEvent::E2eeStateChanged { participant, state } => {
                        tracing::warn!(
                            room_id = %room_id,
                            participant = %participant.identity(),
                            state = ?state,
                            "LiveKit frame-cryptor E2EE state changed"
                        );
                        None
                    }
                    RoomEvent::TrackPublished { participant, publication } => {
                        tracing::info!(
                            room_id = %room_id,
                            participant = %participant.identity(),
                            participant_sid = %participant.sid(),
                            track_sid = %publication.sid(),
                            track_kind = ?publication.kind(),
                            track_source = ?publication.source(),
                            muted = publication.is_muted(),
                            subscribed = publication.is_subscribed(),
                            "LiveKit remote track published"
                        );
                        None
                    }
                    RoomEvent::TrackSubscriptionFailed { participant, track_sid, error } => {
                        tracing::error!(
                            room_id = %room_id,
                            participant = %participant.identity(),
                            participant_sid = %participant.sid(),
                            track_sid = %track_sid,
                            error = ?error,
                            "LiveKit remote track subscription failed"
                        );
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "track_subscription_failed",
                            "identity": participant.identity().to_string(),
                            "track_sid": track_sid.to_string(),
                            "error": format!("{error:?}")
                        }))
                    }
                    RoomEvent::TrackSubscribed { participant, track, .. } => {
                        tracing::info!(
                            room_id = %room_id,
                            participant = %participant.identity(),
                            participant_sid = %participant.sid(),
                            track_sid = %track.sid(),
                            track_kind = ?track.kind(),
                            track_source = ?track.source(),
                            muted = track.is_muted(),
                            "LiveKit remote track subscription succeeded"
                        );
                        if let RemoteTrack::Audio(audio_track) = &track {
                            let identity = participant.identity().to_string();
                            let track_sid = audio_track.sid().to_string();
                            let key = format!("{room_id}\u{1f}{identity}\u{1f}{track_sid}");
                            tracing::info!(
                                room_id = %room_id,
                                participant = %identity,
                                track_sid = %track_sid,
                                source = ?audio_track.source(),
                                muted = audio_track.is_muted(),
                                "LiveKit remote audio track subscribed"
                            );
                            let task_key = key.clone();
                            let task_room_id = room_id.clone();
                            let task_identity = identity.clone();
                            let mut stream = NativeAudioStream::new(audio_track.rtc_track(), 48_000, 2);
                            let task_audio_queue = Arc::clone(&audio_frame_queue);
                            let task = runtime.spawn(async move {
                                let mut logged_first_frame = false;
                                while let Some(frame) = stream.next().await {
                                    if frame.data.is_empty()
                                        || frame.num_channels == 0
                                        || frame.sample_rate == 0
                                    {
                                        continue;
                                    }
                                    let mut pcm = Vec::with_capacity(frame.data.len() * 2);
                                    for sample in frame.data.iter() {
                                        pcm.extend_from_slice(&sample.to_le_bytes());
                                    }
                                    if !logged_first_frame {
                                        tracing::info!(
                                            room_id = %task_room_id,
                                            participant = %task_identity,
                                            sample_rate = frame.sample_rate,
                                            channels = frame.num_channels,
                                            samples = frame.samples_per_channel,
                                            "First decoded LiveKit remote audio frame received"
                                        );
                                        logged_first_frame = true;
                                    }
                                    if let Ok(mut frames) = task_audio_queue.lock() {
                                        if frames.len() >= RTC_AUDIO_FRAME_QUEUE_LIMIT {
                                            frames.pop_front();
                                        }
                                        frames.push_back(RtcRemoteAudioFrame {
                                            key: task_key.clone(),
                                            room_id: task_room_id.clone(),
                                            identity: task_identity.clone(),
                                            sample_rate: frame.sample_rate,
                                            channels: frame.num_channels,
                                            data: pcm,
                                        });
                                    }
                                }
                                tracing::info!(
                                    room_id = %task_room_id,
                                    participant = %task_identity,
                                    "LiveKit remote audio stream ended"
                                );
                            });
                            audio_tasks_to_add.push((key, task.abort_handle()));
                        }
                        if let RemoteTrack::Video(video_track) = track {
                            let track_source = match video_track.source() {
                                TrackSource::Camera => Some("camera"),
                                TrackSource::Screenshare => Some("screenshare"),
                                _ => None,
                            };
                            if let Some(track_source) = track_source {
                                let identity = participant.identity().to_string();
                                let track_sid = video_track.sid().to_string();
                                tracing::info!(
                                    room_id = %room_id,
                                    participant = %identity,
                                    track_sid = %track_sid,
                                    source = track_source,
                                    "LiveKit remote video track subscribed"
                                );
                                let key = format!("{room_id}\u{1f}{identity}\u{1f}{track_sid}");
                                let task_key = key.clone();
                                let task_room_id = room_id.clone();
                                let task_identity = identity.clone();
                                let task_source = track_source.to_string();
                                let mut stream = NativeVideoStream::new(video_track.rtc_track());
                                let task_frame_queue = Arc::clone(&frame_queue);
                                let task = runtime.spawn(async move {
                                    let mut logged_first_frame = false;
                                    let mut logged_pack_failure = false;
                                    while let Some(frame) = stream.next().await {
                                        let width = frame.buffer.width();
                                        let height = frame.buffer.height();
                                        let data = match pack_remote_video_frame(frame.buffer.as_ref()) {
                                            Some(data) => data,
                                            None => {
                                                if !logged_pack_failure {
                                                    tracing::warn!(
                                                        room_id = %task_room_id,
                                                        participant = %task_identity,
                                                        width,
                                                        height,
                                                        "Could not pack decoded LiveKit remote video frame as I420"
                                                    );
                                                    logged_pack_failure = true;
                                                }
                                                continue;
                                            }
                                        };

                                        if !logged_first_frame {
                                            tracing::info!(
                                                room_id = %task_room_id,
                                                participant = %task_identity,
                                                width,
                                                height,
                                                "First decoded LiveKit camera frame received"
                                            );
                                            logged_first_frame = true;
                                        }

                                        let next_frame = RtcRemoteVideoFrame {
                                            key: task_key.clone(),
                                            room_id: task_room_id.clone(),
                                            identity: task_identity.clone(),
                                            source: task_source.clone(),
                                            width,
                                            height,
                                            data,
                                        };

                                        if let Ok(mut frames) = task_frame_queue.lock() {
                                            if let Some(existing) = frames.iter_mut()
                                                .find(|queued| queued.key == task_key)
                                            {
                                                *existing = next_frame;
                                            } else {
                                                if frames.len() >= RTC_VIDEO_FRAME_QUEUE_LIMIT {
                                                    frames.pop_front();
                                                }
                                                frames.push_back(next_frame);
                                            }
                                        }
                                    }
                                });
                                tasks_to_add.push((key, task.abort_handle()));
                            }
                        }

                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "track_subscribed",
                            "identity": participant.identity().to_string(),
                            "name": participant.name()
                        }))
                    }
                    RoomEvent::TrackUnsubscribed { participant, track, .. } => {
                        let is_audio = matches!(&track, RemoteTrack::Audio(_));
                        let track_source = match &track {
                            RemoteTrack::Video(video_track) => match video_track.source() {
                                TrackSource::Camera => Some("camera"),
                                TrackSource::Screenshare => Some("screenshare"),
                                _ => None,
                            },
                            _ => None,
                        };
                        let is_camera = track_source == Some("camera");
                        let is_screenshare = track_source == Some("screenshare");
                        let key = match &track {
                            RemoteTrack::Audio(audio_track) => Some(format!(
                                "{room_id}\u{1f}{}\u{1f}{}",
                                participant.identity(), audio_track.sid()
                            )),
                            RemoteTrack::Video(video_track) => Some(format!(
                                "{room_id}\u{1f}{}\u{1f}{}",
                                participant.identity(), video_track.sid()
                            )),
                        };
                        if let Some(key) = key { cancelled_keys.insert(key); }
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "track_unsubscribed",
                            "is_camera": is_camera,
                            "is_screenshare": is_screenshare,
                            "is_audio": is_audio,
                            "identity": participant.identity().to_string(),
                            "name": participant.name()
                        }))
                    }
                    RoomEvent::ConnectionStateChanged(state) => {
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "connection_state_changed",
                            "state": format!("{state:?}")
                        }))
                    }
                    RoomEvent::Reconnecting => {
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "reconnecting"
                        }))
                    }
                    RoomEvent::Reconnected => {
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "reconnected"
                        }))
                    }
                    RoomEvent::Disconnected { reason } => {
                        cancelled_prefixes.insert(format!("{room_id}\u{1f}"));
                        Some(serde_json::json!({
                            "room_id": room_id,
                            "type": "disconnected",
                            "reason": format!("{reason:?}")
                        }))
                    }
                    _ => None,
                };

                if let Some(update) = update {
                    updates.push(update);
                }
            }
        }

        for (key, task) in tasks_to_add {
            if let Some(previous) = self.rtc_remote_video_tasks.insert(key, task) {
                previous.abort();
            }
        }
        for (key, task) in audio_tasks_to_add {
            if let Some(previous) = self.rtc_remote_audio_tasks.insert(key, task) {
                previous.abort();
            }
        }

        if !cancelled_keys.is_empty() || !cancelled_prefixes.is_empty() {
            let task_keys: Vec<String> = self.rtc_remote_video_tasks.keys()
                .chain(self.rtc_remote_audio_tasks.keys())
                .filter(|key| {
                    cancelled_keys.contains(*key)
                        || cancelled_prefixes.iter().any(|prefix| key.starts_with(prefix))
                })
                .cloned()
                .collect::<HashSet<_>>()
                .into_iter()
                .collect();

            for key in &task_keys {
                if let Some(task) = self.rtc_remote_video_tasks.remove(key) {
                    task.abort();
                }
                if let Some(task) = self.rtc_remote_audio_tasks.remove(key) {
                    task.abort();
                }
            }

            if let Ok(mut frames) = self.rtc_remote_audio_frames.lock() {
                frames.retain(|frame| {
                    !cancelled_keys.contains(&frame.key)
                        && !cancelled_prefixes.iter().any(|prefix| frame.key.starts_with(prefix))
                });
            }

            if let Ok(mut frames) = self.rtc_remote_video_frames.lock() {
                frames.retain(|frame| {
                    !cancelled_keys.contains(&frame.key)
                        && !cancelled_prefixes.iter().any(|prefix| frame.key.starts_with(prefix))
                });
            }
        }

        serde_json::json!({ "events": updates }).to_string()
    }

    pub fn take_rtc_audio_frame(
        &mut self,
    ) -> Option<(String, String, u32, u32, Vec<u8>)> {
        let frame = self.rtc_remote_audio_frames.lock().ok()?.pop_front()?;
        Some((frame.room_id, frame.identity, frame.sample_rate, frame.channels, frame.data))
    }

    pub fn take_rtc_video_frame(
        &mut self,
    ) -> Option<(String, String, String, u32, u32, Vec<u8>)> {
        let frame = self.rtc_remote_video_frames.lock().ok()?.pop_front()?;
        Some((frame.room_id, frame.identity, frame.source, frame.width, frame.height, frame.data))
    }

    /// Withdraw Astro's own compatibility-mode MatrixRTC membership.
    pub fn leave_rtc_room(&mut self, room_id: &str) -> Result<(), String> {
        let room_id = room_id.trim();
        if room_id.is_empty() {
            return Err("Room ID is empty".into());
        }

        let client = self.client.as_ref()
            .ok_or_else(|| "No active Matrix session".to_string())?
            .clone();

        let parsed_room_id = matrix_sdk::ruma::RoomId::parse(room_id)
            .map_err(|error| format!("Invalid room ID: {error}"))?;

        let remembered_key = self.rtc_local_memberships.get(room_id).cloned();
        let remembered_modern = self.rtc_modern_memberships.get(room_id).cloned();

        let state_key = self.runtime.block_on(async move {
            let room = client.joined_rooms().into_iter()
                .find(|room| room.room_id() == &parsed_room_id)
                .ok_or_else(|| "You are no longer joined to this room".to_string())?;

            let key = match remembered_key {
                Some(key) => key,
                None => {
                    let user_id = client.user_id()
                        .ok_or_else(|| "Matrix user ID is unavailable".to_string())?
                        .to_string();
                    let device_id = client.device_id()
                        .ok_or_else(|| "Matrix device ID is unavailable".to_string())?
                        .to_string();
                    rtc_membership_state_key(&room, &user_id, &device_id)
                }
            };

            room.send_state_event_raw(
                "org.matrix.msc3401.call.member", &key, serde_json::json!({})
            ).await.map_err(|error| format!("Could not withdraw MatrixRTC call presence: {error}"))?;
            if let Some((_member_id, mut modern_content)) = remembered_modern {
                modern_content["member"]["membership"] = serde_json::json!("leave");
                room.send_raw("m.rtc.member", modern_content)
                    .with_sticky_duration(Duration::from_secs(60 * 60)).await
                    .map_err(|error| format!("Could not publish MatrixRTC leave membership: {error}"))?;
            }
            Ok::<String, String>(key)
        })?;

        self.rtc_local_memberships.remove(room_id);
        self.rtc_outgoing_call_notifications.remove(room_id);
        self.rtc_modern_memberships.remove(room_id);
        self.rtc_e2ee_sessions.remove(room_id);
        if let Ok(mut renewals) = self.rtc_modern_renewals.lock() { renewals.remove(room_id); }
        self.rtc_livekit_services.remove(room_id);
        if let Ok(mut renewals) = self.rtc_renewals.lock() {
            renewals.remove(room_id);
        }

        // Force the next membership poll to publish the cleared state to QML.
        self.rtc_last_snapshots.remove(room_id);

        let _ = state_key;
        Ok(())
    }

    pub fn poll_rtc_memberships_json(&mut self) -> String {
        let Some(client) = self.client.as_ref().cloned() else {
            return serde_json::json!({ "updates": [] }).to_string();
        };

        let own_user_id = client.user_id().map(|id| id.to_string());
        let mut changed_updates = Vec::new();
        let mut current_rooms = HashSet::new();

        for room in client.joined_rooms() {
            let room_id = room.room_id().to_string();
            current_rooms.insert(room_id.clone());

            let mut memberships = Vec::new();

            let state_events = self.runtime.block_on(
                room.get_state_events(
                    matrix_sdk::ruma::events::StateEventType::from(
                        "org.matrix.msc3401.call.member"
                    )
                )
            ).unwrap_or_default();

            let now_ms = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|duration| duration.as_millis() as u64)
                .unwrap_or(0);

            for event in state_events {
                let event_json = match event {
                    matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Sync(raw) => {
                        raw.json().get().to_owned()
                    }
                    matrix_sdk::deserialized_responses::RawAnySyncOrStrippedState::Stripped(raw) => {
                        raw.json().get().to_owned()
                    }
                };

                let Ok(raw_event) =
                    serde_json::from_str::<serde_json::Value>(&event_json)
                else {
                    continue;
                };

                let sender = raw_event["sender"].as_str().unwrap_or("");
                if sender.is_empty() {
                    continue;
                }

                let content = &raw_event["content"];
                if !content.is_object() || content.as_object().is_some_and(|v| v.is_empty()) {
                    continue;
                }

                let entries: Vec<&serde_json::Value> =
                    if let Some(array) = content["memberships"].as_array() {
                        array.iter().collect()
                    } else {
                        vec![content]
                    };

                for member in entries {
                    let application = member["application"]
                        .as_str()
                        .or_else(|| content["application"].as_str())
                        .unwrap_or("");

                    let call_id = member["call_id"].as_str().unwrap_or("");
                    let scope = member["scope"].as_str().unwrap_or("");

                    if application != "m.call"
                        || !call_id.is_empty()
                        || (!scope.is_empty() && scope != "m.room")
                    {
                        continue;
                    }

                    let device_id = member["device_id"].as_str().unwrap_or("");
                    let state_key = raw_event["state_key"].as_str().unwrap_or("");
                    if state_key.is_empty() || device_id.is_empty() {
                        continue;
                    }
                    let member_id = state_key;

                    // Legacy memberships use an expiry duration. Session
                    // memberships normally have no timestamp-based expiry.
                    let absolute_expiry = member["expires_ts"].as_u64()
                        .or_else(|| {
                            let duration = member["expires"].as_u64()?;
                            let event_ts = raw_event["origin_server_ts"].as_u64();
                            let created_ts = member["created_ts"].as_u64();

                            let base = created_ts.or(event_ts)?;

                            base.checked_add(duration)
                        });

                    if absolute_expiry.is_some_and(|expiry| now_ms > expiry) {
                        continue;
                    }

                    let cache_key = (room_id.clone(), sender.to_owned());

                    if !self.rtc_participant_profiles.contains_key(&cache_key) {
                        if let Ok(user_id) =
                            matrix_sdk::ruma::UserId::parse(sender)
                        {
                            // MatrixRTC can advertise a participant before this
                            // client's lazy-loaded room-member cache contains their
                            // m.room.member event. get_member() ensures that member
                            // state is fetched when needed; get_member_no_sync()
                            // silently returns None in that situation.
                            let profile_result = self.runtime.block_on(
                                room.get_member(&user_id)
                            );

                            match profile_result {
                                Ok(Some(profile)) => {
                                    let display_name =
                                        profile.display_name().map(str::to_owned);
                                    let avatar_url =
                                        profile.avatar_url().map(ToString::to_string);

                                    eprintln!(
                                        "ASTRO RTC PROFILE: room={} user={} member_found=true display_name={} avatar={}",
                                        room_id,
                                        sender,
                                        display_name.is_some(),
                                        avatar_url.is_some()
                                    );

                                    self.rtc_participant_profiles.insert(
                                        cache_key.clone(),
                                        RtcParticipantProfile {
                                            display_name,
                                            avatar_url,
                                        },
                                    );
                                }
                                Ok(None) => {
                                    eprintln!(
                                        "ASTRO RTC PROFILE: room={} user={} member_found=false",
                                        room_id,
                                        sender
                                    );
                                }
                                Err(error) => {
                                    eprintln!(
                                        "ASTRO RTC PROFILE: room={} user={} lookup_failed={}",
                                        room_id,
                                        sender,
                                        error
                                    );
                                }
                            }
                        }
                    }

                    let profile = self.rtc_participant_profiles.get(&cache_key);

                    memberships.push(serde_json::json!({
                        "sender": sender,
                        "member_id": member_id,
                        "slot_id": "",
                        "display_name": profile.and_then(|p| p.display_name.clone()),
                        "avatar_url": profile.and_then(|p| p.avatar_url.clone())
                    }));
                }
            }   //Congrats you found Devabu's secret comment :); 24.11.26

            memberships.sort_by(|a, b| {
                a["sender"].as_str().cmp(&b["sender"].as_str())
                    .then_with(|| {
                        a["member_id"].as_str().cmp(&b["member_id"].as_str())
                    })
            });

            let slot_available = true;

            let local_member_id = self.rtc_local_memberships.get(&room_id);

            let joined_by_me = local_member_id.is_some_and(|local_id| {
                memberships.iter().any(|item| {
                    item["sender"].as_str() == own_user_id.as_deref()
                        && item["member_id"].as_str() == Some(local_id.as_str())
                })
            });

            let snapshot = serde_json::json!({
                "memberships": memberships,
                "slot_available": slot_available,
                "joined_by_me": joined_by_me
            });

            if self.rtc_last_snapshots.get(&room_id) != Some(&snapshot) {
                self.rtc_last_snapshots.insert(room_id.clone(), snapshot.clone());

                changed_updates.push(serde_json::json!({
                    "room_id": room_id,
                    "memberships": snapshot["memberships"],
                    "slot_available": slot_available,
                    "joined_by_me": joined_by_me
                }));
            }
        }

        // Clear QML state for rooms which are no longer joined.
        let departed_rooms: Vec<String> = self.rtc_last_snapshots
            .keys()
            .filter(|room_id| !current_rooms.contains(*room_id))
            .cloned()
            .collect();

        for room_id in departed_rooms {
            self.rtc_last_snapshots.remove(&room_id);
            self.rtc_local_memberships.remove(&room_id);
            self.rtc_livekit_services.remove(&room_id);
            if let Ok(mut renewals) = self.rtc_renewals.lock() {
                renewals.remove(&room_id);
            }

            changed_updates.push(serde_json::json!({
                "room_id": room_id,
                "memberships": [],
                "slot_available": false,
                "joined_by_me": false
            }));
        }

        serde_json::json!({ "updates": changed_updates }).to_string()
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
            ids: PusherIds::new(push_token.to_owned(), "astro.madabu_astro".to_owned()),
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
            // Remove the previous Astro pusher ID used before the Ubuntu
            // PushClient app ID was aligned with the package hook ID.
            let legacy_ids = PusherIds::new(
                push_token.to_owned(),
                "astro.madabu".to_owned(),
            );
            if let Err(error) = client.pusher().delete(legacy_ids).await {
                eprintln!("ASTRO PUSH: legacy pusher cleanup was not needed or failed: {}", error);
            }

            client
                .pusher()
                .set(pusher.into(), false)
                .await
                .map_err(|error| format!("Could not register Matrix push pusher: {error}"))?;

            // Matrix does not currently provide a default push rule for
            // MSC4075 call notifications on every homeserver. Install
            // recipient-specific rules so the homeserver forwards ring
            // events to the registered Ubuntu Push gateway.
            let own_user_id = client
                .user_id()
                .ok_or_else(|| "No authenticated Matrix user for call push rules".to_owned())?
                .to_string();
            let notification_settings = client.notification_settings().await;

            for (rule_id, event_type, legacy_format, room_mention) in [
                ("astro_rtc_ring_msc4075_v3", "org.matrix.msc4075.rtc.notification", false, false),
                ("astro_rtc_ring_stable_v3", "m.rtc.notification", false, false),
                ("astro_rtc_ring_legacy_v3", "m.call.notify", true, false),
                ("astro_rtc_ring_legacy_unstable_v3", "org.matrix.msc4075.call.notify", true, false),
                ("astro_rtc_ring_msc4075_room_v3", "org.matrix.msc4075.rtc.notification", false, true),
                ("astro_rtc_ring_stable_room_v3", "m.rtc.notification", false, true),
                ("astro_rtc_ring_legacy_room_v3", "m.call.notify", true, true),
                ("astro_rtc_ring_legacy_unstable_room_v3", "org.matrix.msc4075.call.notify", true, true),
            ] {
                let mut conditions = vec![
                    PushCondition::EventMatch(EventMatchConditionData::new(
                        "type".to_owned(),
                        event_type.to_owned(),
                    )),
                ];

                if room_mention {
                    conditions.push(PushCondition::EventPropertyIs(
                        EventPropertyIsConditionData::new(
                            r"content.m\.mentions.room".to_owned(),
                            true.into(),
                        ),
                    ));
                } else {
                    conditions.push(PushCondition::EventPropertyContains(
                        EventPropertyContainsConditionData::new(
                            r"content.m\.mentions.user_ids".to_owned(),
                            own_user_id.clone().into(),
                        ),
                    ));
                }

                if legacy_format {
                    if event_type == "m.call.notify" {
                        conditions.push(PushCondition::EventPropertyIs(
                            EventPropertyIsConditionData::new(
                                "content.application".to_owned(),
                                "m.call".into(),
                            ),
                        ));
                    }
                    conditions.push(PushCondition::EventPropertyIs(
                        EventPropertyIsConditionData::new(
                            "content.notify_type".to_owned(),
                            "ring".into(),
                        ),
                    ));
                }

                let actions = vec![
                    PushAction::Notify,
                    PushAction::SetTweak(Tweak::Sound("ring".to_owned().into())),
                ];

                match notification_settings
                    .is_push_rule_enabled(RuleKind::Override, rule_id)
                    .await
                {
                    Ok(true) => {
                        eprintln!("ASTRO PUSH: {} push rule already enabled", rule_id);
                        continue;
                    }
                    Ok(false) => {
                        match notification_settings
                            .set_push_rule_enabled(RuleKind::Override, rule_id, true)
                            .await
                        {
                            Ok(()) => {
                                eprintln!("ASTRO PUSH: re-enabled {} push rule", rule_id);
                            }
                            Err(error) => {
                                eprintln!(
                                    "ASTRO PUSH: could not re-enable {} push rule: {}",
                                    rule_id, error
                                );
                            }
                        }
                        continue;
                    }
                    Err(_) => {}
                }

                if let Err(error) = notification_settings
                    .create_custom_conditional_push_rule(
                        rule_id.to_owned(),
                        RuleKind::Override,
                        actions,
                        conditions,
                    )
                    .await
                {
                    eprintln!(
                        "ASTRO PUSH: could not install {} push rule: {}",
                        rule_id, error
                    );
                } else {
                    eprintln!("ASTRO PUSH: installed {} push rule", rule_id);
                }
            }
            Ok(())
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
            "astro.madabu_astro".to_owned(),
        );

        self.runtime.block_on(async {
            client
                .pusher()
                .delete(pusher_ids)
                .await
                .map_err(|error| format!("Could not unregister Matrix push pusher: {error}"))?;

            let legacy_ids = PusherIds::new(
                push_token.to_owned(),
                "astro.madabu".to_owned(),
            );
            if let Err(error) = client.pusher().delete(legacy_ids).await {
                eprintln!("ASTRO PUSH: legacy pusher cleanup failed: {}", error);
            }

            Ok(())
        })
    }

    pub fn client(&self) -> &Client {
        self.client.as_ref().unwrap()
    }
}

impl Drop for MatrixClientManager {
    fn drop(&mut self) {
        // Stop MatrixRTC sticky-event syncing before destroying the Matrix client.
        if let Some(task) = self.rtc_sticky_task.take() {
            task.stop();
        }

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

        self.rtc_participant_profiles.clear();
        self.rtc_last_snapshots.clear();

        drop(self.client.take());

        eprintln!("ASTRO DEBUG: Matrix client manager cleanup completed");
    }
}
