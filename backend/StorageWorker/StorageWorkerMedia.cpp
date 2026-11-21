
#include "StorageWorker.h"

#include <QByteArray>
#include <QBuffer>
#include <QIODevice>
#include <QImage>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QUrl>
#include <QSize>
#include <QDateTime>

#include <climits>
#include <limits>

// =============================================================
// Room and sender avatars
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

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(INT_MAX)) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit roomAvatarFailed(
            roomId,
            QStringLiteral("Downloaded room avatar is empty or too large")
        );
        return;
    }

    // Copy Rust-owned memory before releasing the original buffer.
    const QByteArray encoded(
        reinterpret_cast<const char *>(data),
        static_cast<int>(len)
    );

    astro_media_bytes_free(data, len);

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

    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit roomAvatarRetrieved(roomId, dataUrl);
}

void StorageWorker::fetchSenderAvatar(
    const QString &requestKey,
    const QString &mxcUri,
    quint32 width,
    quint32 height
)
{
    if (!m_matrixClient) {
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
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

    if (width == 0 || height == 0 ||
        width > 1024 || height > 1024) {

        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Invalid avatar dimensions")
        );
        return;
    }

    const QByteArray uriBytes = mxcUri.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

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
            QStringLiteral("Could not download sender avatar");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit senderAvatarFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(INT_MAX)) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Downloaded sender avatar is empty or too large")
        );
        return;
    }

    const QByteArray encoded(
        reinterpret_cast<const char *>(data),
        static_cast<int>(len)
    );

    astro_media_bytes_free(data, len);

    QImage image;

    if (!image.loadFromData(encoded)) {
        emit senderAvatarFailed(
            requestKey,
            QStringLiteral(
                "Downloaded sender avatar has an unsupported image format"
            )
        );
        return;
    }

    QByteArray pngData;
    QBuffer buffer(&pngData);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {

        emit senderAvatarFailed(
            requestKey,
            QStringLiteral("Could not convert sender avatar to PNG")
        );
        return;
    }

    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit senderAvatarRetrieved(requestKey, dataUrl);
}

// =============================================================
// Message image retrieval
// =============================================================

void StorageWorker::fetchMessageImage(
    const QString &requestKey,
    const QString &mediaSourceJson
)
{
    if (!m_matrixClient) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
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

    const QByteArray sourceBytes = mediaSourceJson.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    const int result = astro_matrix_image(
        m_matrixClient,
        sourceBytes.constData(),
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download message image");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageImageFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(INT_MAX)) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageImageFailed(
            requestKey,
            QStringLiteral("Downloaded image is empty or too large")
        );
        return;
    }

    const QByteArray encoded(
        reinterpret_cast<const char *>(data),
        static_cast<int>(len)
    );

    astro_media_bytes_free(data, len);

    // Preserve animated GIF data instead of flattening it to PNG.
    if (encoded.startsWith("GIF87a") ||
        encoded.startsWith("GIF89a")) {

        const QString dataUrl =
            QStringLiteral("data:image/gif;base64,") +
            QString::fromLatin1(encoded.toBase64());

        emit messageImageRetrieved(requestKey, dataUrl);
        return;
    }

    QImage image;

    if (!image.loadFromData(encoded)) {
        emit messageImageFailed(
            requestKey,
            QStringLiteral(
                "Downloaded message image has an unsupported format"
            )
        );
        return;
    }

    // Limit large images before passing them to Qt Quick.
    if (image.width() > 1600 || image.height() > 1600) {
        image = image.scaled(
            QSize(1600, 1600),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        );
    }

    QByteArray pngData;
    QBuffer buffer(&pngData);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {

        emit messageImageFailed(
            requestKey,
            QStringLiteral("Could not convert message image to PNG")
        );
        return;
    }

    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit messageImageRetrieved(requestKey, dataUrl);
}

// =============================================================
// Export original message images
// =============================================================

