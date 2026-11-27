

#include "Backend.h"
#include "StorageWorker/StorageWorker.h"
#include "notifications/NotificationManager.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDebug>
#include <QDataStream>
#include <QUrl>
#include <cmath>
#include <algorithm>
#include <QUuid>
#include <QMap>
#include <QVariantMap>
#include <QList>
#include <QThread>
#include <QMetaObject>
#include <functional>
#include <QStringList>

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


Backend::~Backend()
{
    if (m_storageThread && m_storageThread->isRunning()) {

        // Close Rust-side resources on the worker thread.
        // BlockingQueuedConnection ensures this finishes before
        // the worker thread is stopped.

        if (m_storageWorker) {
            QMetaObject::invokeMethod(
                m_storageWorker,
                "closeStorage",
                Qt::BlockingQueuedConnection
            );
        }

        m_storageThread->quit();
        m_storageThread->wait();
    }
}

// =============================================================
// General application status
// =============================================================


QString Backend::rtcRingbackToneUrl() const
{
    const QString cachePath =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cachePath.isEmpty()) {
        qWarning() << "ASTRO_CALL_RINGBACK: CacheLocation is empty"
                   << "applicationName:" << QCoreApplication::applicationName()
                   << "applicationDirPath:" << QCoreApplication::applicationDirPath();
        return QString();
    }

    QDir directory;
    if (!directory.mkpath(cachePath)) {
        qWarning() << "ASTRO_CALL_RINGBACK: could not create cache directory"
                   << cachePath
                   << "exists:" << QFileInfo::exists(cachePath)
                   << "parentWritable:" << QFileInfo(QFileInfo(cachePath).absolutePath()).isWritable();
        return QString();
    }

    const QFileInfo directoryInfo(cachePath);
    if (!directoryInfo.isDir() || !directoryInfo.isWritable()) {
        qWarning() << "ASTRO_CALL_RINGBACK: cache directory is not writable"
                   << cachePath
                   << "isDir:" << directoryInfo.isDir()
                   << "isWritable:" << directoryInfo.isWritable();
        return QString();
    }

    const QString filePath =
        directory.filePath(QStringLiteral("rtc-ringback.wav"));
    constexpr quint32 sampleRate = 48000;
    constexpr quint32 sampleCount = sampleRate * 3;
    constexpr quint32 pcmBytes = sampleCount * 2;
    constexpr qint64 expectedFileSize = 44 + pcmBytes;

    QFile existing(filePath);
    if (existing.open(QIODevice::ReadOnly)) {
        const bool correctSize = existing.size() == expectedFileSize;
        const QByteArray header = existing.read(12);
        const bool validHeader =
            header.size() == 12 &&
            header.mid(0, 4) == QByteArrayLiteral("RIFF") &&
            header.mid(8, 4) == QByteArrayLiteral("WAVE");
        if (correctSize && validHeader) {
            existing.close();
            qDebug() << "ASTRO_CALL_RINGBACK: using cached WAV"
                     << filePath << "bytes:" << expectedFileSize;
            return QUrl::fromLocalFile(filePath).toString();
        }

        qWarning() << "ASTRO_CALL_RINGBACK: cached WAV is invalid; regenerating"
                   << filePath
                   << "bytes:" << existing.size()
                   << "expectedBytes:" << expectedFileSize
                   << "validHeader:" << validHeader;
        existing.close();
    } else if (QFileInfo::exists(filePath)) {
        qWarning() << "ASTRO_CALL_RINGBACK: existing WAV cannot be opened"
                   << filePath << existing.errorString()
                   << "error:" << existing.error();
    }

    QByteArray pcm;
    pcm.resize(static_cast<int>(pcmBytes));
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());

    // Australian-style ringback cadence: 400 ms on, 200 ms off,
    // 400 ms on, then 2 seconds of silence.
    constexpr quint32 firstEnd = sampleRate * 2 / 5;
    constexpr quint32 secondStart = sampleRate * 3 / 5;
    constexpr quint32 secondEnd = sampleRate;
    constexpr quint32 fadeSamples = sampleRate / 200;

    for (quint32 i = 0; i < sampleCount; ++i) {
        const bool toneOn = i < firstEnd ||
            (i >= secondStart && i < secondEnd);
        if (!toneOn) {
            samples[i] = 0;
            continue;
        }

        const quint32 pulsePosition =
            i < firstEnd ? i : i - secondStart;
        const quint32 pulseLength = firstEnd;
        const quint32 edgeDistance =
            std::min(pulsePosition, pulseLength - 1 - pulsePosition);
        const double envelope = std::min(
            1.0,
            static_cast<double>(edgeDistance) /
                static_cast<double>(fadeSamples)
        );
        const double phase =
            2.0 * 3.14159265358979323846 * 400.0 *
            static_cast<double>(i) / static_cast<double>(sampleRate);
        samples[i] = static_cast<qint16>(
            std::sin(phase) * 6000.0 * envelope
        );
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "ASTRO_CALL_RINGBACK: cannot open WAV for writing"
                   << filePath << file.errorString()
                   << "error:" << file.error();
        return QString();
    }

    auto writeExact = [&file, &filePath](const char *data, qint64 length,
                                         const char *section) -> bool {
        const qint64 written = file.write(data, length);
        if (written != length) {
            qWarning() << "ASTRO_CALL_RINGBACK: WAV write failed"
                       << "section:" << section
                       << "path:" << filePath
                       << "expectedBytes:" << length
                       << "writtenBytes:" << written
                       << "error:" << file.errorString()
                       << "errorCode:" << file.error();
            return false;
        }
        return true;
    };

    bool writeOk = writeExact("RIFF", 4, "RIFF header");
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);

    if (writeOk) {
        stream << static_cast<quint32>(36 + pcmBytes);
        writeOk = stream.status() == QDataStream::Ok;
    }
    if (writeOk)
        writeOk = writeExact("WAVEfmt ", 8, "WAVE/fmt header");
    if (writeOk) {
        stream << static_cast<quint32>(16)
               << static_cast<quint16>(1)
               << static_cast<quint16>(1)
               << sampleRate
               << static_cast<quint32>(sampleRate * 2)
               << static_cast<quint16>(2)
               << static_cast<quint16>(16);
        writeOk = stream.status() == QDataStream::Ok;
    }
    if (writeOk)
        writeOk = writeExact("data", 4, "data header");
    if (writeOk) {
        stream << pcmBytes;
        writeOk = stream.status() == QDataStream::Ok;
    }
    if (writeOk)
        writeOk = writeExact(pcm.constData(), pcm.size(), "PCM payload");

    if (!writeOk) {
        if (stream.status() != QDataStream::Ok) {
            qWarning() << "ASTRO_CALL_RINGBACK: WAV header stream failed"
                       << filePath << "streamStatus:" << stream.status()
                       << "fileError:" << file.errorString()
                       << "fileErrorCode:" << file.error();
        }
        file.close();
        if (!file.remove()) {
            qWarning() << "ASTRO_CALL_RINGBACK: could not remove incomplete WAV"
                       << filePath << file.errorString();
        }
        return QString();
    }

    if (!file.flush()) {
        qWarning() << "ASTRO_CALL_RINGBACK: flushing WAV failed"
                   << filePath << file.errorString()
                   << "error:" << file.error();
        file.close();
        file.remove();
        return QString();
    }

    const qint64 writtenSize = file.size();
    file.close();
    if (writtenSize != expectedFileSize) {
        qWarning() << "ASTRO_CALL_RINGBACK: generated WAV has unexpected size"
                   << filePath
                   << "bytes:" << writtenSize
                   << "expectedBytes:" << expectedFileSize;
        QFile::remove(filePath);
        return QString();
    }

    QFile verification(filePath);
    if (!verification.open(QIODevice::ReadOnly)) {
        qWarning() << "ASTRO_CALL_RINGBACK: generated WAV cannot be reopened"
                   << filePath << verification.errorString()
                   << "error:" << verification.error();
        QFile::remove(filePath);
        return QString();
    }
    const QByteArray finalHeader = verification.read(44);
    const bool validFinalHeader =
        finalHeader.size() == 44 &&
        finalHeader.mid(0, 4) == QByteArrayLiteral("RIFF") &&
        finalHeader.mid(8, 4) == QByteArrayLiteral("WAVE") &&
        finalHeader.mid(36, 4) == QByteArrayLiteral("data");
    verification.close();

    if (!validFinalHeader) {
        qWarning() << "ASTRO_CALL_RINGBACK: generated WAV header failed verification"
                   << filePath
                   << "headerBytes:" << finalHeader.size();
        QFile::remove(filePath);
        return QString();
    }

    qDebug() << "ASTRO_CALL_RINGBACK: generated WAV successfully"
             << filePath
             << "bytes:" << writtenSize
             << "sampleRate:" << sampleRate
             << "durationMs:" << 3000;

    return QUrl::fromLocalFile(filePath).toString();
}

