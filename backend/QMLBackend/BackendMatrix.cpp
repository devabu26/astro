

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
#include <QCamera>
#include <QCameraInfo>
#include <QTimer>
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



void Backend::declineRtcCall(const QString &roomId, const QString &eventId)
{
    if (!m_storageReady || roomId.isEmpty() || eventId.isEmpty()) {
        emit matrixRtcActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID or call event ID is empty")
        );
        return;
    }
    emit requestDeclineRtcCall(roomId, eventId);
}


void Backend::acceptRtcCall(const QString &roomId)
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
    emit requestAcceptRtcCall(roomId);
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


QObject *Backend::rtcCamera() const
{
    return m_rtcCamera.data();
}

void Backend::switchRtcCamera(const QString &roomId)
{
    if (!m_storageReady || roomId.isEmpty() ||
        !m_rtcCamera || m_rtcCameraRoomId != roomId) {
        emit matrixRtcVideoActionFinished(
            roomId,
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : !m_rtcCamera
                    ? QStringLiteral("Camera is not running")
                    : QStringLiteral("Camera belongs to another room")
        );
        return;
    }

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    if (cameras.size() < 2) {
        emit matrixRtcVideoActionFinished(
            roomId, QStringLiteral("Only one camera is available")
        );
        return;
    }

    const QCameraInfo current(*m_rtcCamera);
    QCameraInfo selected;
    for (const QCameraInfo &candidate : cameras) {
        if (candidate.deviceName() != current.deviceName() &&
            candidate.position() != QCamera::UnspecifiedPosition &&
            current.position() != QCamera::UnspecifiedPosition &&
            candidate.position() != current.position()) {
            selected = candidate;
            break;
        }
    }
    if (selected.deviceName().isEmpty()) {
        for (int i = 0; i < cameras.size(); ++i) {
            if (cameras.at(i).deviceName() == current.deviceName()) {
                selected = cameras.at((i + 1) % cameras.size());
                break;
            }
        }
    }
    if (selected.deviceName().isEmpty() ||
        selected.deviceName() == current.deviceName()) {
        emit matrixRtcVideoActionFinished(
            roomId, QStringLiteral("Could not find another camera device")
        );
        return;
    }

    // Release the current camera asynchronously and report completion through
    // the video-action signal expected by CallPage.qml.
    m_rtcCameraSwitchPending = true;
    startRtcBackgroundCamera(roomId, selected.deviceName());
}

