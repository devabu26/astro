#pragma once

#include <QAbstractVideoFilter>
#include <QPointer>
#include <QMutex>
#include <QString>

class RtcCameraVideoFilter : public QAbstractVideoFilter
{
    Q_OBJECT
    Q_PROPERTY(QObject *backend READ backend WRITE setBackend)
    Q_PROPERTY(QString roomId READ roomId WRITE setRoomId)

public:
    explicit RtcCameraVideoFilter(QObject *parent = nullptr);

    QObject *backend() const;
    void setBackend(QObject *backend);

    QString roomId() const;
    void setRoomId(const QString &roomId);

    QVideoFilterRunnable *createFilterRunnable() override;

private:
    mutable QMutex m_mutex;
    QPointer<QObject> m_backend;
    QString m_roomId;
};
