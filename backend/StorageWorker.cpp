#include "StorageWorker.h"

#include <QByteArray>
#include <QBuffer>
#include <QIODevice>
#include <QImage>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>

#include <climits>

// =============================================================
// Construction and destruction
// =============================================================

StorageWorker::StorageWorker(QObject *parent)
    : QObject(parent)
{
    m_timelinePollTimer = new QTimer(this);
    m_timelinePollTimer->setInterval(100);

    connect(
        m_timelinePollTimer,
        &QTimer::timeout,
        this,
        &StorageWorker::pollRoomTimelineUpdates
    );
}

StorageWorker::~StorageWorker()
{
    if (m_timelinePollTimer) {
        m_timelinePollTimer->stop();
    }

    // Clear the retained passphrase before destruction.
    m_storagePassphrase.fill('\0');

    // Release the Rust Matrix client if one is still active.
    if (m_matrixClient) {
        astro_matrix_close(m_matrixClient);
        m_matrixClient = nullptr;
    }
}

// =============================================================
// Encrypted storage operations
// =============================================================

void StorageWorker::openStorage(const QString &passphrase)
{
    if (m_matrixClient || !m_storagePassphrase.isEmpty()) {
        emit storageFailed(
            QStringLiteral("Encrypted storage is already open")
        );
        return;
    }

    const QString dataPath =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (dataPath.isEmpty()) {
        emit storageFailed(
            QStringLiteral(
                "Could not determine the application data directory"
            )
        );
        return;
    }

    QDir directory;

    if (!directory.mkpath(dataPath)) {
        emit storageFailed(
            QStringLiteral(
                "Could not create the application data directory"
            )
        );
        return;
    }

    const QByteArray pathBytes = dataPath.toUtf8();
    QByteArray passphraseBytes = passphrase.toUtf8();

    if (passphraseBytes.isEmpty()) {
        emit storageFailed(
            QStringLiteral("The storage passphrase must not be empty")
        );
        return;
    }

    // Open the encrypted application storage.
    char *error = nullptr;

    AstroStorageManager *storage = astro_storage_open(
        pathBytes.constData(),
        passphraseBytes.constData(),
        &error
    );

    if (!storage) {
        QString message =
            QStringLiteral("Could not open encrypted storage");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        passphraseBytes.fill('\0');

        emit storageFailed(message);
        return;
    }

    astro_storage_close(storage);

    // Attempt to restore the previously saved Matrix session.
    AstroMatrixClient *client = nullptr;
    error = nullptr;

    const int result = astro_matrix_restore(
        pathBytes.constData(),
        passphraseBytes.constData(),
        &client,
        &error
    );

    if (result < 0) {
        QString message =
            QStringLiteral("Could not restore Matrix session");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        passphraseBytes.fill('\0');

        emit storageFailed(message);
        return;
    }

    // Retain the passphrase only while storage is unlocked.
    m_storagePassphrase = passphraseBytes;
    passphraseBytes.fill('\0');

    m_matrixClient = client;
    m_matrixSessionActive = (result == 1);

    emit storageOpened();
    emit sessionRestored(result == 1);
}

void StorageWorker::closeStorage()
{
    if (m_timelinePollTimer) {
        m_timelinePollTimer->stop();
    }

    // Clear the retained encrypted-storage passphrase.
    m_storagePassphrase.fill('\0');
    m_storagePassphrase.clear();
    m_matrixSessionActive = false;

    // Release the Matrix client.
    if (m_matrixClient) {
        astro_matrix_close(m_matrixClient);
        m_matrixClient = nullptr;
    }

    emit storageClosed();
}


// =============================================================
// Delete all local data, attempting Matrix logout first.
// =============================================================

void StorageWorker::deleteAllData(const QString &path)
{
    const bool storageWasUnlocked = !m_storagePassphrase.isEmpty();
    const bool logoutAttempted = m_matrixSessionActive;

    bool logoutSucceeded = false;
    QString logoutError;

    // Attempt remote logout only when a session was restored
    // or successfully created during this storage session.
    if (logoutAttempted) {
        if (!m_matrixClient) {
            logoutError = QStringLiteral(
                "The active Matrix client is unavailable"
            );
        } else {
            char *error = nullptr;

            const int result = astro_matrix_logout(
                m_matrixClient,
                &error
            );

            if (result == 0) {
                logoutSucceeded = true;
            } else {
                logoutError = QStringLiteral(
                    "Matrix logout could not be confirmed"
                );

                if (error) {
                    logoutError = QString::fromUtf8(error);
                }
            }

            if (error) {
                astro_string_free(error);
            }
        }
    }

    // Always release the client and clear the passphrase,
    // even when the homeserver could not complete logout.
    closeStorage();

    // Only delete local files after Rust-side resources close.
    QDir directory(path);
    const bool deleted = directory.removeRecursively();

    QString deletionError;

    if (!deleted) {
        deletionError = QStringLiteral(
            "Could not delete Astro local data"
        );
    }

    emit dataDeletionFinished(
        deleted,
        storageWasUnlocked,
        logoutAttempted,
        logoutSucceeded,
        logoutError,
        deletionError
    );
}

// =============================================================
// Matrix account operations
// =============================================================

