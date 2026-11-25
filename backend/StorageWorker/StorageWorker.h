
#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QTimer>
#include <QMutex>
#include <QCamera>
#include <QAudioInput>
#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QIODevice>
#include <cstdint>

class RtcCameraVideoSurface;
class QVideoFrame;
class QImage;
class QOpenGLContext;
class QOffscreenSurface;

#include "../astro_core_ffi.h"

#include <mir_toolkit/mir_client_library.h>
#include <mir_toolkit/mir_screencast.h>
#include <mir_toolkit/mir_buffer_stream.h>

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

    void setRtcCameraOpenGLContext(
        QOpenGLContext *context,
        QOffscreenSurface *surface
    );
    bool makeRtcCameraOpenGLContextCurrent();
    void releaseRtcCameraOpenGLContext();

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
    void joinRtcRoom(const QString &roomId);
    void leaveRtcRoom(const QString &roomId);
    void startRtcAudio(const QString &roomId);
    void stopRtcAudio(const QString &roomId);
    void setRtcAudioMuted(const QString &roomId, bool muted);

    // ASTRO_RTC_VIDEO_STAGE3
    void startRtcVideo(const QString &roomId, int width, int height);
    void startRtcScreenShare(const QString &roomId, int width, int height, bool includeDeviceAudio);
    void stopRtcScreenShare(const QString &roomId);
    // Called directly by the Qt Quick video-filter render thread while its GL context is current.
    void processRtcCameraFrame(const QVideoFrame &frame);
    void processRtcCameraImage(const QString &roomId, const QImage &image);
    void switchRtcCamera(const QString &roomId);
    void pushRtcVideoFrame(
        const QString &roomId,
        int width,
        int height,
        const QByteArray &i420
    );
    void stopRtcVideo(const QString &roomId);

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

    void sendRoomReply(
        const QString &roomId,
        const QString &eventId,
        const QString &message
    );
    void editRoomMessage(
        const QString &roomId,
        const QString &eventId,
        const QString &message
    );
    void redactRoomEvent(
        const QString &roomId,
        const QString &eventId
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
    void pollRtcRemoteVideoFrames();


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

    void matrixMessageActionFinished(
        const QString &roomId,
        const QString &eventId,
        const QString &action,
        const QString &error
    );

    // Live subscription updates are forwarded to Backend, which
    // also passes relevant events to NotificationManager.
    void matrixLiveTimelineUpdated(const QString &json);

    // MatrixRTC membership changes are forwarded separately from timeline events.
    void matrixRtcMembershipsUpdated(const QString &json);
    void matrixRtcActionFinished(const QString &roomId, const QString &error);

    // LiveKit audio events and audio-session actions are separate from MatrixRTC presence.
    void matrixRtcAudioEventsUpdated(const QString &json);
    void matrixRtcAudioActionFinished(const QString &roomId, const QString &error);
    void matrixRtcVideoActionFinished(const QString &roomId, const QString &error);
    void matrixRtcScreenShareActionFinished(const QString &roomId, const QString &error);
    void matrixRtcRemoteVideoFrame(
        const QString &roomId,
        const QString &identity,
        const QString &source,
        int width,
        int height,
        const QByteArray &i420
    );

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
    void stopRtcCamera();
    void stopRtcScreenShareCapture();
    bool startRtcScreenShareAudioCapture(const QString &roomId, QString *error);
    void captureRtcScreenShareAudio();
    void captureRtcScreenShareFrame();

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
    QOpenGLContext *m_rtcCameraOpenGLContext = nullptr;
    QOffscreenSurface *m_rtcCameraOffscreenSurface = nullptr;
    QMutex m_rtcCameraFrameMutex;
    QCamera *m_rtcCamera = nullptr;
    RtcCameraVideoSurface *m_rtcVideoSurface = nullptr;
    QString m_rtcVideoRoomId;
    QString m_rtcScreenShareRoomId;
    MirConnection *m_rtcScreenShareConnection = nullptr;
    MirScreencast *m_rtcScreenCast = nullptr;
    MirBufferStream *m_rtcScreenShareStream = nullptr;
    QTimer *m_rtcScreenShareTimer = nullptr;
    int m_rtcScreenShareWidth = 0;
    int m_rtcScreenShareHeight = 0;
    bool m_rtcScreenShareStartPending = false;
    QAudioInput *m_rtcScreenShareAudioInput = nullptr;
    QIODevice *m_rtcScreenShareAudioDevice = nullptr;
    QByteArray m_rtcScreenShareAudioBuffer;
    QString m_rtcScreenShareAudioRoomId;

    // Retained push notification configuration.
    QByteArray m_pushToken;
    QString m_pushPrivacyLevel = "sender_room";
    bool m_pushNotificationsEnabled = true;

    // Polls retained timeline subscriptions on this worker's thread.
    QTimer *m_timelinePollTimer = nullptr;
    QTimer *m_rtcVideoPollTimer = nullptr;
    bool m_loggedFirstRemoteRtcFrame = false;

    // Retained only while encrypted storage is unlocked.
    QByteArray m_storagePassphrase;

    // Storage may be unlocked without an authenticated Matrix session.
    bool m_matrixSessionActive = false;
};
