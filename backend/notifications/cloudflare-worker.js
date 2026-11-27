const MATRIX_PATH = "/_matrix/push/v1/notify";
const UBUNTU_PUSH_URL = "https://push.ubuntu.com/notify";
const ASTRO_APP_ID = "astro.madabu_astro";
const DEFAULT_MESSAGE_TTL_SECONDS = 10 * 60;
const CALL_DEDUP_TTL_SECONDS = 10 * 60;

function jsonResponse(value, status = 200) {
    return new Response(JSON.stringify(value), {
        status,
        headers: { "content-type": "application/json; charset=utf-8" }
    });
}

function safeString(value, fallback = "") {
    return typeof value === "string" && value.length > 0 ? value : fallback;
}

function encodeQuery(values) {
    return Object.keys(values)
        .filter((key) => values[key] !== undefined && values[key] !== null && values[key] !== "")
        .map((key) => encodeURIComponent(key) + "=" + encodeURIComponent(String(values[key])))
        .join("&");
}

function getCallDetails(notification) {
    const type = notification.type || "";
    const content = notification.content || {};
    const isRingType = type === "org.matrix.msc4075.rtc.notification"
        || type === "m.rtc.notification";
    if (!isRingType || content.notification_type !== "ring") {
        return null;
    }

    const lifetime = Number(content.lifetime);
    const senderTs = Number(content.sender_ts);
    const now = Date.now();
    const lifetimeMs = Number.isFinite(lifetime) && lifetime > 0
        ? Math.min(lifetime, 120000)
        : 30000;
    // sender_ts is only authoritative when it is close to the homeserver's
    // receive time. The Push Gateway payload does not include origin_server_ts.
    const baseTs = Number.isFinite(senderTs) && senderTs > 0
        && Math.abs(senderTs - now) <= 20000 ? senderTs : now;
    const expiresAtMs = baseTs + lifetimeMs;

    return {
        callIntent: content["m.call.intent"] || content.call_intent || "unknown",
        expiresAtMs
    };
}

function buildPushData(notification, isCall, call, privacyLevel) {
    const eventId = safeString(notification.event_id);
    const roomId = safeString(notification.room_id);
    const actualSender = safeString(
        notification.sender_display_name,
        safeString(notification.sender, "Someone")
    );
    const sender = privacyLevel === "private" ? "Someone" : actualSender;
    const roomName = safeString(notification.room_name);
    const content = notification.content || {};
    const tag = (isCall ? "astro-call-" : "astro-message-") + eventId;

    let summary;
    let body;
    let message;

    if (isCall) {
        const intent = call.callIntent === "video" ? "video"
            : call.callIntent === "audio" ? "voice" : "";
        summary = "Incoming " + (intent ? intent + " call" : "call");
        body = privacyLevel === "private"
            ? "Someone is calling you"
            : (intent ? intent.charAt(0).toUpperCase() + intent.slice(1) + " call from " + sender
                : "Call from " + sender);
        message = {
            kind: "incoming_call",
            room_id: roomId,
            event_id: eventId,
            sender,
            room_name: roomName,
            call_intent: call.callIntent,
            expires_at_ms: call.expiresAtMs,
            tag
        };
    } else {
        const fullBody = safeString(content.body, "");
        summary = privacyLevel === "private"
            ? "Astro"
            : sender;
        body = privacyLevel === "full_message"
            ? fullBody || "New message"
            : privacyLevel === "sender_room"
                ? roomName || "New message"
                : "You have a new message";
        message = {
            kind: "message",
            room_id: roomId,
            event_id: eventId,
            sender,
            room_name: roomName,
            body
        };
    }

    const url = "astro://open?" + encodeQuery(isCall ? {
        kind: "call",
        room_id: roomId,
        event_id: eventId,
        sender,
        call_intent: call.callIntent,
        expires_at_ms: call.expiresAtMs
    } : {
        room_id: roomId,
        event_id: eventId
    });

    return {
        tag,
        message,
        notification: {
            tag,
            sound: isCall ? true : false,
            card: {
                summary,
                body,
                popup: true,
                persist: isCall,
                timestamp: Math.floor(Date.now() / 1000),
                actions: [url]
            }
        }
    };
}

