
#include "StorageWorker.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>

// =============================================================
// Construction and destruction
// =============================================================

StorageWorker::StorageWorker(QObject *parent)
    : QObject(parent)
{
    // Timeline updates are polled on the worker thread, keeping
    // Matrix SDK operations away from the QML-facing thread.
    m_timelinePollTimer = new QTimer(this);
    m_timelinePollTimer->setInterval(100);

    connect(
        m_timelinePollTimer,
        &QTimer::timeout,
        this,
        &StorageWorker::pollRoomTimelineUpdates
    );

    // Remote video is polled independently so timeline polling does not
    // cap incoming video refresh to its slower interval.
    m_rtcVideoPollTimer = new QTimer(this);
    m_rtcVideoPollTimer->setInterval(33);
    connect(
        m_rtcVideoPollTimer,
        &QTimer::timeout,
        this,
        &StorageWorker::pollRtcRemoteVideoFrames
    );

    // Poll asynchronous LiveKit screen-share unpublishing without blocking
    // the worker's event loop on network signalling.
    m_rtcScreenShareStopPollTimer = new QTimer(this);
    m_rtcScreenShareStopPollTimer->setInterval(50);
    connect(
        m_rtcScreenShareStopPollTimer,
        &QTimer::timeout,
        this,
        &StorageWorker::pollRtcScreenShareStop
    );
}

StorageWorker::~StorageWorker()
{
    if (m_rtcScreenShareStopPollTimer)
        m_rtcScreenShareStopPollTimer->stop();
    stopRtcScreenShareCapture();
    if (m_timelinePollTimer) {
        m_timelinePollTimer->stop();
    }
    if (m_rtcVideoPollTimer) {
        m_rtcVideoPollTimer->stop();
    }

    // Clear retained credentials before releasing the worker.
    m_storagePassphrase.fill('\0');
    m_pushToken.fill('\0');

    if (m_matrixClient) {
        astro_matrix_close(m_matrixClient);
        m_matrixClient = nullptr;
    }
}

// =============================================================
// Encrypted storage operations
// =============================================================

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
            QStringLiteral(
                "Could not determine the application data directory"
            )
        );
        return;
    }

    QDir directory;

    if (!directory.mkpath(dataPath)) {
        emit storageFailed(
            QStringLiteral(
                "Could not create the application data directory"
            )
        );
        return;
    }

    const QByteArray pathBytes = dataPath.toUtf8();
    QByteArray passphraseBytes = passphrase.toUtf8();

    if (passphraseBytes.isEmpty()) {
        passphraseBytes.fill('\0');

        emit storageFailed(
            QStringLiteral("The storage passphrase must not be empty")
        );
        return;
    }

    // Open the encrypted storage database.
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
        }

        if (error) {
            astro_string_free(error);
        }

        passphraseBytes.fill('\0');
        emit storageFailed(message);
        return;
    }

    // The storage handle is only needed to verify the passphrase.
    astro_storage_close(storage);

    // Restore the previously saved Matrix session, if one exists.
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
        }

        if (error) {
            astro_string_free(error);
        }

        passphraseBytes.fill('\0');
        emit storageFailed(message);
        return;
    }

    // Release any unexpected error string returned on success.
    if (error) {
        astro_string_free(error);
    }

    // Retain the passphrase only while encrypted storage is unlocked.
    m_storagePassphrase = passphraseBytes;
    passphraseBytes.fill('\0');

    m_matrixClient = client;
    m_matrixSessionActive = (result == 1);

    if (m_rtcVideoPollTimer) {
        m_rtcVideoPollTimer->start();
    }

    registerPushTokenIfReady();

    if (m_timelinePollTimer) {
        if (m_matrixSessionActive) {
            m_timelinePollTimer->start();
        } else {
            m_timelinePollTimer->stop();
        }
    }

    emit storageOpened();
    emit sessionRestored(result == 1);
}

void StorageWorker::closeStorage()
{
    stopRtcScreenShareCapture();
    stopRtcCamera();
    if (m_timelinePollTimer) {
        m_timelinePollTimer->stop();
    }
    if (m_rtcVideoPollTimer) {
        m_rtcVideoPollTimer->stop();
    }

    // Clear the retained passphrase before closing the session.
    m_storagePassphrase.fill('\0');
    m_storagePassphrase.clear();
    m_matrixSessionActive = false;

    if (m_matrixClient) {
        astro_matrix_close(m_matrixClient);
        m_matrixClient = nullptr;
    }

    emit storageClosed();
}