void StorageWorker::loginMatrix(
    const QString &homeserver,
    const QString &username,
    const QString &password
)
{
    if (m_storagePassphrase.isEmpty()) {
        emit storageFailed(
            QStringLiteral("Encrypted storage is not unlocked")
        );
        return;
    }

    if (m_matrixClient) {
        emit storageFailed(
            QStringLiteral("A Matrix session is already active")
        );
        return;
    }

    const QString dataPath =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (dataPath.isEmpty()) {
        emit storageFailed(
            QStringLiteral(
                "Could not determine the application data directory"
            )
        );
        return;
    }

    const QByteArray pathBytes = dataPath.toUtf8();
    const QByteArray homeserverBytes = homeserver.toUtf8();
    const QByteArray usernameBytes = username.toUtf8();

    QByteArray passwordBytes = password.toUtf8();

    if (homeserverBytes.isEmpty() ||
        usernameBytes.isEmpty() ||
        passwordBytes.isEmpty()) {

        passwordBytes.fill('\0');

        emit storageFailed(
            QStringLiteral(
                "Enter your homeserver, username and password"
            )
        );
        return;
    }

    // Authenticate through the Rust Matrix backend.
    char *error = nullptr;

    AstroMatrixClient *client = astro_matrix_login(
        pathBytes.constData(),
        m_storagePassphrase.constData(),
        homeserverBytes.constData(),
        usernameBytes.constData(),
        passwordBytes.constData(),
        &error
    );

    passwordBytes.fill('\0');

    if (!client) {
        QString message =
            QStringLiteral("Matrix login failed");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    m_matrixClient = client;
    m_matrixSessionActive = true;

    emit matrixLoginSucceeded();
}

void StorageWorker::logoutMatrix()
{
    if (!m_matrixClient) {
        emit storageFailed(
            QStringLiteral("No Matrix session is active")
        );
        return;
    }

    char *error = nullptr;

    const int result = astro_matrix_logout(
        m_matrixClient,
        &error
    );

    if (result != 0) {
        QString message =
            QStringLiteral("Matrix logout failed");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    // Stop polling before releasing the logged-out client.
    if (m_timelinePollTimer) {
        m_timelinePollTimer->stop();
    }

    // Release the client after a successful logout.
    astro_matrix_close(m_matrixClient);
    m_matrixClient = nullptr;
    m_matrixSessionActive = false;

    emit matrixLogoutSucceeded();
}

// =============================================================
// Matrix room and Space retrieval
// =============================================================

void StorageWorker::fetchMatrixRooms()
{
    if (!m_matrixClient) {
        emit storageFailed(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    // Retrieve cached rooms without waiting for a network sync.
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

        emit storageFailed(message);
        return;
    }

    const QString result = QString::fromUtf8(json);

    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    // Backend forwards this to QML before scheduling the network sync.
    emit matrixRoomsUpdated(result, false);
}

void StorageWorker::syncMatrixRooms()
{
    if (!m_matrixClient) {
        emit storageFailed(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    // Perform the network sync separately from cached room retrieval.
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

        emit storageFailed(message);
        return;
    }

    const QString result = QString::fromUtf8(json);

    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixRoomsUpdated(result, true);
}

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

    // Begin polling after the first timeline subscription is established.
    if (m_timelinePollTimer && !m_timelinePollTimer->isActive()) {
        m_timelinePollTimer->start();
    }

    emit matrixTimelineUpdated(roomId, result, QString());
}

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

    // Avoid sending an empty update every timer interval.
    if (result != QStringLiteral("{\"updates\":[]}")) {
        emit matrixLiveTimelineUpdated(result);
    }
}

// =============================================================
// Matrix message sending
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

// =============================================================
// Matrix media and room avatar retrieval
// =============================================================

void StorageWorker::fetchRoomAvatar(
    const QString &roomId,
    const QString &mxcUri,
    quint32 width,
    quint32 height
)
{
    if (!m_matrixClient) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty() || mxcUri.isEmpty()) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Room ID or avatar URI is empty")
        );
        return;
    }

    if (width == 0 || height == 0 ||
        width > 1024 || height > 1024) {

        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Invalid avatar dimensions")
        );
        return;
    }

    const QByteArray uriBytes = mxcUri.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    // Download the thumbnail through the Rust Matrix SDK.
    const int result = astro_matrix_thumbnail(
        m_matrixClient,
        uriBytes.constData(),
        width,
        height,
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download room avatar");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit roomAvatarFailed(roomId, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    // Protect against oversized buffers before converting to QByteArray.
    if (len > static_cast<uintptr_t>(INT_MAX)) {
        astro_media_bytes_free(data, len);

        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Room avatar is too large")
        );
        return;
    }

    // Copy the Rust-owned bytes before freeing the Rust buffer.
    const QByteArray encoded(
        reinterpret_cast<const char *>(data),
        static_cast<int>(len)
    );

    astro_media_bytes_free(data, len);

    // Decode the downloaded image.
    QImage image;

    if (!image.loadFromData(encoded)) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral(
                "Downloaded room avatar has an unsupported image format"
            )
        );
        return;
    }

    // Convert the image to PNG for Qt Quick.
    QByteArray pngData;

    QBuffer buffer(&pngData);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {

        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Could not convert room avatar to PNG")
        );
        return;
    }

    // Send a data URL to QML.
    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit roomAvatarRetrieved(roomId, dataUrl);
}