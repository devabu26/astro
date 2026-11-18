
#include "StorageWorker.h"

#include <QByteArray>
#include <QBuffer>
#include <QIODevice>
#include <QImage>
#include <QDir>
#include <QStandardPaths>

#include <climits>

// =============================================================
// Construction and destruction
// =============================================================

StorageWorker::StorageWorker(QObject *parent)
    : QObject(parent)
{
}

StorageWorker::~StorageWorker()
{
    // Clear the retained passphrase before destruction.
    m_storagePassphrase.fill('\0');

    // Release the Rust Matrix client if one is still active.
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
        emit storageFailed(
            QStringLiteral("The storage passphrase must not be empty")
        );
        return;
    }

    // Open the encrypted application storage.
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

    // Attempt to restore the previously saved Matrix session.
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

    // Retain the passphrase only while storage is unlocked.
    m_storagePassphrase = passphraseBytes;
    passphraseBytes.fill('\0');

    m_matrixClient = client;

    emit storageOpened();
    emit sessionRestored(result == 1);
}

void StorageWorker::closeStorage()
{
    // Clear the retained encrypted-storage passphrase.
    m_storagePassphrase.fill('\0');
    m_storagePassphrase.clear();

    // Release the Matrix client.
    if (m_matrixClient) {
        astro_matrix_close(m_matrixClient);
        m_matrixClient = nullptr;
    }

    emit storageClosed();
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
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    // Release the client after a successful logout.
    astro_matrix_close(m_matrixClient);
    m_matrixClient = nullptr;

    emit matrixLogoutSucceeded();
}

// =============================================================
// Matrix room and Space retrieval
// =============================================================


void StorageWorker::fetchMatrixRooms()
{
    if (!m_matrixClient) {
        emit storageFailed(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    // Retrieve cached rooms without waiting for a network sync.
    char *error = nullptr;

    char *json = astro_matrix_get_cached_rooms_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not retrieve cached Matrix rooms");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    const QString result = QString::fromUtf8(json);

    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    // Backend forwards this to QML before scheduling the network sync.
    emit matrixRoomsUpdated(result, false);
}

void StorageWorker::syncMatrixRooms()
{
    if (!m_matrixClient) {
        emit storageFailed(
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    // Perform the network sync separately from cached room retrieval.
    char *error = nullptr;

    char *json = astro_matrix_get_rooms_json(
        m_matrixClient,
        &error
    );

    if (!json) {
        QString message =
            QStringLiteral("Could not refresh Matrix rooms");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        emit storageFailed(message);
        return;
    }

    const QString result = QString::fromUtf8(json);

    astro_string_free(json);

    if (error) {
        astro_string_free(error);
    }

    emit matrixRoomsUpdated(result, true);
}


// Matrix media and room avatar retrieval
// =============================================================

void StorageWorker::fetchRoomAvatar(
    const QString &roomId,
    const QString &mxcUri,
    quint32 width,
    quint32 height
)
{
    if (!m_matrixClient) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral("No active Matrix session")
        );
        return;
    }

    if (roomId.isEmpty() || mxcUri.isEmpty()) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Room ID or avatar URI is empty")
        );
        return;
    }

    if (width == 0 || height == 0 ||
        width > 1024 || height > 1024) {

        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Invalid avatar dimensions")
        );
        return;
    }

    const QByteArray uriBytes = mxcUri.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    // Download the thumbnail through the Rust Matrix SDK.
    const int result = astro_matrix_thumbnail(
        m_matrixClient,
        uriBytes.constData(),
        width,
        height,
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download room avatar");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit roomAvatarFailed(roomId, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    // Protect against oversized buffers before converting to QByteArray.
    if (len > static_cast<uintptr_t>(INT_MAX)) {
        astro_media_bytes_free(data, len);

        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Room avatar is too large")
        );
        return;
    }

    // Copy the Rust-owned bytes before freeing the Rust buffer.
    const QByteArray encoded(
        reinterpret_cast<const char *>(data),
        static_cast<int>(len)
    );

    astro_media_bytes_free(data, len);

    // Decode the downloaded image.
    QImage image;

    if (!image.loadFromData(encoded)) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral(
                "Downloaded room avatar has an unsupported image format"
            )
        );
        return;
    }

    // Convert the image to PNG for Qt Quick.
    QByteArray pngData;

    QBuffer buffer(&pngData);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {

        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Could not convert room avatar to PNG")
        );
        return;
    }

    // Send a data URL to QML.
    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit roomAvatarRetrieved(roomId, dataUrl);
}
