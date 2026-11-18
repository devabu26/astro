
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

    Q_INVOKABLE void requestRoomTimeline(const QString &roomId);
    Q_INVOKABLE void requestOlderRoomTimeline(const QString &roomId);

    signals:

    // ---------------------------------------------------------
    // Application and storage state
    // ---------------------------------------------------------

    void storageReadyChanged();
    void storageStatusChanged(const QString &status);
    void secretServiceTestFinished(const QString &result);
    void dataDeleted();

    // ---------------------------------------------------------
    // Matrix account signals
    // ---------------------------------------------------------

    void matrixSessionRestored(bool restored);
    void matrixLoginSucceeded();
    void matrixLogoutSucceeded();

    // ---------------------------------------------------------
    // Matrix room and Space signals
    // ---------------------------------------------------------

    void matrixRoomsRetrieved(const QString &json);
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

    // Matrix media requests.
    void requestFetchRoomAvatar(
        const QString &roomId,
        const QString &mxcUri,
        quint32 width,
        quint32 height
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
