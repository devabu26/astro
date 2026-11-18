
#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

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

    // ---------------------------------------------------------
    // Matrix account operations
    // ---------------------------------------------------------

    void loginMatrix(
        const QString &homeserver,
        const QString &username,
        const QString &password
    );

    void logoutMatrix();

    // ---------------------------------------------------------
    // Matrix room and Space retrieval
    // ---------------------------------------------------------

    void fetchMatrixRooms();
    void syncMatrixRooms();
    void fetchRoomTimeline(const QString &roomId);
    void paginateRoomTimeline(const QString &roomId);
    // ---------------------------------------------------------
    // Matrix media and avatar retrieval
    // ---------------------------------------------------------

    void fetchRoomAvatar(
        const QString &roomId,
        const QString &mxcUri,
        quint32 width,
        quint32 height
    );

signals:

    // ---------------------------------------------------------
    // Encrypted storage signals
    // ---------------------------------------------------------

    void storageOpened();
    void storageClosed();
    void storageFailed(const QString &error);
    void sessionRestored(bool restored);

    // ---------------------------------------------------------
    // Matrix account signals
    // ---------------------------------------------------------

    void matrixLoginSucceeded();
    void matrixLogoutSucceeded();

    // ---------------------------------------------------------
    // Matrix room and Space signals
    // ---------------------------------------------------------

    void matrixRoomsUpdated(const QString &json, bool finished);
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

private:

    // ---------------------------------------------------------
    // Rust Matrix client
    // ---------------------------------------------------------

    AstroMatrixClient *m_matrixClient = nullptr;

    // Retained only while encrypted storage is unlocked.
    QByteArray m_storagePassphrase;
};