async function isDuplicate(env, key) {
    if (env.ASTRO_PUSH_DEDUP) {
        return (await env.ASTRO_PUSH_DEDUP.get(key)) !== null;
    }

    // Fallback only for deployments where the KV binding has not yet been
    // configured. Configure ASTRO_PUSH_DEDUP for durable retry suppression.
    return false;
}

async function rememberDelivery(env, key) {
    if (env.ASTRO_PUSH_DEDUP) {
        await env.ASTRO_PUSH_DEDUP.put(key, "1", {
            expirationTtl: CALL_DEDUP_TTL_SECONDS
        });
    }
}

async function forwardToUbuntu(device, data, expiresAt) {
    const payload = {
        appid: device.app_id,
        token: device.pushkey,
        expire_on: new Date(expiresAt).toISOString(),
        replace_tag: data.notification.tag,
        data
    };

    const response = await fetch(UBUNTU_PUSH_URL, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(payload)
    });

    const responseText = await response.text();
    if (!response.ok) {
        throw new Error("Ubuntu Push returned HTTP " + response.status + ": "
            + responseText.slice(0, 500));
    }
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);

        if (request.method === "GET" && url.pathname === "/") {
            return jsonResponse({
                service: "Astro Push Gateway",
                status: "online",
                matrix_endpoint: MATRIX_PATH,
                ubuntu_push_endpoint: UBUNTU_PUSH_URL
            });
        }

        if (request.method !== "POST" || url.pathname !== MATRIX_PATH) {
            return jsonResponse({ errcode: "M_UNRECOGNIZED", error: "Unknown endpoint" }, 404);
        }

        let body;
        try {
            body = await request.json();
        } catch (_) {
            return jsonResponse({ errcode: "M_BAD_JSON", error: "Invalid JSON body" }, 400);
        }

        const notification = body && body.notification;
        if (!notification || !Array.isArray(notification.devices)) {
            return jsonResponse({ errcode: "M_BAD_JSON", error: "Missing notification.devices array" }, 400);
        }

        const eventId = safeString(notification.event_id);
        const roomId = safeString(notification.room_id);
        const call = getCallDetails(notification);
        const isCall = call !== null;

        if (!eventId || !roomId) {
            return jsonResponse({ rejected: [] });
        }

        if (isCall && call.expiresAtMs <= Date.now()) {
            return jsonResponse({ rejected: [] });
        }

        const rejected = [];
        const deliveries = notification.devices
            .filter((device) => device
                && device.app_id === ASTRO_APP_ID
                && safeString(device.pushkey))
            .map(async (device) => {
                const privacyLevel = safeString(
                    device.data && device.data.privacy_level,
                    "sender_room"
                );
                const data = buildPushData(notification, isCall, call, privacyLevel);
                const dedupKey = await crypto.subtle.digest(
                    "SHA-256",
                    new TextEncoder().encode(device.pushkey + "|" + eventId)
                ).then((hash) => Array.from(new Uint8Array(hash))
                    .map((byte) => byte.toString(16).padStart(2, "0")).join(""));
                const key = "push:" + dedupKey;

                if (await isDuplicate(env, key)) {
                    return;
                }

                const expiresAt = isCall
                    ? call.expiresAtMs
                    : Date.now() + DEFAULT_MESSAGE_TTL_SECONDS * 1000;

                try {
                    await forwardToUbuntu(device, data, expiresAt);
                    await rememberDelivery(env, key);
                } catch (error) {
                    console.error("ASTRO PUSH: delivery failed", error);
                    // A transient upstream error must be retried by the
                    // homeserver; do not incorrectly reject a valid token.
                    throw error;
                }
            });

        try {
            await Promise.all(deliveries);
        } catch (_) {
            return jsonResponse({
                errcode: "M_PUSH_GATEWAY_ERROR",
                error: "Could not forward one or more notifications to Ubuntu Push"
            }, 502);
        }

        return jsonResponse({ rejected });
    }
};