void StorageWorker::exportMessageImage(
    const QString &requestKey,
    const QString &mediaSourceJson,
    const QString &mimeType,
    const QString &fileName
)
{
    if (!m_matrixClient) {
        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
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

    const QString cacheLocation =
        QStandardPaths::writableLocation(
            QStandardPaths::CacheLocation
        );

    if (cacheLocation.isEmpty()) {
        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Could not determine the media export directory")
        );
        return;
    }

    const QString cachePath =
        QDir(cacheLocation).filePath(
            QStringLiteral("media-exports")
        );

    QDir directory;

    if (!directory.mkpath(cachePath)) {
        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Could not create media export directory")
        );
        return;
    }

    // Remove abandoned temporary exports older than 24 hours.
    const QDateTime expiryTime =
        QDateTime::currentDateTime().addSecs(-24 * 60 * 60);

    const QFileInfoList oldExports =
        QDir(cachePath).entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot
        );

    for (const QFileInfo &info : oldExports) {
        if (info.lastModified() < expiryTime) {
            QFile::remove(info.absoluteFilePath());
        }
    }

    // Strip directory components and replace unsafe filename characters.
    QString safeName = QFileInfo(fileName).fileName().trimmed();

    for (int i = 0; i < safeName.size(); ++i) {
        const QChar character = safeName.at(i);

        if (character.unicode() < 32 ||
            QStringLiteral("/\\:*?\"<>|").contains(character)) {
            safeName[i] = QLatin1Char('_');
        }
    }

    while (safeName.startsWith('.')) {
        safeName.remove(0, 1);
    }

    safeName = safeName.trimmed();

    QMimeDatabase mimeDatabase;

    const QString suffix =
        mimeDatabase.mimeTypeForName(mimeType).preferredSuffix();

    if (safeName.isEmpty()) {
        safeName = QStringLiteral("image");
    }

    if (QFileInfo(safeName).suffix().isEmpty() &&
        !suffix.isEmpty() &&
        suffix != QStringLiteral("bin")) {

        safeName += QStringLiteral(".") + suffix;
    }

    // Hash the request key to avoid filename collisions.
    const QByteArray keyHash =
        QCryptographicHash::hash(
            requestKey.toUtf8(),
            QCryptographicHash::Sha256
        ).toHex();

    const QString filePath =
        QDir(cachePath).filePath(
            QString::fromLatin1(keyHash) +
            QStringLiteral("_") +
            safeName
        );

    const QByteArray sourceBytes = mediaSourceJson.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    const int result = astro_matrix_image(
        m_matrixClient,
        sourceBytes.constData(),
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download original message image");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageImageExportFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(
            std::numeric_limits<qint64>::max()
        )) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Downloaded image is empty or too large")
        );
        return;
    }

    QSaveFile output(filePath);

    if (!output.open(QIODevice::WriteOnly)) {
        astro_media_bytes_free(data, len);

        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Could not open export file: ") +
            output.errorString()
        );
        return;
    }

    const qint64 written = output.write(
        reinterpret_cast<const char *>(data),
        static_cast<qint64>(len)
    );

    astro_media_bytes_free(data, len);

    if (written != static_cast<qint64>(len)) {
        output.cancelWriting();

        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Could not write the complete image")
        );
        return;
    }

    if (!output.commit()) {
        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Could not save exported image: ") +
            output.errorString()
        );
        return;
    }

    // Make the completed export readable by its owner.
    QFile::Permissions permissions = QFile::permissions(filePath);
    permissions |= QFileDevice::ReadOwner;

    if (!QFile::setPermissions(filePath, permissions)) {

        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Could not set read permissions on exported file")
        );
        return;
    }

    // Verify the committed file before returning its URL to QML.
    QFileInfo exportedInfo(filePath);

    if (!exportedInfo.exists() ||
        !exportedInfo.isFile() ||
        !exportedInfo.isReadable() ||
        exportedInfo.size() != static_cast<qint64>(len)) {

        emit messageImageExportFailed(
            requestKey,
            QStringLiteral("Exported file failed verification")
        );
        return;
    }

    emit messageImageExported(
        requestKey,
        QUrl::fromLocalFile(filePath).toString(),
        safeName
    );
}

// =============================================================
// Message video thumbnails
// =============================================================