QString Backend::status() const
{
    const char *rustStatus = astro_get_status();

    if (!rustStatus) {
        return QStringLiteral(
            "Astro Rust backend returned no status!"
        );
    }

    return QString::fromUtf8(rustStatus);
}

// =============================================================
// Application data directory
// =============================================================


QString Backend::prepareDataDirectory() const
{
    const QString path =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (path.isEmpty()) {
        return QString();
    }

    QDir directory;

    if (!directory.mkpath(path)) {
        return QString();
    }

    const QFileInfo info(path);

    if (!info.isDir() || !info.isWritable()) {
        return QString();
    }

    return path;
}

// =============================================================
// Existing encrypted storage detection
// =============================================================


bool Backend::hasExistingStorage() const
{
    const QString path =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation
        );

    if (path.isEmpty()) {
        return false;
    }

    const QDir directory(path);

    if (!directory.exists()) {
        return false;
    }

    return
        QFileInfo::exists(
            directory.filePath("matrix-sdk-state.sqlite3")
        )
        ||
        QFileInfo::exists(
            directory.filePath("matrix-sdk-event-cache.sqlite3")
        )
        ||
        QFileInfo::exists(
            directory.filePath("matrix-sdk-media.sqlite3")
        )
        ||
        QFileInfo::exists(
            directory.filePath("matrix-sdk-crypto.sqlite3")
        );
}

// =============================================================
// Secret Service status diagnostic
// =============================================================


bool Backend::storageReady() const
{
    return m_storageReady;
}


int Backend::applicationState() const
{
    return m_applicationState;
}


void Backend::setStorageReady(bool ready)
{
    if (m_storageReady == ready) {
        return;
    }

    m_storageReady = ready;

    emit storageReadyChanged();
}
