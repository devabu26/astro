

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
// Backend construction and worker-thread setup
// =============================================================



Backend::Backend(QObject *parent)
    : QObject(parent)
{
    QCoreApplication::setApplicationName(
        QStringLiteral("astro.madabu")
    );

    m_notificationManager = new NotificationManager(this);

    // ---------------------------------------------------------
    // Create the dedicated storage thread and worker.
    // ---------------------------------------------------------

    m_storageThread = new QThread(this);
    m_storageWorker = new StorageWorker();

    m_storageWorker->moveToThread(m_storageThread);

    connect(
        m_storageThread,
        &QThread::finished,
        m_storageWorker,
        &QObject::deleteLater
    );

    // ---------------------------------------------------------
    // Encrypted storage request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestOpenStorage,
        m_storageWorker,
        &StorageWorker::openStorage,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestCloseStorage,
        m_storageWorker,
        &StorageWorker::closeStorage,
        Qt::QueuedConnection
    );

    // ---------------------------------------------------------
    // Matrix account request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestLoginMatrix,
        m_storageWorker,
        &StorageWorker::loginMatrix,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestLogoutMatrix,
        m_storageWorker,
        &StorageWorker::logoutMatrix,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestSetPushToken,
        m_storageWorker,
        &StorageWorker::setPushToken,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestSetPushPrivacyLevel,
        m_storageWorker,
        &StorageWorker::setPushPrivacyLevel,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestSetPushNotificationsEnabled,
        m_storageWorker,
        &StorageWorker::setPushNotificationsEnabled,
        Qt::QueuedConnection
    );

    // ---------------------------------------------------------
    // Matrix room and Space request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestFetchMatrixRooms,
        m_storageWorker,
        &StorageWorker::fetchMatrixRooms,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchMatrixDevices,
        m_storageWorker,
        &StorageWorker::fetchMatrixDevices,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestStartDeviceVerification,
        m_storageWorker,
        &StorageWorker::requestDeviceVerification,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestMatrixDeviceManagementUrl,
        m_storageWorker,
        &StorageWorker::checkMatrixDeviceManagementUrl,
        Qt::QueuedConnection
    );

    connect(
        m_storageWorker,
        &StorageWorker::matrixDeviceManagementUrlReady,
        this,
        &Backend::matrixDeviceManagementUrlReady,
        Qt::QueuedConnection
    );

   connect(
       this,
       &Backend::requestRevokeMatrixDevice,
       m_storageWorker,
       &StorageWorker::revokeMatrixDevice,
       Qt::QueuedConnection
   );

   connect(
       this,
       &Backend::requestRenameMatrixDevice,
       m_storageWorker,
       &StorageWorker::renameMatrixDevice,
       Qt::QueuedConnection
   );
