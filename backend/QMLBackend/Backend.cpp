

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
    if (cachePath.isEmpty())
        return QString();

    QDir directory;
    if (!directory.mkpath(cachePath))
        return QString();

    const QString filePath =
        directory.filePath(QStringLiteral("rtc-ringback.wav"));
    constexpr quint32 sampleRate = 48000;
    constexpr quint32 sampleCount = sampleRate * 3;
    constexpr quint32 pcmBytes = sampleCount * 2;

    QFile existing(filePath);
    if (existing.open(QIODevice::ReadOnly) &&
        existing.size() == static_cast<qint64>(44 + pcmBytes)) {
        existing.close();
        return QUrl::fromLocalFile(filePath).toString();
    }
    existing.close();

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
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();

    file.write("RIFF", 4);
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << static_cast<quint32>(36 + pcmBytes);
    file.write("WAVEfmt ", 8);
    stream << static_cast<quint32>(16)
           << static_cast<quint16>(1)
           << static_cast<quint16>(1)
           << sampleRate
           << static_cast<quint32>(sampleRate * 2)
           << static_cast<quint16>(2)
           << static_cast<quint16>(16);
    file.write("data", 4);
    stream << pcmBytes;
    if (file.write(pcm) != pcm.size() || stream.status() != QDataStream::Ok) {
        file.close();
        file.remove();
        return QString();
    }

    file.close();
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
