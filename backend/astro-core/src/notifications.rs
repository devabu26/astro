use matrix_sdk::{
    ruma::events::{
        room::message::{MessageType, OriginalSyncRoomMessageEvent},
        AnySyncMessageLikeEvent, SyncMessageLikeEvent,
        rtc::notification::{CallIntent, NotificationType},
    },
    Client, Room,
};
use serde_json::Value;
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

/// A message received from Matrix that may produce a user notification.
#[derive(Debug)]
struct NotificationEvent {
    room_id: String,
    event_id: String,
    sender: String,
    body: String,
}

/// A live MatrixRTC ring addressed to this account.
#[derive(Debug)]
struct IncomingCallEvent {
    room_id: String,
    event_id: String,
    sender: String,
    call_intent: String,
}

/// Thread-safe queues shared by Matrix sync handlers and polling code.
#[derive(Clone, Default)]
pub struct NotificationQueue {
    events: Arc<Mutex<Vec<NotificationEvent>>>,
    incoming_calls: Arc<Mutex<Vec<IncomingCallEvent>>>,
}

impl NotificationQueue {
    fn push(&self, event: NotificationEvent) {
        if let Ok(mut events) = self.events.lock() {
            events.push(event);
        }
    }

    fn push_incoming_call(&self, event: IncomingCallEvent) {
        if let Ok(mut events) = self.incoming_calls.lock() {
            events.push(event);
        }
    }

    /// Take pending message notifications and convert them to JSON.
    pub fn drain_json(&self) -> Vec<Value> {
        self.events
            .lock()
            .map(|mut events| {
                std::mem::take(&mut *events)
                    .into_iter()
                    .map(|event| {
                        serde_json::json!({
                            "room_id": event.room_id,
                            "event_id": event.event_id,
                            "sender": event.sender,
                            "body": event.body
                        })
                    })
                    .collect()
            })
            .unwrap_or_default()
    }

    /// Take pending incoming-call notifications and convert them to JSON.
    pub fn drain_incoming_calls_json(&self) -> Vec<Value> {
        self.incoming_calls
            .lock()
            .map(|mut events| {
                std::mem::take(&mut *events)
                    .into_iter()
                    .map(|event| {
                        serde_json::json!({
                            "room_id": event.room_id,
                            "event_id": event.event_id,
                            "sender": event.sender,
                            "call_intent": event.call_intent
                        })
                    })
                    .collect()
            })
            .unwrap_or_default()
    }
}

/// Register a global Matrix sync handler. This does not depend on an open
/// room timeline, so messages in other joined rooms are also collected.
pub fn register_notification_handler(client: &Client, queue: NotificationQueue) {
    client.add_event_handler(
        move |event: OriginalSyncRoomMessageEvent, room: Room, client: Client| {
            let queue = queue.clone();

            async move {
                // Ignore messages sent by the currently logged-in account.
                let sender = event.sender.to_string();
                let own_user_id = client.user_id().map(|id| id.to_string());

                if own_user_id.as_deref() == Some(sender.as_str()) {
                    return;
                }

                // Verification requests are handled by Astro's verification
                // subsystem, not the message notification subsystem.
                if matches!(&event.content.msgtype, MessageType::VerificationRequest(_)) {
                    return;
                }

                queue.push(NotificationEvent {
                    room_id: room.room_id().to_string(),
                    event_id: event.event_id.to_string(),
                    sender,
                    body: event.content.body().to_owned(),
                });
            }
        },
    );
}

/// Register a global handler for MSC4075 incoming-call notifications.
/// Only live ring events explicitly mentioning this account are surfaced.
pub fn register_incoming_call_handler(client: &Client, queue: NotificationQueue) {
    client.add_event_handler(
        // Use the catch-all message-like event type and filter by variant.
        // matrix-sdk 0.19.1's typed handler dispatch matches the canonical
        // event type string exactly, so a typed m.rtc.notification handler
        // does not receive the MSC4075 unstable org.matrix.msc4075.* alias.
        // AnySyncMessageLikeEvent has no single static event type and lets us
        // accept both the stable and unstable names.
        move |event: AnySyncMessageLikeEvent, room: Room, client: Client| {
            let queue = queue.clone();

            async move {
                let AnySyncMessageLikeEvent::RtcNotification(
                    SyncMessageLikeEvent::Original(event)
                ) = event else {
                    return;
                };

                if event.content.notification_type != NotificationType::Ring {
                    return;
                }

                let Some(own_user_id) = client.user_id() else {
                    return;
                };

                if event.sender == own_user_id {
                    return;
                }

                let addressed_to_user = event.content.mentions.as_ref().is_some_and(
                    |mentions| {
                        mentions.room
                            || mentions.user_ids.iter().any(|user_id| user_id == &own_user_id)
                    },
                );

                if !addressed_to_user {
                    return;
                }

                // MSC4075 uses sender_ts unless it differs from the homeserver
                // timestamp by more than 20 seconds. Ignore expired rings.
                let expires_at_ms: u64 = event
                    .content
                    .expiration_ts(event.origin_server_ts, None)
                    .get()
                    .into();
                let now_ms = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_millis() as u64;

                if expires_at_ms <= now_ms {
                    return;
                }

                let call_intent = match event.content.call_intent.as_ref() {
                    Some(CallIntent::Audio) => "audio",
                    Some(CallIntent::Video) => "video",
                    Some(_) | None => "unknown",
                };

                queue.push_incoming_call(IncomingCallEvent {
                    room_id: room.room_id().to_string(),
                    event_id: event.event_id.to_string(),
                    sender: event.sender.to_string(),
                    call_intent: call_intent.to_owned(),
                });
            }
        },
    );
}
