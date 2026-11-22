
#include "StorageWorker.h"

#include <QByteArray>
#include <QDebug>

// =============================================================
// Matrix room and Space retrieval
// =============================================================

void StorageWorker::fetchMatrixRooms()
{
    if (!m_matrixClient) {
        emit matrixRoomsFailed(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    // Return cached rooms first; Backend schedules network sync afterward.
    char *error = nullptr;

    char *json = astro_matrix_get_cached_rooms_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve cached Matrix rooms");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixRoomsFailed(message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixRoomsUpdated(result, false);
}

void StorageWorker::fetchMatrixDevices()
{
    if (!m_matrixClient) {
        emit matrixDevicesUpdated(
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    char *error = nullptr;

    char *json = astro_matrix_get_devices_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        const QString message = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Could not retrieve registered Matrix devices");

        if (error) {
            astro_string_free(error);
        }

        emit matrixDevicesUpdated(QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixDevicesUpdated(result, QString());
}

void StorageWorker::syncMatrixRooms()
{
    if (!m_matrixClient) {
        emit matrixRoomsFailed(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    // Network sync is deliberately separate from cached room retrieval.
    char *error = nullptr;

    char *json = astro_matrix_get_rooms_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not refresh Matrix rooms");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixRoomsFailed(message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixRoomsUpdated(result, true);
}

void StorageWorker::resumeMatrixSync()
{
    if (!m_matrixClient) {
        emit matrixSyncResumeFinished(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    char *error = nullptr;
    const int32_t result = astro_matrix_resume_sync(
        m_matrixClient,
        &error
    );

    QString message;
    if (error) {
        message = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result == 0 && message.isEmpty()) {
        message = QStringLiteral("Could not resume Matrix syncing");
    }

    emit matrixSyncResumeFinished(message);
}

// =============================================================
// Room timeline retrieval
// =============================================================

void StorageWorker::fetchRoomTimeline(const QString &roomId)
{
    if (!m_matrixClient) {
        emit matrixTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    char *json = astro_matrix_get_room_timeline_json(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve room timeline");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixTimelineUpdated(roomId, QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    // Polling begins once a room timeline has been loaded.
    if (m_timelinePollTimer && !m_timelinePollTimer->isActive()) {
        m_timelinePollTimer->start();
    }

    emit matrixTimelineUpdated(roomId, result, QString());
}

void StorageWorker::refreshRoomTimeline(const QString &roomId)
{
    if (!m_matrixClient) {
        emit matrixTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    char *json = astro_matrix_refresh_room_timeline_json(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not refresh room timeline");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixTimelineUpdated(roomId, QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    if (m_timelinePollTimer && !m_timelinePollTimer->isActive()) {
        m_timelinePollTimer->start();
    }

    emit matrixTimelineUpdated(roomId, result, QString());
}

void StorageWorker::paginateRoomTimeline(const QString &roomId)
{
    if (!m_matrixClient) {
        emit matrixOlderTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixOlderTimelineUpdated(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    char *json = astro_matrix_paginate_room_timeline_json(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve older room messages");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixOlderTimelineUpdated(roomId, QString(), message);
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixOlderTimelineUpdated(roomId, result, QString());
}

// =============================================================
// Live Matrix timeline updates
// =============================================================

void StorageWorker::pollRoomTimelineUpdates()
{
    if (!m_matrixClient) {
        if (m_timelinePollTimer) {
            m_timelinePollTimer->stop();
        }
        return;
    }

    char *error = nullptr;

    char *json = astro_matrix_poll_room_timeline_updates_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve live timeline updates");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        qWarning() << "ASTRO: Live timeline polling failed:" << message;
        return;
    }

    const QString result = QString::fromUtf8(json);
    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    // Suppress empty results rather than emitting them every timer tick.
    if (result != QStringLiteral("{\"updates\":[]}")) {
        emit matrixLiveTimelineUpdated(result);
    }
}

// =============================================================
// Read receipts
// =============================================================

void StorageWorker::markRoomAsRead(const QString &roomId)
{
    if (!m_matrixClient || roomId.isEmpty()) {
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    char *error = nullptr;

    const int result = astro_matrix_mark_room_as_read(
        m_matrixClient,
        roomIdBytes.constData(),
        &error
    );

    if (result < 0) {
        QString message = QStringLiteral("Could not mark room as read");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        qWarning() << "ASTRO: Read receipt failed for"
                   << roomId << ":" << message;
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (result == 1) {
        qDebug() << "ASTRO: Read receipt sent for" << roomId;
    }
}

// =============================================================
// Matrix message sending and reactions
// =============================================================

void StorageWorker::sendRoomMessage(
    const QString &roomId,
    const QString &message
)
{
    if (!m_matrixClient) {
        emit matrixMessageSent(
            roomId,
            QString(),
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixMessageSent(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    if (message.trimmed().isEmpty()) {
        emit matrixMessageSent(
            roomId,
            QString(),
            QStringLiteral("Message must not be empty")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    const QByteArray messageBytes = message.toUtf8();

    char *error = nullptr;

    char *eventId = astro_matrix_send_room_message(
        m_matrixClient,
        roomIdBytes.constData(),
        messageBytes.constData(),
        &error
    );

    if (!eventId) {
        QString errorMessage =
            QStringLiteral("Could not send Matrix message");

        if (error) {
            errorMessage = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit matrixMessageSent(roomId, QString(), errorMessage);
        return;
    }

    const QString result = QString::fromUtf8(eventId);
    astro_string_free(eventId);

    if (error) {
        astro_string_free(error);
    }

    emit matrixMessageSent(roomId, result, QString());
}

void StorageWorker::toggleRoomReaction(
    const QString &roomId,
    const QString &eventId,
    const QString &reactionKey
)
{
    if (!m_matrixClient) {
        emit matrixReactionToggled(
            roomId, eventId, reactionKey, false,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty() || eventId.isEmpty() || reactionKey.isEmpty()) {
        emit matrixReactionToggled(
            roomId, eventId, reactionKey, false,
            QStringLiteral("Room ID, event ID and reaction key are required")
        );
        return;
    }

    const QByteArray roomIdBytes = roomId.toUtf8();
    const QByteArray eventIdBytes = eventId.toUtf8();
    const QByteArray reactionKeyBytes = reactionKey.toUtf8();

    char *error = nullptr;

    const int32_t result = astro_matrix_toggle_room_reaction(
        m_matrixClient,
        roomIdBytes.constData(),
        eventIdBytes.constData(),
        reactionKeyBytes.constData(),
        &error
    );

    QString errorMessage;

    if (error) {
        errorMessage = QString::fromUtf8(error);
        astro_string_free(error);
    }

    if (result < 0) {
        if (errorMessage.isEmpty()) {
            errorMessage = QStringLiteral("Could not toggle Matrix reaction");
        }

        emit matrixReactionToggled(
            roomId, eventId, reactionKey, false, errorMessage
        );
        return;
    }

    emit matrixReactionToggled(
        roomId, eventId, reactionKey, result == 1, QString()
    );
}
