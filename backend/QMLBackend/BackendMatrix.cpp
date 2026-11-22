

#include "Backend.h"
#include "StorageWorker/StorageWorker.h"
#include "notifications/NotificationManager.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QUuid>
#include <QMap>
#include <QVariantMap>
#include <QList>
#include <QThread>
#include <QMetaObject>
#include <functional>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusArgument>
#include <QDBusMetaType>

// =============================================================


void Backend::setPushToken(const QString &token)
{
    if (token.isEmpty()) {
        return;
    }

    emit requestSetPushToken(token);
}


void Backend::setPushNotificationsEnabled(bool enabled)
{
    emit requestSetPushNotificationsEnabled(enabled);
}


void Backend::setPushPrivacyLevel(const QString &privacyLevel)
{
    if (privacyLevel != "sender_room"
        && privacyLevel != "full_message"
        && privacyLevel != "private") {
        qWarning() << "ASTRO: Invalid notification privacy level";
        return;
    }

    emit requestSetPushPrivacyLevel(privacyLevel);
}


void Backend::loginMatrix(
    const QString &homeserver,
    const QString &username,
    const QString &password
)
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "Another operation is already in progress"
            )
        );

        return;
    }

    if (!m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Unlock encrypted storage first")
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Connecting to Matrix...")
    );

    emit requestLoginMatrix(
        homeserver,
        username,
        password
    );
}

// =============================================================
// Matrix room and Space retrieval
// =============================================================


void Backend::refreshMatrixDevices()
{
    if (!m_storageReady) {
        emit matrixDevicesRetrieved(
            QString(),
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    emit requestFetchMatrixDevices();
}


bool Backend::refreshMatrixRooms()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "Another operation is already in progress"
            )
        );

        return false;
    }

    if (!m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Unlock encrypted storage first")
        );

        return false;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Retrieving Matrix rooms...")
    );

    emit requestFetchMatrixRooms();
    return true;
}

void Backend::resumeMatrixSync()
{
    if (!m_storageReady || m_matrixResumeInProgress)
        return;

    m_matrixResumeInProgress = true;
    emit requestResumeMatrixSync();
}

// =============================================================
// Matrix media and room avatar requests
// =============================================================


void Backend::requestDeviceVerification(const QString &deviceId)
{
    if (!m_storageReady || deviceId.isEmpty()) {
        emit matrixDeviceVerificationFinished(
            deviceId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Device ID is empty")
        );
        return;
    }

    emit requestStartDeviceVerification(deviceId);
}


void Backend::renameMatrixDevice(
    const QString &deviceId,
    const QString &displayName
)
{
    if (!m_storageReady || deviceId.isEmpty() || displayName.trimmed().isEmpty()) {
        emit matrixDeviceRenameFinished(
            deviceId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : deviceId.isEmpty()
                    ? QStringLiteral("Device ID is empty")
                    : QStringLiteral("Device name cannot be empty")
        );
        return;
    }

    emit requestRenameMatrixDevice(deviceId, displayName.trimmed());
}


void Backend::checkMatrixDeviceManagementUrl(
    const QString &deviceId
) {
    if (!m_storageReady || deviceId.isEmpty()) {
        emit matrixDeviceManagementUrlReady(
            deviceId,
            QString(),
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Device ID is empty")
        );
        return;
    }

    emit requestMatrixDeviceManagementUrl(deviceId);
}


void Backend::revokeMatrixDevice(
    const QString &deviceId,
    const QString &password
)
{
    if (!m_storageReady || deviceId.isEmpty() || password.isEmpty()) {
        emit matrixDeviceRevocationFinished(
            deviceId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : deviceId.isEmpty()
                    ? QStringLiteral("Device ID is empty")
                    : QStringLiteral("Account password is required")
        );
        return;
    }

    emit requestRevokeMatrixDevice(deviceId, password);
}


