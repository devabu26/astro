#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QTimer>

#include "astro_core_ffi.h"

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
    // Matrix room and Space retrieval
    // ---------------------------------------------------------

    void fetchMatrixDevices();
    void requestDeviceVerification(const QString &deviceId);
    void renameMatrixDevice(const QString &deviceId, const QString &displayName);
    void checkMatrixDeviceManagementUrl(const QString &deviceId);
    void revokeMatrixDevice(const QString &deviceId, const QString &password);
    void fetchMatrixRooms();
    void syncMatrixRooms();
    void fetchRoomTimeline(const QString &roomId);
    void refreshRoomTimeline(const QString &roomId);
    void paginateRoomTimeline(const QString &roomId);
    void markRoomAsRead(const QString &roomId);
    void sendRoomMessage(const QString &roomId, const QString &message);
    void toggleRoomReaction(
        const QString &roomId,
        const QString &eventId,
        const QString &reactionKey
    );

    // Matrix device verification actions.
    void acceptVerificationRequest(const QString &userId, const QString &flowId);
    void cancelVerificationRequest(const QString &userId, const QString &flowId);
    void confirmVerification(const QString &userId, const QString &flowId);
    void mismatchVerification(const QString &userId, const QString &flowId);

    // Poll retained Matrix timeline subscriptions.
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
    // Encrypted storage signals
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
    // Matrix account signals
    // ---------------------------------------------------------

    void matrixLoginSucceeded();
    void matrixLogoutSucceeded();

    // ---------------------------------------------------------
    // Matrix room and Space signals
    // ---------------------------------------------------------

    void matrixDevicesUpdated(const QString &json, const QString &error);
    void matrixRoomsUpdated(const QString &json, bool finished);
    void matrixRoomsFailed(const QString &error);

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

    void matrixReactionToggled(
        const QString &roomId,
        const QString &eventId,
        const QString &reactionKey,
        bool added,
        const QString &error
    );

    // Live timeline changes forwarded to Backend and QML.
    void matrixLiveTimelineUpdated(const QString &json);

    void matrixDeviceVerificationFinished(
        const QString &deviceId,
        const QString &error
    );

    void matrixDeviceRenameFinished(const QString &deviceId, const QString &error);
    void matrixDeviceRevocationFinished(const QString &deviceId, const QString &error);
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
    // Matrix media signals
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

    // ---------------------------------------------------------
    // Register a retained Ubuntu push token when a Matrix session is ready.
   void registerPushTokenIfReady();

   // Rust Matrix client
    // ---------------------------------------------------------

    AstroMatrixClient *m_matrixClient = nullptr;
   QByteArray m_pushToken;
   QString m_pushPrivacyLevel = "sender_room";
   bool m_pushNotificationsEnabled = true;
    // Polls retained timeline subscriptions on this worker's thread.
    QTimer *m_timelinePollTimer = nullptr;

    // Retained only while encrypted storage is unlocked.
    QByteArray m_storagePassphrase;
    bool m_matrixSessionActive = false;
};
