
#include "StorageWorker.h"

#include <QByteArray>
#include <QDir>
#include <QStandardPaths>

StorageWorker::StorageWorker(QObject *parent)
    : QObject(parent)
{
}

StorageWorker::~StorageWorker()
{
    m_storagePassphrase.fill('\0');

    if (m_matrixClient) {
        astro_matrix_close(m_matrixClient);
        m_matrixClient = nullptr;
    }
}

void StorageWorker::openStorage(const QString &passphrase)
{
    if (m_matrixClient || !m_storagePassphrase.isEmpty()) {
        emit storageFailed(
            QStringLiteral("Encrypted storage is already open")
        );
        return;
    }

    const QString dataPath =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (dataPath.isEmpty()) {
        emit storageFailed(
            QStringLiteral("Could not determine the application data directory")
        );
        return;
    }

    QDir directory;

    if (!directory.mkpath(dataPath)) {
        emit storageFailed(
            QStringLiteral("Could not create the application data directory")
        );
        return;
    }

    const QByteArray pathBytes = dataPath.toUtf8();
    QByteArray passphraseBytes = passphrase.toUtf8();

    if (passphraseBytes.isEmpty()) {
        emit storageFailed(
            QStringLiteral("The storage passphrase must not be empty")
        );
        return;
    }

    char *error = nullptr;

    AstroStorageManager *storage = astro_storage_open(
        pathBytes.constData(),
        passphraseBytes.constData(),
        &error
    );

    if (!storage) {
        QString message =
            QStringLiteral("Could not open encrypted storage");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        passphraseBytes.fill('\0');
        emit storageFailed(message);
        return;
    }

    astro_storage_close(storage);

    AstroMatrixClient *client = nullptr;
    error = nullptr;

    const int result = astro_matrix_restore(
        pathBytes.constData(),
        passphraseBytes.constData(),
        &client,
        &error
    );

    if (result < 0) {
        QString message =
            QStringLiteral("Could not restore Matrix session");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        passphraseBytes.fill('\0');
        emit storageFailed(message);
        return;
    }

    m_storagePassphrase = passphraseBytes;
    passphraseBytes.fill('\0');

    m_matrixClient = client;

    emit storageOpened();
    emit sessionRestored(result == 1);
}

void StorageWorker::loginMatrix(
    const QString &homeserver,
    const QString &username,
    const QString &password
)
{
    if (m_storagePassphrase.isEmpty()) {
        emit storageFailed(
            QStringLiteral("Encrypted storage is not unlocked")
        );
        return;
    }

    if (m_matrixClient) {
        emit storageFailed(
            QStringLiteral("A Matrix session is already active")
        );
        return;
    }

    const QString dataPath =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (dataPath.isEmpty()) {
        emit storageFailed(
            QStringLiteral("Could not determine the application data directory")
        );
        return;
    }

    const QByteArray pathBytes = dataPath.toUtf8();
    const QByteArray homeserverBytes = homeserver.toUtf8();
    const QByteArray usernameBytes = username.toUtf8();
    QByteArray passwordBytes = password.toUtf8();

    if (homeserverBytes.isEmpty() ||
        usernameBytes.isEmpty() ||
        passwordBytes.isEmpty()) {
        passwordBytes.fill('\0');
        emit storageFailed(
            QStringLiteral("Enter your homeserver, username and password")
        );
        return;
    }

    char *error = nullptr;

    AstroMatrixClient *client = astro_matrix_login(
        pathBytes.constData(),
        m_storagePassphrase.constData(),
        homeserverBytes.constData(),
        usernameBytes.constData(),
        passwordBytes.constData(),
        &error
    );

    passwordBytes.fill('\0');

    if (!client) {
        QString message =
            QStringLiteral("Matrix login failed");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    m_matrixClient = client;

    emit matrixLoginSucceeded();
}

void StorageWorker::logoutMatrix()
{
    if (!m_matrixClient) {
        emit storageFailed(QStringLiteral("No Matrix session is active"));
        return;
    }

    char *error = nullptr;
    const int result = astro_matrix_logout(m_matrixClient, &error);

    if (result != 0) {
        QString message = QStringLiteral("Matrix logout failed");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    astro_matrix_close(m_matrixClient);
    m_matrixClient = nullptr;

    emit matrixLogoutSucceeded();
}

void StorageWorker::closeStorage()
{
    m_storagePassphrase.fill('\0');
    m_storagePassphrase.clear();

    if (m_matrixClient) {
        astro_matrix_close(m_matrixClient);
        m_matrixClient = nullptr;
    }

    emit storageClosed();
}
