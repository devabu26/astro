

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
#include <QStringList>
#include <gio/gio.h>

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

// =============================================================
// Ubuntu Touch application lifecycle exemption
// =============================================================

namespace {

static const char kLifecycleSchema[] = "com.canonical.qtmir";
static const char kLifecycleKey[] = "lifecycle-exempt-appids";
static const char kAstroAppId[] = "astro.madabu";

GSettings *createLifecycleSettings(QString *error)
{
    GSettingsSchemaSource *source =
        g_settings_schema_source_get_default();

    if (!source) {
        if (error)
            *error = QStringLiteral("Ubuntu Touch settings schemas are unavailable.");
        return nullptr;
    }

    GSettingsSchema *schema = g_settings_schema_source_lookup(
        source, kLifecycleSchema, TRUE
    );

    if (!schema) {
        if (error)
            *error = QStringLiteral("The Ubuntu Touch lifecycle settings schema was not found.");
        return nullptr;
    }

    if (!g_settings_schema_has_key(schema, kLifecycleKey)) {
        g_settings_schema_unref(schema);
        if (error)
            *error = QStringLiteral("The lifecycle exemption setting is not available on this device.");
        return nullptr;
    }

    GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
    g_settings_schema_unref(schema);
    return settings;
}

} // namespace

bool Backend::lifecycleExemptionEnabled() const
{
    return m_lifecycleExemptionEnabled;
}

QString Backend::lifecycleExemptionError() const
{
    return m_lifecycleExemptionError;
}

void Backend::setLifecycleExemptionState(
    bool enabled,
    const QString &error
)
{
    if (m_lifecycleExemptionEnabled == enabled
            && m_lifecycleExemptionError == error) {
        return;
    }

    m_lifecycleExemptionEnabled = enabled;
    m_lifecycleExemptionError = error;
    emit lifecycleExemptionChanged();
}

bool Backend::refreshLifecycleExemption()
{
    QString error;
    GSettings *settings = createLifecycleSettings(&error);

    if (!settings) {
        setLifecycleExemptionState(
            m_lifecycleExemptionEnabled, error
        );
        qWarning() << "Could not read Ubuntu Touch lifecycle exemption:"
                   << error;
        return false;
    }

    gchar **appIds = g_settings_get_strv(
        settings, kLifecycleKey
    );

    bool enabled = false;
    for (gchar **entry = appIds; entry && *entry; ++entry) {
        if (g_strcmp0(*entry, kAstroAppId) == 0) {
            enabled = true;
            break;
        }
    }

    g_strfreev(appIds);
    g_object_unref(settings);

    setLifecycleExemptionState(enabled, QString());
    return true;
}

bool Backend::setLifecycleExemption(bool enabled)
{
    QString error;
    GSettings *settings = createLifecycleSettings(&error);

    if (!settings) {
        setLifecycleExemptionState(
            m_lifecycleExemptionEnabled, error
        );
        qWarning() << "Could not update Ubuntu Touch lifecycle exemption:"
                   << error;
        return false;
    }

    if (!g_settings_is_writable(settings, kLifecycleKey)) {
        g_object_unref(settings);
        error = QStringLiteral(
            "Ubuntu Touch does not allow Astro to change this system setting."
        );
        setLifecycleExemptionState(
            m_lifecycleExemptionEnabled, error
        );
        qWarning() << error;
        return false;
    }

    gchar **current = g_settings_get_strv(
        settings, kLifecycleKey
    );

    QStringList appIds;
    for (gchar **entry = current; entry && *entry; ++entry)
        appIds.append(QString::fromUtf8(*entry));
    g_strfreev(current);

    const QString astroId = QString::fromLatin1(kAstroAppId);
    if (enabled) {
        if (!appIds.contains(astroId))
            appIds.append(astroId);
    } else {
        appIds.removeAll(astroId);
    }

    gchar **updated = g_new0(gchar *, appIds.size() + 1);
    for (int i = 0; i < appIds.size(); ++i)
        updated[i] = g_strdup(appIds.at(i).toUtf8().constData());

    const gboolean accepted = g_settings_set_strv(
        settings,
        kLifecycleKey,
        const_cast<const gchar * const *>(updated)
    );

    g_strfreev(updated);
    g_settings_sync();
    g_object_unref(settings);

    if (!accepted) {
        error = QStringLiteral(
            "Ubuntu Touch rejected the lifecycle exemption change."
        );
        setLifecycleExemptionState(
            m_lifecycleExemptionEnabled, error
        );
        qWarning() << error;
        return false;
    }

    if (!refreshLifecycleExemption())
        return false;

    if (m_lifecycleExemptionEnabled != enabled) {
        error = QStringLiteral(
            "The system setting did not retain the requested change."
        );
        setLifecycleExemptionState(
            m_lifecycleExemptionEnabled, error
        );
        qWarning() << error;
        return false;
    }

    return true;
}
