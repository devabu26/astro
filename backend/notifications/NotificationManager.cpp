#include "NotificationManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

NotificationManager::NotificationManager(QObject *parent)
    : QObject(parent)
{
}

void NotificationManager::processTimelineJson(const QString &json)
{
    const QJsonDocument document =
        QJsonDocument::fromJson(json.toUtf8());

    if (!document.isObject()) {
        return;
    }

    const QJsonArray notifications =
        document.object().value(
            QStringLiteral("notifications")
        ).toArray();

    for (const QJsonValue &value : notifications) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject event = value.toObject();

        const QString eventId =
            event.value(QStringLiteral("event_id")).toString();
        const QString sender =
            event.value(QStringLiteral("sender")).toString();
        const QString body =
            event.value(QStringLiteral("body")).toString();
        const QString roomId =
            event.value(QStringLiteral("room_id")).toString();

        if (eventId.isEmpty() || sender.isEmpty() || body.isEmpty()) {
            continue;
        }

        if (m_seenEventIds.contains(eventId)) {
            continue;
        }

        m_seenEventIds.insert(eventId);
        m_eventIdOrder.enqueue(eventId);

        while (m_eventIdOrder.size() > 512) {
            m_seenEventIds.remove(m_eventIdOrder.dequeue());
        }

        emit notificationReceived(sender, body, roomId, eventId);
    }
}
