#pragma once

#include <QObject>
#include <QString>

class QThread;
class StorageWorker;

class Backend : public QObject
{
    Q_OBJECT

    Q_PROPERTY(
        bool storageReady
        READ storageReady
        NOTIFY storageReadyChanged
    )

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    // ---------------------------------------------------------
    // General application and storage information
    // ---------------------------------------------------------

    Q_INVOKABLE QString status() const;
    Q_INVOKABLE QString prepareDataDirectory() const;
    Q_INVOKABLE bool hasExistingStorage() const;
    Q_INVOKABLE QString secretServiceStatus() const;

    Q_INVOKABLE void testSecretService();

    bool storageReady() const;

    // ---------------------------------------------------------
    // Encrypted storage operations
    // ---------------------------------------------------------

    Q_INVOKABLE void openStorage(const QString &passphrase);
    Q_INVOKABLE void closeStorage();
    Q_INVOKABLE void deleteAllData();

    // ---------------------------------------------------------
    // Matrix account operations
    // ---------------------------------------------------------

    Q_INVOKABLE void loginMatrix(
        const QString &homeserver,
        const QString &username,
        const QString &password
    );

    Q_INVOKABLE void logoutMatrix();

    // ---------------------------------------------------------
    // Matrix rooms and Spaces
    // ---------------------------------------------------------

    Q_INVOKABLE void refreshMatrixRooms();

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
    Q_INVOKABLE void requestOlderRoomTimeline(const QString &roomId);
    Q_INVOKABLE void markRoomAsRead(const QString &roomId);
    Q_INVOKABLE void sendRoomMessage(const QString &roomId, const QString &message);

    // Matrix device verification
    Q_INVOKABLE void acceptVerificationRequest(const QString &userId, const QString &flowId);
    Q_INVOKABLE void cancelVerificationRequest(const QString &userId, const QString &flowId);
    Q_INVOKABLE void confirmVerification(const QString &userId, const QString &flowId);
    Q_INVOKABLE void mismatchVerification(const QString &userId, const QString &flowId);

signals:

    // ---------------------------------------------------------
    // Application and storage state
    // ---------------------------------------------------------

    void storageReadyChanged();
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

    // ---------------------------------------------------------
    // Matrix room and Space signals
    // ---------------------------------------------------------

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

    // Incoming and updated room timeline events.
    void matrixLiveTimelineUpdated(const QString &json);

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

    // Matrix room requests.
    void requestFetchMatrixRooms();
    void requestSyncMatrixRooms();
    void requestFetchRoomTimeline(const QString &roomId);
    void requestPaginateRoomTimeline(const QString &roomId);
    void requestMarkRoomAsRead(const QString &roomId);
    void requestSendRoomMessage(const QString &roomId, const QString &message);

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
    bool m_storageOperationInProgress = false;

    // ---------------------------------------------------------
    // Worker thread
    // ---------------------------------------------------------

    QThread *m_storageThread = nullptr;
    StorageWorker *m_storageWorker = nullptr;

    // ---------------------------------------------------------
    // Internal helpers
    // ---------------------------------------------------------

    void setStorageReady(bool ready);
};