void StorageWorker::fetchMessageThumbnail(
    const QString &requestKey,
    const QString &mediaSourceJson,
    quint32 width,
    quint32 height
)
{
    if (!m_matrixClient) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
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

    if (width == 0 || height == 0 ||
        width > 2048 || height > 2048) {

        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Thumbnail dimensions are invalid")
        );
        return;
    }

    const QByteArray sourceBytes = mediaSourceJson.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    const int result = astro_matrix_media_thumbnail(
        m_matrixClient,
        sourceBytes.constData(),
        width,
        height,
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download video thumbnail");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageThumbnailFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(INT_MAX)) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Downloaded thumbnail is empty or too large")
        );
        return;
    }

    const QByteArray encoded(
        reinterpret_cast<const char *>(data),
        static_cast<int>(len)
    );

    astro_media_bytes_free(data, len);

    QImage image;

    if (!image.loadFromData(encoded)) {
        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral(
                "Downloaded video thumbnail has an unsupported format"
            )
        );
        return;
    }

    if (image.width() > 1024 || image.height() > 1024) {
        image = image.scaled(
            QSize(1024, 1024),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        );
    }

    QByteArray pngData;
    QBuffer buffer(&pngData);

    if (!buffer.open(QIODevice::WriteOnly) ||
        !image.save(&buffer, "PNG")) {

        emit messageThumbnailFailed(
            requestKey,
            QStringLiteral("Could not encode video thumbnail")
        );
        return;
    }

    const QString dataUrl =
        QStringLiteral("data:image/png;base64,") +
        QString::fromLatin1(pngData.toBase64());

    emit messageThumbnailRetrieved(requestKey, dataUrl);
}

// =============================================================
// Message video downloads and local caching
// =============================================================

void StorageWorker::fetchMessageVideo(
    const QString &requestKey,
    const QString &mediaSourceJson,
    const QString &mimeType
)
{
    if (!m_matrixClient) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("No active Matrix session")
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

    const QString dataPath =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (dataPath.isEmpty()) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral(
                "Could not determine the application data directory"
            )
        );
        return;
    }

    const QString cachePath =
        QDir(dataPath).filePath(QStringLiteral("video-cache"));

    QDir directory;

    if (!directory.mkpath(cachePath)) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not create the video cache directory")
        );
        return;
    }

    QMimeDatabase mimeDatabase;

    QString suffix =
        mimeDatabase.mimeTypeForName(mimeType).preferredSuffix();

    if (suffix.isEmpty() || suffix == QStringLiteral("bin")) {
        suffix = QStringLiteral("mp4");
    }

    // The request key gives each cached media request a stable filename.
    const QByteArray keyHash =
        QCryptographicHash::hash(
            requestKey.toUtf8(),
            QCryptographicHash::Sha256
        ).toHex();

    const QString filePath =
        QDir(cachePath).filePath(
            QString::fromLatin1(keyHash) +
            QStringLiteral(".") +
            suffix
        );

    const QFileInfo cachedFile(filePath);

    if (cachedFile.exists() &&
        cachedFile.isFile() &&
        cachedFile.isReadable() &&
        cachedFile.size() > 0) {

        emit messageVideoRetrieved(
            requestKey,
            QUrl::fromLocalFile(filePath).toString()
        );
        return;
    }

    const QByteArray sourceBytes = mediaSourceJson.toUtf8();

    uint8_t *data = nullptr;
    uintptr_t len = 0;
    char *error = nullptr;

    const int result = astro_matrix_video(
        m_matrixClient,
        sourceBytes.constData(),
        &data,
        &len,
        &error
    );

    if (result != 1) {
        QString message =
            QStringLiteral("Could not download message video");

        if (error) {
            message = QString::fromUtf8(error);
            astro_string_free(error);
        }

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageVideoFailed(requestKey, message);
        return;
    }

    if (error) {
        astro_string_free(error);
    }

    if (!data || len == 0 ||
        len > static_cast<uintptr_t>(
            std::numeric_limits<qint64>::max()
        )) {

        if (data) {
            astro_media_bytes_free(data, len);
        }

        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Downloaded video is empty or too large")
        );
        return;
    }

    QSaveFile output(filePath);

    if (!output.open(QIODevice::WriteOnly)) {
        astro_media_bytes_free(data, len);

        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not open video cache file: ") +
            output.errorString()
        );
        return;
    }

    const qint64 written = output.write(
        reinterpret_cast<const char *>(data),
        static_cast<qint64>(len)
    );

    astro_media_bytes_free(data, len);

    if (written != static_cast<qint64>(len)) {
        output.cancelWriting();

        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not write the complete video to cache")
        );
        return;
    }

    if (!output.commit()) {
        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Could not save cached video: ") +
            output.errorString()
        );
        return;
    }

    const QFileInfo savedFile(filePath);

    if (!savedFile.exists() ||
        !savedFile.isFile() ||
        !savedFile.isReadable() ||
        savedFile.size() != static_cast<qint64>(len)) {

        QFile::remove(filePath);

        emit messageVideoFailed(
            requestKey,
            QStringLiteral("Cached video failed verification")
        );
        return;
    }

    emit messageVideoRetrieved(
        requestKey,
        QUrl::fromLocalFile(filePath).toString()
    );
}
