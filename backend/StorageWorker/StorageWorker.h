
#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QTimer>
#include <cstdint>

#include "../astro_core_ffi.h"

/*
 * Background worker responsible for encrypted storage, Matrix operations,
 * device management, timeline updates and media retrieval.
 *
 * Backend owns this object and moves it to a dedicated QThread.
 * Operations should therefore be dispatched through queued connections
 * rather than called directly from the QML/UI thread.
 *
 * The Rust Matrix client is managed by this class through the FFI.
 */
class StorageWorker : public QObject
{
    Q_OBJECT

public:
    explicit StorageWorker(QObject *parent = nullptr);
    ~StorageWorker() override;

public slots:

    // ---------------------------------------------------------
    // Encrypted storage operations
    // ---------------------------------------------------------

    void openStorage(const QString &passphrase);
    void closeStorage();

    // The implementation must validate the deletion target before
    // performing recursive filesystem operations.
    void deleteAllData(const QString &path);


    // ---------------------------------------------------------
    // Matrix account operations
    // ---------------------------------------------------------

    void loginMatrix(
        const QString &homeserver,
        const QString &username,
        const QString &password
    );

    void logoutMatrix();

    void setPushToken(const QString &token);
    void setPushPrivacyLevel(const QString &privacyLevel);
    void setPushNotificationsEnabled(bool enabled);


    // ---------------------------------------------------------
    // Matrix devices, rooms and timelines
    // ---------------------------------------------------------

    void fetchMatrixDevices();
    void requestDeviceVerification(const QString &deviceId);
    void renameMatrixDevice(
        const QString &deviceId,
        const QString &displayName
    );
    void checkMatrixDeviceManagementUrl(const QString &deviceId);
    void revokeMatrixDevice(
        const QString &deviceId,
        const QString &password
    );

    void fetchMatrixRooms();
    void syncMatrixRooms();
    void resumeMatrixSync();

    void fetchRoomTimeline(const QString &roomId);
    void refreshRoomTimeline(const QString &roomId);
    void paginateRoomTimeline(const QString &roomId);
    void markRoomAsRead(const QString &roomId);
    void sendRoomMessage(const QString &roomId, const QString &message);

    void sendRoomAttachment(
        const QString &requestKey,
        const QString &roomId,
        const QString &fileUrl,
        const QString &caption
    );

    void toggleRoomReaction(
        const QString &roomId,
        const QString &eventId,
        const QString &reactionKey
    );


    // ---------------------------------------------------------
    // Matrix end-to-end device verification
    // ---------------------------------------------------------

    void acceptVerificationRequest(
        const QString &userId,
        const QString &flowId
    );

    void cancelVerificationRequest(
        const QString &userId,
        const QString &flowId
    );

    void confirmVerification(
        const QString &userId,
        const QString &flowId
    );

    void mismatchVerification(
        const QString &userId,
        const QString &flowId
    );


    // Called by the worker-thread timer to process retained
    // Matrix timeline subscriptions.
    void pollRoomTimelineUpdates();


    // ---------------------------------------------------------
    // Matrix media and avatar retrieval
    // ---------------------------------------------------------

    void fetchRoomAvatar(
        const QString &roomId,
        const QString &mxcUri,
        quint32 width,
        quint32 height
    );

    void fetchSenderAvatar(
        const QString &requestKey,
        const QString &mxcUri,
        quint32 width,
        quint32 height
    );

    void fetchMessageImage(
        const QString &requestKey,
        const QString &mediaSourceJson
    );

    void exportMessageImage(
        const QString &requestKey,
        const QString &mediaSourceJson,
        const QString &mimeType,
        const QString &fileName
    );

    void fetchMessageThumbnail(
        const QString &requestKey,
        const QString &mediaSourceJson,
        quint32 width,
        quint32 height
    );

    void fetchMessageVideo(
        const QString &requestKey,
        const QString &mediaSourceJson,
        const QString &mimeType
    );


signals:

    // ---------------------------------------------------------
    // Encrypted storage results
    // ---------------------------------------------------------

    void storageOpened();
    void storageClosed();
    void storageFailed(const QString &error);
    void sessionRestored(bool restored);

    void dataDeletionFinished(
        bool deleted,
        bool storageWasUnlocked,
        bool logoutAttempted,
        bool logoutSucceeded,
        const QString &logoutError,
        const QString &deletionError
    );


    // ---------------------------------------------------------
    // Matrix account results
    // ---------------------------------------------------------