void Backend::startRtcBackgroundCamera(
    const QString &roomId,
    const QString &deviceId
)
{
    if (!m_storageReady || roomId.isEmpty()) {
        emit matrixRtcBackgroundCameraActionFinished(
            roomId, false, QString(),
            !m_storageReady
                ? QStringLiteral("Encrypted storage is not open")
                : QStringLiteral("Room ID is empty")
        );
        return;
    }

    if (m_rtcCamera) {
        const QString currentDevice =
            QCameraInfo(*m_rtcCamera).deviceName();
        if (!m_rtcCameraUnloading &&
            m_rtcCameraRoomId == roomId &&
            (deviceId.isEmpty() || deviceId == currentDevice) &&
            m_rtcCamera->status() == QCamera::ActiveStatus) {
            emit matrixRtcBackgroundCameraActionFinished(
                roomId, true, currentDevice, QString()
            );
            return;
        }

        m_rtcCameraPendingRoomId = roomId;
        m_rtcCameraPendingDeviceId = deviceId;
        releaseRtcCamera(true);
        return;
    }

    const QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    QCameraInfo selected;
    if (!deviceId.isEmpty()) {
        for (const QCameraInfo &candidate : cameras) {
            if (candidate.deviceName() == deviceId) {
                selected = candidate;
                break;
            }
        }
    }
    if (selected.deviceName().isEmpty() && !deviceId.isEmpty()) {
        const QString message = QStringLiteral(
            "The requested camera device is no longer available"
        );
        emit matrixRtcBackgroundCameraActionFinished(
            roomId, false, QString(), message
        );
        if (m_rtcCameraSwitchPending) {
            m_rtcCameraSwitchPending = false;
            emit matrixRtcVideoActionFinished(roomId, message);
        }
        return;
    }
    if (selected.deviceName().isEmpty())
        selected = QCameraInfo::defaultCamera();

    if (selected.deviceName().isEmpty()) {
        const QString message = QStringLiteral(
            "No native camera device is available"
        );
        emit matrixRtcBackgroundCameraActionFinished(
            roomId, false, QString(), message
        );
        if (m_rtcCameraSwitchPending) {
            m_rtcCameraSwitchPending = false;
            emit matrixRtcVideoActionFinished(roomId, message);
        }
        return;
    }

    QCamera *camera = new QCamera(selected, this);
    m_rtcCamera = camera;
    m_rtcCameraRoomId = roomId;
    const quint64 generation = ++m_rtcCameraGeneration;
    const QPointer<QCamera> guard(camera);

    qInfo() << "ASTRO_RTC_CAMERA: creating C++ camera for VideoOutput"
            << "room:" << roomId
            << "device:" << selected.deviceName()
            << "description:" << selected.description()
            << "thread:" << QThread::currentThreadId();

    connect(
        camera, &QCamera::statusChanged, this,
        [this, guard, generation, roomId, selected](QCamera::Status status) {
            if (!guard || m_rtcCamera != guard.data() ||
                m_rtcCameraGeneration != generation)
                return;

            if (status == QCamera::LoadedStatus &&
                guard->state() != QCamera::ActiveState) {
                qInfo() << "ASTRO_RTC_CAMERA: camera loaded; starting capture"
                        << "device:" << selected.deviceName();
                guard->start();
                return;
            }

            if (status == QCamera::ActiveStatus) {
                qInfo() << "ASTRO_RTC_CAMERA: C++ camera active"
                        << "device:" << selected.deviceName()
                        << "videoOutputSource:" << (rtcCamera() == guard.data());
                emit matrixRtcBackgroundCameraActionFinished(
                    roomId, true, selected.deviceName(), QString()
                );
                if (m_rtcCameraSwitchPending) {
                    m_rtcCameraSwitchPending = false;
                    emit matrixRtcVideoActionFinished(roomId, QString());
                }
            }
        }
    );

    connect(
        camera, QOverload<QCamera::Error>::of(&QCamera::error), this,
        [this, guard, generation, roomId](QCamera::Error error) {
            if (!guard || m_rtcCamera != guard.data() ||
                m_rtcCameraGeneration != generation ||
                error == QCamera::NoError)
                return;
            const QString message = guard->errorString().isEmpty()
                ? QStringLiteral("The native camera failed")
                : guard->errorString();
            qWarning() << "ASTRO_RTC_CAMERA: C++ camera error"
                       << static_cast<int>(error) << message;
            emit matrixRtcBackgroundCameraActionFinished(
                roomId, false, QString(), message
            );
            if (m_rtcCameraSwitchPending) {
                m_rtcCameraSwitchPending = false;
                emit matrixRtcVideoActionFinished(roomId, message);
            }
            releaseRtcCamera(false);
        }
    );

    camera->setCaptureMode(QCamera::CaptureVideo);
    // Notify QML before loading so VideoOutput attaches its renderer surface
    // before the AAL backend attempts to start the camera preview.
    emit rtcCameraChanged();
    camera->load();

    QTimer::singleShot(10000, this, [this, guard, generation, roomId, selected]() {
        if (!guard || m_rtcCamera != guard.data() ||
            m_rtcCameraGeneration != generation ||
            guard->status() == QCamera::ActiveStatus ||
            m_rtcCameraUnloading)
            return;
        const QString message = guard->errorString().isEmpty()
            ? QStringLiteral("The camera did not become active within 10 seconds")
            : guard->errorString();
        qWarning() << "ASTRO_RTC_CAMERA: camera startup timed out"
                   << "device:" << selected.deviceName()
                   << "status:" << static_cast<int>(guard->status())
                   << "state:" << static_cast<int>(guard->state());
        emit matrixRtcBackgroundCameraActionFinished(
            roomId, false, QString(), message
        );
        if (m_rtcCameraSwitchPending) {
            m_rtcCameraSwitchPending = false;
            emit matrixRtcVideoActionFinished(roomId, message);
        }
        releaseRtcCamera(false);
    });
}