// ---------------------------------------------------------
    connect(
       this,
       &Backend::requestSyncMatrixRooms,
       m_storageWorker,
       &StorageWorker::syncMatrixRooms,
       Qt::QueuedConnection
   );

   connect(
       this,
       &Backend::requestResumeMatrixSync,
       m_storageWorker,
       &StorageWorker::resumeMatrixSync,
       Qt::QueuedConnection
   );

   // Matrix room timeline request connection.
   connect(
        m_storageWorker,
        &StorageWorker::matrixLiveTimelineUpdated,
        this,
        &Backend::matrixLiveTimelineUpdated
   );

   // Forward MatrixRTC membership updates directly to Backend.
   // These must not pass through NotificationManager.
   connect(
       m_storageWorker,
       &StorageWorker::matrixRtcMembershipsUpdated,
       this,
       &Backend::matrixRtcMembershipsUpdated
   );

   connect(
       m_storageWorker,
       &StorageWorker::matrixRtcActionFinished,
       this,
       &Backend::matrixRtcActionFinished
   );

   // Forward LiveKit audio events and action results separately from MatrixRTC.
   connect(
       m_storageWorker,
       &StorageWorker::matrixRtcAudioEventsUpdated,
       this,
       &Backend::matrixRtcAudioEventsUpdated
   );

   connect(
       m_storageWorker,
       &StorageWorker::matrixRtcAudioActionFinished,
       this,
       &Backend::matrixRtcAudioActionFinished
   );

   connect(
        m_storageWorker,
        &StorageWorker::matrixRtcVideoActionFinished,
        this,
        &Backend::matrixRtcVideoActionFinished
    );

    connect(
        m_storageWorker,
        &StorageWorker::matrixRtcRemoteVideoFrame,
        this,
        &Backend::matrixRtcRemoteVideoFrame
    );

   connect(
       m_storageWorker,
       &StorageWorker::matrixLiveTimelineUpdated,
       m_notificationManager,
       &NotificationManager::processTimelineJson
   );

   connect(
       m_notificationManager,
       &NotificationManager::notificationReceived,
       this,
       &Backend::notificationReceived
   );

   connect(
       this,
       &Backend::requestFetchRoomTimeline,
       m_storageWorker,
       &StorageWorker::fetchRoomTimeline,
       Qt::QueuedConnection
   );

   connect(
       this,
       &Backend::requestRefreshRoomTimelineWorker,
       m_storageWorker,
       &StorageWorker::refreshRoomTimeline,
       Qt::QueuedConnection
   );

   connect(
        this,
        &Backend::requestPaginateRoomTimeline,
        m_storageWorker,
        &StorageWorker::paginateRoomTimeline,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestMarkRoomAsRead,
        m_storageWorker,
        &StorageWorker::markRoomAsRead,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestSendRoomMessage,
        m_storageWorker,
        &StorageWorker::sendRoomMessage,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestSendRoomAttachment,
        m_storageWorker,
        &StorageWorker::sendRoomAttachment,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestJoinRtcRoom,
       m_storageWorker,
       &StorageWorker::joinRtcRoom,
       Qt::QueuedConnection
   );

   connect(
       this,
       &Backend::requestLeaveRtcRoom,
       m_storageWorker,
       &StorageWorker::leaveRtcRoom,
       Qt::QueuedConnection
   );

   connect(
       this,
       &Backend::requestStartRtcAudio,
       m_storageWorker,
       &StorageWorker::startRtcAudio,
       Qt::QueuedConnection
   );

    // ASTRO_RTC_VIDEO_STAGE3
    connect(
        this,
        &Backend::requestStartRtcVideo,
        m_storageWorker,
        &StorageWorker::startRtcVideo,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestStopRtcVideo,
        m_storageWorker,
        &StorageWorker::stopRtcVideo,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestSwitchRtcCamera,
        m_storageWorker,
        &StorageWorker::switchRtcCamera,
        Qt::QueuedConnection
    );

   connect(
       this,
       &Backend::requestStopRtcAudio,
       m_storageWorker,
       &StorageWorker::stopRtcAudio,
       Qt::QueuedConnection
   );

   connect(
       this,
       &Backend::requestSetRtcAudioMuted,
       m_storageWorker,
       &StorageWorker::setRtcAudioMuted,
       Qt::QueuedConnection
   );

   connect(
       this,
       &Backend::requestToggleRoomReaction,
        m_storageWorker,
        &StorageWorker::toggleRoomReaction,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestSendRoomReply,
        m_storageWorker,
        &StorageWorker::sendRoomReply,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestEditRoomMessage,
        m_storageWorker,
        &StorageWorker::editRoomMessage,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestRedactRoomEvent,
        m_storageWorker,
        &StorageWorker::redactRoomEvent,
        Qt::QueuedConnection
    );

    connect(
        m_storageWorker,
        &StorageWorker::matrixMessageActionFinished,
        this,
        &Backend::matrixMessageActionFinished,
        Qt::QueuedConnection
    );

    // ---------------------------------------------------------
    // Matrix device verification request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestAcceptVerificationRequest,
        m_storageWorker,
        &StorageWorker::acceptVerificationRequest,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestCancelVerificationRequest,
        m_storageWorker,
        &StorageWorker::cancelVerificationRequest,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestConfirmVerification,
        m_storageWorker,
        &StorageWorker::confirmVerification,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestMismatchVerification,
        m_storageWorker,
        &StorageWorker::mismatchVerification,
        Qt::QueuedConnection
    );

    // Forward verification action results back to QML.
    connect(
        m_storageWorker,
        &StorageWorker::matrixVerificationActionFinished,
        this,
        &Backend::matrixVerificationActionFinished,
        Qt::QueuedConnection
    );


   // Forward room timeline results to QML.
   connect(
       m_storageWorker,
       &StorageWorker::matrixTimelineUpdated,
       this,
       [this](
           const QString &roomId,
           const QString &json,
           const QString &error
       )
       {
           emit matrixTimelineRetrieved(roomId, json, error);
       }
   );

   connect(
        m_storageWorker,
        &StorageWorker::matrixOlderTimelineUpdated,
        this,
        [this](
            const QString &roomId,
            const QString &json,
            const QString &error
        )
        {
            emit matrixOlderTimelineRetrieved(roomId, json, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::matrixMessageSent,
        this,
        [this](
            const QString &roomId,
            const QString &eventId,
            const QString &error
        )
        {
            emit matrixMessageSent(roomId, eventId, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::matrixAttachmentSent,
        this,
        [this](
            const QString &requestKey,
            const QString &roomId,
            const QString &eventId,
            const QString &error
        )
        {
            emit matrixAttachmentSent(
                requestKey, roomId, eventId, error
            );
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::matrixAttachmentProgress,
        this,
        &Backend::matrixAttachmentProgress,
        Qt::QueuedConnection
    );

    connect(
        m_storageWorker,
        &StorageWorker::matrixReactionToggled,
        this,
        [this](
            const QString &roomId,
            const QString &eventId,
            const QString &reactionKey,
            bool added,
            const QString &error
        )
        {
            emit matrixReactionToggled(
                roomId, eventId, reactionKey, added, error
            );
        }
    );


   // Matrix media request connections
    // ---------------------------------------------------------

    connect(
        this,
        &Backend::requestFetchRoomAvatar,
        m_storageWorker,
        &StorageWorker::fetchRoomAvatar,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchSenderAvatar,
        m_storageWorker,
        &StorageWorker::fetchSenderAvatar,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchMessageImage,
        m_storageWorker,
        &StorageWorker::fetchMessageImage,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestExportMessageImage,
        m_storageWorker,
        &StorageWorker::exportMessageImage,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchMessageThumbnail,
        m_storageWorker,
        &StorageWorker::fetchMessageThumbnail,
        Qt::QueuedConnection
    );

    connect(
        this,
        &Backend::requestFetchMessageVideo,
        m_storageWorker,
        &StorageWorker::fetchMessageVideo,
        Qt::QueuedConnection
    );

    // =========================================================
    // Storage worker response connections
    // =========================================================

    // ---------------------------------------------------------
    // Successful storage opening
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::storageOpened,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;

            setStorageReady(true);

            emit storageStatusChanged(
                QStringLiteral(
                    "Encrypted storage opened successfully"
                )
            );
        }
    );

    // ---------------------------------------------------------
    // Successful storage closing
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::storageClosed,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;

            setStorageReady(false);

            emit storageStatusChanged(
                QStringLiteral("Encrypted storage closed")
            );
        }
    );

    // ---------------------------------------------------------
    // Storage errors
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::storageFailed,
        this,
        [this](const QString &error)
        {
            m_storageOperationInProgress = false;

            emit storageStatusChanged(error);
        }
    );

    // ---------------------------------------------------------
    connect(
        m_storageWorker,
        &StorageWorker::matrixRoomsFailed,
        this,
        [this](const QString &error)
        {
            m_storageOperationInProgress = false;

            emit storageStatusChanged(
                QStringLiteral("Matrix room sync failed: ") + error
            );
        },
        Qt::QueuedConnection
    );

    // Matrix session restoration
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::sessionRestored,
        this,
        [this](bool restored)
        {
            emit matrixSessionRestored(restored);

            emit storageStatusChanged(
                restored
                    ? QStringLiteral("Matrix session restored")
                    : QStringLiteral(
                        "Storage unlocked. Please sign in to Matrix."
                    )
            );
        }
    );

    // =========================================================
    // Matrix account response connections
    // =========================================================

    // ---------------------------------------------------------
    // Matrix login
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::matrixLoginSucceeded,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;

            emit matrixLoginSucceeded();

            emit storageStatusChanged(
                QStringLiteral("Matrix login successful")
            );
        }
    );

    // ---------------------------------------------------------
    // Matrix logout
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::matrixLogoutSucceeded,
        this,
        [this]()
        {
            m_storageOperationInProgress = false;

            emit matrixLogoutSucceeded();

            emit storageStatusChanged(
                QStringLiteral("Logged out of Matrix")
            );
        }
    );

    // =========================================================
   connect(
        m_storageWorker,
        &StorageWorker::dataDeletionFinished,
        this,
        [this](
            bool deleted,
            bool storageWasUnlocked,
            bool logoutAttempted,
            bool logoutSucceeded,
            const QString &logoutError,
            const QString &deletionError
        )
        {
            m_storageOperationInProgress = false;

            if (deleted) {
                setStorageReady(false);
                emit dataDeleted();
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
    );

    // Matrix room and Space response connections
   // =========================================================

   connect(
       m_storageWorker,
       &StorageWorker::matrixRoomsUpdated,
       this,
       [this](const QString &json, bool finished)
       {
           // Forward cached or freshly synchronised rooms to QML.
           // QML can queue avatar requests while handling the cached list.
           emit matrixRoomsRetrieved(json, finished);

           // Schedule network sync only after cached rooms reach QML.
           if (!finished) {
               emit requestSyncMatrixRooms();
               return;
           }

           m_storageOperationInProgress = false;

           emit storageStatusChanged(
               QStringLiteral("Matrix rooms retrieved")
           );
       },
       Qt::QueuedConnection
   );

   connect(
       m_storageWorker,
       &StorageWorker::matrixSyncResumeFinished,
       this,
       [this](const QString &error) {
           m_matrixResumeInProgress = false;

           if (!error.isEmpty()) {
               emit storageStatusChanged(
                   QStringLiteral("Matrix sync resume failed: ") + error
               );
           }
       },
       Qt::QueuedConnection
   );

   connect(
       m_storageWorker,
       &StorageWorker::matrixDevicesUpdated,
       this,
       [this](const QString &json, const QString &error) {
           emit matrixDevicesRetrieved(json, error);
       },
       Qt::QueuedConnection
   );

   connect(
       m_storageWorker,
       &StorageWorker::matrixDeviceVerificationFinished,
       this,
       &Backend::matrixDeviceVerificationFinished,
       Qt::QueuedConnection
   );


   connect(
       m_storageWorker,
       &StorageWorker::matrixDeviceRevocationFinished,
       this,
       &Backend::matrixDeviceRevocationFinished,
       Qt::QueuedConnection
   );

   connect(
       m_storageWorker,
       &StorageWorker::matrixDeviceRenameFinished,
       this,
       &Backend::matrixDeviceRenameFinished,
       Qt::QueuedConnection
   );
// =========================================================
     // Matrix media and room avatar response connections
   // =========================================================

   // ---------------------------------------------------------
   // Successfully retrieved room avatar
   // ---------------------------------------------------------

   connect(
        m_storageWorker,
        &StorageWorker::roomAvatarRetrieved,
        this,
        [this](
            const QString &roomId,
            const QString &dataUrl
        )
        {
            emit roomAvatarRetrieved(roomId, dataUrl);
        }
    );

    // ---------------------------------------------------------
    // Room avatar retrieval failure
    // ---------------------------------------------------------

    connect(
        m_storageWorker,
        &StorageWorker::roomAvatarFailed,
        this,
        [this](
            const QString &roomId,
            const QString &error
        )
        {
            emit roomAvatarFailed(roomId, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::senderAvatarRetrieved,
        this,
        [this](
            const QString &requestKey,
            const QString &dataUrl
        )
        {
            emit senderAvatarRetrieved(requestKey, dataUrl);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::senderAvatarFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit senderAvatarFailed(requestKey, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageImageRetrieved,
        this,
        [this](
            const QString &requestKey,
            const QString &dataUrl
        )
        {
            emit messageImageRetrieved(requestKey, dataUrl);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageImageFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit messageImageFailed(requestKey, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageImageExported,
        this,
        [this](
            const QString &requestKey,
            const QString &fileUrl,
            const QString &fileName
        )
        {
            emit messageImageExported(
                requestKey,
                fileUrl,
                fileName
            );
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageImageExportFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit messageImageExportFailed(requestKey, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageThumbnailRetrieved,
        this,
        [this](
            const QString &requestKey,
            const QString &dataUrl
        )
        {
            emit messageThumbnailRetrieved(requestKey, dataUrl);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageThumbnailFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit messageThumbnailFailed(requestKey, error);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageVideoRetrieved,
        this,
        [this](
            const QString &requestKey,
            const QString &fileUrl
        )
        {
            emit messageVideoRetrieved(requestKey, fileUrl);
        }
    );

    connect(
        m_storageWorker,
        &StorageWorker::messageVideoFailed,
        this,
        [this](
            const QString &requestKey,
            const QString &error
        )
        {
            emit messageVideoFailed(requestKey, error);
        }
    );

    // ---------------------------------------------------------
    // Start the worker thread after all connections exist.
    // ---------------------------------------------------------

    m_storageThread->start();

    // ---------------------------------------------------------
    // Report the application data directory.
    // Do not create it here.
    // ---------------------------------------------------------

    const QString path =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (path.isEmpty()) {
        qWarning()
            << "Astro: Could not determine application data directory";
    } else {
        qInfo()
            << "Astro: Application data directory:"
            << path;
    }
}
