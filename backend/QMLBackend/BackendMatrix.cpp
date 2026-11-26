

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
#include <QQuickWindow>
#include <QRunnable>
#include <QPointer>
#include <QTimer>
#include <atomic>
#include <memory>
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


namespace {

class ScreenShareSceneGraphFenceJob final : public QRunnable
{
public:
    ScreenShareSceneGraphFenceJob(
        Backend *backend,
        const QString &roomId,
        const std::shared_ptr<std::atomic_bool> &completed
    )
        : m_backend(backend),
          m_roomId(roomId),
          m_completed(completed)
    {
    }

    void run() override
    {
        if (m_completed->exchange(true))
            return;

        QPointer<Backend> backend = m_backend;
        const QString roomId = m_roomId;
        qInfo() << "ASTRO_SCREEN_SHARE_DIAG: scenegraph fence reached after frame swap"
                << "room:" << roomId
                << "renderThread:" << QThread::currentThreadId();

        if (!backend)
            return;

        QMetaObject::invokeMethod(
            backend.data(),
            [backend, roomId]() {
                if (backend)
                    backend->stopRtcScreenShare(roomId);
            },
            Qt::QueuedConnection
        );
    }

private:
    QPointer<Backend> m_backend;
    QString m_roomId;
    std::shared_ptr<std::atomic_bool> m_completed;
};

} // namespace

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


void Backend::refreshMatrixAccountProfile()
{
    if (!m_storageReady) {
        emit matrixAccountProfileRetrieved(
            QString(),
            QStringLiteral("Unlock encrypted storage to view your Matrix account.")
        );
        return;
    }

    emit requestFetchMatrixAccountProfile();
}


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



void Backend::joinRtcRoom(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }
    emit requestJoinRtcRoom(roomId);
}

void Backend::leaveRtcRoom(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }
    emit requestLeaveRtcRoom(roomId);
}


void Backend::startRtcAudio(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcAudioActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }
    emit requestStartRtcAudio(roomId);
}

void Backend::stopRtcAudio(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcAudioActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }
    emit requestStopRtcAudio(roomId);
}

void Backend::setRtcAudioMuted(const QString &roomId, bool muted)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcAudioActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }
    emit requestSetRtcAudioMuted(roomId, muted);
}

void Backend::startRtcVideo(
    const QString &roomId,
    int width,
    int height
)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcVideoActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    if (width < 2 || height < 2) {
        emit matrixRtcVideoActionFinished(
            roomId,
            QStringLiteral("Invalid video dimensions")
        );
        return;
    }

    emit requestStartRtcVideo(roomId, width, height);
}

void Backend::startRtcScreenShare(
    const QString &roomId,
    int width,
    int height,
    bool includeDeviceAudio
)
{
    qInfo() << "ASTRO_SCREEN_SHARE: backend request received"
            << "storageReady:" << m_storageReady
            << "roomIdEmpty:" << roomId.isEmpty()
            << "size:" << width << "x" << height;
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcScreenShareActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    if (width < 2 || height < 2) {
        emit matrixRtcScreenShareActionFinished(
            roomId, QStringLiteral("Invalid screen-share dimensions")
        );
        return;
    }

    emit requestStartRtcScreenShare(roomId, width, height, includeDeviceAudio);
}

void Backend::stopRtcScreenShare(const QString &roomId)
{
    qInfo() << "ASTRO_SCREEN_SHARE_DIAG: backend stop received"
            << "room:" << roomId;
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcScreenShareActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestStopRtcScreenShare(roomId);
}

void Backend::stopRtcScreenShareAfterSceneGraph(
    QObject *windowObject,
    const QString &roomId
)
{
    if (!m_storageReady || roomId.isEmpty()) {
        stopRtcScreenShare(roomId);
        return;
    }

    QQuickWindow *window = qobject_cast<QQuickWindow *>(windowObject);
    if (!window || !window->isExposed()) {
        qWarning() << "ASTRO_SCREEN_SHARE_DIAG: no exposed Qt Quick window;"
                   << "stopping without scenegraph fence"
                   << "room:" << roomId;
        stopRtcScreenShare(roomId);
        return;
    }

    const auto completed =
        std::make_shared<std::atomic_bool>(false);

    qInfo() << "ASTRO_SCREEN_SHARE_DIAG: scheduling scenegraph fence"
            << "room:" << roomId
            << "stage: AfterSwap";

    window->scheduleRenderJob(
        new ScreenShareSceneGraphFenceJob(this, roomId, completed),
        QQuickWindow::AfterSwapStage
    );
    window->update();

    // If the window becomes hidden or stops rendering before the job runs,
    // do not leave the Stop operation waiting indefinitely. The atomic flag
    // ensures the fallback and render job can never dispatch teardown twice.
    QTimer::singleShot(500, this, [this, roomId, completed]() {
        if (completed->exchange(true))
            return;

        qWarning() << "ASTRO_SCREEN_SHARE_DIAG: scenegraph fence timed out;"
                   << "using bounded fallback"
                   << "room:" << roomId;
        stopRtcScreenShare(roomId);
    });
}

void Backend::switchRtcCamera(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcVideoActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestSwitchRtcCamera(roomId);
}

void Backend::startRtcBackgroundCamera(
    const QString &roomId,
    const QString &deviceId
)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcBackgroundCameraActionFinished(
            roomId,
            false,
            QString(),
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestStartRtcBackgroundCamera(roomId, deviceId);
}

void Backend::stopRtcBackgroundCamera(const QString &roomId)
{
    if (roomId.isEmpty()) {
        emit matrixRtcBackgroundCameraActionFinished(
            roomId,
            false,
            QString(),
            QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestStopRtcBackgroundCamera(roomId);
}

void Backend::stopRtcVideo(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcVideoActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    emit requestStopRtcVideo(roomId);
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