void Backend::stopRtcBackgroundCamera(const QString &roomId)
{
    if (m_rtcCamera && !roomId.isEmpty() &&
        roomId != m_rtcCameraRoomId)
        return;

    m_rtcCameraRestartAfterUnload = false;
    m_rtcCameraSwitchPending = false;
    m_rtcCameraPendingRoomId.clear();
    m_rtcCameraPendingDeviceId.clear();
    releaseRtcCamera(false);
}

void Backend::releaseRtcCamera(bool restartAfterUnload)
{
    if (!m_rtcCamera) {
        m_rtcCameraUnloading = false;
        if (restartAfterUnload && !m_rtcCameraPendingRoomId.isEmpty()) {
            const QString room = m_rtcCameraPendingRoomId;
            const QString device = m_rtcCameraPendingDeviceId;
            m_rtcCameraRestartAfterUnload = false;
            m_rtcCameraPendingRoomId.clear();
            m_rtcCameraPendingDeviceId.clear();
            QTimer::singleShot(0, this, [this, room, device]() {
                startRtcBackgroundCamera(room, device);
            });
        }
        return;
    }

    if (m_rtcCameraUnloading) {
        // A start/switch request can arrive while the previous unload is
        // already in progress. Preserve the new pending target and make sure
        // the completion callback knows whether to recreate the camera.
        m_rtcCameraRestartAfterUnload = restartAfterUnload;
        if (!restartAfterUnload) {
            m_rtcCameraPendingRoomId.clear();
            m_rtcCameraPendingDeviceId.clear();
        }
        return;
    }

    m_rtcCameraRestartAfterUnload = restartAfterUnload;
    QCamera *camera = m_rtcCamera.data();
    const quint64 generation = ++m_rtcCameraGeneration;
    m_rtcCameraUnloading = true;

    auto finishUnload = [this, camera, generation]() {
        if (!camera || m_rtcCamera != camera ||
            m_rtcCameraGeneration != generation ||
            !m_rtcCameraUnloading)
            return;

        m_rtcCamera = nullptr;
        m_rtcCameraRoomId.clear();
        m_rtcCameraUnloading = false;
        emit rtcCameraChanged();
        camera->deleteLater();

        const bool restart = m_rtcCameraRestartAfterUnload;
        const QString room = m_rtcCameraPendingRoomId;
        const QString device = m_rtcCameraPendingDeviceId;
        m_rtcCameraRestartAfterUnload = false;
        m_rtcCameraPendingRoomId.clear();
        m_rtcCameraPendingDeviceId.clear();

        if (restart && !room.isEmpty()) {
            QTimer::singleShot(0, this, [this, room, device]() {
                startRtcBackgroundCamera(room, device);
            });
        }
    };

    connect(
        camera, &QCamera::statusChanged, this,
        [finishUnload](QCamera::Status status) {
            if (status == QCamera::UnloadedStatus)
                finishUnload();
        }
    );

    qInfo() << "ASTRO_RTC_CAMERA: unloading C++ camera"
            << "room:" << m_rtcCameraRoomId
            << "status:" << static_cast<int>(camera->status())
            << "state:" << static_cast<int>(camera->state());
    camera->stop();
    camera->unload();
    if (camera->status() == QCamera::UnloadedStatus)
        finishUnload();
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
