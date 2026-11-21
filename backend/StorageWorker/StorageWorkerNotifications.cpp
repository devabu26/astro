
#include "StorageWorker.h"

#include <QByteArray>
#include <QDebug>

// =============================================================
// Matrix push notification settings
// =============================================================

void StorageWorker::setPushToken(const QString &token)
{
    QByteArray newToken = token.toUtf8();

    if (newToken.isEmpty()) {
        return;
    }

    if (m_pushToken != newToken) {
        m_pushToken.fill('\0');
        m_pushToken = newToken;
    }

    newToken.fill('\0');
    registerPushTokenIfReady();
}

void StorageWorker::setPushPrivacyLevel(const QString &privacyLevel)
{
    if (privacyLevel != "sender_room"
        && privacyLevel != "full_message"
        && privacyLevel != "private") {
        qWarning() << "ASTRO: Invalid notification privacy level";
        return;
    }

    if (m_pushPrivacyLevel == privacyLevel) {
        return;
    }

    m_pushPrivacyLevel = privacyLevel;
    registerPushTokenIfReady();
}

void StorageWorker::setPushNotificationsEnabled(bool enabled)
{
    if (m_pushNotificationsEnabled == enabled) {
        return;
    }

    m_pushNotificationsEnabled = enabled;
    registerPushTokenIfReady();
}

void StorageWorker::registerPushTokenIfReady()
{
    if (!m_matrixSessionActive || !m_matrixClient || m_pushToken.isEmpty()) {
        return;
    }

    if (!m_pushNotificationsEnabled) {
        char *error = nullptr;

        const int result = astro_matrix_unregister_push_token(
            m_matrixClient,
            m_pushToken.constData(),
            &error
        );

        if (error) {
            qWarning() << "ASTRO: Matrix push pusher removal error:" << error;
            astro_string_free(error);
        }

        if (result != 0) {
            qWarning() << "ASTRO: Matrix push pusher unregistration failed";
        } else {
            qInfo() << "ASTRO: Matrix push pusher unregistered";
        }

        return;
    }

    char *error = nullptr;
    const QByteArray privacyLevel = m_pushPrivacyLevel.toUtf8();

    const int result = astro_matrix_register_push_token(
        m_matrixClient,
        m_pushToken.constData(),
        privacyLevel.constData(),
        &error
    );

    if (error) {
        astro_string_free(error);
    }

    if (result != 0) {
        qWarning() << "ASTRO: Matrix push pusher registration failed";
    } else {
        qInfo() << "ASTRO: Matrix push pusher registered";
    }
}
