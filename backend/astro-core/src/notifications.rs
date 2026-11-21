use matrix_sdk::{
    ruma::events::room::message::{MessageType, OriginalSyncRoomMessageEvent},
    Client, Room,
};
use serde_json::Value;
use std::sync::{Arc, Mutex};

/// A message received from Matrix that may produce a user notification.
#[derive(Debug)]
struct NotificationEvent {
    room_id: String,
    event_id: String,
    sender: String,
    body: String,
}

/// Thread-safe queue shared by the Matrix sync handler and polling code.
#[derive(Clone, Default)]
pub struct NotificationQueue {
    events: Arc<Mutex<Vec<NotificationEvent>>>,
}

impl NotificationQueue {
    fn push(&self, event: NotificationEvent) {
        if let Ok(mut events) = self.events.lock() {
            events.push(event);
        }
    }

    /// Take all pending events and convert them to JSON for the existing FFI.
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