// =============================================================
// Delete all local application data
// =============================================================

void StorageWorker::deleteAllData(const QString &path)
{
    const bool storageWasUnlocked = !m_storagePassphrase.isEmpty();

    // Never allow this operation to recursively delete an arbitrary
    // directory supplied by another part of the application.
    const QString appDataPath =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    const QString expectedPath =
        appDataPath.isEmpty()
            ? QString()
            : QDir(appDataPath).canonicalPath();

    const QString requestedPath =
        path.isEmpty()
            ? QString()
            : QFileInfo(path).canonicalFilePath();

    if (expectedPath.isEmpty() ||
        requestedPath.isEmpty() ||
        requestedPath != expectedPath ||
        expectedPath == QDir::rootPath()) {

        emit dataDeletionFinished(
            false,
            storageWasUnlocked,
            false,
            false,
            QString(),
            QStringLiteral("Refused to delete an invalid application data path")
        );
        return;
    }

    const bool logoutAttempted = m_matrixSessionActive;

    bool logoutSucceeded = false;
    QString logoutError;

    // Attempt remote logout before removing the local session.
    if (logoutAttempted) {
        if (!m_matrixClient) {
            logoutError = QStringLiteral(
                "The active Matrix client is unavailable"
            );
        } else {
            char *error = nullptr;

            const int result = astro_matrix_logout(
                m_matrixClient,
                &error
            );

            if (result == 0) {
                logoutSucceeded = true;
            } else {
                logoutError = QStringLiteral(
                    "Matrix logout could not be confirmed"
                );

                if (error) {
                    logoutError = QString::fromUtf8(error);
                }
            }

            if (error) {
                astro_string_free(error);
            }
        }
    }

    // Close Rust-side resources before deleting their storage files.
    closeStorage();

    QDir directory(expectedPath);
    const bool deleted = directory.removeRecursively();

    const QString deletionError =
        deleted
            ? QString()
            : QStringLiteral("Could not delete Astro local data");

    emit dataDeletionFinished(
        deleted,
        storageWasUnlocked,
        logoutAttempted,
        logoutSucceeded,
        logoutError,
        deletionError
    );
}

// =============================================================
// Matrix account operations
// =============================================================

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
            QStringLiteral(
                "Could not determine the application data directory"
            )
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
            QStringLiteral(
                "Enter your homeserver, username and password"
            )
        );
        return;
    }

    // Authenticate through the Rust Matrix backend.
    char *error = nullptr;

    AstroMatrixClient *client = astro_matrix_login(
        pathBytes.constData(),
        m_storagePassphrase.constData(),
        homeserverBytes.constData(),
        usernameBytes.constData(),
        passwordBytes.constData(),
        &error
    );

    // The password is no longer needed after the FFI call.
    passwordBytes.fill('\0');

    if (!client) {
        QString message =
            QStringLiteral("Matrix login failed");

        if (error) {
            message = QString::fromUtf8(error);
        }

        if (error) {
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    m_matrixClient = client;
    m_matrixSessionActive = true;

    registerPushTokenIfReady();

    if (m_timelinePollTimer) {
        m_timelinePollTimer->start();
    }

    emit matrixLoginSucceeded();
}

void StorageWorker::logoutMatrix()
{
    if (!m_matrixClient) {
        emit storageFailed(
            QStringLiteral("No Matrix session is active")
        );
        return;
    }

    char *error = nullptr;

    const int result = astro_matrix_logout(
        m_matrixClient,
        &error
    );

    if (result != 0) {
        QString message =
            QStringLiteral("Matrix logout failed");

        if (error) {
            message = QString::fromUtf8(error);
        }

        if (error) {
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    // Stop screen capture before releasing the LiveKit client.
    stopRtcScreenShareCapture();

    // Stop polling before releasing the logged-out client.
    if (m_timelinePollTimer) {
        m_timelinePollTimer->stop();
    }

    astro_matrix_close(m_matrixClient);
    m_matrixClient = nullptr;
    m_matrixSessionActive = false;

    emit matrixLogoutSucceeded();
}
