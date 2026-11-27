#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QPointer>

#class QThread;
class QCamera;
class QVideoFrame;
class QImage;
class StorageWorker;
class NotificationManager;

class Backend : public QObject
{
    Q_OBJECT

    Q_PROPERTY(
        bool storageReady
        READ storageReady
        NOTIFY storageReadyChanged
    )
    Q_PROPERTY(
        int applicationState
        READ applicationState
        NOTIFY applicationStateChanged
    )
    Q_PROPERTY(QObject *rtcCamera READ rtcCamera NOTIFY rtcCameraChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    // ---------------------------------------------------------
    // General application and storage information
    // ---------------------------------------------------------

    Q_INVOKABLE QString status() const;
    Q_INVOKABLE QString rtcRingbackToneUrl() const;
    Q_INVOKABLE QString rtcConnectionToneUrl() const;
    Q_INVOKABLE QString rtcHangupToneUrl() const;
    Q_INVOKABLE QString prepareDataDirectory() const;
    Q_INVOKABLE bool hasExistingStorage() const;
    Q_INVOKABLE QString secretServiceStatus() const;

    Q_INVOKABLE void testSecretService();

    bool storageReady() const;
    int applicationState() const;
    QObject *rtcCamera() const;

    // ---------------------------------------------------------
    // Encrypted storage operations
    // ---------------------------------------------------------

    Q_INVOKABLE void openStorage(const QString &passphrase);
    Q_INVOKABLE void closeStorage();
    Q_INVOKABLE void deleteAllData();

    // ---------------------------------------------------------
    // Matrix account operations
    // ---------------------------------------------------------
    Q_INVOKABLE void checkMatrixDeviceManagementUrl(
        const QString &deviceId
    );

    Q_INVOKABLE void loginMatrix(
        const QString &homeserver,
        const QString &username,
        const QString &password
    );

    Q_INVOKABLE void logoutMatrix();
    Q_INVOKABLE void refreshMatrixAccountProfile();
    Q_INVOKABLE void setPushToken(const QString &token);
    Q_INVOKABLE void setPushPrivacyLevel(const QString &privacyLevel);
    Q_INVOKABLE void setPushNotificationsEnabled(bool enabled);

    // ---------------------------------------------------------
    // Matrix rooms and Spaces
    // ---------------------------------------------------------

    Q_INVOKABLE bool refreshMatrixRooms();
    Q_INVOKABLE void resumeMatrixSync();

    // ---------------------------------------------------------
    // Matrix media and room avatars
    // ---------------------------------------------------------

    Q_INVOKABLE void requestRoomAvatar(
        const QString &roomId,
        const QString &mxcUri
    );

    Q_INVOKABLE void requestSenderAvatar(
        const QString &requestKey,
        const QString &mxcUri
    );

    Q_INVOKABLE void requestMessageImage(
        const QString &requestKey,
        const QString &mediaSourceJson
    );

    Q_INVOKABLE void exportMessageImage(
        const QString &requestKey,
        const QString &mediaSourceJson,
        const QString &mimeType,
        const QString &fileName
    );

    Q_INVOKABLE void requestMessageThumbnail(
        const QString &requestKey,
        const QString &mediaSourceJson,
        quint32 width,
        quint32 height
    );

    Q_INVOKABLE void requestMessageVideo(
        const QString &requestKey,
        const QString &mediaSourceJson,
        const QString &mimeType
    );

    Q_INVOKABLE void requestRoomTimeline(const QString &roomId);
    Q_INVOKABLE void requestRefreshRoomTimeline(const QString &roomId);
    Q_INVOKABLE void requestOlderRoomTimeline(const QString &roomId);
    Q_INVOKABLE void markRoomAsRead(const QString &roomId);
    Q_INVOKABLE void sendRoomMessage(const QString &roomId, const QString &message);
    Q_INVOKABLE void declineRtcCall(const QString &roomId, const QString &eventId);
    Q_INVOKABLE void acceptRtcCall(const QString &roomId);
    Q_INVOKABLE void joinRtcRoom(const QString &roomId);
    Q_INVOKABLE void leaveRtcRoom(const QString &roomId);
    Q_INVOKABLE void startRtcAudio(const QString &roomId);
    Q_INVOKABLE void stopRtcAudio(const QString &roomId);
    Q_INVOKABLE void setRtcAudioMuted(const QString &roomId, bool muted);

    Q_INVOKABLE void startRtcVideo(
        const QString &roomId,
        int width,
        int height
    );
    Q_INVOKABLE void stopRtcVideo(const QString &roomId);
    Q_INVOKABLE void startRtcScreenShare(const QString &roomId, int width, int height, bool includeDeviceAudio);
    Q_INVOKABLE void stopRtcScreenShare(const QString &roomId);
    Q_INVOKABLE void switchRtcCamera(const QString &roomId);
    Q_INVOKABLE void startRtcBackgroundCamera(
        const QString &roomId,
        const QString &deviceId
    );
    Q_INVOKABLE void stopRtcBackgroundCamera(const QString &roomId);

    // Called directly by the Qt Quick video-filter render thread.
    void processRtcCameraFrame(const QString &roomId, const QVideoFrame &frame);
    Q_INVOKABLE void processRtcCameraImage(const QString &roomId, const QImage &image);

    Q_INVOKABLE void sendRoomAttachment(
        const QString &requestKey,
        const QString &roomId,
        const QString &fileUrl,
        const QString &caption
    );
    Q_INVOKABLE void toggleRoomReaction(
        const QString &roomId,
        const QString &eventId,
        const QString &reactionKey
    );

    Q_INVOKABLE void sendRoomReply(
        const QString &roomId,
        const QString &eventId,
        const QString &message
    );
    Q_INVOKABLE void editRoomMessage(
        const QString &roomId,
        const QString &eventId,
        const QString &message
    );
    Q_INVOKABLE void redactRoomEvent(
        const QString &roomId,
        const QString &eventId
    );

    // Matrix device verification
    Q_INVOKABLE void acceptVerificationRequest(const QString &userId, const QString &flowId);
    Q_INVOKABLE void cancelVerificationRequest(const QString &userId, const QString &flowId);
    Q_INVOKABLE void confirmVerification(const QString &userId, const QString &flowId);
    Q_INVOKABLE void refreshMatrixDevices();
    Q_INVOKABLE void requestDeviceVerification(const QString &deviceId);
    Q_INVOKABLE void renameMatrixDevice(const QString &deviceId, const QString &displayName);
    Q_INVOKABLE void revokeMatrixDevice(const QString &deviceId, const QString &password);
    Q_INVOKABLE void mismatchVerification(const QString &userId, const QString &flowId);

signals:

    // ---------------------------------------------------------
    // Application and storage state
    // ---------------------------------------------------------

    void storageReadyChanged();
    void rtcCameraChanged();
    void storageStatusChanged(const QString &status);
    void secretServiceTestFinished(const QString &result);
    void dataDeleted();
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

    void matrixSessionRestored(bool restored);
    void matrixLoginSucceeded();
    void matrixLogoutSucceeded();
    void requestMatrixDeviceManagementUrl(const QString &deviceId);
    void matrixDeviceManagementUrlReady(
        const QString &deviceId,
        const QString &url,
        const QString &error
    );
    // ---------------------------------------------------------
    // Matrix room and Space signals
    // ---------------------------------------------------------

    void matrixAccountProfileRetrieved(const QString &json, const QString &error);
    void matrixDevicesRetrieved(const QString &json, const QString &error);
    void matrixRoomsRetrieved(const QString &json, bool finished);

    void matrixTimelineRetrieved(
        const QString &roomId,
        const QString &json,
        const QString &error
    );

    void matrixOlderTimelineRetrieved(
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

    void matrixMessageActionFinished(
        const QString &roomId,
        const QString &eventId,
        const QString &action,
        const QString &error
    );

    // Incoming and updated room timeline events.
    void matrixLiveTimelineUpdated(const QString &json);
    // MatrixRTC membership snapshots, separate from timeline notifications.
    void matrixRtcMembershipsUpdated(const QString &json);
    void matrixRtcActionFinished(const QString &roomId, const QString &error);

    // LiveKit audio/camera events and actions are separate from MatrixRTC presence.
    void matrixRtcAudioEventsUpdated(const QString &json);
    void matrixRtcAudioActionFinished(const QString &roomId, const QString &error);

    void matrixRtcVideoActionFinished(
        const QString &roomId,
        const QString &error
    );
    void matrixRtcBackgroundCameraActionFinished(
        const QString &roomId,
        bool running,
        const QString &deviceId,
        const QString &error
    );
    void applicationActiveChanged(bool active);
    void applicationStateChanged(int state);
    void matrixRtcScreenShareActionFinished(
        const QString &roomId,
        const QString &error
    );
    void matrixRtcRemoteVideoFrame(
        const QString &roomId,
        const QString &identity,
        const QString &source,
        int width,
        int height,
        const QByteArray &i420
    );
    void matrixRtcLocalVideoFrame(
        const QString &roomId,
        int width,
        int height,
        const QByteArray &i420
    );

    void notificationReceived(
        const QString &sender,
        const QString &body,
        const QString &roomId,
        const QString &eventId
    );

    void matrixDeviceVerificationFinished(
        const QString &deviceId,
        const QString &error
    );

    void matrixDeviceRenameFinished(const QString &deviceId, const QString &error);
    void matrixDeviceRevocationFinished(const QString &deviceId, const QString &error);

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

    // ---------------------------------------------------------
    // Requests sent to StorageWorker
    // ---------------------------------------------------------

    // Encrypted storage requests.
    void requestOpenStorage(const QString &passphrase);
    void requestCloseStorage();

    // Matrix account requests.
    void requestLoginMatrix(
        const QString &homeserver,
        const QString &username,
        const QString &password
    );

    void requestLogoutMatrix();
    void requestSetPushToken(const QString &token);
    void requestSetPushPrivacyLevel(const QString &privacyLevel);
    void requestSetPushNotificationsEnabled(bool enabled);

    // Matrix room requests.
    void requestFetchMatrixAccountProfile();
    void requestFetchMatrixDevices();
    void requestStartDeviceVerification(const QString &deviceId);
    void requestRenameMatrixDevice(const QString &deviceId, const QString &displayName);
    void requestRevokeMatrixDevice(const QString &deviceId, const QString &password);
    void requestFetchMatrixRooms();
    void requestSyncMatrixRooms();
    void requestResumeMatrixSync();
    void requestFetchRoomTimeline(const QString &roomId);
    void requestRefreshRoomTimelineWorker(const QString &roomId);
    void requestPaginateRoomTimeline(const QString &roomId);
    void requestMarkRoomAsRead(const QString &roomId);
    void requestSendRoomMessage(const QString &roomId, const QString &message);
    void requestDeclineRtcCall(const QString &roomId, const QString &eventId);
    void requestAcceptRtcCall(const QString &roomId);
    void requestJoinRtcRoom(const QString &roomId);
    void requestLeaveRtcRoom(const QString &roomId);
    void requestStartRtcAudio(const QString &roomId);
    void requestStopRtcAudio(const QString &roomId);
    void requestSetRtcAudioMuted(const QString &roomId, bool muted);

   // ASTRO_RTC_VIDEO_STAGE3
   void requestStartRtcVideo(const QString &roomId, int width, int height);
   void requestStopRtcVideo(const QString &roomId);
   void requestStartRtcBackgroundCamera(
       const QString &roomId,
       const QString &deviceId
   );
   void requestStopRtcBackgroundCamera(const QString &roomId);
   void requestStartRtcScreenShare(const QString &roomId, int width, int height, bool includeDeviceAudio);
   void requestStopRtcScreenShare(const QString &roomId);
   void requestSwitchRtcCamera(const QString &roomId);

    void requestSendRoomAttachment(
        const QString &requestKey,
        const QString &roomId,
        const QString &fileUrl,
        const QString &caption
    );
    void requestToggleRoomReaction(
        const QString &roomId,
        const QString &eventId,
        const QString &reactionKey
    );

    void requestSendRoomReply(
        const QString &roomId,
        const QString &eventId,
        const QString &message
    );
    void requestEditRoomMessage(
        const QString &roomId,
        const QString &eventId,
        const QString &message
    );
    void requestRedactRoomEvent(
        const QString &roomId,
        const QString &eventId
    );

    void requestAcceptVerificationRequest(
        const QString &userId, const QString &flowId
    );
    void requestCancelVerificationRequest(
        const QString &userId, const QString &flowId
    );
    void requestConfirmVerification(
        const QString &userId, const QString &flowId
    );
    void requestMismatchVerification(
        const QString &userId, const QString &flowId
    );

    // Matrix media requests.
    void requestFetchRoomAvatar(
        const QString &roomId,
        const QString &mxcUri,
        quint32 width,
        quint32 height
    );

    void requestFetchSenderAvatar(
        const QString &requestKey,
        const QString &mxcUri,
        quint32 width,
        quint32 height
    );

    void requestFetchMessageImage(
        const QString &requestKey,
        const QString &mediaSourceJson
    );

    void requestExportMessageImage(
        const QString &requestKey,
        const QString &mediaSourceJson,
        const QString &mimeType,
        const QString &fileName
    );

    void requestFetchMessageThumbnail(
        const QString &requestKey,
        const QString &mediaSourceJson,
        quint32 width,
        quint32 height
    );

    void requestFetchMessageVideo(
        const QString &requestKey,
        const QString &mediaSourceJson,
        const QString &mimeType
    );

private:
    // ---------------------------------------------------------
    // Internal storage state
    // ---------------------------------------------------------

    bool m_storageReady = false;
    bool m_applicationActive = true;
    int m_applicationState = 4; // Qt::ApplicationActive
    bool m_storageOperationInProgress = false;
    bool m_matrixResumeInProgress = false;

    // ---------------------------------------------------------
    // Worker thread
    // ---------------------------------------------------------

    QThread *m_storageThread = nullptr;
    StorageWorker *m_storageWorker = nullptr;
    NotificationManager *m_notificationManager = nullptr;

    // QCamera is owned and controlled on the GUI thread; VideoOutput is its sink.
    QPointer<QCamera> m_rtcCamera;
    QString m_rtcCameraRoomId;
    quint64 m_rtcCameraGeneration = 0;
    bool m_rtcCameraUnloading = false;
    bool m_rtcCameraRestartAfterUnload = false;
    bool m_rtcCameraSwitchPending = false;
    QString m_rtcCameraPendingRoomId;
    QString m_rtcCameraPendingDeviceId;

    // ---------------------------------------------------------
    // Internal helpers
    // ---------------------------------------------------------

    void setStorageReady(bool ready);
    void releaseRtcCamera(bool restartAfterUnload);
};