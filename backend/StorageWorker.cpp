#include "StorageWorker.h"

#include <QByteArray>
#include <QBuffer>
#include <QIODevice>
#include <QImage>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QUrl>
#include <limits>

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

    // Start unread-count polling for restored Matrix sessions.
    if (m_timelinePollTimer) {
        if (m_matrixSessionActive)
            m_timelinePollTimer->start();
        else
            m_timelinePollTimer->stop();
    }

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

    // Start unread-count polling immediately after login.
    if (m_timelinePollTimer)
        m_timelinePollTimer->start();

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
// Matrix device verification
// =============================================================

void StorageWorker::runVerificationAction(
    const QString &userId,
    const QString &flowId,
    const QString &actionName,
    VerificationAction action
)
{
    if (!m_matrixClient) {
        emit matrixVerificationActionFinished(
            userId, flowId, actionName,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, actionName,
            QStringLiteral("User ID or verification flow ID is empty")
        );
        return;
    }

    const QByteArray userIdBytes = userId.toUtf8();
    const QByteArray flowIdBytes = flowId.toUtf8();

    char *error = nullptr;

    const int32_t result = action(
        m_matrixClient,
        userIdBytes.constData(),
        flowIdBytes.constData(),
        &error
    );

    QString errorMessage;

    if (result != 1) {
        errorMessage = error
            ? QString::fromUtf8(error)
            : QStringLiteral("Matrix verification action failed");
    }

    if (error) {
        astro_string_free(error);
    }

    emit matrixVerificationActionFinished(
        userId,
        flowId,
        actionName,
        errorMessage
    );
}

void StorageWorker::acceptVerificationRequest(
    const QString &userId,
    const QString &flowId
)
{
    runVerificationAction(
        userId, flowId, QStringLiteral("accept"),
        astro_matrix_accept_verification_request
    );
}

void StorageWorker::cancelVerificationRequest(
    const QString &userId,
    const QString &flowId
)
{
    runVerificationAction(
        userId, flowId, QStringLiteral("cancel"),
        astro_matrix_cancel_verification_request
    );
}

void StorageWorker::confirmVerification(
    const QString &userId,
    const QString &flowId
)
{
    runVerificationAction(
        userId, flowId, QStringLiteral("confirm"),
        astro_matrix_confirm_verification
    );
}

void StorageWorker::mismatchVerification(
    const QString &userId,
    const QString &flowId
)
{
    runVerificationAction(
        userId, flowId, QStringLiteral("mismatch"),
        astro_matrix_mismatch_verification
    );
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

void StorageWorker::fetchSenderAvatar(
    const QString &requestKey,
    const QString &mxcUri,
    quint32 width,
    quint32 height
)
{
    if (!m_matrixClient) {
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (requestKey.isEmpty() || mxcUri.isEmpty()) {
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Request key or avatar URI is empty")
        );
        return;
    }

    if (width == 0 || height == 0 ||
        width > 1024 || height > 1024) {

        emit senderAvatarFailed(
            requestKey,
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
            QStringLiteral("Could not download sender avatar");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit senderAvatarFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    // Protect against oversized buffers before converting to QByteArray.
    if (len > static_cast<uintptr_t>(INT_MAX)) {
        astro_media_bytes_free(data, len);

        emit senderAvatarFailed(
            requestKey,
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
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral(
                "Downloaded sender avatar has an unsupported image format"
            )
        );
        return;
    }

    // Convert the image to PNG for Qt Quick.
    QByteArray pngData;

    QBuffer buffer(&pngData);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {

        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Could not convert sender avatar to PNG")
        );
        return;
    }

    // Send a data URL to QML.
    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit senderAvatarRetrieved(requestKey, dataUrl);
}

void StorageWorker::fetchMessageImage(
    const QString &requestKey,
    const QString &mediaSourceJson
)
{
    if (!m_matrixClient) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    const QByteArray sourceBytes = mediaSourceJson.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    const int result = astro_matrix_image(
        m_matrixClient,
        sourceBytes.constData(),
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download message image");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageImageFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(INT_MAX)) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageImageFailed(
            requestKey,
            QStringLiteral("Downloaded image is empty or too large")
        );
        return;
    }

    const QByteArray encoded(
        reinterpret_cast<const char *>(data),
        static_cast<int>(len)
    );

    astro_media_bytes_free(data, len);

    // Preserve animated GIF data instead of flattening it to PNG.
    if (encoded.startsWith("GIF87a") ||
        encoded.startsWith("GIF89a")) {
        const QString dataUrl =
            QStringLiteral("data:image/gif;base64,") +
            QString::fromLatin1(encoded.toBase64());

        emit messageImageRetrieved(requestKey, dataUrl);
        return;
    }

    QImage image;

    if (!image.loadFromData(encoded)) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral("Downloaded message image has an unsupported format")
        );
        return;
    }

    // Limit large images before passing them to Qt Quick.
    if (image.width() > 1600 || image.height() > 1600) {
        image = image.scaled(
            QSize(1600, 1600),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        );
    }

    QByteArray pngData;
    QBuffer buffer(&pngData);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {

        emit messageImageFailed(
            requestKey,
            QStringLiteral("Could not convert message image to PNG")
        );
        return;
    }

    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit messageImageRetrieved(requestKey, dataUrl);
}


