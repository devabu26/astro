
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
    void logoutMatrix();
    void openStorage(const QString &passphrase);
    void loginMatrix(
        const QString &homeserver,
        const QString &username,
        const QString &password
    );
    void closeStorage();

signals:
    void matrixLogoutSucceeded();
    void storageOpened();
    void storageClosed();
    void storageFailed(const QString &error);
    void sessionRestored(bool restored);
    void matrixLoginSucceeded();

private:
    AstroMatrixClient *m_matrixClient = nullptr;

    // Retained only while encrypted storage is unlocked.
    QByteArray m_storagePassphrase;
};
