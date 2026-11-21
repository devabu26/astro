

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


void Backend::setStorageReady(bool ready)
{
    if (m_storageReady == ready) {
        return;
    }

    m_storageReady = ready;

    emit storageReadyChanged();
}