void Backend::requestRoomTimeline(const QString &roomId)
{
    if (!m_storageReady) {
        emit matrixTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestFetchRoomTimeline(roomId);
}

// Explicitly purge and reload one room's Matrix SDK event cache.

void Backend::requestRefreshRoomTimeline(const QString &roomId)
{
    if (!m_storageReady) {
        emit matrixTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestRefreshRoomTimelineWorker(roomId);
}

// Request another batch of older messages for an existing room timeline.

void Backend::requestOlderRoomTimeline(const QString &roomId)
{
    if (!m_storageReady) {
        emit matrixOlderTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (roomId.isEmpty()) {
        emit matrixOlderTimelineRetrieved(
            roomId,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestPaginateRoomTimeline(roomId);
}


void Backend::markRoomAsRead(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty()) {
        return;
    }

    emit requestMarkRoomAsRead(roomId);
}


void Backend::sendRoomMessage(
    const QString &roomId,
    const QString &message
)
{
    if (!m_storageReady) {
        emit matrixMessageSent(
            roomId,
            QString(),
            QStringLiteral("Encrypted storage is not open")
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

    emit requestSendRoomMessage(roomId, message);
}




void Backend::sendRoomAttachment(
    const QString &requestKey,
    const QString &roomId,
    const QString &fileUrl,
    const QString &caption
)
{
    if (!m_storageReady) {
        emit matrixAttachmentSent(
            requestKey, roomId, QString(),
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (requestKey.isEmpty() || roomId.isEmpty() || fileUrl.isEmpty()) {
        emit matrixAttachmentSent(
            requestKey, roomId, QString(),
            QStringLiteral("Attachment request is incomplete")
        );
        return;
    }

    emit requestSendRoomAttachment(
        requestKey,
        roomId,
        fileUrl,
        caption
    );
}


void Backend::toggleRoomReaction(
    const QString &roomId,
    const QString &eventId,
    const QString &reactionKey
)
{
    if (!m_storageReady) {
        emit matrixReactionToggled(
            roomId, eventId, reactionKey, false,
            QStringLiteral("Encrypted storage is not open")
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

    emit requestToggleRoomReaction(roomId, eventId, reactionKey);
}


void Backend::sendRoomReply(
    const QString &roomId,
    const QString &eventId,
    const QString &message
)
{
    if (!m_storageReady || roomId.isEmpty() ||
        eventId.isEmpty() || message.trimmed().isEmpty()) {
        emit matrixMessageActionFinished(
            roomId, eventId, QStringLiteral("reply"),
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID, event ID and message are required")
        );
        return;
    }

    emit requestSendRoomReply(roomId, eventId, message);
}


void Backend::editRoomMessage(
    const QString &roomId,
    const QString &eventId,
    const QString &message
)
{
    if (!m_storageReady || roomId.isEmpty() ||
        eventId.isEmpty() || message.trimmed().isEmpty()) {
        emit matrixMessageActionFinished(
            roomId, eventId, QStringLiteral("edit"),
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID, event ID and message are required")
        );
        return;
    }

    emit requestEditRoomMessage(roomId, eventId, message);
}


void Backend::redactRoomEvent(
    const QString &roomId,
    const QString &eventId
)
{
    if (!m_storageReady || roomId.isEmpty() || eventId.isEmpty()) {
        emit matrixMessageActionFinished(
            roomId, eventId, QStringLiteral("delete"),
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID and event ID are required")
        );
        return;
    }

    emit requestRedactRoomEvent(roomId, eventId);
}


void Backend::acceptVerificationRequest(
    const QString &userId,
    const QString &flowId
)
{
    if (!m_storageReady || userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, QStringLiteral("accept"),
            QStringLiteral("Storage is not ready or verification details are missing")
        );
        return;
    }

    emit requestAcceptVerificationRequest(userId, flowId);
}


void Backend::cancelVerificationRequest(
    const QString &userId,
    const QString &flowId
)
{
    if (!m_storageReady || userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, QStringLiteral("cancel"),
            QStringLiteral("Storage is not ready or verification details are missing")
        );
        return;
    }

    emit requestCancelVerificationRequest(userId, flowId);
}


void Backend::confirmVerification(
    const QString &userId,
    const QString &flowId
)
{
    if (!m_storageReady || userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, QStringLiteral("confirm"),
            QStringLiteral("Storage is not ready or verification details are missing")
        );
        return;
    }

    emit requestConfirmVerification(userId, flowId);
}


void Backend::mismatchVerification(
    const QString &userId,
    const QString &flowId
)
{
    if (!m_storageReady || userId.isEmpty() || flowId.isEmpty()) {
        emit matrixVerificationActionFinished(
            userId, flowId, QStringLiteral("mismatch"),
            QStringLiteral("Storage is not ready or verification details are missing")
        );
        return;
    }

    emit requestMismatchVerification(userId, flowId);
}


void Backend::logoutMatrix()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "Another operation is already in progress"
            )
        );

        return;
    }

    if (!m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Unlock encrypted storage first")
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Logging out of Matrix...")
    );

    emit requestLogoutMatrix();
}
