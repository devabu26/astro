

#include "Backend.h"
#include "StorageWorker/StorageWorker.h"
#include "notifications/NotificationManager.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QUuid>
#include <QMap>
#include <QVariantMap>
#include <QList>
#include <QThread>
#include <QMetaObject>
#include <functional>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusArgument>
#include <QDBusMetaType>

// =============================================================


void Backend::requestRoomAvatar(
    const QString &roomId,
    const QString &mxcUri
)
{
    if (!m_storageReady) {
        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Encrypted storage is not open")
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

    // Request a 96 x 96 thumbnail.
    // The request is queued on the existing worker thread.
    // Avatar retrieval does not block normal storage operations.

    emit requestFetchRoomAvatar(
        roomId,
        mxcUri,
        96,
        96
    );
}


void Backend::requestSenderAvatar(
    const QString &requestKey,
    const QString &mxcUri
)
{
    if (!m_storageReady) {
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );

        return;
    }

    if (requestKey.isEmpty() || mxcUri.isEmpty()) {
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Request key or avatar URI is empty")
        );

        return;
    }

    // Request a 96 x 96 thumbnail.
    // The request is queued on the existing worker thread.
    // Avatar retrieval does not block normal storage operations.

    emit requestFetchSenderAvatar(
        requestKey,
        mxcUri,
        96,
        96
    );
}


void Backend::requestMessageImage(
    const QString &requestKey,
    const QString &mediaSourceJson
)
{
    if (!m_storageReady) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    emit requestFetchMessageImage(requestKey, mediaSourceJson);
}




void Backend::exportMessageImage(
    const QString &requestKey,
    const QString &mediaSourceJson,
    const QString &mimeType,
    const QString &fileName
)
{
    if (!m_storageReady) {
        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    emit requestExportMessageImage(
        requestKey,
        mediaSourceJson,
        mimeType,
        fileName
    );
}


void Backend::requestMessageThumbnail(
    const QString &requestKey,
    const QString &mediaSourceJson,
    quint32 width,
    quint32 height
)
{
    if (!m_storageReady) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    if (width == 0 || height == 0 || width > 2048 || height > 2048) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Thumbnail dimensions are invalid")
        );
        return;
    }

    emit requestFetchMessageThumbnail(
        requestKey,
        mediaSourceJson,
        width,
        height
    );
}


void Backend::requestMessageVideo(
    const QString &requestKey,
    const QString &mediaSourceJson,
    const QString &mimeType
)
{
    if (!m_storageReady) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Encrypted storage is not open")
        );
        return;
    }

    if (requestKey.isEmpty() || mediaSourceJson.isEmpty()) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Request key or media source is empty")
        );
        return;
    }

    emit requestFetchMessageVideo(
        requestKey,
        mediaSourceJson,
        mimeType
    );
}

// =============================================================
// Matrix logout
// =============================================================

// Request a room's initial message timeline.