void StorageWorker::fetchMessageThumbnail(
    const QString &requestKey,
    const QString &mediaSourceJson,
    quint32 width,
    quint32 height
)
{
    if (!m_matrixClient) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    if (width == 0 || height == 0 || width > 2048 || height > 2048) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Thumbnail dimensions are invalid")
        );
        return;
    }

    const QByteArray sourceBytes = mediaSourceJson.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    const int result = astro_matrix_media_thumbnail(
        m_matrixClient,
        sourceBytes.constData(),
        width,
        height,
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download video thumbnail");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageThumbnailFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(INT_MAX)) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Downloaded thumbnail is empty or too large")
        );
        return;
    }

    const QByteArray encoded(
        reinterpret_cast<const char *>(data),
        static_cast<int>(len)
    );

    astro_media_bytes_free(data, len);

    QImage image;

    if (!image.loadFromData(encoded)) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Downloaded video thumbnail has an unsupported format")
        );
        return;
    }

    if (image.width() > 1024 || image.height() > 1024) {
        image = image.scaled(
            QSize(1024, 1024),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        );
    }

    QByteArray pngData;
    QBuffer buffer(&pngData);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Could not encode video thumbnail")
        );
        return;
    }

    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit messageThumbnailRetrieved(requestKey, dataUrl);
}


void StorageWorker::fetchMessageVideo(
    const QString &requestKey,
    const QString &mediaSourceJson,
    const QString &mimeType
)
{
    if (!m_matrixClient) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    const QString dataPath =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (dataPath.isEmpty()) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not determine the application data directory")
        );
        return;
    }

    QDir directory;
    const QString cachePath =
        directory.filePath(dataPath + QStringLiteral("/video-cache"));

    if (!directory.mkpath(cachePath)) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not create the video cache directory")
        );
        return;
    }

    QMimeDatabase mimeDatabase;
    QString suffix =
        mimeDatabase.mimeTypeForName(mimeType).preferredSuffix();

    if (suffix.isEmpty() || suffix == QStringLiteral("bin")) {
        suffix = QStringLiteral("mp4");
    }

    const QByteArray keyHash =
        QCryptographicHash::hash(
            requestKey.toUtf8(),
            QCryptographicHash::Sha256
        ).toHex();

    const QString filePath =
        QDir(cachePath).filePath(
            QString::fromLatin1(keyHash) +
            QStringLiteral(".") + suffix
        );

    const QFileInfo cachedFile(filePath);

    if (cachedFile.exists() && cachedFile.size() > 0) {
        emit messageVideoRetrieved(
            requestKey,
            QUrl::fromLocalFile(filePath).toString()
        );
        return;
    }

    const QByteArray sourceBytes = mediaSourceJson.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    const int result = astro_matrix_video(
        m_matrixClient,
        sourceBytes.constData(),
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download message video");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageVideoFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(
            std::numeric_limits<qint64>::max()
        )) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Downloaded video is empty or too large")
        );
        return;
    }

    QSaveFile output(filePath);

    if (!output.open(QIODevice::WriteOnly)) {
        astro_media_bytes_free(data, len);

        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not open video cache file: ") +
            output.errorString()
        );
        return;
    }

    const qint64 written = output.write(
        reinterpret_cast<const char *>(data),
        static_cast<qint64>(len)
    );

    astro_media_bytes_free(data, len);

    if (written != static_cast<qint64>(len)) {
        output.cancelWriting();

        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not write the complete video to cache")
        );
        return;
    }

    if (!output.commit()) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not save cached video: ") +
            output.errorString()
        );
        return;
    }

    emit messageVideoRetrieved(
        requestKey,
        QUrl::fromLocalFile(filePath).toString()
    );
}

