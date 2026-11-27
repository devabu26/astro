const MATRIX_PATH = "/_matrix/push/v1/notify";
const UBUNTU_PUSH_URL = "https://push.ubports.com/notify";
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
    const isMsc4075 = type === "org.matrix.msc4075.rtc.notification"
        || type === "m.rtc.notification";
    const isLegacyCallNotify = type === "m.call.notify"
        || type === "org.matrix.msc4075.call.notify";

    // notification_type is deprecated and may be omitted by newer MSC4075
    // senders. When present, only "ring" should create a persistent call card.
    if (isMsc4075) {
        if (content.notification_type !== undefined
            && content.notification_type !== "ring") {
            return null;
        }
    } else if (isLegacyCallNotify) {
        if (content.application !== "m.call" || content.notify_type !== "ring") {
            return null;
        }
    } else {
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


async function saveDiagnostic(env, value) {
    if (!env.ASTRO_PUSH_DEDUP) return false;
    await env.ASTRO_PUSH_DEDUP.put("diagnostics:last", JSON.stringify(value), { expirationTtl: 86400 });
    return true;
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
        // Keep only a short upstream diagnostic. Never log or return the
        // request payload, push token, or notification content.
        const detail = responseText.replace(/[\\r\\n\\t]+/g, " ").slice(0, 240);
        throw new Error("Ubuntu Push HTTP " + response.status
            + (detail ? ": " + detail : ""));
    }
    return response.status;
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);

        if (request.method === "GET" && url.pathname === "/__diagnostics") {
            if (!env.ASTRO_PUSH_DEDUP) return jsonResponse({ status: "unavailable", reason: "ASTRO_PUSH_DEDUP KV binding is not configured" }, 503);
            try {
                const last = await env.ASTRO_PUSH_DEDUP.get("diagnostics:last");
                return jsonResponse(last ? JSON.parse(last) : { status: "no_diagnostics_yet", message: "Send a fresh Matrix message to generate a diagnostic." });
            } catch (_) {
                return jsonResponse({ status: "unavailable", reason: "Could not read diagnostic from KV" }, 503);
            }
        }

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
        const matchingDevices = notification.devices.filter((device) => device
            && device.app_id === ASTRO_APP_ID
            && safeString(device.pushkey));

        console.info("ASTRO PUSH: request received", {
            deviceCount: notification.devices.length,
            matchingDeviceCount: matchingDevices.length,
            notificationKind: isCall ? "call" : "message",
            eventType: safeString(notification.type, "unknown"),
            hasNotificationType: Boolean(notification.content && notification.content.notification_type),
            hasEventId: Boolean(eventId),
            hasRoomId: Boolean(roomId)
        });

        if (!eventId || !roomId) {
            console.warn("ASTRO PUSH: skipped request because event or room ID is missing");
            return jsonResponse({ rejected: [] });
        }

        if (isCall && call.expiresAtMs <= Date.now()) {
            console.info("ASTRO PUSH: skipped expired call notification");
            return jsonResponse({ rejected: [] });
        }

        if (matchingDevices.length === 0) {
            console.warn("ASTRO PUSH: no devices matched app ID " + ASTRO_APP_ID);
        }

        let duplicateCount = 0;
        let forwardedCount = 0;
        const upstreamStatuses = [];
        const rejected = [];
        const deliveries = matchingDevices.map(async (device) => {
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
                    duplicateCount += 1;
                    console.info("ASTRO PUSH: skipped duplicate delivery");
                    return;
                }

                const expiresAt = isCall
                    ? call.expiresAtMs
                    : Date.now() + DEFAULT_MESSAGE_TTL_SECONDS * 1000;

                let stage = "ubuntu_push";
                try {
                    const upstreamStatus = await forwardToUbuntu(device, data, expiresAt);
                    stage = "dedup_kv";
                    await rememberDelivery(env, key);
                    forwardedCount += 1;
                    upstreamStatuses.push(upstreamStatus);
                    console.info("ASTRO PUSH: forwarded notification to Ubuntu Push", {
                        upstreamStatus
                    });
                } catch (error) {
                    const reason = error instanceof Error ? error.message : "Unknown error";
                    console.error("ASTRO PUSH: delivery failed", {
                        stage,
                        reason: reason.slice(0, 320)
                    });
                    // A transient upstream/KV error must be retried by the
                    // homeserver; do not incorrectly reject a valid token.
                    throw new Error(stage + ": " + reason.slice(0, 320));
                }
            });

        try {
            await Promise.all(deliveries);
        } catch (error) {
            const reason = error instanceof Error ? error.message : "Unknown delivery error";
            const diagnostic = {
                status: "failed",
                notificationKind: isCall ? "call" : "message",
                eventType: safeString(notification.type, "unknown"),
                stage: reason.split(":")[0],
                matchingDeviceCount: matchingDevices.length,
                forwardedCount,
                duplicateCount,
                reason: reason.slice(0, 360),
                timestamp: new Date().toISOString()
            };
            console.error("ASTRO PUSH: request failed", diagnostic);
            try { await saveDiagnostic(env, diagnostic); } catch (_) {}
            return jsonResponse({
                errcode: "M_PUSH_GATEWAY_ERROR",
                error: "Push delivery failed",
                diagnostic: reason.slice(0, 360)
            }, 502);
        }

        const diagnostic = {
            status: "success",
            notificationKind: isCall ? "call" : "message",
            eventType: safeString(notification.type, "unknown"),
            matchingDeviceCount: matchingDevices.length,
            forwardedCount,
            duplicateCount,
            upstreamStatuses,
            timestamp: new Date().toISOString()
        };
        console.info("ASTRO PUSH: request completed", diagnostic);
        try { await saveDiagnostic(env, diagnostic); } catch (_) {}
        return jsonResponse({ rejected });
    }
};