    void matrixLoginSucceeded();
    void matrixLogoutSucceeded();


    // ---------------------------------------------------------
    // Matrix device and room results
    // ---------------------------------------------------------

    void matrixDevicesUpdated(const QString &json, const QString &error);

    /*
     * finished distinguishes the cached room response from the
     * completed homeserver synchronisation response.
     * Backend relies on this cached-first loading sequence.
     */
    void matrixRoomsUpdated(const QString &json, bool finished);
    void matrixRoomsFailed(const QString &error);
    void matrixSyncResumeFinished(const QString &error);

    void matrixTimelineUpdated(
        const QString &roomId,
        const QString &json,
        const QString &error
    );

    void matrixOlderTimelineUpdated(
        const QString &roomId,
        const QString &json,
        const QString &error
    );

    void matrixMessageSent(
        const QString &roomId,
        const QString &eventId,
        const QString &error
    );

    void matrixAttachmentSent(
        const QString &requestKey,
        const QString &roomId,
        const QString &eventId,
        const QString &error
    );

    void matrixAttachmentProgress(
        const QString &requestKey,
        const QString &roomId,
        quint64 current,
        quint64 total
    );

    void matrixReactionToggled(
        const QString &roomId,
        const QString &eventId,
        const QString &reactionKey,
        bool added,
        const QString &error
    );

    // Live subscription updates are forwarded to Backend, which
    // also passes relevant events to NotificationManager.
    void matrixLiveTimelineUpdated(const QString &json);

    void matrixDeviceVerificationFinished(
        const QString &deviceId,
        const QString &error
    );

    void matrixDeviceRenameFinished(
        const QString &deviceId,
        const QString &error
    );

    void matrixDeviceRevocationFinished(
        const QString &deviceId,
        const QString &error
    );

    void matrixDeviceManagementUrlReady(
        const QString &deviceId,
        const QString &url,
        const QString &error
    );

    void matrixVerificationActionFinished(
        const QString &userId,
        const QString &flowId,
        const QString &action,
        const QString &error
    );


    // ---------------------------------------------------------
    // Matrix media results
    // ---------------------------------------------------------

    void roomAvatarRetrieved(
        const QString &roomId,
        const QString &dataUrl
    );

    void roomAvatarFailed(
        const QString &roomId,
        const QString &error
    );

    void senderAvatarRetrieved(
        const QString &requestKey,
        const QString &dataUrl
    );

    void senderAvatarFailed(
        const QString &requestKey,
        const QString &error
    );

    void messageImageRetrieved(
        const QString &requestKey,
        const QString &dataUrl
    );

    void messageImageFailed(
        const QString &requestKey,
        const QString &error
    );

    void messageImageExported(
        const QString &requestKey,
        const QString &fileUrl,
        const QString &fileName
    );

    void messageImageExportFailed(
        const QString &requestKey,
        const QString &error
    );

    void messageThumbnailRetrieved(
        const QString &requestKey,
        const QString &dataUrl
    );

    void messageThumbnailFailed(
        const QString &requestKey,
        const QString &error
    );

    void messageVideoRetrieved(
        const QString &requestKey,
        const QString &fileUrl
    );

    void messageVideoFailed(
        const QString &requestKey,
        const QString &error
    );


private:

    /*
     * Common signature for Rust verification actions.
     * The final argument receives a Rust-allocated error string,
     * which must be released using the appropriate FFI function.
     */
    using VerificationAction = int32_t (*)(
        AstroMatrixClient *,
        const char *,
        const char *,
        char **
    );

    void runVerificationAction(
        const QString &userId,
        const QString &flowId,
        const QString &actionName,
        VerificationAction action
    );


    // Register the retained push token when the Matrix client
    // and authenticated session are ready.
    void registerPushTokenIfReady();


    // ---------------------------------------------------------
    // Worker-owned state
    // ---------------------------------------------------------

    // Rust Matrix client; its lifecycle is managed through the FFI.
    AstroMatrixClient *m_matrixClient = nullptr;

    // Retained push notification configuration.
    QByteArray m_pushToken;
    QString m_pushPrivacyLevel = "sender_room";
    bool m_pushNotificationsEnabled = true;

    // Polls retained timeline subscriptions on this worker's thread.
    QTimer *m_timelinePollTimer = nullptr;

    // Retained only while encrypted storage is unlocked.
    QByteArray m_storagePassphrase;

    // Storage may be unlocked without an authenticated Matrix session.
    bool m_matrixSessionActive = false;
};
