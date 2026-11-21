#pragma once

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

class NotificationManager : public QObject
{
    Q_OBJECT

public:
    explicit NotificationManager(QObject *parent = nullptr);

public slots:
    void processTimelineJson(const QString &json);

signals:
    void notificationReceived(
        const QString &sender,
        const QString &body,
        const QString &roomId,
        const QString &eventId
    );

private:
    QSet<QString> m_seenEventIds;
    QQueue<QString> m_eventIdOrder;
};
