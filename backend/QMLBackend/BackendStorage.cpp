

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


void Backend::openStorage(const QString &passphrase)
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "A storage operation is already in progress"
            )
        );

        return;
    }

    if (m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral(
                "Encrypted storage is already open"
            )
        );

        return;
    }

    if (passphrase.isEmpty()) {
        emit storageStatusChanged(
            QStringLiteral(
                "Please enter a storage passphrase"
            )
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Opening encrypted storage...")
    );

    emit requestOpenStorage(passphrase);
}

// =============================================================
// Matrix login
// =============================================================


void Backend::closeStorage()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral(
                "A storage operation is already in progress"
            )
        );

        return;
    }

    if (!m_storageReady) {
        emit storageStatusChanged(
            QStringLiteral("Encrypted storage is not open")
        );

        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Closing encrypted storage...")
    );

    emit requestCloseStorage();
}

// =============================================================
// Delete all Astro application data
// =============================================================


void Backend::deleteAllData()
{
    if (m_storageOperationInProgress) {
        emit storageStatusChanged(
            QStringLiteral("Another operation is already in progress")
        );
        return;
    }

    const QString path = prepareDataDirectory();

    if (path.isEmpty()) {
        emit storageStatusChanged(
            QStringLiteral("Could not determine Astro data directory")
        );
        return;
    }

    m_storageOperationInProgress = true;

    emit storageStatusChanged(
        QStringLiteral("Deleting Astro local data...")
    );

    // Run logout, client cleanup and deletion on the storage thread.
    QMetaObject::invokeMethod(
        m_storageWorker,
        [this, path]()
        {
            m_storageWorker->deleteAllData(path);
        },
        Qt::QueuedConnection
    );
}
